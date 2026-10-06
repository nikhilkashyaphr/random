/*
 * fake_iwfg.so — an LD_PRELOAD stand-in for the iwfg DMA device, so the REAL
 * iwfg_h2c and c2h_stream binaries can be exercised without a card.
 *
 * H2C node (iwfg_h2c):
 *   open(node)   -> a dummy fd
 *   ADD_BUF      -> remember the buffer (fails above FAKE_IWFG_MAX_BUF)
 *   DMA_DATA     -> "the card consumed it": the data is appended to the
 *                   capture (what the DAC would play), and the call takes
 *                   size / FAKE_IWFG_DAC_BPS seconds plus FAKE_IWFG_OVERHEAD_US.
 *                   With FAKE_IWFG_LOOP_FILE set, the buffer is also published
 *                   there whenever its content changes: the "RF loopback" the
 *                   C2H side reads back.
 *
 * C2H node (c2h_stream):
 *   DMA_DATA     -> fills the buffer with FAKE_IWFG_C2H_CHANNELS interleaved
 *                   complex int16 channels. Channel 0 is the loop file cycled
 *                   sample by sample (the DAC output looped into the ADC);
 *                   the others are zero. Paced at FAKE_IWFG_ADC_BPS.
 *
 * Environment:
 *   FAKE_IWFG_H2C_NODE       path treated as the H2C node (default /dev/iwfg1)
 *   FAKE_IWFG_C2H_NODE       path treated as the C2H node (default /dev/iwfg0)
 *   FAKE_IWFG_CAPTURE        file receiving the DAC stream
 *   FAKE_IWFG_CAPTURE_BYTES  how much to capture (default 64 MiB)
 *   FAKE_IWFG_CAPTURE_AFTER  start capturing after this many CONTENT CHANGES
 *                            of the transferred buffer (default 0 = at once)
 *   FAKE_IWFG_DAC_BPS        DAC consumption, bytes/s (default 800e6)
 *   FAKE_IWFG_OVERHEAD_US    fixed cost per transfer (default 0)
 *   FAKE_IWFG_MAX_BUF        ADD_BUF larger than this fails with EINVAL
 *   FAKE_IWFG_LOOP_FILE      DAC -> ADC loopback file
 *   FAKE_IWFG_C2H_CHANNELS   channels in the C2H stream (default 1)
 *   FAKE_IWFG_ADC_BPS        C2H delivery, bytes/s (default 400e6)
 *   Faults:
 *   FAKE_IWFG_H2C_STALL_ABOVE  H2C transfers larger than this never complete:
 *                            the call waits 2 s and fails ETIMEDOUT, like the
 *                            fixed driver's bounded wait (EINTR on a signal)
 *   FAKE_IWFG_H2C_STALL_ALL  every H2C transfer stalls (card not consuming)
 *   FAKE_IWFG_H2C_STALL_FILE H2C transfers stall while this file exists (the
 *                            card starts consuming when it is removed)
 *   FAKE_IWFG_C2H_FAIL_AFTER after this many C2H transfers, one fails with EIO
 *   FAKE_IWFG_C2H_FAIL_ONCE  marker file: the failure happens only if it does
 *                            not exist yet (it is created), so a restarted
 *                            helper runs clean
 *   FAKE_IWFG_H2C_STALL_ONCE marker file: the first process to transfer
 *                            creates it and never completes a transfer; later
 *                            processes run clean (a stall a reopen cures)
 *   FAKE_IWFG_C2H_STALL_AFTER after this many C2H transfers the card stops
 *                            delivering: the call waits until a signal
 *   FAKE_IWFG_C2H_STALL_ONCE marker file: only one process stalls
 *   FAKE_IWFG_BLOCK_STOP     block SIGINT/SIGTERM: only SIGKILL stops it
 *   FAKE_IWFG_OPEN_LOG       append one line per open and close of a fake node
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include "iwfg_user.h"

static int      g_fd = -1;           /* H2C dummy fd */
static int      g_cfd = -1;          /* C2H dummy fd */
static void    *g_buf[16];
static uint32_t g_size[16];
static int      g_nbuf;
static FILE    *g_cap;
static uint64_t g_capLeft;
static long     g_after, g_changes;
static uint32_t g_lastSum;
static int      g_haveSum;
static double   g_bps = 800e6, g_overheadUs, g_adcBps = 400e6;
static uint64_t g_maxBuf, g_stallAbove;
static int      g_stallAll, g_channels = 1;
static long     g_failAfter = -1, g_c2hCount;
static const char *g_failOnce, *g_loopFile, *g_openLog, *g_stallFile;
static const char *g_h2cStallOnce, *g_c2hStallOnce;
static long     g_c2hStallAfter = -1;
static int      g_h2cStallThisProc = -1;   /* -1 undecided, 0 no, 1 yes */

