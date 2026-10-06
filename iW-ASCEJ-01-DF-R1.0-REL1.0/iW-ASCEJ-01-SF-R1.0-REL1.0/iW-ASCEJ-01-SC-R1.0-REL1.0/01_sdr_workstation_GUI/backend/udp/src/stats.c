/*
 * stats.c - periodic statistics reporting.
 *
 * Reads the relaxed atomic counters, differentiates against the previous
 * sample and prints one line per interval.  All arithmetic uses local
 * snapshots so a torn view across counters merely skews one report by a
 * few packets - it can never corrupt state.
 */
#include "iwfg.h"
#include "ring.h"
#include "stats.h"

#include <inttypes.h>
#include <stdio.h>
#include <time.h>

struct snap {
    uint64_t rx_pkts, rx_bytes, wr_pkts, wr_bytes;
    uint64_t ring_drops, sock_drops, bad_size, reopen;
    uint64_t t_ns;
};

static void take(struct snap *s)
{
    s->rx_pkts    = atomic_load_explicit(&g_stats.rx_pkts,    memory_order_relaxed);
    s->rx_bytes   = atomic_load_explicit(&g_stats.rx_bytes,   memory_order_relaxed);
    s->wr_pkts    = atomic_load_explicit(&g_stats.wr_pkts,    memory_order_relaxed);
    s->wr_bytes   = atomic_load_explicit(&g_stats.wr_bytes,   memory_order_relaxed);
    s->ring_drops = atomic_load_explicit(&g_stats.ring_drops, memory_order_relaxed);
    s->sock_drops = atomic_load_explicit(&g_stats.sock_drops, memory_order_relaxed);
    s->bad_size   = atomic_load_explicit(&g_stats.bad_size,   memory_order_relaxed);
    s->reopen     = atomic_load_explicit(&g_stats.fifo_reopen, memory_order_relaxed);
    s->t_ns       = iwfg_now_ns();
}

void *stats_thread(void *arg)
{
    struct stats_ctx *ctx = arg;
    struct ring *r = ctx->ring;
    int interval = ctx->cfg->stats_sec;

    iwfg_pin_thread(-1, "iwfg-stats");

    struct snap prev, cur;
    take(&prev);

    while (!g_stop) {
        /* Sleep in 100 ms steps so shutdown is prompt. */
        for (int i = 0; i < interval * 10 && !g_stop; i++) {
            struct timespec ts = { 0, 100 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        if (g_stop)
            break;

        take(&cur);
        double dt = (double)(cur.t_ns - prev.t_ns) / 1e9;
        if (dt <= 0)
            dt = 1;

        double rx_pps  = (double)(cur.rx_pkts  - prev.rx_pkts)  / dt;
        double rx_gbps = (double)(cur.rx_bytes - prev.rx_bytes) * 8.0 / dt / 1e9;
        double wr_gbps = (double)(cur.wr_bytes - prev.wr_bytes) * 8.0 / dt / 1e9;

        uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
        uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
        double fill = 100.0 * (double)(tail - head) / (double)r->slots;

        iwfg_log("rx %.0f pps %.3f Gbps | fifo %.3f Gbps | ring %5.1f%% | "
                 "drops ring=%" PRIu64 " sock=%" PRIu64
                 " badsz=%" PRIu64 " reopen=%" PRIu64,
                 rx_pps, rx_gbps, wr_gbps, fill,
                 cur.ring_drops, cur.sock_drops, cur.bad_size, cur.reopen);
        prev = cur;
    }
    return NULL;
}

void stats_final(const struct ring *ring)
{
    (void)ring;
    struct snap s;
    take(&s);
    fprintf(stderr,
        "---- final counters ----\n"
        "  received : %" PRIu64 " pkts, %" PRIu64 " bytes\n"
        "  written  : %" PRIu64 " pkts, %" PRIu64 " bytes\n"
        "  drops    : ring=%" PRIu64 " socket=%" PRIu64 "\n"
        "  bad size : %" PRIu64 "\n"
        "  reopens  : %" PRIu64 "\n",
        s.rx_pkts, s.rx_bytes, s.wr_pkts, s.wr_bytes,
        s.ring_drops, s.sock_drops, s.bad_size, s.reopen);
}
