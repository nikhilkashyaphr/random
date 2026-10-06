#define _POSIX_C_SOURCE 200809L

/*
 * fifo_pipe.c - iwfg C2H live streaming to a FIFO (e.g. for GNU Radio)
 *
 * FIXES versus the previous revision (each explained at the point of use):
 *
 * 1. SHUTDOWN NEVER HAPPENED: signal() installs handlers with SA_RESTART,
 *    so when Ctrl+C interrupted the blocked IWFG_IOCTL_DMA_DATA (the driver
 *    parks the caller in wait_event_interruptible() inside
 *    iwfg_wait_for_data()), the kernel's -ERESTARTSYS made libc restart the
 *    ioctl transparently and the loop never observed keep_running == 0.
 *    The same applied to the blocking open(fifo, O_WRONLY) and write().
 *    -> Handlers are now installed with sigaction() and sa_flags = 0
 *       (no SA_RESTART), so every blocking syscall returns -1/EINTR and
 *       the loop exits cleanly.
 *
 * 2. SIGPIPE KILLED THE PROCESS: if the FIFO reader (GNU Radio) exited,
 *    write() raised SIGPIPE whose default action terminates the process
 *    immediately - no unlink, no orderly close, stream left "dirty".
 *    -> SIGPIPE is ignored; write() now returns -1/EPIPE which is handled
 *       as a normal "reader disconnected" shutdown.
 *
 * 3. WRONG TEARDOWN ORDER / INCOMPLETE CLEANUP: buffers were free()d while
 *    still pinned/DMA-mapped by the driver, and the FIFO was not unlinked
 *    on the error path. The driver releases everything (clear_rx_queue,
 *    c2h_stop, delete_user_buf, and a HW reset when no stream remains) in
 *    iwfg_release(), i.e. on close(fd).
 *    -> Teardown is now: close(fifo) -> unlink(fifo) -> close(device)
 *       -> free(buffers), on every exit path.
 *
 * 4. EINTR LOOPS: write_all() retried on EINTR unconditionally, so a
 *    signal during a FIFO write could spin instead of stopping.
 *    -> EINTR now also checks keep_running.
 *
 * 5. STATS: printed one buffer's size as "MB/s" regardless of how many
 *    buffers moved that second. Now measures real bytes per interval.
 *
 * 6. DEFAULT DEVICE: "/dev/iwfg" does not exist; the driver creates
 *    /dev/iwfg0 (C2H) and /dev/iwfg1 (H2C). Default is now /dev/iwfg0.
 *
 * Build:  gcc -O2 -Wall -o fifo_pipe fifo_pipe.c
 * Usage:  ./fifo_pipe [/dev/iwfg0] [/tmp/iwfg_fifo]
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

#include "iwfg_user.h"

#define BUF_SIZE   (16 * 1024 * 1024)
#define NUM_BUFS   4

#ifndef IWFG_HAVE_C2H_PREARM
struct iwfg_c2h_prearm {
    void *next_buf;
    uint32_t next_size;
};
#endif

static volatile sig_atomic_t keep_running = 1;

static void sig_handler(int sig)
{
    (void)sig;
    keep_running = 0;
}

/*
 * Install SIGINT/SIGTERM WITHOUT SA_RESTART so they actually interrupt the
 * blocked ioctl/write/open (fix #1), and ignore SIGPIPE so a vanished FIFO
 * reader surfaces as write()==-1/EPIPE instead of killing us (fix #2).
 */
static int install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* deliberately NO SA_RESTART */

    if (sigaction(SIGINT, &sa, NULL) < 0)
        return -1;
    if (sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, NULL) < 0)
        return -1;

    return 0;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t left = len;

    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) {
                if (!keep_running)   /* fix #4: signal means stop, not retry */
                    return -1;
                continue;
            }
            return -1;               /* includes EPIPE: reader disconnected */
        }

        if (n == 0)
            return -1;

        p += (size_t)n;
        left -= (size_t)n;
    }

    return 0;
}

