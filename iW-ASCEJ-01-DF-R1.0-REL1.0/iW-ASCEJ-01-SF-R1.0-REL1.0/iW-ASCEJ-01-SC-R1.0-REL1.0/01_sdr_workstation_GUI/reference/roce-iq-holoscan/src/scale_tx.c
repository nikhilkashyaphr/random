/*
 * scale_tx.c -- N-link RoCEv2 load generator (1..128 RC connections).
 *
 * Each "link" is its own rdma_cm connection with its own RC queue pair --
 * exactly what 100 independent RoCEv2 endpoints look like to the NIC,
 * minus 99 cables. All QPs share one completion queue (the realistic
 * aggregation point) and a global token-bucket pacer.
 *
 * Every frame carries a 64 B header (seq, send timestamp, link id in
 * 'flags') and a payload whose every 8-byte lane is a pure function of
 * (link, seq, lane) -- scale_rx can verify integrity without checksums.
 *
 * Usage:
 *   sudo ip netns exec ns_tx ./scale_tx -s 192.168.100.2 -n 100 -g 40 -t 60
 *     -n links      number of RC connections        (default 8, max 128)
 *     -g gbps       AGGREGATE pacing, 0 = unlimited (default 0)
 *     -b bytes      frame size incl. 64 B header    (default 262144)
 *     -o n          outstanding sends per link      (default 4)
 *     -t secs       run time, 0 = until Ctrl-C      (default 0)
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
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

static void wait_event(struct rdma_event_channel *ec,
                       enum rdma_cm_event_type want)
{
    struct rdma_cm_event *ev = NULL;
    if (rdma_get_cm_event(ec, &ev)) DIE("rdma_get_cm_event");
    if (ev->event != want) {
        fprintf(stderr, "FATAL: CM event '%s' (status %d), wanted '%s'\n",
                rdma_event_str(ev->event), ev->status, rdma_event_str(want));
        exit(1);
    }
    rdma_ack_cm_event(ev);
}

struct link {
    struct rdma_cm_id *id;
    uint64_t seq;          /* next sequence to send       */
    uint32_t inflight;
    uint8_t *slots;        /* o x frame_bytes             */
};

