/*
 * iwfg_c2h.c  v2  -  C2H: card -> GNU Radio FIFO
 *
 * ARCHITECTURE (preserved from v1): two-thread pipeline so DMA never
 * waits on FIFO backpressure.
 *   Thread 1 [dma_thread]:    blocking DMA ioctl fills the ring
 *   Thread 2 [writer_thread]: ring -> FIFO write
 *
 * REWORKED FOR THE CURRENT iwfg DRIVER (source of truth: iwfg_main.c,
 * iwfg_dma.c, reference app fifo_pipe.c):
 *
 *  1. DEVICE NODE: /dev/iwfg0 is the C2H node (qid 0). "/dev/iwfg"
 *     does not exist; direction is derived from the node in-kernel.
 *
 *  2. NO STREAM_START/STREAM_STOP: removed from the driver (-ENOTTY).
 *     open("/dev/iwfg0") initializes the RX queue AND starts the C2H
 *     hardware stream (iwfg_qdma_c2h_start); close() stops it, unpins
 *     all buffers and soft-resets the QDMA when no stream remains.
 *
 *  3. BLOCKING DMA + PREARM: DMA_DATA returns when the buffer is
 *     COMPLETELY filled. A struct iwfg_c2h_prearm appended after the
 *     dma_req in the same ioctl arg lets the driver post the NEXT ring
 *     buffer's descriptors before the current one drains - without it
 *     the QDMA ring starves between ioctls and the stream develops
 *     periodic gaps. next_size must equal the current req.size.
 *
 *  4. ERRORS: EAGAIN = the driver's FPGA fifo-overflow recovery ran
 *     (queues re-initialized in-kernel); back off briefly and retry.
 *     EINTR = signal during the in-kernel wait (normal Ctrl+C path).
 *
 *  5. ADD_BUF: buf_type = IWFG_BUF_TYPE_USERPTR and import_fd = -1 are
 *     mandatory; DMA is issued by buf_index (buf = NULL).
 *
 *  6. SIGNALS: sigaction() with sa_flags = 0 (NO SA_RESTART) so Ctrl+C
 *     actually interrupts the blocked ioctl/write/open instead of being
 *     transparently restarted. SIGPIPE ignored (EPIPE = GR left).
 *
 *  7. TEARDOWN ORDER: join threads -> close(FIFO) -> unlink ->
 *     close(device) [driver unpins pages here] -> free(buffers).
 *     v1 freed the ring while it was still pinned/DMA-mapped.
 *
 *  8. BUFFER SIZING: v1 used 16 KB DMA buffers; under the new model one
 *     ioctl = one full buffer, so 16 KB means an ioctl round-trip every
 *     ~22 us at 737 MB/s. Default is now 4 MB x 4 ring (tunable), same
 *     order as the proven fifo_pipe.c reference (16 MB x 4).
 *
 * Build:  gcc -O2 -Wall -pthread -o iwfg_c2h iwfg_c2h.c
 * Usage:  ./iwfg_c2h [/dev/iwfg0] [/tmp/iwfg_c2h.fifo] [buf_mbytes]
 */

#define _GNU_SOURCE   /* F_SETPIPE_SZ, pthread_tryjoin_np, usleep */

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

#include "iwfg_user.h"

#define DEF_DEVICE   "/dev/iwfg0"        /* C2H node (qid 0) */
#define DEF_FIFO     "/tmp/iwfg_c2h.fifo"
#define DEF_BUF_MB   16   /* matches the proven fifo_pipe.c configuration */
#define NUM_BUFS     4                   /* DMA ring depth */

static volatile sig_atomic_t g_stop = 0;
static volatile uint64_t g_dma_done = 0;   /* completed C2H buffers */
static void sig_handler(int s) { (void)s; g_stop = 1; }