/* C2H loop state */
static uint32_t *g_loop;              /* 32-bit I/Q words */
static size_t    g_loopWords, g_loopPos;
static ino_t     g_loopIno;
static time_t    g_loopMtime;
static long      g_loopMtimeNs;
static off_t     g_loopSize;

static const char *env(const char *k) { return getenv(k); }

static void init_env(void)
{
    const char *c = env("FAKE_IWFG_CAPTURE");
    const char *n = env("FAKE_IWFG_CAPTURE_BYTES");
    const char *a = env("FAKE_IWFG_CAPTURE_AFTER");
    const char *b = env("FAKE_IWFG_DAC_BPS");
    const char *o = env("FAKE_IWFG_OVERHEAD_US");
    const char *m = env("FAKE_IWFG_MAX_BUF");
    const char *sa = env("FAKE_IWFG_H2C_STALL_ABOVE");
    const char *ab = env("FAKE_IWFG_ADC_BPS");
    const char *ch = env("FAKE_IWFG_C2H_CHANNELS");
    const char *fa = env("FAKE_IWFG_C2H_FAIL_AFTER");
    g_capLeft    = n ? strtoull(n, NULL, 0) : (64ull << 20);
    g_after      = a ? strtol(a, NULL, 0) : 0;
    g_bps        = b ? strtod(b, NULL) : 800e6;
    g_overheadUs = o ? strtod(o, NULL) : 0.0;
    g_maxBuf     = m ? strtoull(m, NULL, 0) : 0;
    g_stallAbove = sa ? strtoull(sa, NULL, 0) : 0;
    g_stallAll   = env("FAKE_IWFG_H2C_STALL_ALL") != NULL;
    g_adcBps     = ab ? strtod(ab, NULL) : 400e6;
    g_channels   = ch ? atoi(ch) : 1;
    if (g_channels < 1) g_channels = 1;
    g_failAfter  = fa ? strtol(fa, NULL, 0) : -1;
    g_failOnce   = env("FAKE_IWFG_C2H_FAIL_ONCE");
    g_loopFile   = env("FAKE_IWFG_LOOP_FILE");
    g_openLog    = env("FAKE_IWFG_OPEN_LOG");
    g_stallFile  = env("FAKE_IWFG_H2C_STALL_FILE");
    g_h2cStallOnce = env("FAKE_IWFG_H2C_STALL_ONCE");
    g_c2hStallOnce = env("FAKE_IWFG_C2H_STALL_ONCE");
    {
        const char *sa2 = env("FAKE_IWFG_C2H_STALL_AFTER");
        g_c2hStallAfter = sa2 ? strtol(sa2, NULL, 0) : -1;
    }
    if (c && !g_cap) g_cap = fopen(c, "wb");
    if (env("FAKE_IWFG_BLOCK_STOP")) {
        sigset_t s;
        sigemptyset(&s);
        sigaddset(&s, SIGINT);
        sigaddset(&s, SIGTERM);
        sigprocmask(SIG_BLOCK, &s, NULL);
    }
}

static int real_open(const char *p, int f, mode_t m)
{
    int (*o)(const char *, int, ...) = dlsym(RTLD_NEXT, "open");
    return o(p, f, m);
}

static int is_node(const char *path, const char *var, const char *def)
{
    const char *n = getenv(var);
    return path && strcmp(path, n ? n : def) == 0;
}

