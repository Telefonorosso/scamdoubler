/* SPDX-License-Identifier: MIT
 * Bounded event ring and asynchronous 2 MiB Chip RAM sample cache.
 * CPU1 owns the request queue; CPU0 owns physical sample reads.
 * No CPU1 GPIO, bus transactions, malloc, logging or blocking waits.
 */
#include "paula_mirror.h"
#include "paula_probe.h"

#define PAGE_COUNT (PAULA_MIRROR_CHIP_LIMIT / PAULA_MIRROR_PAGE_SIZE)
#define PAGE_MASK (PAULA_MIRROR_PAGE_SIZE - 1u)
#define REQUEST_CAPACITY PAGE_COUNT

static paula_mirror_event events[PAULA_MIRROR_EVENT_CAPACITY];
static uint32_t event_head, event_tail, event_lock;
static uint32_t enabled, captured, dropped;
/* Producer-owned register shadow, updated even when the event ring is full.
 * Only the producer lock protects writes; the consumer takes it briefly to
 * obtain a consistent snapshot and discard the obsolete queue prefix. */
static paula_mirror_snapshot latest;

static void mirror_shadow_word(uint32_t address, uint16_t value)
{
    const uint32_t r = address & 0xffeu;
    uint16_t *global = 0;
    unsigned bit = 0;
    if (r == 0x096u) { global = &latest.dma; bit = 1u; }
    else if (r == 0x09au) { global = &latest.intena; bit = 2u; }
    else if (r == 0x09cu) { global = &latest.intreq; bit = 4u; }
    else if (r == 0x09eu) { global = &latest.adkcon; bit = 8u; }
    if (global) {
        if (value & 0x8000u) *global |= value & 0x7fffu;
        else *global &= (uint16_t)~(value & 0x7fffu);
        latest.seen_global |= bit;
        return;
    }
    if (r < 0x0a0u || r > 0x0dau) return;
    const unsigned ch = (r - 0x0a0u) >> 4;
    const unsigned reg = (r - 0x0a0u) & 15u;
    if (ch >= 4u || (reg & 1u) || reg > 10u) return;
    const unsigned index = reg >> 1;
    latest.reg[ch][index] = value;
    latest.seen[ch] |= (uint8_t)(1u << index);
}


static uint8_t shadow[PAULA_MIRROR_CHIP_LIMIT];
/* 0 = missing, 1 = queued, 2 = loading, 3 = ready */
/* Low two bits are state, upper bits are the invalidation generation.
 * A single atomic CAS prevents a stale DMA snapshot becoming ready. */
static uint64_t page_meta[PAGE_COUNT];
/* Diagnostic only: renderer miss since the last successful publication. */
static uint8_t page_miss_seen[PAGE_COUNT];
/* Only CPU1 accesses this flag. The producer/worker never touches it. */
static unsigned render_miss;
void paula_mirror_render_begin(void) { render_miss=0; }
int paula_mirror_render_missed(void) { return render_miss!=0; }

static uint16_t requests[REQUEST_CAPACITY];
static uint32_t request_head, request_tail;
static uint32_t misses, pages_ready, invalid_addresses;

static uint64_t mirror_ticks(void)
{
#if defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return 0;
#endif
}

static int is_audio_register(uint32_t address)
{
    const uint32_t r = address & 0xfffu;
    return (r >= 0xa0u && r <= 0xdbu) ||
           (r >= 0x96u && r <= 0x97u) ||
           (r >= 0x9au && r <= 0x9fu);
}

void paula_mirror_enable(void)
{
    /* Called once before 68k execution, after CPU1 initialization. */
    __atomic_store_n(&enabled, 0u, __ATOMIC_RELEASE);
    event_head = event_tail = event_lock = 0;
    request_head = request_tail = 0;
    captured = dropped = misses = pages_ready = invalid_addresses = 0;
    for (unsigned i = 0; i < sizeof(latest); ++i)
        ((unsigned char *)&latest)[i] = 0;
    for (unsigned i = 0; i < PAGE_COUNT; ++i) {
        page_meta[i] = 0;
        page_miss_seen[i] = 0;
    }
    __atomic_store_n(&enabled, 1u, __ATOMIC_RELEASE);
    paula_probe_flag(PP_FLAG_ARMED,1);
}

