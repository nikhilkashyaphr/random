/*
 * ring.h - lock-free single-producer / single-consumer slot ring.
 *
 * The ring is an array of fixed-size slots (one UDP payload per slot).
 * The producer (RX thread) reserves a contiguous run of slots, lets the
 * kernel copy payloads straight into them via recvmmsg(), then publishes
 * with a release store.  The consumer (FIFO writer) drains the largest
 * physically contiguous run per iteration with a single write(), then
 * releases the slots with a release store.
 *
 * head/tail are monotonically increasing 64-bit slot counters; the slot
 * index is (counter & mask).  With 2^64 slots of headroom, wraparound of
 * the counters themselves is not a practical concern.
 *
 * Memory ordering:
 *   producer: acquire-load head (slot reuse), release-store tail (publish)
 *   consumer: acquire-load tail (data visibility), release-store head
 *
 * Cached copies of the remote index keep the hot path free of cross-core
 * cache-line ping-pong: each side only re-reads the other's index when
 * its cached view says the ring is full/empty.
 */
#ifndef IWFG_RING_H
#define IWFG_RING_H

#include "iwfg.h"

struct ring {
    uint8_t  *buf;
    size_t    map_len;    /* actual mmap length      */
    uint64_t  slots;      /* power of two            */
    uint64_t  mask;
    uint32_t  slot_size;

    _Alignas(IWFG_CACHELINE) atomic_uint_fast64_t tail;   /* producer */
    _Alignas(IWFG_CACHELINE) atomic_uint_fast64_t head;   /* consumer */

    /* producer-private */
    _Alignas(IWFG_CACHELINE) uint64_t cached_head;
    /* consumer-private */
    _Alignas(IWFG_CACHELINE) uint64_t cached_tail;
};

int  ring_init(struct ring *r, uint32_t slots, uint32_t slot_size,
               bool hugepages);
void ring_free(struct ring *r);

/* ---------------- producer side ---------------- */

/* How many slots can the producer use right now (refreshing the cached
 * consumer index only when needed)? */
static inline uint64_t ring_free_slots(struct ring *r, uint64_t want)
{
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint64_t free_ = r->slots - (tail - r->cached_head);
    if (free_ < want) {
        r->cached_head = atomic_load_explicit(&r->head, memory_order_acquire);
        free_ = r->slots - (tail - r->cached_head);
    }
    return free_;
}

static inline uint8_t *ring_slot_ptr(struct ring *r, uint64_t seq)
{
    return r->buf + (size_t)(seq & r->mask) * r->slot_size;
}

static inline uint64_t ring_tail(struct ring *r)
{
    return atomic_load_explicit(&r->tail, memory_order_relaxed);
}

/* Publish n freshly written slots. */
static inline void ring_publish(struct ring *r, uint64_t n)
{
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    atomic_store_explicit(&r->tail, tail + n, memory_order_release);
}

/* ---------------- consumer side ---------------- */

/* Number of published, unread slots (refreshing cached tail on demand). */
static inline uint64_t ring_avail(struct ring *r)
{
    uint64_t head  = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint64_t avail = r->cached_tail - head;
    if (avail == 0) {
        r->cached_tail = atomic_load_explicit(&r->tail, memory_order_acquire);
        avail = r->cached_tail - head;
    }
    return avail;
}

/* Largest run of readable slots that is contiguous in memory. */
static inline uint64_t ring_contig(struct ring *r, uint64_t avail)
{
    uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint64_t to_end = r->slots - (head & r->mask);
    return avail < to_end ? avail : to_end;
}

static inline uint8_t *ring_read_ptr(struct ring *r)
{
    uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    return ring_slot_ptr(r, head);
}

/* Release n consumed slots back to the producer. */
static inline void ring_release(struct ring *r, uint64_t n)
{
    uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    atomic_store_explicit(&r->head, head + n, memory_order_release);
}

#endif /* IWFG_RING_H */