int main(int argc, char **argv)
{
    const char *devnode   = (argc > 1) ? argv[1] : "/dev/iwfg0"; /* fix #6 */
    const char *fifo_path = (argc > 2) ? argv[2] : "/tmp/iwfg_fifo";

    void *bufs[NUM_BUFS] = { NULL };
    uint32_t buf_indices[NUM_BUFS];
    int fd = -1, fifo_fd = -1;
    int exit_code = 1;
    int i;

    if (install_signal_handlers() < 0) {
        perror("sigaction");
        return 1;
    }

    /* Create FIFO if it doesn't exist */
    unlink(fifo_path);  /* Remove old if exists */
    if (mkfifo(fifo_path, 0666) < 0) {
        perror("mkfifo");
        /* Continue anyway - might already exist */
    }

    printf("Opening device: %s\n", devnode);
    fd = open(devnode, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open device");
        goto cleanup;
    }

    /* Allocate page-aligned buffers and register each with the driver */
    for (i = 0; i < NUM_BUFS; i++) {
        if (posix_memalign(&bufs[i], 4096, BUF_SIZE) != 0) {
            bufs[i] = NULL;
            perror("posix_memalign");
            goto cleanup;
        }

        struct iwfg_user_buf user_buf = {
            .buf        = bufs[i],
            .size       = BUF_SIZE,
            .buf_type   = IWFG_BUF_TYPE_USERPTR,
            .import_fd  = -1,
            .is_chunked = false,
            .chunk_size = 0,
            .dmabuf_fd  = -1,
            .buf_index  = 0
        };

        if (ioctl(fd, IWFG_IOCTL_ADD_BUF, &user_buf) < 0) {
            perror("IWFG_IOCTL_ADD_BUF");
            goto cleanup;
        }

        buf_indices[i] = user_buf.buf_index;
    }

    printf("DMA streaming ready. Waiting for FIFO reader: %s\n", fifo_path);

    /*
     * Open FIFO - blocks until a reader opens the other end. With
     * SA_RESTART removed, Ctrl+C now interrupts this open with EINTR
     * instead of the app being stuck here forever (fix #1).
     */
    while (keep_running) {
        fifo_fd = open(fifo_path, O_WRONLY);
        if (fifo_fd >= 0)
            break;
        if (errno == EINTR)
            continue;   /* loop re-checks keep_running */
        perror("open fifo");
        goto cleanup;
    }
    if (!keep_running || fifo_fd < 0) {
        printf("Interrupted before a FIFO reader connected.\n");
        exit_code = 0;
        goto cleanup;
    }

    printf("FIFO connected! Streaming data...\n");

    size_t total_bytes    = 0;
    size_t interval_bytes = 0;                 /* fix #5 */
    time_t last_time      = time(NULL);
    int    buf_idx        = 0;  /* Round-robin through registered buffers */

    while (keep_running) {
        int next_idx = (buf_idx + 1) % NUM_BUFS;

        /* Issue a C2H DMA transfer into the current buffer */
        struct {
            struct iwfg_user_dma_req req;
            struct iwfg_c2h_prearm   prearm;
        } dma_req = {
            .req = {
                .buf       = NULL,
                .size      = BUF_SIZE,
                .direction = IWFG_DIR_C2H_BUF,
                .offset    = 0,
                .buf_index = buf_indices[buf_idx],
                .flags     = 0
            },
            .prearm = {
                .next_buf  = bufs[next_idx],
                .next_size = BUF_SIZE
            }
        };

        int ret = ioctl(fd, IWFG_IOCTL_DMA_DATA, &dma_req);
        if (ret < 0) {
            if (errno == EAGAIN) {
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
                nanosleep(&ts, NULL);  /* No data yet, retry */
                continue;
            }
            if (errno == EINTR) {
                /*
                 * Signal interrupted the in-kernel wait (fix #1). This is
                 * the normal Ctrl+C path now - not an error.
                 */
                printf("\nTransfer interrupted by signal, shutting down...\n");
                break;
            }
            perror("IWFG_IOCTL_DMA_DATA (C2H)");
            break;
        }

        /* Write received data to FIFO (blocking - GNU Radio will read) */
        if (write_all(fifo_fd, bufs[buf_idx], BUF_SIZE) < 0) {
            if (errno == EPIPE)
                printf("\nFIFO reader disconnected, shutting down...\n");
            else if (errno == EINTR)
                printf("\nFIFO write interrupted by signal, shutting down...\n");
            else
                perror("write to fifo");
            break;
        }

        /* Advance to next buffer (round-robin) */
        buf_idx = next_idx;

        /* Stats: real bytes moved in the last interval (fix #5) */
        total_bytes    += BUF_SIZE;
        interval_bytes += BUF_SIZE;
        time_t now = time(NULL);
        if (now > last_time) {
            double secs = (double)(now - last_time);
            double mbps = (double)interval_bytes / (1024.0 * 1024.0) / secs;
            printf("Streaming: %.2f MB/s (total: %.2f MB)\n",
                   mbps, total_bytes / (1024.0 * 1024.0));
            interval_bytes = 0;
            last_time = now;
        }
    }

    printf("\nStopping stream...\n");
    exit_code = 0;

cleanup:
    /*
     * Teardown in dependency order (fix #3):
     *
     *  1. FIFO first - unblocks/EOFs the reader and is always unlinked,
     *     including on error paths that previously skipped it.
     *  2. close(fd) - this is what actually "clears the system": the
     *     driver's iwfg_release() tears down the RX queue, stops the C2H
     *     stream, unpins/unmaps every registered buffer
     *     (iwfg_delete_user_buf) and soft-resets the QDMA hardware when
     *     no stream remains active.
     *  3. Only after the driver has dropped its page pins is it correct
     *     to free() the buffers back to the allocator.
     */
    if (fifo_fd >= 0)
        close(fifo_fd);
    unlink(fifo_path);

    if (fd >= 0)
        close(fd);

    for (i = 0; i < NUM_BUFS; i++)
        free(bufs[i]);

    printf("Cleanup complete.\n");
    return exit_code;
}
