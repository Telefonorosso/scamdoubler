// paula.cpp -- amiga paula 8364 audio chip emulator. clean-room, MIT.
// see paula.h for the design notes.

#include "paula.h"


namespace amiga {

namespace {

// paula's 6-bit volume (0..64) -> linear gain. the real dac is quasi-linear;
// we take it exactly linear. >64 is clamped (some replayers write past it and
// lean on hardware clipping).
inline float volumeToGain(uint16_t vol) noexcept {
    const uint16_t v = (vol < 64u ? vol : 64u);
    return float(v) / 64.0f;
}

// signed 8-bit sample -> float [-1, +1].
inline float sampleToFloat(int8_t s) noexcept {
    return float(s) / 128.0f;
}

// stereo routing: ch0+3 -> left, ch1+2 -> right. -1 = left, +1 = right.
inline int channelStereoSide(int ch) noexcept {
    switch (ch) {
        case 0: case 3: return -1;
        case 1: case 2: return +1;
        default:         return 0;
    }
}

} // namespace

Paula::Paula(double clockHz) noexcept : clockHz_(clockHz) {
    reset();
}

void Paula::reset() noexcept {
    for (auto& c : channels_) {
        c = Channel{};
    }
    dmaCon_ = 0;
    intReq_ = 0;
    intEna_ = 0;
    adkCon_ = 0;
    vhposrCounter_ = 0;
    /* Mixer gains are output state: preserve them across replay resyncs. */
}

void Paula::writeRegister16(uint32_t addr, uint16_t value) noexcept {
    const uint32_t reg = addr & 0xFFFu;

    // dma control. setclr=1 -> OR the bits in, setclr=0 -> AND-NOT (clear).
    // the standard $dff09x semantics.
    if (reg == kReg_DMACON) {
        const bool setClr = (value & kDmaConSetClr) != 0;
        const uint16_t bits = value & 0x7FFFu;
        if (setClr) dmaCon_ |= bits;
        else        dmaCon_ &= ~bits;
        // a channel that just came on re-primes: it reloads from
        // audxlch/audxlen on the next fetch. a channel already running is
        // left alone.
        for (int ch = 0; ch < kPaulaChannels; ++ch) {
            const uint16_t mask = uint16_t(1u << ch);
            if ((dmaCon_ & mask) && (dmaCon_ & kDmaConMaster)) {
                if (!channels_[ch].dmaWantsRestart) {
                    // running -- don't disturb.
                } else {
                    auto& c = channels_[ch];
                    c.curPtr = c.locPtrLatched;
                    c.curWordsLeft = c.lenWordsLatched;
                    c.onLowByte = true;
                    c.curSampleL = c.curSampleH = 0;
                    c.curOut = c.nextOut = 0;
                    c.phase = 0.0;
                    c.dmaWantsRestart = false;
                }
            } else {
                channels_[ch].dmaWantsRestart = true;
            }
        }
        return;
    }
    if (reg == kReg_INTREQ) {
        const bool setClr = (value & kDmaConSetClr) != 0;
        const uint16_t bits = value & 0x7FFFu;
        if (setClr) intReq_ |= bits;
        else        intReq_ &= ~bits;
        return;
    }
    if (reg == kReg_INTENA) {
        const bool setClr = (value & kDmaConSetClr) != 0;
        const uint16_t bits = value & 0x7FFFu;
        if (setClr) intEna_ |= bits;
        else        intEna_ &= ~bits;
        return;
    }
    if (reg == kReg_ADKCON) {
        const bool setClr = (value & kDmaConSetClr) != 0;
        const uint16_t bits = value & 0x7FFFu;
        if (setClr) adkCon_ |= bits;
        else        adkCon_ &= ~bits;
        return;
    }

    // per-channel block.
    for (int ch = 0; ch < kPaulaChannels; ++ch) {
        if (reg == kReg_AUDxLCH(ch)) {
            channels_[ch].locPtrLatched =
                (channels_[ch].locPtrLatched & 0x0000FFFFu) |
                (uint32_t(value) << 16);
            return;
        }
        if (reg == kReg_AUDxLCL(ch)) {
            channels_[ch].locPtrLatched =
                (channels_[ch].locPtrLatched & 0xFFFF0000u) |
                uint32_t(value);
            return;
        }
        if (reg == kReg_AUDxLEN(ch)) {
            // real paula reads len=0 as 65536 words. mirror that.
            channels_[ch].lenWordsLatched = value ? value : 65536u;
            return;
        }
        if (reg == kReg_AUDxPER(ch)) {
            // period 0 = divide by zero; real hw also chokes below ~124 and
            // eats dma bandwidth. clamp to 1, music never writes that low.
            channels_[ch].period = (value ? value : 1u);
            return;
        }
        if (reg == kReg_AUDxVOL(ch)) {
            channels_[ch].volume = value & 0x7Fu;
            return;
        }
        if (reg == kReg_AUDxDAT(ch)) {
            // direct dma-bypass write. not modelled (nothing here needs it).
            return;
        }
    }
}

void Paula::setLoop(int ch, uint32_t loc, uint16_t lenWords) noexcept {
    if (ch < 0 || ch >= kPaulaChannels) return;
    // overwrite ONLY the latched (reload-at-block-end) pair. the live playhead
    // (curPtr / curWordsLeft) is left alone, so the one-shot just programmed
    // finishes and the channel then loops this region -- the amiga audxlc+audxlen
    // reload mechanism (advanceOneSourceSample_'s curWordsLeft==0 branch copies
    // locPtrLatched/lenWordsLatched into the live registers).
    channels_[ch].locPtrLatched   = loc;
    channels_[ch].lenWordsLatched = lenWords ? lenWords : 65536u;
}

uint16_t Paula::readRegister16(uint32_t addr) const noexcept {
    const uint32_t reg = addr & 0xFFFu;
    if (reg == kReg_DMACONR) return dmaCon_;
    if (reg == kReg_INTREQR) return intReq_;
    if (reg == kReg_INTENAR) return intEna_;
    if (reg == kReg_ADKCONR) return adkCon_;
    // vhposr ($dff006) + vpos ($dff004). some replayers busy-wait on the beam
    // position to sync to the raster (move.w vhposr,d5; add #4<<8,d5; .wait:
    // cmp.w vhposr,d5; bgt.s .wait). there's no real raster here, so we just
    // advance the counter on every read -- +0x100 = +1 raster line -- and the
    // loop terminates after a handful of polls whatever the target was.
    if (reg == 0x006u) { // vhposr
        vhposrCounter_ = uint16_t((vhposrCounter_ + 0x0100u) & 0xFFFFu);
        return vhposrCounter_;
    }
    if (reg == 0x004u) { // vpos -- high byte (vertical position)
        vhposrCounter_ = uint16_t((vhposrCounter_ + 0x0100u) & 0xFFFFu);
        return uint16_t((vhposrCounter_ >> 8) & 0x00FFu);
    }
    return 0;
}

void Paula::advanceOneSourceSample_(Channel& c, int chIdx) noexcept {
    // one paula tick = one signed 8-bit sample. paula fetches a word (2 bytes)
    // per dma transfer and plays the bytes in linear memory order. we model
    // that order straight: read a byte, advance, repeat.

    if (!read_) {
        c.curOut = c.nextOut = 0;
        return;
    }

    // dma enable = master + this channel's bit.
    const uint16_t mask = uint16_t(1u << chIdx);
    if ((dmaCon_ & mask) == 0 || (dmaCon_ & kDmaConMaster) == 0) {
        c.curOut = c.nextOut = 0;
        return;
    }

    if (c.onLowByte) {
        // at a word boundary: check exhaustion first -- paula raises the irq
        // on the start-of-word that would have been the new pointer load.
        if (c.curWordsLeft == 0) {
            // reload from latched values = the loop point.
            c.curPtr = c.locPtrLatched;
            c.curWordsLeft = c.lenWordsLatched;
            // set the intreq bit regardless of intena (the request flag is
            // independent of enable). the host's callback decides what to do
            // with it.
            const uint16_t intBit = uint16_t(uint16_t(0x80u) << chIdx);
            intReq_ |= intBit;
            if (onInterrupt_) {
                onInterrupt_(intBit);
            }
        }
        // fetch the next word: 2 bytes from chip ram.
        c.curSampleL = read_(c.curPtr);
        c.curSampleH = read_(c.curPtr + 1);
        c.curPtr += 2;
        if (c.curWordsLeft > 0) {
            --c.curWordsLeft;
        }
        c.curOut = c.nextOut;
        c.nextOut = int8_t(c.curSampleL);
        c.onLowByte = false;
    } else {
        c.curOut = c.nextOut;
        c.nextOut = int8_t(c.curSampleH);
        c.onLowByte = true;
    }
}

void Paula::serviceChannel_(Channel& c, int chIdx,
                            double samplesPerOutFrame,
                            float& mixOut) noexcept {
    // paula period -> source rate in hz: src_hz = clock / period.
    const double srcSrHz = clockHz_ / double((c.period ? c.period : 1u));

    // source samples to advance per output frame. the caller hands us
    // 1/outSr, so step = src_hz / outSr.
    const double step = srcSrHz * samplesPerOutFrame;

    c.phase += step;
    while (c.phase >= 1.0) {
        c.phase -= 1.0;
        advanceOneSourceSample_(c, chIdx);
    }

    // resample. nearest = zero-order hold (bright, the amiga default); linear =
    // one-tap interp between the current and next source sample (softer).
    float interp;
    if (interp_ == Interp::Linear) {
        const float a = sampleToFloat(c.curOut);
        const float b = sampleToFloat(c.nextOut);
        interp = a + float(c.phase) * (b - a);
    } else {
        interp = sampleToFloat(c.curOut);
    }

    // anti-click: glide the channel gain toward its target over ~2 ms instead
    // of stepping. paula volume writes are otherwise instantaneous, so every
    // note-on / envelope step / setvolume produced a zipper click.
    const float targetGain = volumeToGain(c.volume);
    c.gainSmoothed += (targetGain - c.gainSmoothed) * gainSmoothCoeff_;

    // a muted channel ran its full dma + gain state above; only its mix
    // contribution is dropped, so unmuting is glitch-free.
    if (!channelMuted_[chIdx]) {
        mixOut += interp * c.gainSmoothed;
    }
}

void Paula::render(float* outL, float* outR, int frames, double outSr) noexcept {
    if (frames <= 0 || outSr <= 0.0) {
        return;
    }
    const double secondsPerOutFrame = 1.0 / outSr;
    // anti-click 1-pole coefficient for a ~2 ms gain glide at this rate.
    /* Fixed 48 kHz POC: original 2 ms exponential gain coefficient. */
    gainSmoothCoeff_ = 0.0103626011f;

    // stereo-separation cross-blend: a hard-panned LRRL voice bleeds (1-sep)/2
    // into the opposite side. sep=1.0 -> exact hard-pan; sep<1 -> natural image.
    /* 2 ms linear output-only transitions. DMA, sample pointers and
     * the physical Paula are never affected by the mixer mute state. */
    const float step = 1.0f / 96.0f;
    for (int f = 0; f < frames; ++f) {
        float delta = stereoSep_ - stereoCurrent_;
        if (delta > step) delta=step;
        if (delta < -step) delta=-step;
        stereoCurrent_ += delta;
        const float toOpp = (1.0f - stereoCurrent_) * 0.5f;
        const float toSame = 1.0f - toOpp;
        float l = 0.0f, r = 0.0f;
        for (int ch = 0; ch < kPaulaChannels; ++ch) {
            float mono = 0.0f;
            serviceChannel_(channels_[ch], ch, secondsPerOutFrame, mono);
            const float target = channelMuted_[ch] ? 0.0f : 1.0f;
            float d = target - channelMixGain_[ch];
            if (d > step) d=step;
            if (d < -step) d=-step;
            channelMixGain_[ch] += d;
            mono *= channelMixGain_[ch];
            if (channelStereoSide(ch) < 0) { l += mono * toSame; r += mono * toOpp; }
            else                           { r += mono * toSame; l += mono * toOpp; }
        }
        outL[f] = l;
        outR[f] = r;
    }
}

Paula::FetchState Paula::fetchState(int ch) const noexcept {
    FetchState s{};
    if (ch < 0 || ch >= kPaulaChannels) return s;
    const auto& c=channels_[ch];
    const uint16_t mask=uint16_t(1u<<ch);
    s.cursor=c.curPtr;
    s.wordsLeft=c.curWordsLeft;
    s.loop=c.locPtrLatched;
    s.loopWords=c.lenWordsLatched;
    s.enabled=(dmaCon_&mask) && (dmaCon_&kDmaConMaster);
    if (c.dmaWantsRestart || (c.onLowByte && !s.wordsLeft)) {
        s.cursor=s.loop;
        s.wordsLeft=s.loopWords;
    }
    return s;
}

Paula::ChannelState Paula::channelState(int ch) const noexcept {
    ChannelState s{};
    if (ch < 0 || ch >= kPaulaChannels) return s;
    const auto& c = channels_[ch];
    const uint16_t mask = uint16_t(1u << ch);
    s.locPtr      = c.locPtrLatched;
    s.lenWords    = uint16_t(c.lenWordsLatched);
    s.periodTicks = c.period;
    s.volume      = c.volume;
    s.dmaEnabled  = (dmaCon_ & mask) && (dmaCon_ & kDmaConMaster);
    return s;
}

} // namespace amiga
