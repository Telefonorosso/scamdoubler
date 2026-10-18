/* SPDX-License-Identifier: MIT
 * PaulaProbe v1: read-only diagnostics over Emu68's existing debug hole.
 * Do not use this address for physical hardware on a non-Emu68 machine.
 */
#ifndef PAULA_PROBE_H
#define PAULA_PROBE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PAULA_PROBE_BASE 0xDEADB000UL
#define PAULA_PROBE_MAGIC 0x5041554cUL
#define PAULA_PROBE_VERSION 1u
#define PAULA_PROBE_WORDS 128u

enum {
    PP_MAGIC=0, PP_VERSION, PP_WORDS, PP_FLAGS,
    PP_PCM_FRAMES, PP_CAPTURED, PP_DROPPED, PP_CONSUMED,
    PP_PENDING, PP_LAST_ADDR, PP_LAST_VALUE, PP_LAST_TICKS_LO,
    PP_LAST_TICKS_HI, PP_CACHE_MISSES, PP_CACHE_READY, PP_CACHE_PENDING,
    PP_CACHE_FETCHES, PP_CACHE_INVALIDATIONS, PP_CACHE_INVALID_ADDR,
    PP_APPLIED, PP_IGNORED, PP_AUDDAT_WRITES, PP_DMACON,
    PP_INTENA, PP_INTREQ, PP_ADKCON, PP_NONZERO_FRAMES,
    PP_PEAK_LEFT, PP_PEAK_RIGHT, PP_MAI_CTL, PP_MAI_ERROR,
    PP_SAMPLE_RATE, PP_COUNTER_FREQ, PP_RENDER_FRAMES,
    PP_SAMPLE_READS, PP_VIRTUAL_IRQS, PP_CACHE_REQUESTS,
    PP_CAPTURE_LOCK_DROPS, PP_CAPTURE_FULL_DROPS,
    PP_CACHE_STALE, PP_RESYNCS, PP_RESYNC_SKIPPED,
    PP_REPLAY_LAG_US, PP_REPLAY_SILENT_FRAMES, PP_SNAPSHOT_RETRIES,
    PP_CACHE_ZERO_RETURNS, PP_CACHE_MISS_READY,
    PP_CACHE_WAIT_FRAMES, PP_CACHE_WAIT_RECOVERIES, PP_CACHE_PREFETCH_PAGES
};
/* Mixer v2: atomic complete settings word.
 * bit31 valid, 0..6 master, 7..13 stereo, 14 linear, 15..18 mute mask.
 * The original diagnostic ABI remains unchanged.
 */
#define PAULA_MIXER_MAGIC 0x504d4958u
#define PAULA_MIXER_VERSION 2u
#define PM_MAGIC 96u
#define PM_VERSION 97u
#define PM_REQUEST 98u
#define PM_APPLIED 99u
#define PM_DEFAULTS 100u
#define PM_VALID 0x80000000u
#define PM_ALLOWED 0x8007ffffu
#define PM_DEFAULT (PM_VALID | 100u | (100u << 7))
static inline uint32_t paula_mixer_pack(unsigned master, unsigned stereo,
                                         unsigned linear, unsigned mute)
{
    return PM_VALID | (master & 127u) | ((stereo & 127u) << 7) |
           ((linear & 1u) << 14) | ((mute & 15u) << 15);
}
static inline int paula_mixer_valid(uint32_t word)
{
    return (word & ~PM_ALLOWED) == 0u &&
           (word & PM_VALID) != 0u &&
           (word & 127u) <= 100u &&
           ((word >> 7) & 127u) <= 100u;
}

#define PP_CHANNEL_BASE 64u
#define PP_CHANNEL_STRIDE 8u
enum {
    PP_CH_LOC=0, PP_CH_LEN, PP_CH_PER, PP_CH_VOL,
    PP_CH_DAT, PP_CH_FLAGS, PP_CH_WRITES, PP_CH_RESERVED
};
#define PP_FLAG_ARMED 1u
#define PP_FLAG_HDMI_READY 2u
#define PP_FLAG_RENDERING 4u
#define PP_FLAG_CAPTURE_FAILED 8u
#define PP_FLAG_MAI_FAILED 16u
#define PP_FLAG_CACHE_WORKER 32u

/* Internal firmware APIs: atomic 32-bit scalar metrics, no bus I/O. */
void paula_probe_init(void);
void paula_probe_set(unsigned index, uint32_t value);
void paula_probe_add(unsigned index, uint32_t value);
uint32_t paula_probe_get(unsigned index);
void paula_probe_flag(uint32_t mask, int enabled);

/* Called before the normal SYSReadValFromAddr handlers on Classic only. */
int paula_probe_bus_write(uint64_t value, int size, uint64_t address);
int paula_probe_bus_read(uint64_t *value, uint64_t *value2,
                         int size, uint64_t address);

#ifdef __cplusplus
}
#endif
#endif
