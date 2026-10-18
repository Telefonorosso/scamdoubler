/* SPDX-License-Identifier: MIT
 * Audio-frame clock for the experimental 48 kHz Paula mirror.
 * Integer remainder avoids accumulating fractional counter-tick drift.
 */
#ifndef EMU68_PAULA_REPLAY_H
#define EMU68_PAULA_REPLAY_H
#include <stdint.h>
typedef struct {
    uint64_t tick;
    uint32_t remainder;
    uint32_t frequency;
} paula_replay_clock;

static inline void paula_replay_start(paula_replay_clock *c,
                                      uint64_t now, uint32_t frequency,
                                      uint64_t latency)
{
    c->frequency = frequency;
    c->remainder = 0;
    c->tick = now > latency ? now - latency : 0;
}

static inline void paula_replay_advance(paula_replay_clock *c)
{
    c->tick += c->frequency / 48000u;
    c->remainder += c->frequency % 48000u;
    if (c->remainder >= 48000u) {
        c->remainder -= 48000u;
        ++c->tick;
    }
}

/* Signed differences are safe for intervals much shorter than 2^63 ticks. */
static inline int paula_replay_due(uint64_t now, uint64_t target)
{
    return (int64_t)(now - target) >= 0;
}

static inline int paula_replay_late(const paula_replay_clock *c,
                                    uint64_t now, uint64_t latency,
                                    uint64_t limit)
{
    return (int64_t)(now - c->tick) > (int64_t)(latency + limit);
}
#endif