void paula_mirror_capture(uint32_t address, uint32_t value, unsigned width)
{
    address &= 0xffffffu;
    if ((address & 0xfff000u) != 0xdff000u ||
        !is_audio_register(address) ||
        !__atomic_load_n(&enabled, __ATOMIC_RELAXED))
        return;

    /* Never make a physical bus write wait for the audio renderer. */
    if (__atomic_exchange_n(&event_lock, 1u, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&dropped, 1u, __ATOMIC_RELAXED);
        paula_probe_add(PP_DROPPED,1);
        paula_probe_add(PP_CAPTURE_LOCK_DROPS,1);
        return;
    }

    /* The Classic byte bus replicates a byte onto both data lanes.
     * Preserve the original width in the event, but reconstruct the word
     * actually presented to Paula for the latest-register shadow. */
    const uint16_t word = width == 1u
        ? (uint16_t)((value & 255u) * 0x0101u) : (uint16_t)value;
    mirror_shadow_word(address, word);
    latest.ticks = mirror_ticks();

    const uint32_t w = event_head;
    const uint32_t r = __atomic_load_n(&event_tail, __ATOMIC_ACQUIRE);
    if (w - r >= PAULA_MIRROR_EVENT_CAPACITY) {
        __atomic_add_fetch(&dropped, 1u, __ATOMIC_RELAXED);
        paula_probe_add(PP_DROPPED,1);
        paula_probe_add(PP_CAPTURE_FULL_DROPS,1);
    } else {
        paula_mirror_event *e =
            &events[w & (PAULA_MIRROR_EVENT_CAPACITY - 1u)];
        e->ticks = mirror_ticks();
        e->address = address;
        e->value = (uint16_t)value;
        e->width = (uint8_t)width;
        e->reserved = 0;
        __atomic_store_n(&event_head, w + 1u, __ATOMIC_RELEASE);
        __atomic_add_fetch(&captured, 1u, __ATOMIC_RELAXED);
        paula_probe_add(PP_CAPTURED,1);
        paula_probe_set(PP_LAST_ADDR,address);
        paula_probe_set(PP_LAST_VALUE,(uint16_t)value | ((width & 255u)<<16));
        paula_probe_set(PP_LAST_TICKS_LO,(uint32_t)e->ticks);
        paula_probe_set(PP_LAST_TICKS_HI,(uint32_t)(e->ticks>>32));
        paula_probe_set(PP_PENDING,w+1u-r);
    }
    __atomic_store_n(&event_lock, 0u, __ATOMIC_RELEASE);
}

int paula_mirror_peek(paula_mirror_event *event)
{
    const uint32_t r = event_tail;
    if (r == __atomic_load_n(&event_head, __ATOMIC_ACQUIRE))
        return 0;
    *event = events[r & (PAULA_MIRROR_EVENT_CAPACITY - 1u)];
    return 1;
}

void paula_mirror_pop(void)
{
    const uint32_t r = event_tail;
    if (r != __atomic_load_n(&event_head, __ATOMIC_ACQUIRE))
    {
        __atomic_store_n(&event_tail, r + 1u, __ATOMIC_RELEASE);
        paula_probe_add(PP_CONSUMED,1);
        paula_probe_set(PP_PENDING,
            __atomic_load_n(&event_head,__ATOMIC_ACQUIRE)-(r+1u));
    }
}

uint32_t paula_mirror_dropped(void)
{
    return __atomic_load_n(&dropped, __ATOMIC_RELAXED);
}

uint32_t paula_mirror_captured(void)
{
    return __atomic_load_n(&captured, __ATOMIC_RELAXED);
}

uint32_t paula_mirror_pending(void)
{
    return __atomic_load_n(&event_head, __ATOMIC_ACQUIRE) -
           __atomic_load_n(&event_tail, __ATOMIC_ACQUIRE);
}

int paula_mirror_snapshot_take(paula_mirror_snapshot *snapshot)
{
    if (!snapshot || !__atomic_load_n(&enabled, __ATOMIC_ACQUIRE))
        return 0;
    /* A bounded try-lock, never a wait on the physical bus producer. */
    if (__atomic_exchange_n(&event_lock, 1u, __ATOMIC_ACQUIRE))
        return 0;

    const uint32_t h = __atomic_load_n(&event_head, __ATOMIC_ACQUIRE);
    const uint32_t t = __atomic_load_n(&event_tail, __ATOMIC_RELAXED);
    *snapshot = latest;
    snapshot->skipped = h - t;
    snapshot->dropped = __atomic_load_n(&dropped, __ATOMIC_RELAXED);

    /* CPU1 is the only consumer. Writes appended after releasing the lock
     * have a greater sequence number and remain available for replay. */
    __atomic_store_n(&event_tail, h, __ATOMIC_RELEASE);
    paula_probe_add(PP_CONSUMED, snapshot->skipped);
    paula_probe_set(PP_PENDING, 0u);
    __atomic_store_n(&event_lock, 0u, __ATOMIC_RELEASE);
    return 1;
}

