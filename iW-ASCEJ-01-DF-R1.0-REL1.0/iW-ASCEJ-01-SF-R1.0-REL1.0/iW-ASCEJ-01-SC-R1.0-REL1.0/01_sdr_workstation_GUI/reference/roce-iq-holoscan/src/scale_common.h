/*
 * scale_common.h -- shared bits for scale_tx / scale_rx (N-link validation).
 *
 * Payload integrity: every 8-byte lane of every frame is a pure function of
 * (link, seq, lane), so the receiver can verify any byte of any frame
 * without the sender shipping checksums. flags field carries the link id.
 */
#ifndef SCALE_COMMON_H
#define SCALE_COMMON_H

#include <stdint.h>
#include <string.h>
#include "rdma_common.h"

#define SCALE_MAX_LINKS 128u

/* splitmix64: tiny, fast, good-enough PRF for pattern generation */
static inline uint64_t sc_mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static inline uint64_t sc_lane(uint32_t link, uint64_t seq, uint64_t lane)
{
    return sc_mix(((uint64_t)link << 48) ^ (seq << 16) ^ lane);
}

static inline void sc_fill(uint8_t *payload, uint32_t bytes,
                           uint32_t link, uint64_t seq)
{
    uint64_t *p = (uint64_t *)payload;
    uint64_t lanes = bytes / 8;
    for (uint64_t i = 0; i < lanes; i++)
        p[i] = sc_lane(link, seq, i);
}

/* Verify: full=1 checks every lane; full=0 samples head, tail and every
 * 512th lane (4 KiB granularity) -- catches truncation, misplacement and
 * slot mixups at negligible CPU cost. Returns number of bad lanes found. */
static inline uint64_t sc_verify(const uint8_t *payload, uint32_t bytes,
                                 uint32_t link, uint64_t seq, int full)
{
    const uint64_t *p = (const uint64_t *)payload;
    uint64_t lanes = bytes / 8, bad = 0;
    if (full) {
        for (uint64_t i = 0; i < lanes; i++)
            bad += (p[i] != sc_lane(link, seq, i));
    } else {
        for (uint64_t i = 0; i < 8 && i < lanes; i++)
            bad += (p[i] != sc_lane(link, seq, i));
        for (uint64_t i = (lanes > 8 ? lanes - 8 : 0); i < lanes; i++)
            bad += (p[i] != sc_lane(link, seq, i));
        for (uint64_t i = 512; i + 8 < lanes; i += 512)
            bad += (p[i] != sc_lane(link, seq, i));
    }
    return bad;
}

/* ---- one-way latency histogram (microseconds, fixed log-ish buckets) ---- */
#define LAT_NBUCKETS 13
static const double lat_edges_us[LAT_NBUCKETS] = {
    2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 1e18
};

struct lat_hist {
    uint64_t bucket[LAT_NBUCKETS];
    uint64_t count;
    double   sum_us, min_us, max_us;
};

static inline void lat_init(struct lat_hist *h)
{
    memset(h, 0, sizeof(*h));
    h->min_us = 1e18;
}

static inline void lat_add(struct lat_hist *h, double us)
{
    if (us < 0) us = 0;
    int b = 0;
    while (b < LAT_NBUCKETS - 1 && us >= lat_edges_us[b]) b++;
    h->bucket[b]++;
    h->count++;
    h->sum_us += us;
    if (us < h->min_us) h->min_us = us;
    if (us > h->max_us) h->max_us = us;
}

/* Upper-bound percentile from the histogram (conservative). */
static inline double lat_pct(const struct lat_hist *h, double pct)
{
    if (!h->count) return 0;
    uint64_t target = (uint64_t)(pct / 100.0 * (double)h->count), acc = 0;
    for (int b = 0; b < LAT_NBUCKETS; b++) {
        acc += h->bucket[b];
        if (acc >= target)
            return (b == LAT_NBUCKETS - 1) ? h->max_us : lat_edges_us[b];
    }
    return h->max_us;
}

static inline void lat_merge(struct lat_hist *dst, const struct lat_hist *s)
{
    for (int b = 0; b < LAT_NBUCKETS; b++) dst->bucket[b] += s->bucket[b];
    dst->count  += s->count;
    dst->sum_us += s->sum_us;
    if (s->count && s->min_us < dst->min_us) dst->min_us = s->min_us;
    if (s->max_us > dst->max_us) dst->max_us = s->max_us;
}

#endif /* SCALE_COMMON_H */