int main(int argc, char **argv)
{
    const char *server = NULL;
    int port = DEFAULT_TCP_PORT;
    uint32_t nlinks = 8, frame_b = 262144, outst = 4;
    double gbps = 0.0, run_secs = 0.0;

    int opt;
    while ((opt = getopt(argc, argv, "s:p:n:g:b:o:t:h")) != -1) {
        switch (opt) {
        case 's': server  = optarg;                  break;
        case 'p': port    = atoi(optarg);            break;
        case 'n': nlinks  = (uint32_t)atoi(optarg);  break;
        case 'g': gbps    = atof(optarg);            break;
        case 'b': frame_b = (uint32_t)atoi(optarg);  break;
        case 'o': outst   = (uint32_t)atoi(optarg);  break;
        case 't': run_secs = atof(optarg);           break;
        default:
            fprintf(stderr, "see header comment for usage\n");
            return 1;
        }
    }
    if (!server || nlinks < 1 || nlinks > SCALE_MAX_LINKS ||
        frame_b < 128 || (frame_b & 7) || outst < 1 || outst > 32) {
        fprintf(stderr, "bad arguments (need -s; 1<=n<=%u; b>=128 and "
                "multiple of 8; 1<=o<=32)\n", SCALE_MAX_LINKS);
        return 1;
    }
    const uint32_t payload_b = frame_b - (uint32_t)sizeof(struct frame_hdr);

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct sockaddr_in dst; memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, server, &dst.sin_addr) != 1) DIE("bad -s ip");

    struct rdma_event_channel *ec = rdma_create_event_channel();
    if (!ec) DIE("rdma_create_event_channel");

    /* one big slot pool, one MR */
    size_t pool_b = (size_t)nlinks * outst * frame_b;
    uint8_t *pool = NULL;
    if (posix_memalign((void **)&pool, 4096, pool_b)) DIE("alloc pool");
    printf("[stx] %u links x %u outstanding x %u B  (pool %.1f MiB)\n",
           nlinks, outst, frame_b, pool_b / 1048576.0);

    struct link *L = calloc(nlinks, sizeof(*L));
    if (!L) DIE("calloc links");

    struct ibv_pd *pd = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_mr *mr = NULL;
    struct ibv_context *verbs = NULL;

    /* CQ depth covers every possible in-flight WR */
    uint32_t cq_depth = nlinks * outst + 16;

    for (uint32_t li = 0; li < nlinks && !g_stop; li++) {
        struct link *l = &L[li];
        l->slots = pool + (size_t)li * outst * frame_b;

        if (rdma_create_id(ec, &l->id, NULL, RDMA_PS_TCP))
            DIE("rdma_create_id");
        if (rdma_resolve_addr(l->id, NULL, (struct sockaddr *)&dst, 2000))
            DIE("rdma_resolve_addr");
        wait_event(ec, RDMA_CM_EVENT_ADDR_RESOLVED);
        if (rdma_resolve_route(l->id, 2000)) DIE("rdma_resolve_route");
        wait_event(ec, RDMA_CM_EVENT_ROUTE_RESOLVED);

        if (!pd) {                      /* first link: shared resources */
            verbs = l->id->verbs;
            pd = ibv_alloc_pd(verbs);
            if (!pd) DIE("ibv_alloc_pd");
            cq = ibv_create_cq(verbs, (int)cq_depth, NULL, NULL, 0);
            if (!cq) DIE("ibv_create_cq");
            mr = ibv_reg_mr(pd, pool, pool_b, 0);
            if (!mr) DIE("ibv_reg_mr (raise memlock: ulimit -l)");
        } else if (l->id->verbs != verbs) {
            fprintf(stderr, "FATAL: links resolved to different devices\n");
            return 1;
        }

        struct ibv_qp_init_attr qpa = {
            .send_cq = cq, .recv_cq = cq, .qp_type = IBV_QPT_RC,
            .cap = { .max_send_wr = outst, .max_recv_wr = 1,
                     .max_send_sge = 1,    .max_recv_sge = 1 },
        };
        if (rdma_create_qp(l->id, pd, &qpa)) DIE("rdma_create_qp");

        struct rdma_conn_param cp = {
            .responder_resources = 1, .initiator_depth = 1,
            .rnr_retry_count = 7,
        };
        if (rdma_connect(l->id, &cp)) DIE("rdma_connect");
        wait_event(ec, RDMA_CM_EVENT_ESTABLISHED);
        if ((li + 1) % 10 == 0 || li + 1 == nlinks)
            printf("[stx] %u/%u links connected\n", li + 1, nlinks);
    }

    if (gbps > 0)
        printf("[stx] pacing aggregate to %.2f Gb/s\n", gbps);

    /* ---- streaming ----------------------------------------------------- */
    uint64_t t0 = now_ns(), last_t = t0;
    uint64_t sent_bytes = 0, completed = 0, last_completed = 0;
    uint32_t rr = 0;

    while (!g_stop &&
           (run_secs <= 0.0 || (double)(now_ns() - t0) / 1e9 < run_secs)) {

        uint64_t budget = (gbps > 0.0)
            ? (uint64_t)(gbps * 1e9 / 8.0 * ((double)(now_ns() - t0) / 1e9))
            : UINT64_MAX;

        int posted = 0;
        for (uint32_t k = 0; k < nlinks; k++) {
            struct link *l = &L[(rr + k) % nlinks];
            if (l->inflight >= outst) continue;
            if (sent_bytes + frame_b > budget) break;

            uint32_t slot = (uint32_t)(l->seq % outst);
            uint8_t *buf = l->slots + (size_t)slot * frame_b;
            struct frame_hdr *h = (struct frame_hdr *)buf;
            uint32_t link_id = (uint32_t)(l - L);

            h->magic = IQ_FRAME_MAGIC;
            h->seq   = l->seq;
            h->t_ns  = wall_ns();              /* same-host one-way latency */
            h->n_samples = payload_b / 4;
            h->flags = link_id;
            h->sample_rate_hz = 0;
            h->center_freq_hz = 0;
            sc_fill(buf + sizeof(*h), payload_b, link_id, l->seq);

            struct ibv_sge sge = { .addr = (uintptr_t)buf,
                                   .length = frame_b, .lkey = mr->lkey };
            struct ibv_send_wr wr; memset(&wr, 0, sizeof wr);
            struct ibv_send_wr *bad = NULL;
            wr.wr_id = ((uint64_t)link_id << 32) | slot;
            wr.sg_list = &sge;
            wr.num_sge = 1;
            wr.opcode = IBV_WR_SEND;
            wr.send_flags = IBV_SEND_SIGNALED;
            if (ibv_post_send(l->id->qp, &wr, &bad)) DIE("ibv_post_send");
            l->seq++;
            l->inflight++;
            sent_bytes += frame_b;
            posted = 1;
        }
        rr++;

        struct ibv_wc wc[32];
        int n = ibv_poll_cq(cq, 32, wc);
        if (n < 0) DIE("ibv_poll_cq");
        for (int i = 0; i < n; i++) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                fprintf(stderr, "[stx] send error on link %u: %s\n",
                        (uint32_t)(wc[i].wr_id >> 32),
                        ibv_wc_status_str(wc[i].status));
                g_stop = 1;
                break;
            }
            L[wc[i].wr_id >> 32].inflight--;
            completed++;
        }

        if (!posted && n == 0) {
            struct timespec ts = { 0, 20000 };
            nanosleep(&ts, NULL);
        }

        uint64_t t = now_ns();
        if (t - last_t >= 1000000000ull) {
            double dt = (double)(t - last_t) / 1e9;
            printf("[stx] %7.2f Gb/s  %8.0f fps  links=%u  sent=%" PRIu64 "\n",
                   (double)(completed - last_completed) * frame_b * 8.0
                       / dt / 1e9,
                   (double)(completed - last_completed) / dt,
                   nlinks, completed);
            fflush(stdout);
            last_t = t;
            last_completed = completed;
        }
    }

    /* drain */
    uint64_t deadline = now_ns() + 1000000000ull;
    uint64_t posted_total = 0;
    for (uint32_t i = 0; i < nlinks; i++) posted_total += L[i].seq;
    while (completed < posted_total && now_ns() < deadline) {
        struct ibv_wc wc[32];
        int n = ibv_poll_cq(cq, 32, wc);
        for (int i = 0; i < n; i++)
            if (wc[i].status == IBV_WC_SUCCESS) completed++;
    }
    printf("[stx] done: %" PRIu64 " frames (%.2f GB) over %u links\n",
           completed, (double)completed * frame_b / 1e9, nlinks);

    for (uint32_t i = 0; i < nlinks; i++) {
        if (!L[i].id) continue;
        rdma_disconnect(L[i].id);
        rdma_destroy_qp(L[i].id);
        rdma_destroy_id(L[i].id);
    }
    if (mr) ibv_dereg_mr(mr);
    if (cq) ibv_destroy_cq(cq);
    if (pd) ibv_dealloc_pd(pd);
    rdma_destroy_event_channel(ec);
    free(pool);
    free(L);
    return 0;
}
