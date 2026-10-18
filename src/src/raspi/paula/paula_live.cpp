/*
 * Register-level live mirror of the original Akustikrausch Paula core.
 * MIT license in LICENSE. No Amiga hardware is driven from this file.
 */
#include "paula.h"
#include "paula_live.h"
#include "paula_mirror.h"
#include "paula_probe.h"

inline void *operator new(__SIZE_TYPE__, void *p) noexcept { return p; }

namespace {
alignas(amiga::Paula) unsigned char storage[sizeof(amiga::Paula)];
amiga::Paula *core = nullptr;
float master_gain = 1.0f;
float master_target = 1.0f;
uint32_t last_mixer = 0u;
unsigned render_attempt = 0u;
uint32_t speculative_irqs = 0u;
unsigned waiting_for_cache = 0u;
float output_gate = 1.0f;
float last_left = 0.0f, last_right = 0.0f;
unsigned prefetch_countdown = 0u;


uint8_t read_sample(uint32_t address)
{
    return paula_mirror_read_byte(address);
}
void virtual_irq(uint16_t)
{
    if (render_attempt) ++speculative_irqs;
    else paula_probe_add(PP_VIRTUAL_IRQS,1);
    /* The physical Paula remains the sole source of real audio IRQs. */
}
}

extern "C" void paula_live_init(void)
{
    core = new (storage) amiga::Paula(amiga::kPalColorClockHz);
    core->setReadByteCallback(read_sample);
    core->setInterruptCallback(virtual_irq);
    core->setStereoSeparation(1.0f);
    core->setInterpolation(amiga::Paula::Interp::Nearest);
    master_gain = master_target = 1.0f;
    last_mixer = 0u;
    render_attempt=speculative_irqs=waiting_for_cache=0u;
    output_gate=1.0f;
    last_left=last_right=0.0f;
    prefetch_countdown=0u;
    for (unsigned ch=0;ch<4;++ch)
        paula_probe_set(PP_CHANNEL_BASE+ch*PP_CHANNEL_STRIDE+PP_CH_PER,1);
}

extern "C" void paula_live_reset(void)
{
    if (core) core->reset();
    render_attempt=speculative_irqs=waiting_for_cache=0u;
    output_gate=1.0f;
    last_left=last_right=0.0f;
    prefetch_countdown=0u;
    paula_probe_set(PP_DMACON,0);
    paula_probe_set(PP_INTENA,0);
    paula_probe_set(PP_INTREQ,0);
    paula_probe_set(PP_ADKCON,0);
    for (unsigned ch=0;ch<4;++ch) {
        const unsigned b=PP_CHANNEL_BASE+ch*PP_CHANNEL_STRIDE;
        paula_probe_set(b+PP_CH_LOC,0);
        paula_probe_set(b+PP_CH_LEN,0);
        paula_probe_set(b+PP_CH_PER,1);
        paula_probe_set(b+PP_CH_VOL,0);
        paula_probe_set(b+PP_CH_DAT,0);
        paula_probe_set(b+PP_CH_FLAGS,0);
    }
}

static void shadow_setclr(unsigned index, uint16_t value)
{
    uint32_t bits=paula_probe_get(index);
    if (value & 0x8000u) bits |= value & 0x7fffu;
    else bits &= ~(uint32_t)(value & 0x7fffu);
    paula_probe_set(index,bits);
}

extern "C" void paula_live_write(uint32_t address, uint16_t value)
{
    if (!core) return;
    core->writeRegister16(address,value);
    paula_probe_add(PP_APPLIED,1);
    const uint32_t r=address & 0xfffu;

    if (r==0x096u) {
        shadow_setclr(PP_DMACON,value);
        const uint32_t dma=paula_probe_get(PP_DMACON);
        for (unsigned ch=0;ch<4;++ch)
            paula_probe_set(PP_CHANNEL_BASE+ch*PP_CHANNEL_STRIDE+PP_CH_FLAGS,
                            ((dma & 0x0200u) && (dma & (1u<<ch))) ? 1u : 0u);
    } else if (r==0x09au) shadow_setclr(PP_INTENA,value);
    else if (r==0x09cu) shadow_setclr(PP_INTREQ,value);
    else if (r==0x09eu) shadow_setclr(PP_ADKCON,value);
    else if (r>=0x0a0u && r<=0x0dau) {
        const unsigned ch=(r-0x0a0u)>>4;
        const unsigned reg=(r-0x0a0u)&15u;
        const unsigned b=PP_CHANNEL_BASE+ch*PP_CHANNEL_STRIDE;
        if (ch>=4u) return;
        if (reg==0u) {
            paula_probe_set(b+PP_CH_LOC,
                (paula_probe_get(b+PP_CH_LOC)&0xffffu)|((uint32_t)value<<16));
        } else if (reg==2u) {
            paula_probe_set(b+PP_CH_LOC,
                (paula_probe_get(b+PP_CH_LOC)&0xffff0000u)|value);
        } else if (reg==4u) paula_probe_set(b+PP_CH_LEN,value);
        else if (reg==6u) paula_probe_set(b+PP_CH_PER,value);
        else if (reg==8u) paula_probe_set(b+PP_CH_VOL,value&0x7fu);
        else if (reg==10u) {
            paula_probe_set(b+PP_CH_DAT,value);
            paula_probe_add(PP_AUDDAT_WRITES,1);
        } else paula_probe_add(PP_IGNORED,1);
        paula_probe_add(b+PP_CH_WRITES,1);
    } else paula_probe_add(PP_IGNORED,1);
}

