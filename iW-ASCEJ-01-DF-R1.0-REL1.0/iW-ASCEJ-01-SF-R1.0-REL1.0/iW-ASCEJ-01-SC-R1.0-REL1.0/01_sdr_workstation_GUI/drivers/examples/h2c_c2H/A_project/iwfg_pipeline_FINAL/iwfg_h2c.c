/*
 * iwfg_h2c.c  v9  -  H2C: GNU Radio FIFO -> card  (zero-gap looping)
 *
 * Problem: GNU Radio fifo_sink produces ~30 MB/s but the card consumes
 * ~700 MB/s continuous data. Any gap between chunks = burst/silence on
 * the DAC. Solution unchanged since v6: replay the most recent GNU
 * Radio chunk at full DMA speed until a new one arrives.
 *
 * v9 = v8 + ACTIVE, UNBOUNDED SELF-HEALING (this app is a service; it
 * must survive card stalls, FIFO lifecycle races and driver recovery
 * cycles without operator intervention):
 *
 *  A. STALL KICK: the DMA ioctl blocks in-kernel; on an OLD driver
 *     build the wait is unbounded, so a card-side stall (PL H2C FIFO
 *     full / tready low) wedged the app forever and even Ctrl+C could
 *     not reach the main thread (process-directed signals may land on
 *     another thread). The watchdog now sends SIGUSR1 (no-op handler
 *     installed WITHOUT SA_RESTART) to the MAIN thread after 3 s
 *     without a completion, forcing the ioctl out with EINTR. The main
 *     loop distinguishes shutdown (g_stop) from a kick and retries.
 *     Re-posting the same registered buffer after a kicked/timed-out
 *     transfer is safe: the driver re-posts the prebuilt descriptor
 *     list; leftover in-flight descriptors of the SAME chunk drain as
 *     duplicates of data we are replaying anyway, and ring accounting
 *     reconciles in iwfg_tx_clean/iwfg_tx_reclaim.
 *
 *  B. DEVICE REOPEN CYCLE: after KICKS_PER_REOPEN fruitless kicks (or
 *     EBUSY/ETIMEDOUT exhaustion) the app performs the driver's
 *     documented full-reset path IN-APP: close(dev) [queue torn down,
 *     buffers unpinned] -> open(dev) [fresh TX queue, h2c_busy=0, PL
 *     H2C datapath re-enabled by the fixed driver] -> re-ADD_BUF both
 *     ping-pong buffers (fresh indices). Recovery cycles are UNBOUNDED
 *     with rate-limited logging; the app only exits if the reopen
 *     itself fails REOPEN_MAX_FAILS consecutive times (device gone).
 *     Any successful DMA resets all failure counters -> the stream
 *     resumes automatically the moment the RF chain comes back.
 *
 *  C. IMMORTAL READER + FIFO SELF-HEAL: the v7/v8 reader thread DIED
 *     SILENTLY on any non-EINTR open error (e.g. the FIFO vanished:
 *     tmp-cleaners, operator rm, second instance) while the DMA loop
 *     kept replaying the last chunk forever - fresh GR data silently
 *     stopped flowing. The reader now never exits before g_stop: it
 *     validates the fd is a FIFO, recreates the path on ENOENT, and
 *     retries with a 200 ms backoff.
 *
 *  D. FIFO INODE PRESERVATION: v7/v8 unlink()ed the FIFO at startup
 *     AND shutdown. A peer blocked in a plain open(O_WRONLY) waits on
 *     the INODE it resolved, so unlink+mkfifo deadlocked any writer
 *     that connected first (empirically reproduced). v9 never unlinks
 *     a live FIFO: startup reuses an existing FIFO inode (error only
 *     if the path is squatted by a non-FIFO), shutdown leaves it in
 *     place for the next run / a still-blocked peer. (fifo_sink v4
 *     additionally opens non-blocking with path re-resolution, making
 *     the pair robust even against third parties swapping the inode.)
 *
 *  E. Ctrl+C ALWAYS WORKS: on g_stop the watchdog kicks the main
 *     thread once more before exiting, so shutdown is guaranteed even
 *     while wedged inside the ioctl on an old driver.
 *
 * DRIVER CONTRACT (fixed driver; see iwfg_main.c/iwfg_dma.c):
 *   - open("/dev/iwfg1") runs iwfg_init_tx_queue(), resets h2c_busy
 *     AND enables the PL H2C datapath (iwfg_qdma_h2c_start - the
 *     register write lost when STREAM_START was removed; its absence
 *     was the root cause of "stalls after the first chunk").
 *   - DMA_DATA blocks; the in-kernel wait is bounded (2 s) with
 *     writeback reconciliation, so ETIMEDOUT now also means "card not
 *     consuming" (dmesg has the pidx/cidx dump). EBUSY = C2H overflow
 *     recovery in progress (rebuilds the TX queue; retry is safe).
 *   - ADD_BUF: buf_type=USERPTR, import_fd=-1; DMA_DATA with buf=NULL
 *     + returned index. Dedup by (addr,size). offset/flags ignored.
 *   - Teardown: close(FIFO) -> close(dev) [unpins] -> free(buffers).
 *
 * DATA OWNERSHIP (race-free by construction, unchanged):
 *   - reader thread owns the staging ping-pong (stage_fill/stage_ready),
 *     swapped under the mutex only after a complete chunk is read;
 *   - main thread owns both registered DMA buffers: while the blocking
 *     ioctl is in flight on dma_buf[cur], nothing touches dma_buf[cur];
 *     between ioctls, if new data is pending, main copies stage_ready
 *     into the IDLE buffer dma_buf[1-cur] and flips cur.
 *
 * Build:  gcc -O2 -Wall -Wextra -pthread -o iwfg_h2c iwfg_h2c.c
 * Usage:  ./iwfg_h2c [/dev/iwfg1] [/tmp/iwfg_h2c.fifo] [chunk_bytes]
 *         chunk_bytes default 16384; multiple of 4, max 64 MiB.
 */

