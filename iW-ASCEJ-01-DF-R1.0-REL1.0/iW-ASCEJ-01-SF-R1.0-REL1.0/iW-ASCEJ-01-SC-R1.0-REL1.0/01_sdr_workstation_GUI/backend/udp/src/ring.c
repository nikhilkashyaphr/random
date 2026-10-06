/* ring.c - allocation/teardown for the SPSC slot ring. */
#include "ring.h"

#include <string.h>
#include <sys/mman.h>
#include <errno.h>

static uint32_t next_pow2(uint32_t v)
{
    if (v < 2) return 2;
    v--;
    v |= v >> 1;  v |= v >> 2;  v |= v >> 4;
    v |= v >> 8;  v |= v >> 16;
    return v + 1;
}

int ring_init(struct ring *r, uint32_t slots, uint32_t slot_size,
              bool hugepages)
{
    memset(r, 0, sizeof(*r));
    slots = next_pow2(slots);

    size_t bytes = (size_t)slots * slot_size;
    /* round up to 2 MiB for hugepage mappings */
    size_t huge_bytes = (bytes + (2u << 20) - 1) & ~(size_t)((2u << 20) - 1);

    void *mem = MAP_FAILED;
    if (hugepages) {
        mem = mmap(NULL, huge_bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (mem != MAP_FAILED) {
            bytes = huge_bytes;
            iwfg_log("ring: %zu MiB backed by explicit hugepages",
                     bytes >> 20);
        } else {
            iwfg_log("ring: MAP_HUGETLB unavailable (%s), "
                     "falling back to THP", strerror(errno));
        }
    }
    if (mem == MAP_FAILED) {
        mem = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED)
            return -errno;
#ifdef MADV_HUGEPAGE
        (void)madvise(mem, bytes, MADV_HUGEPAGE);
#endif
    }

    /* Touch every page up front so no faults land on the hot path. */
    memset(mem, 0, bytes);
    (void)mlock(mem, bytes);   /* best effort; needs CAP_IPC_LOCK/rlimit */

    r->buf       = mem;
    r->map_len   = bytes;
    r->slots     = slots;
    r->mask      = slots - 1;
    r->slot_size = slot_size;
    atomic_init(&r->tail, 0);
    atomic_init(&r->head, 0);
    r->cached_head = 0;
    r->cached_tail = 0;
    return 0;
}

void ring_free(struct ring *r)
{
    if (r->buf) {
        (void)munmap(r->buf, r->map_len);
        r->buf = NULL;
    }
}
