/*
 * main.c - iwfg_c2h: Mellanox 100G UDP payload -> FIFO streamer.
 *
 * Threading model (3 worker threads + main):
 *
 *      NIC ==> kernel UDP ==> [RX thread] ==> SPSC ring ==> [WR thread]
 *                              recvmmsg        368 MiB        write()
 *                              batch=128       lock-free      <= 4 MiB
 *                                                              chunks
 *                                        [stats thread] 1 Hz
 *
 * Why only two threads on the datapath: the FIFO is a strictly ordered
 * byte stream, so exactly one thread may write it, and exactly one
 * thread must receive (SO_REUSEPORT fan-out would reorder samples).
 * Extra threads could only sit between them - and any hop between the
 * kernel's copy into the ring and the pipe write would add a copy or a
 * queue for zero gain.  Fewer threads, pinned to adjacent cores on the
 * NIC's NUMA node, benchmark faster than a 6-8 thread pipeline here.
 */
#include "iwfg.h"
#include "ring.h"
#include "rx.h"
#include "writer.h"
#include "stats.h"

#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

atomic_int g_stop = 0;
struct iwfg_stats g_stats;

static void on_signal(int sig)
{
    (void)sig;
    int saved = errno;          /* handler must not spoil errno */
    atomic_store(&g_stop, 1);
    errno = saved;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
"iwfg_c2h %s - UDP payload to FIFO streamer\n"
"usage: %s [options]\n"
"  -p, --port N        UDP port to listen on          (default %u)\n"
"  -b, --bind ADDR     local IPv4 address to bind      (default any)\n"
"      --raw IFNAME    AF_PACKET capture mode: accept frames with any\n"
"                      dst MAC / bad IP checksum, filter by UDP dst\n"
"                      port in-kernel, strip 42B headers (needs root)\n"
"  -f, --fifo PATH     output FIFO                     (default %s)\n"
"  -s, --payload N     expected UDP payload bytes      (default %u)\n"
"  -r, --ring N        ring slots, rounded to pow2     (default %u)\n"
"  -B, --batch N       recvmmsg batch size             (default %u)\n"
"      --rcvbuf N      SO_RCVBUF bytes                 (default %u)\n"
"      --pipe-sz N     FIFO pipe buffer bytes          (default max)\n"
"      --busy-poll N   SO_BUSY_POLL microseconds       (default off)\n"
"      --rx-cpu N      pin RX thread to CPU N          (default off)\n"
"      --wr-cpu N      pin writer thread to CPU N      (default off)\n"
"      --stats N       stats interval seconds, 0=off   (default %d)\n"
"      --spin          writer busy-spins when idle (lowest latency)\n"
"      --hugepages     back the ring with explicit 2M hugepages\n"
"  -h, --help          this text\n",
        IWFG_VERSION, argv0, IWFG_DEFAULT_PORT, IWFG_DEFAULT_FIFO,
        IWFG_DEFAULT_PAYLOAD, IWFG_DEFAULT_SLOTS, IWFG_DEFAULT_BATCH,
        IWFG_DEFAULT_RCVBUF, IWFG_DEFAULT_STATS_SEC);
}