#define _GNU_SOURCE              /* pthread_tryjoin_np() */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include "iwfg_user.h"   /* MUST be the current header (with buf_type/import_fd) */

#define DEF_DEVICE   "/dev/iwfg1"        /* H2C node (qid 1) */
#define DEF_FIFO     "/tmp/iwfg_h2c.fifo"
#define DEF_CHUNK    (16 * 1024)
#define MAX_CHUNK    (64u * 1024 * 1024)
#define NUM_DMA_BUFS 2                   /* ping-pong, registered with driver */

#define EBUSY_RETRY_US      2000         /* h2c reset in progress: back off   */
#define EBUSY_MAX_RETRIES   500          /* ~1 s of reset-wait, then reopen   */
#define ETIMEDOUT_MAX       15           /* each costs ~2 s in-kernel; ~30 s
                                            of stalled card, then reopen      */
#define STALL_WARN_S        3            /* watchdog: first loud warning      */
#define STALL_KICK_S        3            /* watchdog: start SIGUSR1 kicks     */
#define KICKS_PER_REOPEN    5            /* fruitless kicks before reopen     */
#define REOPEN_MAX_FAILS    5            /* consecutive reopen failures = exit*/
#define DIAG_EVERY_CYCLES   8            /* full diagnosis rate limit         */

static volatile sig_atomic_t g_stop = 0;
static volatile uint64_t g_dma_done = 0;   /* completed DMA_DATA ioctls       */
static volatile int      g_in_dma  = 0;    /* main is inside the DMA ioctl    */
static pthread_t         g_main_tid;

static void sig_handler(int s)  { (void)s; g_stop = 1; }
static void kick_handler(int s) { (void)s; /* purpose: EINTR the ioctl */ }

/*
 * No SA_RESTART anywhere: the blocked DMA_DATA ioctl, the blocking FIFO
 * open() and read() must all return -1/EINTR on SIGINT/SIGTERM/SIGUSR1
 * instead of being silently restarted by libc.
 */
static int install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                     /* deliberately NO SA_RESTART */
    if (sigaction(SIGINT,  &sa, NULL) < 0) return -1;
    if (sigaction(SIGTERM, &sa, NULL) < 0) return -1;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = kick_handler;        /* must be a handler, not SIG_IGN */
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa, NULL) < 0) return -1;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, NULL) < 0) return -1;

    return 0;
}