/*
 * Stall watchdog: the C2H ioctl blocks until the FPGA has filled the
 * whole buffer. If the card-side stream is not producing (RF chain not
 * configured, ADC path not routed to QDMA C2H), the app goes silent
 * with 0 bytes. Detect it and say what it means.
 */
static void *stall_watchdog(void *arg)
{
    (void)arg;
    uint64_t last = 0;
    int quiet_secs = 0;

    while (!g_stop) {
        sleep(1);
        uint64_t now = g_dma_done;
        if (now == last) {
            if (++quiet_secs == 3)
                fprintf(stderr,
                    "[C2H] WARNING: no buffer completed for 3 s - the card is NOT\n"
                    "[C2H]   producing C2H data. Check PS-side RF configuration:\n"
                    "[C2H]   clocks+MTS locked, ADC enabled, AXIS switch routed\n"
                    "[C2H]   ADC->QDMA C2H.\n");
        } else {
            if (quiet_secs >= 3)
                fprintf(stderr, "[C2H] Card resumed producing data.\n");
            quiet_secs = 0;
        }
        last = now;
    }
    return NULL;
}

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
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, NULL) < 0) return -1;

    return 0;
}

/* ---------------------------------------------------------------------- */
/* DMA ring shared between dma_thread (producer) and writer_thread        */

static size_t   g_buf_size = (size_t)DEF_BUF_MB * 1024 * 1024;
static uint8_t *ring[NUM_BUFS];
static uint32_t ring_idx[NUM_BUFS];      /* driver buf_index per slot     */
static int      ring_head = 0;           /* next slot DMA fills           */
static int      ring_tail = 0;           /* next slot writer drains       */
static int      ring_fill = 0;
static pthread_mutex_t ring_mtx       = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  ring_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  ring_not_full  = PTHREAD_COND_INITIALIZER;

static int g_dev_fd  = -1;
static int g_fifo_fd = -1;

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) {
                if (g_stop) return -1;   /* signal means stop, not retry */
                continue;
            }
            return -1;                   /* includes EPIPE               */
        }
        if (n == 0) return -1;
        p += (size_t)n; len -= (size_t)n;
    }
    return 0;
}

/*
 * Thread 1: blocking DMA into ring[head], pre-arming ring[head+1] so the
 * driver posts its descriptors before the current buffer drains. Never
 * waits on the FIFO.
 */
static void *dma_thread(void *arg)
{
    (void)arg;

    while (!g_stop) {
        /* Wait for a free slot (writer signals ring_not_full). */
        pthread_mutex_lock(&ring_mtx);
        while (ring_fill >= NUM_BUFS && !g_stop)
            pthread_cond_wait(&ring_not_full, &ring_mtx);
        int head = ring_head;
        pthread_mutex_unlock(&ring_mtx);
        if (g_stop) break;

        int next = (head + 1) % NUM_BUFS;

        /*
         * dma_req + prearm concatenated in ONE ioctl argument - the
         * driver reads the prearm block from arg + sizeof(dma_req)
         * (see IWFG_IOCTL_DMA_DATA / DMA_FROM_DEVICE in iwfg_main.c).
         */
        struct {
            struct iwfg_user_dma_req req;
            struct iwfg_c2h_prearm   prearm;
        } dma = {
            .req = {
                .buf       = NULL,               /* lookup by index      */
                .size      = (uint32_t)g_buf_size,
                .offset    = 0,
                .direction = IWFG_DIR_C2H_BUF,   /* informational        */
                .buf_index = ring_idx[head],
                .flags     = IWFG_FLAG_NORM_PACKET,
            },
            .prearm = {
                .next_buf  = ring[next],
                .next_size = (uint32_t)g_buf_size,
            },
        };

        if (ioctl(g_dev_fd, IWFG_IOCTL_DMA_DATA, &dma) < 0) {
            if (errno == EAGAIN) {
                /* FPGA fifo overflow: the driver already re-initialized
                 * the queues; retry into the same slot. */
                usleep(1000);
                continue;
            }
            if (errno == EINTR) {
                /* Ctrl+C interrupted the in-kernel wait - normal path. */
                break;
            }
            if (!g_stop) perror("[C2H DMA] IWFG_IOCTL_DMA_DATA");
            break;
        }

        g_dma_done++;
        pthread_mutex_lock(&ring_mtx);
        ring_head = next;
        ring_fill++;
        pthread_cond_signal(&ring_not_empty);
        pthread_mutex_unlock(&ring_mtx);
    }

    g_stop = 1;
    pthread_cond_broadcast(&ring_not_empty);
    pthread_cond_broadcast(&ring_not_full);
    return NULL;
}