static void parse_args(int argc, char **argv, struct iwfg_cfg *cfg)
{
    static const struct option opts[] = {
        { "port",      required_argument, NULL, 'p' },
        { "bind",      required_argument, NULL, 'b' },
        { "raw",       required_argument, NULL,  9  },
        { "fifo",      required_argument, NULL, 'f' },
        { "payload",   required_argument, NULL, 's' },
        { "ring",      required_argument, NULL, 'r' },
        { "batch",     required_argument, NULL, 'B' },
        { "rcvbuf",    required_argument, NULL,  1  },
        { "pipe-sz",   required_argument, NULL,  2  },
        { "busy-poll", required_argument, NULL,  3  },
        { "rx-cpu",    required_argument, NULL,  4  },
        { "wr-cpu",    required_argument, NULL,  5  },
        { "stats",     required_argument, NULL,  6  },
        { "spin",      no_argument,       NULL,  7  },
        { "hugepages", no_argument,       NULL,  8  },
        { "help",      no_argument,       NULL, 'h' },
        { 0, 0, 0, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "p:b:f:s:r:B:h", opts, NULL)) != -1) {
        switch (c) {
        case 'p': cfg->port    = (uint16_t)atoi(optarg); break;
        case 'b': cfg->bind_addr = optarg;               break;
        case 'f': cfg->fifo_path = optarg;               break;
        case 's': cfg->payload = (uint32_t)atoi(optarg); break;
        case 'r': cfg->slots   = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'B': cfg->batch   = (uint32_t)atoi(optarg); break;
        case  1 : cfg->rcvbuf  = (uint32_t)strtoul(optarg, NULL, 0); break;
        case  2 : cfg->pipe_sz = (uint32_t)strtoul(optarg, NULL, 0); break;
        case  3 : cfg->busy_poll_us = (uint32_t)atoi(optarg); break;
        case  4 : cfg->rx_cpu  = atoi(optarg); break;
        case  5 : cfg->wr_cpu  = atoi(optarg); break;
        case  6 : cfg->stats_sec = atoi(optarg); break;
        case  7 : cfg->spin = true; break;
        case  8 : cfg->use_hugepages = true; break;
        case  9 : cfg->raw_ifname = optarg; break;
        case 'h': usage(argv[0]); exit(0);
        default : usage(argv[0]); exit(2);
        }
    }

    if (cfg->payload == 0 || cfg->payload > 65507)
        iwfg_fatal("payload must be 1..65507");
    if (cfg->batch == 0 || cfg->batch > IWFG_MAX_BATCH)
        iwfg_fatal("batch must be 1..%u", IWFG_MAX_BATCH);
    if (cfg->slots < cfg->batch * 2)
        iwfg_fatal("ring must hold at least 2 batches");
    if (cfg->stats_sec < 0)
        cfg->stats_sec = 0;
}

int main(int argc, char **argv)
{
    struct iwfg_cfg cfg = {
        .fifo_path = IWFG_DEFAULT_FIFO,
        .bind_addr = NULL,
        .port      = IWFG_DEFAULT_PORT,
        .payload   = IWFG_DEFAULT_PAYLOAD,
        .slots     = IWFG_DEFAULT_SLOTS,
        .batch     = IWFG_DEFAULT_BATCH,
        .rcvbuf    = IWFG_DEFAULT_RCVBUF,
        .pipe_sz   = 0,
        .busy_poll_us = 0,
        .rx_cpu    = -1,
        .wr_cpu    = -1,
        .stats_sec = IWFG_DEFAULT_STATS_SEC,
        .spin      = false,
        .use_hugepages = false,
    };
    parse_args(argc, argv, &cfg);

    if (!atomic_is_lock_free(&g_stop))
        iwfg_fatal("atomic_int is not lock-free on this target");

    /* SIGPIPE must never kill us - EPIPE is handled inline. */
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    iwfg_log("iwfg_c2h %s  port=%u payload=%u ring=%u batch=%u fifo=%s%s%s",
             IWFG_VERSION, cfg.port, cfg.payload, cfg.slots, cfg.batch,
             cfg.fifo_path,
             cfg.raw_ifname ? " raw=" : "",
             cfg.raw_ifname ? cfg.raw_ifname : "");

    struct ring ring;
    int rc = ring_init(&ring, cfg.slots, cfg.payload, cfg.use_hugepages);
    if (rc != 0)
        iwfg_fatal("ring_init: %s", strerror(-rc));
    iwfg_log("ring: %" PRIu64 " slots x %u B = %.1f MiB",
             ring.slots, ring.slot_size,
             (double)(ring.slots * ring.slot_size) / (1u << 20));

    struct rx_ctx     rxc = { .cfg = &cfg, .ring = &ring, .sock_fd = -1 };
    struct writer_ctx wrc = { .cfg = &cfg, .ring = &ring };
    struct stats_ctx  stc = { .cfg = &cfg, .ring = &ring };

    pthread_t t_rx, t_wr, t_st;
    bool have_st = false;

    if (pthread_create(&t_wr, NULL, writer_thread, &wrc) != 0)
        iwfg_fatal("pthread_create(writer)");
    if (pthread_create(&t_rx, NULL, rx_thread, &rxc) != 0)
        iwfg_fatal("pthread_create(rx)");
    if (cfg.stats_sec > 0) {
        if (pthread_create(&t_st, NULL, stats_thread, &stc) != 0)
            iwfg_fatal("pthread_create(stats)");
        have_st = true;
    }

    pthread_join(t_rx, NULL);
    atomic_store(&g_stop, 1);   /* rx died or Ctrl+C - stop everyone */
    pthread_join(t_wr, NULL);
    if (have_st)
        pthread_join(t_st, NULL);

    stats_final(&ring);
    ring_free(&ring);
    iwfg_log("clean shutdown");
    return 0;
}
