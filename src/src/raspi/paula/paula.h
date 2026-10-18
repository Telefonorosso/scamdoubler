// paula.h -- amiga paula 8364 audio chip emulator
// ----------------------------------------------------------------------------
// clean-room. built from the public hardware docs only:
//   - amiga hardware reference manual, 3rd ed. (addison-wesley)
//   - http://amigadev.elowar.com/read/ADCD_2.1/Hardware_Manual_guide/
//   - en.wikipedia.org/wiki/Original_Chip_Set#Paula
// not a single line lifted from any other emulator.
//
// paula is the 4-channel sample-dma sound chip in every amiga (ocs/ecs/aga).
// each channel carries its own pointer, length, period (= colour-clock / rate)
// and volume. 8-bit signed samples, hardware loop through the audxlen reload.
// channels 0+3 go to the left output, 1+2 to the right -- the amiga way.
//
// scope is precision-of-behaviour: enough to drive real amiga music replayers
// at a modern output rate. not cycle-accurate silicon. modelled:
//   * dma fetch + 8-bit sample -> output at (colour-clock / period) rate
//     (pal 3.546895 mhz, ntsc 3.579545 mhz)
//   * instant period writes (audxper)
//   * audxlen reload at end-of-sample -> loop, the way trackers that don't
//     toggle dma per row expect
//   * a two-stage loop program: setLoop() latches the loop region a channel
//     reloads on its NEXT block-wrap WITHOUT disturbing the live playhead,
//     so a one-shot intro plays once and the body loops after (the amiga
//     audxlc+audxlen-rewrite-while-playing trick)
//   * volume 0..64 -> linear gain, with a ~2 ms anti-click glide so note-ons
//     and envelope steps don't zipper
//   * dmacon master bit (dmaen) + per-channel audxen bits
//   * per-channel mute (for stems / soloing) -- the channel keeps running,
//     only its mix contribution drops
// left out on purpose: audio modulation (adkcon amod), blitter-fed samples,
// the analog reconstruction filter -- hand bandwidth to your own resampler.
//
// resampling from paula's native rate to your output rate is selectable:
//   * nearest (default) -- zero-order hold. keeps (and aliases) the top
//     octave = the bright, authentic amiga character.
//   * linear -- one-tap interpolation. softer, less aliasing.
// stereo is the hard amiga ch0+3/1+2 split by default, blendable toward
// centre with setStereoSeparation() for a natural image on headphones.
//
// MIT. see LICENSE.
// Bare-metal adaptation: raw callbacks, built-in arrays, fixed 48 kHz smoothing.
// The original upstream files are preserved in the milestone archive.

#pragma once

#include <stdint.h>