static void request_page(unsigned page)
{
    if (page >= PAGE_COUNT) return;
    uint64_t old = __atomic_load_n(&page_meta[page], __ATOMIC_ACQUIRE);
    for (;;) {
        if ((old & 3u) != 0u) return;
        if (__atomic_compare_exchange_n(&page_meta[page], &old, old | 1u,
                                        0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            break;
    }

    /* At most one outstanding request per page. The worker removes its
     * request before changing the page back to missing/ready. */
    const uint32_t w = request_head;
    requests[w & (REQUEST_CAPACITY - 1u)] = (uint16_t)page;
    __atomic_store_n(&request_head, w + 1u, __ATOMIC_RELEASE);
    paula_probe_add(PP_CACHE_REQUESTS,1);
    paula_probe_set(PP_CACHE_PENDING,w+1u-
        __atomic_load_n(&request_tail,__ATOMIC_ACQUIRE));
}

uint8_t paula_mirror_read_byte(uint32_t address)
{
    paula_probe_add(PP_SAMPLE_READS,1);
    if (address >= PAULA_MIRROR_CHIP_LIMIT) {
        ++invalid_addresses;
        paula_probe_add(PP_CACHE_INVALID_ADDR,1);
        return 0;
    }
    const unsigned page = address / PAULA_MIRROR_PAGE_SIZE;
    if ((__atomic_load_n(&page_meta[page], __ATOMIC_ACQUIRE) & 3u) != 3u) {
        render_miss=1;
        __atomic_store_n(&page_miss_seen[page],1u,__ATOMIC_RELAXED);
        request_page(page);
        ++misses;
        paula_probe_add(PP_CACHE_MISSES,1);
        paula_probe_add(PP_CACHE_ZERO_RETURNS,1);
        return 0;
    }
    /* Warm the next page just before the current page ends. This keeps
     * sample streaming lazy instead of prefetching an entire DMA buffer. */
    if ((address & PAGE_MASK) >= PAGE_MASK - 63u && page + 1u < PAGE_COUNT)
        request_page(page + 1u);
    return __atomic_load_n(&shadow[address], __ATOMIC_RELAXED);
}

void paula_mirror_prefetch(uint32_t address, uint32_t length)
{
    if (address >= PAULA_MIRROR_CHIP_LIMIT || length == 0)
        return;
    if (length > PAULA_MIRROR_CHIP_LIMIT - address)
        length = PAULA_MIRROR_CHIP_LIMIT - address;
    const unsigned first = address / PAULA_MIRROR_PAGE_SIZE;
    const unsigned last = (address + length - 1u) / PAULA_MIRROR_PAGE_SIZE;
    for (unsigned page = first; page <= last; ++page) {
        if ((__atomic_load_n(&page_meta[page],__ATOMIC_ACQUIRE)&3u)==0u)
            paula_probe_add(PP_CACHE_PREFETCH_PAGES,1);
        request_page(page);
    }
}

void paula_mirror_invalidate(uint32_t address, unsigned width)
{
    address &= 0xffffffu;
    if (!__atomic_load_n(&enabled, __ATOMIC_RELAXED) ||
        address >= PAULA_MIRROR_CHIP_LIMIT || !width)
        return;
    unsigned first = address / PAULA_MIRROR_PAGE_SIZE;
    unsigned last = (address + width - 1u) / PAULA_MIRROR_PAGE_SIZE;
    if (last >= PAGE_COUNT) last = PAGE_COUNT - 1u;
    for (unsigned page = first; page <= last; ++page) {
        paula_probe_add(PP_CACHE_INVALIDATIONS,1);
        uint64_t old = __atomic_load_n(&page_meta[page], __ATOMIC_ACQUIRE);
        for (;;) {
            if ((old & 3u) == 0u) break;
            const uint64_t next = ((old + 4u) & ~3ULL) |
                                  ((old & 3u) == 3u ? 0u : (old & 3u));
            if (__atomic_compare_exchange_n(&page_meta[page], &old, next,
                                            0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                if ((old & 3u) == 3u) {
                    __atomic_sub_fetch(&pages_ready, 1u, __ATOMIC_RELAXED);
                    paula_probe_set(PP_CACHE_READY,
                        __atomic_load_n(&pages_ready,__ATOMIC_RELAXED));
                }
                break;
            }
        }
    }
}

uint32_t paula_mirror_misses(void) { return misses; }
uint32_t paula_mirror_pages_ready(void)
{
    return __atomic_load_n(&pages_ready, __ATOMIC_RELAXED);
}

int paula_mirror_service_pending(void)
{
    return __atomic_load_n(&enabled, __ATOMIC_ACQUIRE) &&
           request_tail != __atomic_load_n(&request_head, __ATOMIC_ACQUIRE);
}

#ifdef PAULA_MIRROR_HOST_TEST
extern uint8_t paula_mirror_host_read(uint32_t address);
#define PHYSICAL_READ(a) paula_mirror_host_read(a)
#else
#include "../../pistorm/ps_protocol.h"
#if PISTORM_WRITE_BUFFER
#error "POC7 requires the unbuffered Classic bus; CPU1 is reserved for audio"
#endif
#define PHYSICAL_READ(a) ps_read_8_int(a)
#endif

void paula_mirror_service(void)
{
    if (!paula_mirror_service_pending()) return;

    const unsigned page = requests[request_tail & (REQUEST_CAPACITY - 1u)];
    uint64_t meta = __atomic_load_n(&page_meta[page], __ATOMIC_ACQUIRE);
    for (;;) {
        if ((meta & 3u) != 1u) {
            __atomic_store_n(&request_tail, request_tail + 1u, __ATOMIC_RELEASE);
            paula_probe_set(PP_CACHE_PENDING,
                __atomic_load_n(&request_head,__ATOMIC_ACQUIRE)-request_tail);
            return;
        }
        const uint64_t loading = (meta & ~3ULL) | 2u;
        if (__atomic_compare_exchange_n(&page_meta[page], &meta, loading,
                                        0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            break;
    }
    const uint64_t expected = (meta & ~3ULL) | 2u;
    paula_probe_flag(PP_FLAG_CACHE_WORKER,1);
    const uint32_t address = page * PAULA_MIRROR_PAGE_SIZE;

    /* Only CPU0 invokes this function, at an ordinary JIT dispatch point.
     * The unbuffered Classic bus is already CPU0-owned. No other core
     * performs sample reads, and no extra GPIO ownership is introduced. */
    for (unsigned i = 0; i < PAULA_MIRROR_PAGE_SIZE; ++i) {
        const uint8_t value = (uint8_t)PHYSICAL_READ(address + i);
        __atomic_store_n(&shadow[address + i], value, __ATOMIC_RELAXED);
    }

    /* Release the request slot before the page may be queued again. */
    __atomic_store_n(&request_tail, request_tail + 1u, __ATOMIC_RELEASE);
    paula_probe_set(PP_CACHE_PENDING,
        __atomic_load_n(&request_head,__ATOMIC_ACQUIRE)-request_tail);
    paula_probe_add(PP_CACHE_FETCHES,1);
    paula_probe_flag(PP_FLAG_CACHE_WORKER,0);

    /* Reserve the count before publication, so an immediate invalidation
     * cannot transiently underflow pages_ready. */
    __atomic_add_fetch(&pages_ready, 1u, __ATOMIC_RELAXED);
    uint64_t compare = expected;
    if (__atomic_compare_exchange_n(&page_meta[page], &compare, expected | 1u,
                                    0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        paula_probe_set(PP_CACHE_READY,
            __atomic_load_n(&pages_ready,__ATOMIC_RELAXED));
        if (__atomic_exchange_n(&page_miss_seen[page],0u,__ATOMIC_RELAXED))
            paula_probe_add(PP_CACHE_MISS_READY,1);
        return;
    }

    __atomic_sub_fetch(&pages_ready, 1u, __ATOMIC_RELAXED);
    paula_probe_set(PP_CACHE_READY,
        __atomic_load_n(&pages_ready,__ATOMIC_RELAXED));
    paula_probe_add(PP_CACHE_STALE,1);
    /* A CPU write changed the generation. Discard the stale snapshot. */
    for (;;) {
        if ((compare & 3u) != 2u) return;
        const uint64_t missing = compare & ~3ULL;
        if (__atomic_compare_exchange_n(&page_meta[page], &compare, missing,
                                        0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return;
    }
}
