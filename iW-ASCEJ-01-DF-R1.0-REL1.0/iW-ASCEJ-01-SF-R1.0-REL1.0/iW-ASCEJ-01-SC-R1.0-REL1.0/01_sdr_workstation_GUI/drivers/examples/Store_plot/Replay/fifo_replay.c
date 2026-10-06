#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

/*
 * fifo_replay.c - replay a stored capture file into a FIFO for GNU Radio
 * ----------------------------------------------------------------------
 *
 * Feeds a .bin file (from c2h_store) into the same FIFO your live
 * fifo_pipe/fifo_stream app used, so the identical GNU Radio flowgraph
 * plots stored data.
 *
 * Pacing: none needed by default.  The pipe applies backpressure - write()
 * blocks when the pipe is full - so GNU Radio consumes at exactly its own
 * flowgraph rate, the same way it did against the live app.  An optional
 * rate cap (-r) is available if you want slower-than-GR playback.
 *
 * Usage:
 *   ./fifo_replay capture0.bin                      # default FIFO /tmp/iwfg_fifo
 *   ./fifo_replay capture0.bin /tmp/iwfg_fifo -l    # loop forever
 *   ./fifo_replay capture0.bin -s 64 -l             # skip first 64 MB (dead air)
 *   ./fifo_replay capture0.bin -r 30                # cap at 30 MB/s
 *
 * Build: gcc -O2 -Wall -o fifo_replay fifo_replay.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>

#define DEFAULT_FIFO_PATH "/tmp/iwfg_fifo"
#define CHUNK_SIZE (1024 * 1024)

static volatile sig_atomic_t g_stop = 0;

static void stop_handler(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* No SA_RESTART so Ctrl+C interrupts blocked open/write; SIGPIPE ignored so
 * a closed GNU Radio reader surfaces as EPIPE, not process death. */
static void install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = stop_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, NULL);
}

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t left = len;

    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) {
                if (g_stop)
                    return -1;
                continue;
            }
            return -1; /* includes EPIPE: reader disconnected */
        }
        if (n == 0)
            return -1;
        p += (size_t)n;
        left -= (size_t)n;
    }

    return 0;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s <capture.bin> [fifo] [options]\n"
            "\n"
            "Replays a stored capture into a FIFO so the same GNU Radio\n"
            "flowgraph that plotted the live stream plots the stored data.\n"
            "\n"
            "  fifo              FIFO path (default %s)\n"
            "  -l, --loop        loop the file forever (Ctrl+C to stop)\n"
            "  -s, --skip MB     skip this many MB at the start of the file\n"
            "  -r, --rate MB/s   cap playback rate (default 0 = GNU Radio's own pace)\n",
            prog, DEFAULT_FIFO_PATH);
}