/* interruptible millisecond sleep that honors g_stop */
static void msleep_stoppable(unsigned int ms)
{
    struct timespec ts = { .tv_sec  = ms / 1000,
                           .tv_nsec = (long)(ms % 1000) * 1000000L };
    while (!g_stop && nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

/* ---------------------------------------------------------------------- */
/* Shared state between FIFO reader thread and DMA main loop              */

static size_t   g_chunk = DEF_CHUNK;
static uint8_t *stage_fill  = NULL;      /* reader writes here            */
static uint8_t *stage_ready = NULL;      /* latest complete chunk         */
static int      g_new_data  = 0;         /* stage_ready has unseen data   */
static uint64_t g_updates   = 0;         /* chunks received from GR       */
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

static const char *g_fifo_path = DEF_FIFO;

/* ---------------------------------------------------------------------- */
/* FIFO lifecycle: create-if-missing, NEVER unlink a live path (v9 fix D) */

static int ensure_fifo(const char *path)
{
    struct stat st;

    if (stat(path, &st) == 0) {
        if (S_ISFIFO(st.st_mode))
            return 0;                    /* reuse existing inode */
        fprintf(stderr, "[H2C] %s exists and is NOT a FIFO - refusing "
                        "to touch it\n", path);
        return -1;
    }
    if (errno != ENOENT) {
        perror("[H2C] stat fifo");
        return -1;
    }
    if (mkfifo(path, 0666) < 0 && errno != EEXIST) {
        perror("[H2C] mkfifo");
        return -1;
    }
    return 0;
}

/*
 * Open the FIFO for reading; retries forever (until g_stop) across
 * transient errors and self-heals a vanished/squatted path (v9 fix C).
 * Returns fd >= 0, or -1 only when g_stop is set.
 */
static int open_fifo_rd(void)
{
    int warned = 0;

    while (!g_stop) {
        int fd = open(g_fifo_path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            struct stat st;
            if (fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode))
                return fd;
            close(fd);
            fprintf(stderr, "[H2C] %s is no longer a FIFO - recreating\n",
                    g_fifo_path);
            unlink(g_fifo_path);         /* squatted path only, never a FIFO */
        } else {
            if (errno == EINTR)
                continue;                /* re-check g_stop */
            if (!warned) {
                fprintf(stderr, "[H2C] open(%s): %s - retrying\n",
                        g_fifo_path, strerror(errno));
                warned = 1;
            }
        }
        if (ensure_fifo(g_fifo_path) == 0)
            warned = 0;
        msleep_stoppable(200);
    }
    return -1;
}

/* read exactly len bytes; -1 on EOF/error, 0 on success, 1 on stop */
static int read_all(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) {
                if (g_stop) return 1;
                continue;
            }
            return -1;
        }
        if (n == 0) return -1;           /* writer closed (GR stopped)    */
        p += n; len -= (size_t)n;
    }
    return 0;
}

/*
 * Reader thread: read full chunks into stage_fill, publish by swapping
 * stage_fill/stage_ready under the mutex. On EOF or ANY error it
 * reopens the FIFO (self-healing, see open_fifo_rd) while the DMA loop
 * keeps replaying the last chunk - zero-gap across a GNU Radio restart,
 * a vanished FIFO, or any other transient. Exits ONLY on g_stop.
 */
