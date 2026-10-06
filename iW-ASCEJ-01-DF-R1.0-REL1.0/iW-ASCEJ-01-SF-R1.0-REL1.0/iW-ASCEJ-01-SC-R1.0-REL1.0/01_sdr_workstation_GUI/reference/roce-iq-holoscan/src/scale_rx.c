/*
 * scale_rx.c -- N-link RoCEv2 validation receiver.
 *
 * Accepts up to -n connections from scale_tx, then measures everything you
 * need to certify the link under load:
 *
 *   aggregate + per-link throughput       (Gb/s, frames/s)
 *   packet integrity                      (payload pattern verification)
 *   loss                                  (per-link sequence gap counting --
 *                                          on RC QPs any gap means frames
 *                                          were lost ABOVE the transport,
 *                                          i.e. a real bug, since RC itself
 *                                          retransmits)
 *   one-way latency                       (sender timestamp vs. arrival;
 *                                          valid because both clocks are the
 *                                          same machine) as a histogram with
 *                                          min/avg/p50/p99/max
 *
 * Output: live 1 Hz line, optional per-second CSV (-C) and a final JSON
 * summary (-O) for scripted pass/fail evaluation.
 *
 * Usage:
 *   sudo ./scale_rx -a 192.168.100.2 -n 100 [-b 262144] [-V] \
 *                   [-C stats.csv] [-O results.json] [-t 60]
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <rdma/rdma_cma.h>

#include "scale_common.h"

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

#define DIE(msg) do { \
    fprintf(stderr, "FATAL %s:%d %s: %s\n", __FILE__, __LINE__, (msg), \
            strerror(errno)); exit(1); } while (0)

static uint64_t now_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
static uint64_t wall_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

struct conn {
    struct rdma_cm_id *id;
    int      active;
    uint32_t link_id;        /* from frame flags, set on first frame */
    int      have_link_id;
    uint64_t expect_seq;
    uint64_t frames, bytes, gaps, payload_err;
    struct lat_hist lat;
    uint8_t *slots;
};