/* Thread 2: drain ring -> FIFO. Never blocks the DMA thread. */
static void *writer_thread(void *arg)
{
    (void)arg;
    size_t bytes = 0, ibytes = 0;
    time_t last = time(NULL);

    while (!g_stop) {
        pthread_mutex_lock(&ring_mtx);
        while (ring_fill == 0 && !g_stop) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&ring_not_empty, &ring_mtx, &ts);
        }
        if (ring_fill == 0) {            /* g_stop and nothing to drain */
            pthread_mutex_unlock(&ring_mtx);
            break;
        }
        int tail = ring_tail;
        ring_tail = (ring_tail + 1) % NUM_BUFS;
        ring_fill--;
        pthread_cond_signal(&ring_not_full);
        pthread_mutex_unlock(&ring_mtx);

        if (write_all(g_fifo_fd, ring[tail], g_buf_size) < 0) {
            if (errno == EPIPE)
                printf("[C2H writer] GNU Radio disconnected\n");
            else if (!g_stop)
                perror("[C2H writer] write");
            break;
        }

        bytes += g_buf_size; ibytes += g_buf_size;
        time_t now = time(NULL);
        if (now > last) {
            pthread_mutex_lock(&ring_mtx);
            int fill = ring_fill;
            pthread_mutex_unlock(&ring_mtx);
            printf("[C2H] %.2f MB/s  total=%.1f MB  ring=%d/%d\n",
                   ibytes / 1048576.0 / (double)(now - last),
                   bytes / 1048576.0, fill, NUM_BUFS);
            ibytes = 0; last = now;
        }
    }

    g_stop = 1;
    pthread_cond_broadcast(&ring_not_full);
    pthread_cond_broadcast(&ring_not_empty);
    printf("[C2H] Total received: %.1f MB\n", bytes / 1048576.0);
    return NULL;
}

/* Re-signal until the thread joins (covers the signal-before-block race). */
static void stop_and_join(pthread_t tid)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
    int i;

    for (i = 0; i < 100; i++) {          /* <= 5 s */
        pthread_kill(tid, SIGTERM);
        pthread_cond_broadcast(&ring_not_empty);
        pthread_cond_broadcast(&ring_not_full);
        if (pthread_tryjoin_np(tid, NULL) == 0)
            return;
        nanosleep(&ts, NULL);
    }
    pthread_join(tid, NULL);             /* last resort: block */
}