int main(int argc, char **argv)
{
    const char *file_path = NULL;
    char fifo_path[4096];
    bool loop = false;
    unsigned long skip_mb = 0;
    unsigned long rate_mb = 0;
    bool fifo_created = false;
    FILE *in = NULL;
    int fifo_fd = -1;
    int exit_code = 1;
    static uint8_t chunk[CHUNK_SIZE];

    snprintf(fifo_path, sizeof(fifo_path), "%s", DEFAULT_FIFO_PATH);
    install_signal_handlers();
    setvbuf(stdout, NULL, _IOLBF, 0);

    int positional = 0;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(arg, "-l") == 0 || strcmp(arg, "--loop") == 0) {
            loop = true;
            continue;
        }
        if (strcmp(arg, "-s") == 0 || strcmp(arg, "--skip") == 0) {
            if (++i >= argc) { print_usage(argv[0]); return 1; }
            char *end = NULL;
            skip_mb = strtoul(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0') {
                fprintf(stderr, "Invalid skip '%s'\n", argv[i]);
                return 1;
            }
            continue;
        }
        if (strcmp(arg, "-r") == 0 || strcmp(arg, "--rate") == 0) {
            if (++i >= argc) { print_usage(argv[0]); return 1; }
            char *end = NULL;
            rate_mb = strtoul(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0') {
                fprintf(stderr, "Invalid rate '%s'\n", argv[i]);
                return 1;
            }
            continue;
        }
        if (arg[0] == '-') {
            fprintf(stderr, "Unknown argument: %s\n", arg);
            print_usage(argv[0]);
            return 1;
        }

        if (positional == 0)
            file_path = arg;
        else if (positional == 1)
            snprintf(fifo_path, sizeof(fifo_path), "%s", arg);
        else {
            fprintf(stderr, "Too many positional arguments: %s\n", arg);
            print_usage(argv[0]);
            return 1;
        }
        positional++;
    }

    if (!file_path) {
        print_usage(argv[0]);
        return 1;
    }

    in = fopen(file_path, "rb");
    if (!in) {
        perror("open capture file");
        return 1;
    }

    struct stat fst;
    if (stat(file_path, &fst) == 0)
        printf("File   : %s (%.2f MB)\n", file_path,
               (double)fst.st_size / (1024.0 * 1024.0));

    uint64_t skip_bytes = (uint64_t)skip_mb * 1024u * 1024u;
    if (skip_bytes && fseeko(in, (off_t)skip_bytes, SEEK_SET) != 0) {
        perror("seek");
        goto cleanup;
    }
    if (skip_bytes)
        printf("Skip   : %lu MB\n", skip_mb);

    /* Reuse an existing FIFO; recreating the inode strands a reader that is
     * already blocked in open() on the old one. */
    struct stat st;
    if (stat(fifo_path, &st) == 0) {
        if (!S_ISFIFO(st.st_mode)) {
            fprintf(stderr, "%s exists and is not a FIFO\n", fifo_path);
            goto cleanup;
        }
        printf("FIFO   : %s (reusing)\n", fifo_path);
    } else {
        if (mkfifo(fifo_path, 0666) < 0) {
            perror("mkfifo");
            goto cleanup;
        }
        fifo_created = true;
        printf("FIFO   : %s (created)\n", fifo_path);
    }

    printf("Loop   : %s\n", loop ? "yes" : "no");
    if (rate_mb)
        printf("Rate   : capped at %lu MB/s\n", rate_mb);
    else
        printf("Rate   : GNU Radio's own pace (pipe backpressure)\n");
    printf("Waiting for FIFO reader (start your GNU Radio flowgraph)...\n");

    /* Blocks until GNU Radio opens the other end; Ctrl+C interrupts it. */
    while (!g_stop) {
        fifo_fd = open(fifo_path, O_WRONLY);
        if (fifo_fd >= 0)
            break;
        if (errno == EINTR)
            continue;
        perror("open fifo");
        goto cleanup;
    }
    if (g_stop || fifo_fd < 0) {
        printf("Interrupted before a reader connected.\n");
        exit_code = 0;
        goto cleanup;
    }

    printf("Reader connected! Replaying...\n");

    uint64_t total = 0, interval = 0;
    time_t last = time(NULL);
    struct timespec rate_start;
    clock_gettime(CLOCK_MONOTONIC, &rate_start);
    uint64_t rate_sent = 0;

    while (!g_stop) {
        size_t n = fread(chunk, 1, sizeof(chunk), in);

        if (n == 0) {
            if (ferror(in)) {
                perror("read capture file");
                goto done;
            }
            /* EOF */
            if (!loop) {
                printf("\nEnd of file reached.\n");
                break;
            }
            if (fseeko(in, (off_t)skip_bytes, SEEK_SET) != 0) {
                perror("seek (loop)");
                goto done;
            }
            rate_sent = 0;
            clock_gettime(CLOCK_MONOTONIC, &rate_start);
            continue;
        }

        if (write_all(fifo_fd, chunk, n) < 0) {
            if (errno == EPIPE)
                printf("\nFIFO reader disconnected.\n");
            else if (g_stop)
                printf("\nInterrupted.\n");
            else
                perror("write to fifo");
            break;
        }

        total += n;
        interval += n;
        rate_sent += n;

        /* Optional rate cap: sleep whenever we are ahead of schedule. */
        if (rate_mb) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (double)(now.tv_sec - rate_start.tv_sec) +
                             (double)(now.tv_nsec - rate_start.tv_nsec) / 1e9;
            double budget = (double)rate_sent /
                            ((double)rate_mb * 1024.0 * 1024.0);
            if (budget > elapsed) {
                double ahead = budget - elapsed;
                struct timespec ts = {
                    .tv_sec = (time_t)ahead,
                    .tv_nsec = (long)((ahead - (time_t)ahead) * 1e9),
                };
                nanosleep(&ts, NULL);
            }
        }

        time_t now = time(NULL);
        if (now > last) {
            printf("Replaying: %.2f MB/s (total: %.2f MB)\n",
                   (double)interval / (1024.0 * 1024.0) / (double)(now - last),
                   (double)total / (1024.0 * 1024.0));
            interval = 0;
            last = now;
        }
    }

done:
    exit_code = 0;
    printf("Replayed %.2f MB total.\n", (double)total / (1024.0 * 1024.0));

cleanup:
    if (fifo_fd >= 0)
        close(fifo_fd);   /* EOFs the GNU Radio reader */
    if (fifo_created)
        unlink(fifo_path);
    if (in)
        fclose(in);
    return exit_code;
}