namespace amiga {

// colour-clock used as the divisor in paula's period->hz formula:
//   out_hz = clock_hz / period
// pal machines (the demoscene standard): 3.546895 mhz. ntsc: 3.579545 mhz.
inline constexpr double kPalColorClockHz  = 3'546'895.0;
inline constexpr double kNtscColorClockHz = 3'579'545.0;

inline constexpr int kPaulaChannels = 4;  // ch0+3 -> L, ch1+2 -> R

// custom-chip register offsets (relative to the $dff000 base). replayers hit
// these directly with move.w / move.l.
inline constexpr uint32_t kCustomBase = 0x00DFF000u;
inline constexpr uint32_t kReg_DMACON  = 0x096u;
inline constexpr uint32_t kReg_DMACONR = 0x002u;
inline constexpr uint32_t kReg_INTREQ  = 0x09Cu;
inline constexpr uint32_t kReg_INTREQR = 0x01Eu;
inline constexpr uint32_t kReg_INTENA  = 0x09Au;
inline constexpr uint32_t kReg_INTENAR = 0x01Cu;
inline constexpr uint32_t kReg_ADKCON  = 0x09Eu;
inline constexpr uint32_t kReg_ADKCONR = 0x010u;

// per-channel block at $dff0a0 + 16*ch (ch = 0..3).
inline constexpr uint32_t kRegAudBase(int ch) {
    return 0x0A0u + uint32_t(ch) * 0x10u;
}
inline constexpr uint32_t kReg_AUDxLCH(int ch) { return kRegAudBase(ch) + 0x0u; }
inline constexpr uint32_t kReg_AUDxLCL(int ch) { return kRegAudBase(ch) + 0x2u; }
inline constexpr uint32_t kReg_AUDxLEN(int ch) { return kRegAudBase(ch) + 0x4u; }
inline constexpr uint32_t kReg_AUDxPER(int ch) { return kRegAudBase(ch) + 0x6u; }
inline constexpr uint32_t kReg_AUDxVOL(int ch) { return kRegAudBase(ch) + 0x8u; }
inline constexpr uint32_t kReg_AUDxDAT(int ch) { return kRegAudBase(ch) + 0xAu; }

// dmacon bits.
inline constexpr uint16_t kDmaConSetClr = 0x8000u;
inline constexpr uint16_t kDmaConAud0   = 0x0001u;
inline constexpr uint16_t kDmaConAud1   = 0x0002u;
inline constexpr uint16_t kDmaConAud2   = 0x0004u;
inline constexpr uint16_t kDmaConAud3   = 0x0008u;
inline constexpr uint16_t kDmaConAllAud = 0x000Fu;
inline constexpr uint16_t kDmaConMaster = 0x0200u;

// intreq / intena audio-finished bits.
inline constexpr uint16_t kIntAud0 = 0x0080u;
inline constexpr uint16_t kIntAud1 = 0x0100u;
inline constexpr uint16_t kIntAud2 = 0x0200u;
inline constexpr uint16_t kIntAud3 = 0x0400u;

// you give paula a way to read its sample stream out of your "chip ram".
// it never needs to know how your memory is laid out.
using ReadByteFn = uint8_t (*)(uint32_t address);

// paula raises intreq bits when a channel finishes its audxlen words (loop
// point). wire it to a cia timer or post it into your cpu's irq mask -- your
// call.
using InterruptFn = void (*)(uint16_t intBitMask);

class Paula {
public:
    explicit Paula(double clockHz = kPalColorClockHz) noexcept;

    // hook up your memory + interrupt callbacks. set the read callback before
    // render() or you get silence.
    void setReadByteCallback(ReadByteFn fn) { read_ = fn; }
    void setInterruptCallback(InterruptFn fn) { onInterrupt_ = fn; }

    // wipe all channel state. call on load + hard reset.
    void reset() noexcept;

    // a 16-bit register write from your cpu. addr is the full 24-bit address
    // (e.g. $dff096); we mask the high bits and dispatch. word writes only --
    // the real chip ignores byte writes to these registers anyway.
    void writeRegister16(uint32_t addr, uint16_t value) noexcept;

    // read back dmaconr / intreqr / intenar / adkconr (+ vhposr/vpos, see .cpp).
    uint16_t readRegister16(uint32_t addr) const noexcept;

    // render `frames` stereo frames at `outSr` hz into two mono float buffers.
    // ch0+3 sum into outL, ch1+2 into outR. range is roughly [-1, +1] for
    // typical 8-bit samples at full volume -- no clip/limit, your mix bus owns
    // that.
    void render(float* outL, float* outR, int frames, double outSr) noexcept;

    void setClockHz(double hz) noexcept { clockHz_ = hz; }
    double clockHz() const noexcept { return clockHz_; }

    // latch the LOOP region a channel reloads on its NEXT block-wrap, WITHOUT
    // touching the live playhead. mirrors the amiga two-stage program: write
    // audxlc+audxlen once for the one-shot, then latch the loop's loc/len so
    // the dma-finished reload picks them up. `loc` = absolute byte address in
    // your chip ram, `lenWords` = loop length in 16-bit words (audxlen units;
    // 0 = the full 64k words). a tracker that toggles dma per row doesn't need
    // this; sustained/looped synth voices do.
    void setLoop(int ch, uint32_t loc, uint16_t lenWords) noexcept;

