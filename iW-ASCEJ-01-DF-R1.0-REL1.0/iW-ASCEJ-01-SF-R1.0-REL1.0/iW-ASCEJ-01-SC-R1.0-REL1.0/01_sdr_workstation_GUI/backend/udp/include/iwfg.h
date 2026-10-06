/*
 * iwfg.h - common definitions for iwfg_c2h (UDP payload -> FIFO streamer)
 *
 * Copyright (c) 2026. MIT License.
 */
#ifndef IWFG_H
#define IWFG_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IWFG_VERSION        "1.3.0"

#define IWFG_CACHELINE      64
#define IWFG_DEFAULT_FIFO   "/tmp/iwfg_c2h.fifo"
#define IWFG_DEFAULT_PORT   50000
#define IWFG_DEFAULT_PAYLOAD 1472            /* bytes per UDP datagram      */
#define IWFG_DEFAULT_SLOTS  (1u << 18)       /* 262144 slots ~= 368 MiB     */
#define IWFG_DEFAULT_BATCH  128              /* recvmmsg batch size         */
#define IWFG_MAX_BATCH      1024
#define IWFG_DEFAULT_RCVBUF (256u << 20)     /* 256 MiB socket buffer       */
#define IWFG_DEFAULT_STATS_SEC 1

/* ------------------------------------------------------------------ */
/* Runtime configuration (parsed from CLI, read-only after startup).  */
/* ------------------------------------------------------------------ */
struct iwfg_cfg {
    const char *fifo_path;
    const char *bind_addr;      /* NULL => INADDR_ANY                   */
    const char *raw_ifname;     /* non-NULL => AF_PACKET capture mode   */
    uint16_t    port;
    uint32_t    payload;        /* expected UDP payload size (bytes)    */
    uint32_t    slots;          /* ring slots (rounded up to pow2)      */
    uint32_t    batch;          /* recvmmsg batch                       */
    uint32_t    rcvbuf;         /* SO_RCVBUF request                    */
    uint32_t    pipe_sz;        /* F_SETPIPE_SZ request (0 = max)      */
    uint32_t    busy_poll_us;   /* SO_BUSY_POLL (0 = off)               */
    int         rx_cpu;         /* CPU affinity, -1 = unpinned          */
    int         wr_cpu;
    int         stats_sec;      /* stats interval, 0 = silent           */
    bool        spin;           /* writer busy-spins instead of backoff */
    bool        use_hugepages;  /* try MAP_HUGETLB for the ring         */
    bool        loose;          /* accept datagrams of any size <= slot */
};

/* ------------------------------------------------------------------ */
/* Shared statistics. Written by one thread each, read by stats/main. */
/* Each counter on its own cache line to avoid false sharing.         */
/* ------------------------------------------------------------------ */
struct iwfg_stats {
    _Alignas(IWFG_CACHELINE) atomic_uint_fast64_t rx_pkts;
    _Alignas(IWFG_CACHELINE) atomic_uint_fast64_t rx_bytes;
    _Alignas(IWFG_CACHELINE) atomic_uint_fast64_t ring_drops;   /* ring full   */
    _Alignas(IWFG_CACHELINE) atomic_uint_fast64_t sock_drops;   /* SO_RXQ_OVFL */
    _Alignas(IWFG_CACHELINE) atomic_uint_fast64_t bad_size;     /* != payload  */
    _Alignas(IWFG_CACHELINE) atomic_uint_fast64_t wr_pkts;
    _Alignas(IWFG_CACHELINE) atomic_uint_fast64_t wr_bytes;
    _Alignas(IWFG_CACHELINE) atomic_uint_fast64_t fifo_reopen;  /* reader loss */
};

/* Global run/stop flag, flipped by the signal handler.  atomic_int is
 * both async-signal-safe (lock-free on all supported targets, enforced
 * at startup) and thread-safe, unlike volatile sig_atomic_t which C11
 * only defines for signal<->same-thread communication. */
extern atomic_int g_stop;

extern struct iwfg_stats g_stats;

/* util.c */
void iwfg_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void iwfg_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
int  iwfg_pin_thread(int cpu, const char *name);
uint64_t iwfg_now_ns(void);

#endif /* IWFG_H */