extern "C" void paula_live_mixer_apply(void)
{
    if (!core) return;
    const uint32_t word=paula_probe_get(PM_REQUEST);
    if (word==last_mixer || !paula_mixer_valid(word)) return;
    const unsigned master=word&127u;
    const unsigned stereo=(word>>7)&127u;
    const unsigned linear=(word>>14)&1u;
    const unsigned muted=(word>>15)&15u;
    master_target=(float)master*0.01f;
    core->setStereoSeparation((float)stereo*0.01f);
    core->setInterpolation(linear ? amiga::Paula::Interp::Linear :
                                   amiga::Paula::Interp::Nearest);
    for(unsigned ch=0;ch<4u;++ch)
        core->setChannelMuted((int)ch,(muted&(1u<<ch))!=0u);
    last_mixer=word;
    /* Acknowledges adoption of the settings, not completion of gain ramps. */
    paula_probe_set(PM_APPLIED,word);
}

/* At most 1024 bytes per channel. This is a lookahead, not a bulk
 * sample upload: CPU0 remains the only physical Chip RAM reader. The loop
 * window is included when the remaining one-shot is shorter than 1024. */
extern "C" void paula_live_prefetch(void)
{
    if (!core) return;
    for (int ch=0;ch<4;++ch) {
        const amiga::Paula::FetchState f=core->fetchState(ch);
        if (!f.enabled) continue;
        uint32_t bytes=f.wordsLeft>512u ? 1024u : f.wordsLeft*2u;
        if (bytes)
            paula_mirror_prefetch(f.cursor,bytes);
        if (bytes<1024u && f.loopWords) {
            uint32_t remaining=1024u-bytes;
            const uint32_t loopbytes=f.loopWords>512u ? 1024u : f.loopWords*2u;
            if (remaining>loopbytes) remaining=loopbytes;
            paula_mirror_prefetch(f.loop,remaining);
        }
    }
}

extern "C" void paula_live_render(float *left, float *right, unsigned frames)
{
    if (!core) return;
    paula_live_mixer_apply();
    for (unsigned i=0;i<frames;++i) {
        /* Refresh ahead of the live cursor, including during recovery.
         * One full request sweep every 32 output frames; no busy waiting. */
        if (!prefetch_countdown) {
            paula_live_prefetch();
            prefetch_countdown=32u;
        }
        --prefetch_countdown;

        /* A frame is speculative until every requested sample byte was
         * available. On a miss, restore the whole small core state. This
         * preserves DMA pointers, loop counters, phase and gain envelopes.
         * No synthetic zero is committed to the output or source state.
         * Register writes remain outside the transaction and are never
         * rolled back. The original callbacks and mixer settings survive. */
        const amiga::Paula saved=*core;
        const float saved_gain=master_gain;
        float l=0.0f, r=0.0f;
        speculative_irqs=0u;
        paula_mirror_render_begin();
        render_attempt=1u;
        core->render(&l,&r,1,48000.0);
        render_attempt=0u;
        if (paula_mirror_render_missed()) {
            *core=saved;
            master_gain=saved_gain;
            speculative_irqs=0u;
            if (!waiting_for_cache) {
                waiting_for_cache=1u;
                prefetch_countdown=0u;
            }
            paula_probe_add(PP_CACHE_WAIT_FRAMES,1u);
            /* Fade the last valid output to silence without advancing
             * any source sample or the master-gain ramp. */
            output_gate-=1.0f/96.0f;
            if (output_gate<0.0f) output_gate=0.0f;
            left[i]=last_left*output_gate;
            right[i]=last_right*output_gate;
            continue;
        }

        if (speculative_irqs)
            paula_probe_add(PP_VIRTUAL_IRQS,speculative_irqs);
        speculative_irqs=0u;
        if (waiting_for_cache) {
            waiting_for_cache=0u;
            paula_probe_add(PP_CACHE_WAIT_RECOVERIES,1u);
        }
        float delta=master_target-master_gain;
        const float step=1.0f/96.0f;
        if(delta>step) delta=step;
        if(delta< -step) delta= -step;
        master_gain+=delta;
        l*=master_gain;
        r*=master_gain;
        last_left=l;
        last_right=r;
        if (output_gate<1.0f) {
            output_gate+=1.0f/96.0f;
            if (output_gate>1.0f) output_gate=1.0f;
        }
        left[i]=l*output_gate;
        right[i]=r*output_gate;
    }
    paula_probe_add(PP_RENDER_FRAMES,frames);
}