static void *fifo_reader(void *arg)
{
    /* main hands over the fd it used for the first chunk, so the sink
     * sees ONE continuous connection (no disconnect/reconnect blip). */
    int fd = (int)(intptr_t)arg;

    while (!g_stop) {
        if (fd < 0) {
            fd = open_fifo_rd();
            if (fd < 0)
                break;                   /* g_stop */
            printf("[H2C reader] GNU Radio reconnected.\n");
        }

        while (!g_stop) {
            int r = read_all(fd, stage_fill, g_chunk);
            if (r != 0) {
                if (r < 0 && !g_stop)
                    printf("[H2C reader] FIFO EOF/error - waiting for GNU Radio "
                           "to reconnect (replaying last chunk)...\n");
                break;
            }

            pthread_mutex_lock(&g_mtx);
            uint8_t *tmp = stage_ready;
            stage_ready  = stage_fill;
            stage_fill   = tmp;
            g_new_data   = 1;
            g_updates++;
            pthread_mutex_unlock(&g_mtx);
        }

        close(fd);
        fd = -1;
    }
    if (fd >= 0)
        close(fd);
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Stall watchdog (v9 fix A/E): warn, then actively KICK the wedged ioctl */

static void print_stall_diag(void)
{
    fprintf(stderr,
        "[H2C] WARNING: no DMA completion for %d s - the card is NOT\n"
        "[H2C]   consuming H2C data (FPGA H2C FIFO full / tready low).\n"
        "[H2C]   1) Rebuild+reload the iwfg driver: it must enable the\n"
        "[H2C]      H2C datapath at open (iwfg_qdma_h2c_start) and bound\n"
        "[H2C]      the completion wait; an old driver hangs here forever.\n"
        "[H2C]   2) Check PS-side RF configuration: clocks+MTS locked,\n"
        "[H2C]      DAC enabled, AXIS switch routed QDMA->DAC.\n"
        "[H2C]   3) dmesg has the driver's pidx/cidx ring dump.\n"
        "[H2C]   Recovery is automatic: kicking the transfer, then cycling\n"
        "[H2C]   the device if the stall persists. The stream resumes the\n"
        "[H2C]   moment the card starts consuming again.\n",
        STALL_WARN_S);
}

static void *stall_watchdog(void *arg)
{
    (void)arg;
    uint64_t last = g_dma_done;
    int quiet_secs = 0;

    while (!g_stop) {
        msleep_stoppable(1000);
        if (g_stop)
            break;

        uint64_t now = g_dma_done;
        if (now == last && g_in_dma) {
            quiet_secs++;
            if (quiet_secs == STALL_WARN_S)
                print_stall_diag();
            if (quiet_secs >= STALL_KICK_S)
                pthread_kill(g_main_tid, SIGUSR1);   /* EINTR the ioctl */
        } else {
            if (quiet_secs >= STALL_WARN_S)
                fprintf(stderr, "[H2C] Card resumed consuming data.\n");
            quiet_secs = 0;
        }
        last = now;
    }

    /* Guarantee shutdown even if main is wedged in the ioctl (fix E). */
    pthread_kill(g_main_tid, SIGUSR1);
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Device reopen cycle (v9 fix B): the driver's documented full reset     */

static int register_dma_bufs(int fd, uint8_t **bufs, uint32_t *idx)
{
    for (int i = 0; i < NUM_DMA_BUFS; i++) {
        struct iwfg_user_buf ub = {
            .buf        = bufs[i],
            .size       = (uint32_t)g_chunk,
            .buf_type   = IWFG_BUF_TYPE_USERPTR,
            .import_fd  = -1,
            .is_chunked = false,
            .chunk_size = 0,
            .dmabuf_fd  = -1,
            .buf_index  = 0,
        };
        if (ioctl(fd, IWFG_IOCTL_ADD_BUF, &ub) < 0) {
            perror("[H2C] IWFG_IOCTL_ADD_BUF");
            return -1;
        }
        idx[i] = ub.buf_index;
    }
    return 0;
}

/*
 * Close and reopen the device, re-registering the (still-allocated)
 * ping-pong buffers. close() makes the driver tear down the TX queue
 * and unpin the pages; open() builds a fresh queue with h2c_busy = 0
 * and (fixed driver) re-enables the PL H2C datapath; ADD_BUF re-pins
 * the same host memory and returns fresh indices. The reader thread
 * never touches dev_fd, so this is safe while it keeps running.
 *
 * Returns 0 on success; -1 after REOPEN_MAX_FAILS consecutive failures
 * or on g_stop (the device itself is gone/unusable - only exit path).
 */
static int reopen_device(const char *dev, int *fdp,
                         uint8_t **bufs, uint32_t *idx)
{
    int fails = 0;

    if (*fdp >= 0) {
        close(*fdp);
        *fdp = -1;
    }

    while (!g_stop) {
        int fd = open(dev, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (++fails > REOPEN_MAX_FAILS) {
                fprintf(stderr, "[H2C] device reopen failed %d times "
                        "(%s) - giving up\n", fails, strerror(errno));
                return -1;
            }
            fprintf(stderr, "[H2C] reopen open(%s): %s - retry %d/%d\n",
                    dev, strerror(errno), fails, REOPEN_MAX_FAILS);
            msleep_stoppable(500);
            continue;
        }

        if (register_dma_bufs(fd, bufs, idx) < 0) {
            close(fd);
            if (++fails > REOPEN_MAX_FAILS) {
                fprintf(stderr, "[H2C] buffer re-registration failed %d "
                        "times - giving up\n", fails);
                return -1;
            }
            msleep_stoppable(500);
            continue;
        }

        *fdp = fd;
        printf("[H2C] Device reopened: TX queue + PL datapath reinitialized, "
               "buffers re-registered.\n");
        return 0;
    }
    return -1;
}

/* ---------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *dev = (argc > 1) ? argv[1] : DEF_DEVICE;
    g_fifo_path     = (argc > 2) ? argv[2] : DEF_FIFO;
    if (argc > 3) {
        long v = strtol(argv[3], NULL, 0);
        if (v < 4 || (v % 4) != 0 || (unsigned long)v > MAX_CHUNK) {
            fprintf(stderr, "chunk_bytes must be a multiple of 4 in "
                            "[4, %u]\n", MAX_CHUNK);
            return 1;
        }
        g_chunk = (size_t)v;
    }

    uint8_t *dma_bufs[NUM_DMA_BUFS] = { NULL };
    uint32_t dma_idx[NUM_DMA_BUFS];
    int dev_fd = -1;
    int exit_code = 1;
    int reader_started = 0;
    int wd_started = 0;
    uint64_t total_bytes = 0;
    pthread_t reader_tid, wd_tid;
    int i;

    setvbuf(stdout, NULL, _IOLBF, 0);    /* sane logs when piped */
    g_main_tid = pthread_self();

    if (install_signal_handlers() < 0) { perror("sigaction"); return 1; }

    /* v9 fix D: reuse an existing FIFO inode; never unlink a live FIFO. */
    if (ensure_fifo(g_fifo_path) < 0)
        return 1;

    printf("[H2C v9] device=%s  fifo=%s  chunk=%zu bytes\n",
           dev, g_fifo_path, g_chunk);
    printf("[H2C v9] Mode: GNU Radio -> FIFO -> card (zero-gap looping, "
           "blocking DMA, self-healing)\n\n");

    dev_fd = open(dev, O_RDWR | O_CLOEXEC);
    if (dev_fd < 0) { perror("open device"); goto cleanup; }

    /* Staging ping-pong (never registered with the driver) */
    if (posix_memalign((void **)&stage_fill,  4096, g_chunk) != 0 ||
        posix_memalign((void **)&stage_ready, 4096, g_chunk) != 0) {
        perror("posix_memalign (staging)");
        goto cleanup;
    }
    memset(stage_fill,  0, g_chunk);
    memset(stage_ready, 0, g_chunk);

    /*
     * Allocate + register the two DMA ping-pong buffers. The driver
     * pins the pages, DMA-maps them and prebuilds the H2C descriptor
     * list at ADD_BUF time; DMA_DATA then only re-posts those
     * descriptors (dedup by (addr,size)).
     */
    for (i = 0; i < NUM_DMA_BUFS; i++) {
        if (posix_memalign((void **)&dma_bufs[i], 4096, g_chunk) != 0) {
            dma_bufs[i] = NULL;
            perror("posix_memalign (dma)");
            goto cleanup;
        }
        memset(dma_bufs[i], 0, g_chunk);
    }
    if (register_dma_bufs(dev_fd, dma_bufs, dma_idx) < 0)
        goto cleanup;

    /*
     * Wait for the first chunk before entering the DMA loop so the card
     * replays real signal, not zeros. Interruptible: Ctrl+C exits.
     */
    printf("[H2C] Waiting for GNU Radio (fifo_sink) on %s ...\n", g_fifo_path);
    int fifo_fd = open_fifo_rd();
    if (g_stop || fifo_fd < 0) {
        printf("[H2C] Interrupted before GNU Radio connected.\n");
        exit_code = 0;
        if (fifo_fd >= 0) close(fifo_fd);
        goto cleanup;
    }
    printf("[H2C] GNU Radio connected.\n");
    if (read_all(fifo_fd, stage_ready, g_chunk) != 0) {
        printf("[H2C] Interrupted/EOF before first chunk.\n");
        exit_code = 0;
        close(fifo_fd);
        goto cleanup;
    }
    g_updates = 1;
    printf("[H2C] First chunk received. Starting DMA replay loop...\n\n");

    /*
     * Prime dma_bufs[0] with the first chunk BEFORE the reader thread
     * exists - no lock needed, no update can be lost.
     */
    int cur = 0;                      /* index of the buffer being replayed */
    memcpy(dma_bufs[cur], stage_ready, g_chunk);
    g_new_data = 0;

    if (pthread_create(&reader_tid, NULL, fifo_reader,
                       (void *)(intptr_t)fifo_fd) != 0) {
        perror("pthread_create (reader)");
        close(fifo_fd);
        goto cleanup;
    }
    reader_started = 1;               /* reader now owns fifo_fd */

    wd_started = (pthread_create(&wd_tid, NULL, stall_watchdog, NULL) == 0);
    if (!wd_started)
        fprintf(stderr, "[H2C] WARNING: watchdog thread failed to start - "
                        "stall auto-recovery disabled\n");

    /* ------------------------- DMA replay loop ------------------------- */
    uint64_t ibytes = 0;
    uint64_t last_updates = 0;
    time_t   last = time(NULL);
    int      ebusy_retries = 0;
    int      etimedout_retries = 0;
    int      stall_kicks = 0;
    uint64_t reopen_cycles = 0;

    while (!g_stop) {
        /*
         * Swap-in point: the previous ioctl has fully completed (or was
         * abandoned by a kick - either way both buffers are idle from
         * userspace's perspective). If GR delivered a new chunk, copy
         * it into the other buffer and switch.
         */
        pthread_mutex_lock(&g_mtx);
        if (g_new_data) {
            g_new_data = 0;
            int nxt = 1 - cur;
            memcpy(dma_bufs[nxt], stage_ready, g_chunk);
            cur = nxt;
        }
        pthread_mutex_unlock(&g_mtx);

        struct iwfg_user_dma_req req = {
            .buf       = NULL,                    /* lookup by index      */
            .size      = (uint32_t)g_chunk,
            .offset    = 0,                       /* ignored by H2C path  */
            .direction = IWFG_DIR_H2C_BUF,        /* informational        */
            .buf_index = dma_idx[cur],
            .flags     = IWFG_FLAG_NORM_PACKET,   /* ignored by H2C path  */
        };

        /* Blocks until the card has consumed the whole buffer. */
        g_in_dma = 1;
        int rc = ioctl(dev_fd, IWFG_IOCTL_DMA_DATA, &req);
        g_in_dma = 0;

        if (rc < 0) {
            if (errno == EINTR) {
                if (g_stop) {
                    printf("\n[H2C] Transfer interrupted, shutting down...\n");
                    break;
                }
                /* Watchdog kick: the ioctl was wedged (fix A). */
                stall_kicks++;
                if (stall_kicks == 1 ||
                    stall_kicks % DIAG_EVERY_CYCLES == 0)
                    fprintf(stderr, "[H2C] stall kick %d (reopen cycle "
                            "%llu) - retrying transfer\n", stall_kicks,
                            (unsigned long long)reopen_cycles);
                if (stall_kicks >= KICKS_PER_REOPEN) {
                    stall_kicks = 0;
                    reopen_cycles++;
                    if ((reopen_cycles == 1 ||
                         reopen_cycles % DIAG_EVERY_CYCLES == 0))
                        print_stall_diag();
                    if (reopen_device(dev, &dev_fd, dma_bufs, dma_idx) < 0)
                        break;            /* device unrecoverable */
                }
                continue;
            }
            if (errno == EBUSY) {
                /* C2H fifo-overflow recovery is soft-resetting the QDMA;
                 * it rebuilds the TX queue itself - back off and retry. */
                if (++ebusy_retries > EBUSY_MAX_RETRIES) {
                    fprintf(stderr, "[H2C] h2c reset never completed "
                            "(EBUSY x%d) - cycling device\n", ebusy_retries);
                    ebusy_retries = 0;
                    reopen_cycles++;
                    if (reopen_device(dev, &dev_fd, dma_bufs, dma_idx) < 0)
                        break;
                    continue;
                }
                usleep(EBUSY_RETRY_US);
                continue;
            }
            if (errno == ETIMEDOUT) {
                /* Two retryable sources:
                 *  a) h2c_busy contention (iwfg_xmit_data bounded spin);
                 *  b) the driver's 2 s completion timeout - the card
                 *     stopped accepting the AXI-Stream (tready low);
                 *     dmesg has the pidx/cidx ring dump. */
                if (++etimedout_retries > ETIMEDOUT_MAX) {
                    fprintf(stderr,
                        "[H2C] card did not consume data for ~%d s "
                        "(ETIMEDOUT x%d) - cycling device\n",
                        2 * etimedout_retries, etimedout_retries);
                    etimedout_retries = 0;
                    reopen_cycles++;
                    if ((reopen_cycles == 1 ||
                         reopen_cycles % DIAG_EVERY_CYCLES == 0))
                        print_stall_diag();
                    if (reopen_device(dev, &dev_fd, dma_bufs, dma_idx) < 0)
                        break;
                    continue;
                }
                fprintf(stderr, "[H2C] DMA_DATA ETIMEDOUT (%d/%d) - card "
                        "not consuming, retrying\n",
                        etimedout_retries, ETIMEDOUT_MAX);
                continue;
            }
            perror("[H2C] IWFG_IOCTL_DMA_DATA");
            break;
        }

        /* Success: full recovery - reset every failure counter. */
        if (stall_kicks || etimedout_retries || reopen_cycles) {
            if (reopen_cycles)
                fprintf(stderr, "[H2C] Recovered after %llu device "
                        "cycle(s); streaming resumed.\n",
                        (unsigned long long)reopen_cycles);
            stall_kicks = 0;
            etimedout_retries = 0;
            reopen_cycles = 0;
        }
        ebusy_retries = 0;
        g_dma_done++;

        total_bytes += g_chunk;
        ibytes      += g_chunk;

        time_t now = time(NULL);
        if (now > last) {
            uint64_t upd_snapshot;
            pthread_mutex_lock(&g_mtx);
            upd_snapshot = g_updates;
            pthread_mutex_unlock(&g_mtx);

            printf("[H2C] %.2f MB/s  total=%.1f MB  GR_updates=%llu/s\n",
                   ibytes / 1048576.0 / (double)(now - last),
                   total_bytes / 1048576.0,
                   (unsigned long long)(upd_snapshot - last_updates));
            ibytes = 0;
            last_updates = upd_snapshot;
            last = now;
        }
    }

    exit_code = 0;

cleanup:
    g_stop = 1;

    if (wd_started) {
        pthread_kill(wd_tid, SIGTERM);   /* wake its sleep */
        pthread_join(wd_tid, NULL);
    }
    if (reader_started) {
        /*
         * Reader may be blocked in open()/read() on the FIFO; SIGTERM
         * (installed without SA_RESTART) makes those return EINTR. A
         * signal that lands in the tiny window BEFORE the thread blocks
         * would be lost, so re-signal until the thread actually joins.
         */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
        for (i = 0; i < 100; i++) {              /* <= 5 s */
            pthread_kill(reader_tid, SIGTERM);
            if (pthread_tryjoin_np(reader_tid, NULL) == 0)
                break;
            nanosleep(&ts, NULL);
        }
        if (i >= 100)
            pthread_join(reader_tid, NULL);      /* last resort: block */
    }

    /*
     * v9 fix D: do NOT unlink the FIFO. A peer blocked in open() waits
     * on this inode; deleting it would strand that peer forever, and
     * the next run reuses the same inode anyway (ensure_fifo).
     */

    /*
     * close(dev_fd) is what actually releases the hardware: the driver's
     * iwfg_release() clears the TX queue and unpins/unmaps every buffer
     * registered on this cdev (iwfg_delete_user_buf). Only AFTER that is
     * it correct to free() the DMA buffers back to the allocator.
     */
    if (dev_fd >= 0)
        close(dev_fd);

    for (i = 0; i < NUM_DMA_BUFS; i++)
        free(dma_bufs[i]);
    free(stage_fill);
    free(stage_ready);

    printf("[H2C] Stopped. Total sent: %.1f MB\n", total_bytes / 1048576.0);
    return exit_code;
}
