/*
 * writer.c - FIFO writer thread.
 *
 * Drains the SPSC ring into the named pipe in the largest physically
 * contiguous chunks available (capped so slot release stays frequent),
 * so a single write() moves up to several MiB of payload.  The pipe is
 * kept in non-blocking mode and paired with poll(): a stalled or absent
 * reader can never wedge the thread, and shutdown stays responsive.
 *
 * Reader lifecycle:
 *   - no reader yet         -> open(O_NONBLOCK) returns ENXIO, retry
 *   - reader disconnects    -> write() returns EPIPE, reopen and resume
 *     from the exact byte where the stream stopped (unwritten slots are
 *     only released after they are actually in the pipe).
 */
#include "iwfg.h"
#include "ring.h"
#include "writer.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Cap a single drain region so head releases stay frequent and the
 * producer never starves for slots behind one giant write. */
#define WR_MAX_CHUNK (4u << 20)

static int fifo_ensure(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        if (!S_ISFIFO(st.st_mode))
            iwfg_fatal("%s exists and is not a FIFO", path);
        return 0;
    }
    if (errno != ENOENT)
        iwfg_fatal("stat %s: %s", path, strerror(errno));
    if (mkfifo(path, 0666) != 0)
        iwfg_fatal("mkfifo %s: %s", path, strerror(errno));
    iwfg_log("writer: created FIFO %s", path);
    return 0;
}

/* Open the FIFO write end without blocking forever: O_NONBLOCK open
 * fails with ENXIO until a reader appears, so we can poll g_stop. */
static int fifo_open(const struct iwfg_cfg *cfg)
{
    bool announced = false;
    while (!g_stop) {
        int fd = open(cfg->fifo_path, O_WRONLY | O_NONBLOCK);
        if (fd >= 0) {
            /* Enlarge the pipe buffer: bigger pipe = fewer wakeups and
             * far better burst absorption.  0 means "ask for the max". */
            unsigned req = cfg->pipe_sz;
            if (req == 0) {
                FILE *f = fopen("/proc/sys/fs/pipe-max-size", "r");
                if (f) {
                    if (fscanf(f, "%u", &req) != 1)
                        req = 1u << 20;
                    fclose(f);
                } else {
                    req = 1u << 20;
                }
            }
            int got = fcntl(fd, F_SETPIPE_SZ, (int)req);
            iwfg_log("writer: FIFO open, pipe buffer %d bytes%s", got,
                     got < (int)req ? " (raise fs.pipe-max-size for more)"
                                    : "");
            return fd;
        }
        if (errno != ENXIO && errno != EINTR)
            iwfg_fatal("open %s: %s", cfg->fifo_path, strerror(errno));
        if (!announced) {
            iwfg_log("writer: waiting for a FIFO reader on %s ...",
                     cfg->fifo_path);
            announced = true;
        }
        struct timespec ts = { 0, 50 * 1000 * 1000 };   /* 50 ms */
        nanosleep(&ts, NULL);
    }
    return -1;
}

/* Wait until the pipe accepts data or shutdown/reader-loss occurs.
 * Returns 0 = writable, -1 = stop requested, 1 = reader gone. */
static int fifo_wait_writable(int fd)
{
    while (!g_stop) {
        struct pollfd p = { .fd = fd, .events = POLLOUT };
        int rc = poll(&p, 1, 100);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            iwfg_fatal("poll: %s", strerror(errno));
        }
        if (rc == 0)
            continue;
        if (p.revents & POLLERR)
            return 1;
        if (p.revents & POLLOUT)
            return 0;
    }
    return -1;
}

void *writer_thread(void *arg)
{
    struct writer_ctx *ctx = arg;
    const struct iwfg_cfg *cfg = ctx->cfg;
    struct ring *r = ctx->ring;
    const uint32_t slot = r->slot_size;

    iwfg_pin_thread(cfg->wr_cpu, "iwfg-wr");
    fifo_ensure(cfg->fifo_path);

    int fd = fifo_open(cfg);
    if (fd < 0)
        goto out;

    unsigned idle_spins = 0;

    while (!g_stop) {
        uint64_t avail = ring_avail(r);
        if (avail == 0) {
            /* Adaptive idle: spin briefly for latency, then back off to
             * a light sleep so an idle link costs ~0% CPU. */
            if (cfg->spin || ++idle_spins < 2048) {
#if defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause();
#elif defined(__aarch64__)
                __asm__ __volatile__("yield");
#endif
                continue;
            }
            struct timespec ts = { 0, 20 * 1000 };      /* 20 us */
            nanosleep(&ts, NULL);
            continue;
        }
        idle_spins = 0;

        uint64_t run   = ring_contig(r, avail);
        uint64_t bytes = run * slot;
        if (bytes > WR_MAX_CHUNK) {
            run   = WR_MAX_CHUNK / slot;
            bytes = run * slot;
        }

        const uint8_t *p = ring_read_ptr(r);
        uint64_t done = 0;
        bool reopen = false;

        while (done < bytes && !g_stop) {
            ssize_t w = write(fd, p + done, (size_t)(bytes - done));
            if (w > 0) {
                done += (uint64_t)w;
                continue;
            }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                int st = fifo_wait_writable(fd);
                if (st == 0)
                    continue;
                if (st == 1) { reopen = true; break; }
                break;                                   /* stopping */
            }
            if (w < 0 && errno == EINTR)
                continue;
            if (w < 0 && errno == EPIPE) { reopen = true; break; }
            if (w < 0)
                iwfg_fatal("write(fifo): %s", strerror(errno));
        }

        uint64_t whole = done / slot;
        if (whole) {
            ring_release(r, whole);
            atomic_fetch_add_explicit(&g_stats.wr_pkts, whole,
                                      memory_order_relaxed);
            atomic_fetch_add_explicit(&g_stats.wr_bytes, whole * slot,
                                      memory_order_relaxed);
        }
        /* Partial slot in the pipe?  Only possible on EPIPE mid-slot;
         * the new reader must start on a packet boundary, so the
         * remainder of that slot is dropped with the reader. */
        if (reopen) {
            uint64_t part = done % slot;
            if (part)
                ring_release(r, 1);   /* discard the torn slot */
            atomic_fetch_add_explicit(&g_stats.fifo_reopen, 1,
                                      memory_order_relaxed);
            close(fd);
            iwfg_log("writer: FIFO reader disconnected, reopening");
            fd = fifo_open(cfg);
            if (fd < 0)
                break;
        }
    }

    if (fd >= 0)
        close(fd);
out:
    iwfg_log("writer: thread exiting");
    return NULL;
}