static void log_event(const char *what, const char *path)
{
    if (!g_openLog) return;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    FILE *f = fopen(g_openLog, "a");
    if (f) {
        fprintf(f, "%ld.%09ld %d %s %s\n", (long)t.tv_sec, t.tv_nsec, (int)getpid(), what, path);
        fclose(f);
    }
}
static void log_open(const char *path) { log_event("open", path); }

int open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    if (is_node(path, "FAKE_IWFG_H2C_NODE", "/dev/iwfg1")) {
        init_env();
        log_open(path);
        g_fd = real_open("/dev/null", O_RDWR, 0);
        g_nbuf = 0;
        return g_fd;
    }
    if (is_node(path, "FAKE_IWFG_C2H_NODE", "/dev/iwfg0")) {
        init_env();
        log_open(path);
        g_cfd = real_open("/dev/null", O_RDWR, 0);
        g_nbuf = 0;
        return g_cfd;
    }
    return real_open(path, flags, mode);
}
int open64(const char *path, int flags, ...) __attribute__((alias("open")));

static double since(const struct timespec *t0)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - t0->tv_sec) + (double)(t.tv_nsec - t0->tv_nsec) * 1e-9;
}

static void spin_until(const struct timespec *t0, double seconds)
{
    for (;;) {
        double d = since(t0);
        if (d >= seconds) return;
        if (seconds - d > 0.002) usleep(1000);
    }
}

/* Interruptible wait, like wait_event_interruptible_timeout(): -1/EINTR if a
 * signal arrives (the helpers install handlers without SA_RESTART). */
static int stall(double seconds)
{
    struct timespec ts = { (time_t)seconds, (long)((seconds - (time_t)seconds) * 1e9) };
    if (nanosleep(&ts, NULL) < 0 && errno == EINTR) { errno = EINTR; return -1; }
    errno = ETIMEDOUT;
    return -1;
}

static void publish_loop(const void *p, uint32_t size)
{
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp%d", g_loopFile, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    fwrite(p, 1, size, f);
    fclose(f);
    rename(tmp, g_loopFile);
}

static void reload_loop(void)
{
    struct stat st;
    if (!g_loopFile || stat(g_loopFile, &st) != 0 || st.st_size < 4) return;
    if (g_loop && st.st_ino == g_loopIno && st.st_mtim.tv_sec == g_loopMtime
        && st.st_mtim.tv_nsec == g_loopMtimeNs && st.st_size == g_loopSize)
        return;
    FILE *f = fopen(g_loopFile, "rb");
    if (!f) return;
    size_t words = (size_t)st.st_size / 4;
    uint32_t *w = malloc(words * 4);
    if (!w) { fclose(f); return; }
    size_t got = fread(w, 4, words, f);
    fclose(f);
    if (got != words) { free(w); return; }
    free(g_loop);
    g_loop = w; g_loopWords = words; g_loopPos = 0;
    g_loopIno = st.st_ino; g_loopMtime = st.st_mtim.tv_sec;
    g_loopMtimeNs = st.st_mtim.tv_nsec; g_loopSize = st.st_size;
}

static int h2c_dma(struct iwfg_user_dma_req *r)
{
    if (r->buf_index >= (uint32_t)g_nbuf) { errno = EINVAL; return -1; }
    if (g_h2cStallOnce && g_h2cStallThisProc < 0) {
        g_h2cStallThisProc = access(g_h2cStallOnce, F_OK) != 0;
        if (g_h2cStallThisProc) { FILE *f = fopen(g_h2cStallOnce, "w"); if (f) fclose(f); }
    }
    if (g_stallAll || (g_stallAbove && r->size > g_stallAbove)
        || (g_stallFile && access(g_stallFile, F_OK) == 0) || g_h2cStallThisProc == 1)
        return stall(2.0);
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    const uint8_t *p = g_buf[r->buf_index];
    /* content fingerprint: detects when the card starts playing new data */
    uint32_t s = 2166136261u;
    for (uint32_t i = 0; i < r->size; i += 4093) s = (s ^ p[i]) * 16777619u;
    const int changed = !g_haveSum || s != g_lastSum;
    if (g_haveSum && s != g_lastSum) g_changes++;
    g_lastSum = s; g_haveSum = 1;
    if (changed && g_loopFile) publish_loop(p, r->size);
    if (g_cap && g_capLeft && g_changes >= g_after) {
        size_t n = r->size < g_capLeft ? r->size : (size_t)g_capLeft;
        fwrite(p, 1, n, g_cap);
        g_capLeft -= n;
        if (!g_capLeft) { fclose(g_cap); g_cap = NULL; }
    }
    spin_until(&t0, g_overheadUs * 1e-6 + (double)r->size / g_bps);
    return 0;
}