int main(int argc, char **argv)
{
    const char *bind_ip = "0.0.0.0";
    const char *csv_path = NULL, *json_path = NULL;
    int port = DEFAULT_TCP_PORT, full_verify = 0;
    uint32_t nlinks = 0, frame_b = 262144, slots_per = 8;
    double run_secs = 0.0;

    int opt;
    while ((opt = getopt(argc, argv, "a:p:n:b:o:t:C:O:Vh")) != -1) {
        switch (opt) {
        case 'a': bind_ip   = optarg;                 break;
        case 'p': port      = atoi(optarg);           break;
        case 'n': nlinks    = (uint32_t)atoi(optarg); break;
        case 'b': frame_b   = (uint32_t)atoi(optarg); break;
        case 'o': slots_per = (uint32_t)atoi(optarg); break;
        case 't': run_secs  = atof(optarg);           break;
        case 'C': csv_path  = optarg;                 break;
        case 'O': json_path = optarg;                 break;
        case 'V': full_verify = 1;                    break;
        default:
            fprintf(stderr, "see header comment for usage\n");
            return 1;
        }
    }
    if (nlinks < 1 || nlinks > SCALE_MAX_LINKS || frame_b < 128 ||
        (frame_b & 7) || slots_per < 2) {
        fprintf(stderr, "need -n 1..%u; -b >=128 multiple of 8; -o >= 2\n",
                SCALE_MAX_LINKS);
        return 1;
    }
    const uint32_t payload_b = frame_b - (uint32_t)sizeof(struct frame_hdr);

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    size_t pool_b = (size_t)nlinks * slots_per * frame_b;
    uint8_t *pool = NULL;
    if (posix_memalign((void **)&pool, 4096, pool_b)) DIE("alloc pool");
    printf("[srx] expecting up to %u links, %u B frames, verify=%s "
           "(pool %.1f MiB)\n", nlinks, frame_b,
           full_verify ? "FULL" : "sampled", pool_b / 1048576.0);

    struct conn *C = calloc(nlinks, sizeof(*C));
    if (!C) DIE("calloc conns");
    for (uint32_t i = 0; i < nlinks; i++) {
        C[i].slots = pool + (size_t)i * slots_per * frame_b;
        C[i].expect_seq = UINT64_MAX;
        lat_init(&C[i].lat);
    }

    FILE *csv = NULL;
    if (csv_path) {
        csv = fopen(csv_path, "w");
        if (!csv) DIE("fopen csv");
        fprintf(csv, "t_s,links,gbps,fps,gaps,payload_err,"
                     "lat_avg_us,lat_p50_us,lat_p99_us,lat_max_us\n");
    }

    struct rdma_event_channel *ec = rdma_create_event_channel();
    if (!ec) DIE("rdma_create_event_channel");
    struct rdma_cm_id *listen_id = NULL;
    if (rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP))
        DIE("rdma_create_id");
    struct sockaddr_in addr = { .sin_family = AF_INET,
                                .sin_port = htons((uint16_t)port) };
    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) DIE("bad -a ip");
    if (rdma_bind_addr(listen_id, (struct sockaddr *)&addr))
        DIE("rdma_bind_addr");
    if (rdma_listen(listen_id, (int)nlinks)) DIE("rdma_listen");
    printf("[srx] listening on %s:%d\n", bind_ip, port);

    /* CM channel non-blocking: accepts and data interleave in one loop. */
    fcntl(ec->fd, F_SETFL, fcntl(ec->fd, F_GETFL) | O_NONBLOCK);

    struct ibv_pd *pd = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_mr *mr = NULL;
    struct ibv_context *verbs = NULL;
    uint32_t connected = 0, peak_connected = 0;

    uint64_t t_start = 0;           /* set on first frame */
    uint64_t last_t = now_ns();
    uint64_t win_bytes = 0, win_frames = 0;
    struct lat_hist win_lat; lat_init(&win_lat);
    uint64_t tot_frames = 0, tot_bytes = 0;

    while (!g_stop) {
        /* ---------------- connection events ---------------- */
        struct rdma_cm_event *ev = NULL;
        while (rdma_get_cm_event(ec, &ev) == 0) {
            if (ev->event == RDMA_CM_EVENT_CONNECT_REQUEST) {
                if (connected >= nlinks) {
                    rdma_reject(ev->id, NULL, 0);
                    rdma_ack_cm_event(ev);
                    continue;
                }
                struct rdma_cm_id *id = ev->id;
                rdma_ack_cm_event(ev);

                if (!pd) {
                    verbs = id->verbs;
                    pd = ibv_alloc_pd(verbs);
                    if (!pd) DIE("ibv_alloc_pd");
                    cq = ibv_create_cq(verbs,
                                       (int)(nlinks * slots_per + 16),
                                       NULL, NULL, 0);
                    if (!cq) DIE("ibv_create_cq");
                    mr = ibv_reg_mr(pd, pool, pool_b,
                                    IBV_ACCESS_LOCAL_WRITE);
                    if (!mr) DIE("ibv_reg_mr (raise memlock: ulimit -l)");
                }

                /* first free conn struct */
                uint32_t ci = 0;
                while (ci < nlinks && C[ci].active) ci++;
                struct conn *c = &C[ci];

                struct ibv_qp_init_attr qpa = {
                    .send_cq = cq, .recv_cq = cq, .qp_type = IBV_QPT_RC,
                    .cap = { .max_send_wr = 2, .max_recv_wr = slots_per,
                             .max_send_sge = 1, .max_recv_sge = 1 },
                };
                if (rdma_create_qp(id, pd, &qpa)) DIE("rdma_create_qp");

                for (uint32_t s = 0; s < slots_per; s++) {
                    struct ibv_sge sge = {
                        .addr = (uintptr_t)(c->slots + (size_t)s * frame_b),
                        .length = frame_b, .lkey = mr->lkey };
                    struct ibv_recv_wr wr = {
                        .wr_id = ((uint64_t)ci << 32) | s,
                        .sg_list = &sge, .num_sge = 1 };
                    struct ibv_recv_wr *bad = NULL;
                    if (ibv_post_recv(id->qp, &wr, &bad))
                        DIE("ibv_post_recv");
                }
                struct rdma_conn_param cp = {
                    .responder_resources = 1, .initiator_depth = 1,
                    .rnr_retry_count = 7 };
                if (rdma_accept(id, &cp)) DIE("rdma_accept");
                c->id = id;
                id->context = c;
            } else if (ev->event == RDMA_CM_EVENT_ESTABLISHED) {
                struct conn *c = ev->id->context;
                c->active = 1;
                connected++;
                if (connected > peak_connected) peak_connected = connected;
                if (connected % 10 == 0 || connected == nlinks)
                    printf("[srx] %u/%u links up\n", connected, nlinks);
                rdma_ack_cm_event(ev);
            } else if (ev->event == RDMA_CM_EVENT_DISCONNECTED) {
                struct conn *c = ev->id->context;
                if (c && c->active) {
                    c->active = 0;
                    connected--;
                    rdma_disconnect(ev->id);
                    rdma_destroy_qp(ev->id);
                }
                rdma_ack_cm_event(ev);
                if (connected == 0 && tot_frames > 0)
                    g_stop = 1;       /* test over */
            } else {
                rdma_ack_cm_event(ev);
            }
        }

        /* ---------------- data path ---------------- */
        struct ibv_wc wc[32];
        int n = (cq ? ibv_poll_cq(cq, 32, wc) : 0);
        if (n < 0) DIE("ibv_poll_cq");

        for (int i = 0; i < n; i++) {
            uint32_t ci = (uint32_t)(wc[i].wr_id >> 32);
            uint32_t sl = (uint32_t)wc[i].wr_id;
            struct conn *c = &C[ci];
            if (wc[i].status != IBV_WC_SUCCESS) {
                if (wc[i].status != IBV_WC_WR_FLUSH_ERR)
                    fprintf(stderr, "[srx] WC error conn %u: %s\n", ci,
                            ibv_wc_status_str(wc[i].status));
                continue;
            }
            uint8_t *buf = c->slots + (size_t)sl * frame_b;
            struct frame_hdr *h = (struct frame_hdr *)buf;

            if (h->magic == IQ_FRAME_MAGIC && wc[i].byte_len == frame_b) {
                if (!c->have_link_id) {
                    c->link_id = h->flags;
                    c->have_link_id = 1;
                }
                if (c->expect_seq != UINT64_MAX && h->seq != c->expect_seq)
                    c->gaps += (h->seq > c->expect_seq)
                             ? h->seq - c->expect_seq : 1;
                c->expect_seq = h->seq + 1;

                double lat_us =
                    ((double)wall_ns() - (double)h->t_ns) / 1e3;
                lat_add(&c->lat, lat_us);
                lat_add(&win_lat, lat_us);

                c->payload_err += sc_verify(buf + sizeof(*h), payload_b,
                                            h->flags, h->seq, full_verify);
                c->frames++;
                c->bytes += wc[i].byte_len;
                tot_frames++;
                tot_bytes += wc[i].byte_len;
                win_frames++;
                win_bytes += wc[i].byte_len;
                if (!t_start) t_start = now_ns();
            }
            /* repost */
            struct ibv_sge sge = { .addr = (uintptr_t)buf,
                                   .length = frame_b, .lkey = mr->lkey };
            struct ibv_recv_wr wr = { .wr_id = wc[i].wr_id,
                                      .sg_list = &sge, .num_sge = 1 };
            struct ibv_recv_wr *bad = NULL;
            if (c->id && c->id->qp) ibv_post_recv(c->id->qp, &wr, &bad);
        }

        if (n == 0) {
            struct timespec idle = { 0, 20000 };
            nanosleep(&idle, NULL);
        }

        uint64_t t = now_ns();
        if (t - last_t >= 1000000000ull) {
            double dt = (double)(t - last_t) / 1e9;
            uint64_t gaps = 0, perr = 0;
            for (uint32_t i = 0; i < nlinks; i++) {
                gaps += C[i].gaps;
                perr += C[i].payload_err;
            }
            double gbps = (double)win_bytes * 8.0 / dt / 1e9;
            double avg = win_lat.count ? win_lat.sum_us / win_lat.count : 0;
            printf("[srx] %7.2f Gb/s %8.0f fps links=%u "
                   "lat avg/p99 %.0f/%.0f us  gaps=%" PRIu64
                   " payload_err=%" PRIu64 "\n",
                   gbps, (double)win_frames / dt, connected,
                   avg, lat_pct(&win_lat, 99.0), gaps, perr);
            fflush(stdout);
            if (csv)
                fprintf(csv, "%.0f,%u,%.3f,%.0f,%" PRIu64 ",%" PRIu64
                        ",%.1f,%.1f,%.1f,%.1f\n",
                        (double)wall_ns() / 1e9, connected, gbps,
                        (double)win_frames / dt, gaps, perr, avg,
                        lat_pct(&win_lat, 50.0), lat_pct(&win_lat, 99.0),
                        win_lat.max_us);
            win_bytes = win_frames = 0;
            lat_init(&win_lat);
            last_t = t;
        }

        if (run_secs > 0 && t_start &&
            (double)(t - t_start) / 1e9 >= run_secs)
            g_stop = 1;
    }

    /* ---------------- final report ---------------- */
    struct lat_hist agg; lat_init(&agg);
    uint64_t gaps = 0, perr = 0;
    for (uint32_t i = 0; i < nlinks; i++) {
        lat_merge(&agg, &C[i].lat);
        gaps += C[i].gaps;
        perr += C[i].payload_err;
    }
    double dur = t_start ? (double)(now_ns() - t_start) / 1e9 : 0;
    double agg_gbps = dur > 0 ? (double)tot_bytes * 8.0 / dur / 1e9 : 0;

    printf("\n[srx] ===== SUMMARY =====\n");
    printf("[srx] links peak       : %u\n", peak_connected);
    printf("[srx] frames / bytes   : %" PRIu64 " / %.3f GB in %.1f s\n",
           tot_frames, (double)tot_bytes / 1e9, dur);
    printf("[srx] aggregate        : %.2f Gb/s\n", agg_gbps);
    printf("[srx] sequence gaps    : %" PRIu64 "   (loss above RC)\n", gaps);
    printf("[srx] payload errors   : %" PRIu64 "   (corrupt 8 B lanes)\n",
           perr);
    if (agg.count)
        printf("[srx] latency us       : min %.0f  avg %.0f  p50 %.0f  "
               "p99 %.0f  max %.0f\n",
               agg.min_us, agg.sum_us / agg.count,
               lat_pct(&agg, 50.0), lat_pct(&agg, 99.0), agg.max_us);
    printf("[srx] verdict          : %s\n",
           (gaps == 0 && perr == 0) ? "PASS (zero loss, zero corruption)"
                                    : "FAIL");

    if (json_path) {
        FILE *j = fopen(json_path, "w");
        if (j) {
            fprintf(j, "{\n  \"links_peak\": %u,\n  \"duration_s\": %.2f,\n"
                       "  \"frames\": %" PRIu64 ",\n  \"bytes\": %" PRIu64
                       ",\n  \"aggregate_gbps\": %.3f,\n"
                       "  \"gaps\": %" PRIu64 ",\n  \"payload_errors\": %"
                       PRIu64 ",\n",
                    peak_connected, dur, tot_frames, tot_bytes, agg_gbps,
                    gaps, perr);
            fprintf(j, "  \"latency_us\": {\"min\": %.1f, \"avg\": %.1f, "
                       "\"p50\": %.1f, \"p99\": %.1f, \"max\": %.1f},\n",
                    agg.count ? agg.min_us : 0,
                    agg.count ? agg.sum_us / agg.count : 0,
                    lat_pct(&agg, 50.0), lat_pct(&agg, 99.0), agg.max_us);
            fprintf(j, "  \"pass\": %s,\n  \"links\": [\n",
                    (gaps == 0 && perr == 0) ? "true" : "false");
            int first = 1;
            for (uint32_t i = 0; i < nlinks; i++) {
                if (!C[i].frames) continue;
                fprintf(j, "%s    {\"link\": %u, \"frames\": %" PRIu64
                        ", \"gaps\": %" PRIu64 ", \"payload_errors\": %"
                        PRIu64 ", \"lat_p99_us\": %.1f}",
                        first ? "" : ",\n", C[i].link_id, C[i].frames,
                        C[i].gaps, C[i].payload_err,
                        lat_pct(&C[i].lat, 99.0));
                first = 0;
            }
            fprintf(j, "\n  ]\n}\n");
            fclose(j);
            printf("[srx] JSON summary     : %s\n", json_path);
        }
    }
    if (csv) fclose(csv);

    for (uint32_t i = 0; i < nlinks; i++)
        if (C[i].id) rdma_destroy_id(C[i].id);
    if (mr) ibv_dereg_mr(mr);
    if (cq) ibv_destroy_cq(cq);
    if (pd) ibv_dealloc_pd(pd);
    rdma_destroy_id(listen_id);
    rdma_destroy_event_channel(ec);
    free(pool);
    free(C);
    return (gaps == 0 && perr == 0) ? 0 : 2;
}
