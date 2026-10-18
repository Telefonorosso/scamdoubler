/* SPDX-License-Identifier: MIT
 * Experimental CPU-write Paula mirror. The physical Amiga is never driven
 * by this module. All sample-memory bus reads are serviced on CPU0.
 */
#ifndef EMU68_PAULA_MIRROR_H
#define EMU68_PAULA_MIRROR_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PAULA_MIRROR_CHIP_LIMIT 0x200000u
#define PAULA_MIRROR_PAGE_SIZE 256u
#define PAULA_MIRROR_EVENT_CAPACITY 8192u

typedef struct {
    uint64_t ticks;
    uint32_t address;
    uint16_t value;
    uint8_t width;
    uint8_t reserved;
} paula_mirror_event;

/* Best-effort reconstruction after an incomplete event stream.
 * The producer keeps the latest observed register state even when its
 * event ring is full. Snapshot atomically discards older queued events.
 * It cannot reconstruct Copper writes or the physical DMA byte position.
 */
typedef struct {
    uint16_t reg[4][6];       /* LCH,LCL,LEN,PER,VOL,DAT */
    uint8_t seen[4];          /* bit per register */
    uint16_t dma, intena, intreq, adkcon;
    uint32_t seen_global;
    uint64_t ticks;
    uint32_t skipped;
    uint32_t dropped;
} paula_mirror_snapshot;

int paula_mirror_snapshot_take(paula_mirror_snapshot *snapshot);
uint32_t paula_mirror_pending(void);
void paula_mirror_enable(void);
void paula_mirror_capture(uint32_t address, uint32_t value, unsigned width);
void paula_mirror_invalidate(uint32_t address, unsigned width);
int paula_mirror_peek(paula_mirror_event *event);
void paula_mirror_pop(void);
uint32_t paula_mirror_dropped(void);
uint32_t paula_mirror_captured(void);

uint8_t paula_mirror_read_byte(uint32_t address);
/* CPU1-only render transaction: detect synthetic zeros without changing
 * the existing byte callback ABI or performing a physical bus read. */
void paula_mirror_render_begin(void);
int paula_mirror_render_missed(void);
void paula_mirror_prefetch(uint32_t address, uint32_t length);
uint32_t paula_mirror_misses(void);
uint32_t paula_mirror_pages_ready(void);

/* Called only from the CPU0 JIT dispatcher. Returns immediately if idle. */
int paula_mirror_service_pending(void);
void paula_mirror_service(void);

#ifdef __cplusplus
}
#endif
#endif