static int c2h_dma(struct iwfg_user_dma_req *r)
{
    if (r->buf_index >= (uint32_t)g_nbuf) { errno = EINVAL; return -1; }
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    ++g_c2hCount;
    if (g_failAfter >= 0 && g_c2hCount > g_failAfter) {
        int fire = 1;
        if (g_failOnce) {
            if (access(g_failOnce, F_OK) == 0) fire = 0;
            else { FILE *f = fopen(g_failOnce, "w"); if (f) fclose(f); }
        }
        if (fire) { g_failAfter = -1; errno = EIO; return -1; }
        g_failAfter = -1;
    }
    if (g_c2hStallAfter >= 0 && g_c2hCount > g_c2hStallAfter) {
        int fire = 1;
        if (g_c2hStallOnce) {
            if (access(g_c2hStallOnce, F_OK) == 0) fire = 0;
            else { FILE *f = fopen(g_c2hStallOnce, "w"); if (f) fclose(f); }
        }
        g_c2hStallAfter = -1;
        if (fire) {
            /* A wedged card: the C2H wait never completes. Like the driver's
             * wait_event_interruptible(), only a signal ends it. */
            for (;;) {
                struct timespec ts = { 3600, 0 };
                if (nanosleep(&ts, NULL) < 0 && errno == EINTR) { errno = EINTR; return -1; }
            }
        }
    }
    reload_loop();
    uint32_t *out = g_buf[r->buf_index];
    const size_t frames = r->size / (4u * (size_t)g_channels);
    for (size_t i = 0; i < frames; ++i) {
        out[i * g_channels] = g_loopWords ? g_loop[g_loopPos] : 0u;
        if (g_loopWords && ++g_loopPos == g_loopWords) g_loopPos = 0;
        for (int c = 1; c < g_channels; ++c) out[i * g_channels + c] = 0u;
    }
    spin_until(&t0, (double)r->size / g_adcBps);
    return 0;
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    const int isH2c = (fd == g_fd && g_fd >= 0);
    const int isC2h = (fd == g_cfd && g_cfd >= 0);
    if (!isH2c && !isC2h) {
        int (*r)(int, unsigned long, ...) = dlsym(RTLD_NEXT, "ioctl");
        return r(fd, req, arg);
    }
    if (req == IWFG_IOCTL_ADD_BUF) {
        struct iwfg_user_buf *ub = arg;
        if ((g_maxBuf && ub->size > g_maxBuf) || g_nbuf >= 16) { errno = EINVAL; return -1; }
        g_buf[g_nbuf] = ub->buf; g_size[g_nbuf] = ub->size;
        ub->buf_index = (uint32_t)g_nbuf++;
        return 0;
    }
    if (req == IWFG_IOCTL_DMA_DATA)
        return isH2c ? h2c_dma(arg) : c2h_dma(arg);
    errno = ENOTTY;
    return -1;
}

int close(int fd)
{
    int (*real_close)(int) = dlsym(RTLD_NEXT, "close");
    if (fd >= 0 && fd == g_fd) { log_event("close", "h2c"); g_fd = -1; }
    else if (fd >= 0 && fd == g_cfd) { log_event("close", "c2h"); g_cfd = -1; }
    return real_close(fd);
}

/* _FORTIFY_SOURCE builds may call these instead of open(). */
int __open_2(const char *path, int flags)   { return open(path, flags); }
int __open64_2(const char *path, int flags) { return open(path, flags); }