    // resampling mode (see the file header). nearest is the default for the
    // authentic bright amiga sound; linear is softer with less aliasing.
    enum class Interp { Nearest, Linear };
    void setInterpolation(Interp mode) noexcept { interp_ = mode; }
    Interp interpolation() const noexcept { return interp_; }

    // stereo separation 0..1. 1.0 = the hard amiga ch0+3-left / ch1+2-right
    // ping-pong; 0.0 = mono. anything between cross-blends the LRRL voices
    // toward centre, turning the harsh hard-pan into a natural "cd" image on
    // headphones. default 0.85.
    void setStereoSeparation(float s) noexcept {
        stereoSep_ = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
    }
    float stereoSeparation() const noexcept { return stereoSep_; }

    // per-channel mute (stems / solo). default all-audible = a strict no-op.
    // a muted channel keeps its full dma + gain state running; only its
    // contribution to the mix is dropped, so unmuting is glitch-free.
    void setChannelMuted(int ch, bool muted) noexcept {
        if (ch >= 0 && ch < kPaulaChannels) channelMuted_[ch] = muted;
    }
    bool channelMuted(int ch) const noexcept {
        return (ch >= 0 && ch < kPaulaChannels) && channelMuted_[ch];
    }

    // peek at a channel -- handy for tests / vu meters.
    struct ChannelState {
        uint32_t locPtr;
        uint16_t lenWords;
        uint16_t periodTicks;
        uint16_t volume;
        bool     dmaEnabled;
    };
    ChannelState channelState(int ch) const noexcept;
    /* Read-only DMA cursor for bounded asynchronous sample prefetch. */
    struct FetchState {
        uint32_t cursor, wordsLeft, loop, loopWords;
        bool enabled;
    };
    FetchState fetchState(int ch) const noexcept;


    // raw dmacon mirror (master bit $0200 + per-channel bits 0..3).
    uint16_t rawDmaCon() const noexcept { return dmaCon_; }

private:
    struct Channel {
        uint32_t locPtrLatched = 0;   // last full audxlch:audxlcl write
        uint32_t lenWordsLatched = 0;
        uint16_t period   = 1;        // never zero
        uint16_t volume   = 0;
        uint32_t curPtr   = 0;        // live byte ptr into the sample
        uint32_t curWordsLeft = 0;    // words left until reload
        uint8_t  curSampleL = 0;      // 2 bytes per word
        uint8_t  curSampleH = 0;
        bool     onLowByte = true;
        double   phase    = 0.0;      // 0..1 sub-sample accumulator
        bool     dmaWantsRestart = true;
        int8_t   curOut   = 0;
        int8_t   nextOut  = 0;
        float    gainSmoothed = 0.0f; // anti-click: ~2 ms glide toward target gain
    };

    void serviceChannel_(Channel& c, int chIdx,
                         double samplesPerOutFrame,
                         float& mixOut) noexcept;
    void advanceOneSourceSample_(Channel& c, int chIdx) noexcept;

    double      clockHz_;
    Interp      interp_ = Interp::Nearest;  // bright amiga sound by default
    float       stereoSep_ = 0.85f;         // see setStereoSeparation()
    float       gainSmoothCoeff_ = 0.02f;   // anti-click 1-pole, set per render()
    ReadByteFn  read_;
    InterruptFn onInterrupt_;
    Channel channels_[kPaulaChannels];
    bool channelMuted_[kPaulaChannels]{};
    float channelMixGain_[kPaulaChannels] = {1.0f,1.0f,1.0f,1.0f};
    float stereoCurrent_ = 1.0f;  // default all-audible
    uint16_t    dmaCon_  = 0;
    uint16_t    intReq_  = 0;
    uint16_t    intEna_  = 0;
    uint16_t    adkCon_  = 0;
    // synthetic vhposr/vpos counter -- see readRegister16 in the .cpp.
    mutable uint16_t vhposrCounter_ = 0;
};

} // namespace amiga