int main(int argc, char **argv)
{
    const char *dev       = (argc > 1) ? argv[1] : DEF_DEVICE;
    const char *fifo_path = (argc > 2) ? argv[2] : DEF_FIFO;
    if (argc > 3) {
        long mb = strtol(argv[3], NULL, 0);
        if (mb < 1 || mb > 64) {
            fprintf(stderr, "buf_mbytes must be 1..64\n");
            return 1;
        }
        g_buf_size = (size_t)mb * 1024 * 1024;
    }

    int exit_code = 1;
    int threads_started = 0;
    int wd_started = 0;
    pthread_t dma_tid, writer_tid, wd_tid;
    int i;

    if (install_signal_handlers() < 0) { perror("sigaction"); return 1; }

    unlink(fifo_path);
    if (mkfifo(fifo_path, 0666) < 0 && errno != EEXIST) {
        perror("mkfifo");
        return 1;
    }

    printf("[C2H v2] device=%s  fifo=%s  ring=%d x %zu MB\n",
           dev, fifo_path, NUM_BUFS, g_buf_size / (1024 * 1024));

    /* open() initializes the RX queue and starts the C2H hardware stream */
    g_dev_fd = open(dev, O_RDWR | O_CLOEXEC);
    if (g_dev_fd < 0) { perror("open device"); goto cleanup; }

    /* Allocate page-aligned ring buffers and register each with the driver */
    for (i = 0; i < NUM_BUFS; i++) {
        if (posix_memalign((void **)&ring[i], 4096, g_buf_size) != 0) {
            ring[i] = NULL;
            perror("posix_memalign");
            goto cleanup;
        }

        struct iwfg_user_buf ub = {
            .buf        = ring[i],
            .size       = (uint32_t)g_buf_size,
            .buf_type   = IWFG_BUF_TYPE_USERPTR,
            .import_fd  = -1,
            .is_chunked = false,
            .chunk_size = 0,
            .dmabuf_fd  = -1,
            .buf_index  = 0,
        };
        if (ioctl(g_dev_fd, IWFG_IOCTL_ADD_BUF, &ub) < 0) {
            perror("IWFG_IOCTL_ADD_BUF");
            goto cleanup;
        }
        ring_idx[i] = ub.buf_index;
    }

    printf("[C2H] DMA ready. Waiting for GNU Radio reader on %s ...\n", fifo_path);

    /* Blocking open until a reader appears - interruptible by Ctrl+C */
    while (!g_stop) {
        g_fifo_fd = open(fifo_path, O_WRONLY | O_CLOEXEC);
        if (g_fifo_fd >= 0) break;
        if (errno == EINTR) continue;
        perror("open fifo");
        goto cleanup;
    }
    if (g_stop || g_fifo_fd < 0) {
        printf("[C2H] Interrupted before a FIFO reader connected.\n");
        exit_code = 0;
        goto cleanup;
    }
    printf("[C2H] GNU Radio connected. Streaming...\n\n");

    /* Larger pipe buffer reduces writer stalls */
    if (fcntl(g_fifo_fd, F_SETPIPE_SZ, 1024 * 1024) < 0)
        fprintf(stderr, "[C2H] F_SETPIPE_SZ failed (non-fatal): %s\n",
                strerror(errno));

    wd_started = (pthread_create(&wd_tid, NULL, stall_watchdog, NULL) == 0);

    if (pthread_create(&dma_tid, NULL, dma_thread, NULL) != 0 ||
        pthread_create(&writer_tid, NULL, writer_thread, NULL) != 0) {
        perror("pthread_create");
        g_stop = 1;
        goto cleanup;
    }
    threads_started = 1;

    /* Main just waits; either thread setting g_stop ends the run */
    pthread_join(dma_tid, NULL);
    stop_and_join(writer_tid);
    threads_started = 0;

    exit_code = 0;

cleanup:
    g_stop = 1;

    if (wd_started) {
        pthread_kill(wd_tid, SIGTERM);
        pthread_join(wd_tid, NULL);
    }
    if (threads_started) {
        stop_and_join(dma_tid);
        stop_and_join(writer_tid);
    }

    if (g_fifo_fd >= 0)
        close(g_fifo_fd);
    unlink(fifo_path);

    /*
     * close(device) is what releases the hardware: iwfg_release() clears
     * the RX queue, stops the C2H stream, unpins every registered buffer
     * and soft-resets the QDMA when no stream remains. Only AFTER that
     * is it correct to free() the ring (v1 had this order inverted).
     */
    if (g_dev_fd >= 0)
        close(g_dev_fd);

    for (i = 0; i < NUM_BUFS; i++)
        free(ring[i]);

    printf("[C2H] Stopped.\n");
    return exit_code;
}
