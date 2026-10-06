/*
 * rx.c - UDP receive thread.
 *
 * A single RX thread owns the socket.  This is deliberate: the FIFO is a
 * strictly ordered byte stream, and fanning RX out across SO_REUSEPORT
 * sockets would reorder payloads.  One core running recvmmsg() with a
 * large SO_RCVBUF sustains multi-Mpps; ordering is preserved end to end.
 *
 * Zero-copy discipline: the iovec of every batched message points
 * directly at its ring slot, so the only copy on the hot path is the
 * unavoidable kernel->user copy performed inside recvmmsg().  Headers
 * (Ethernet/IP/UDP) never reach userspace at all - the kernel UDP stack
 * strips them, which is precisely the "remove all protocol headers"
 * requirement done in the cheapest possible place.
 */
#include "iwfg.h"
#include "ring.h"
#include "rx.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/filter.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef PACKET_IGNORE_OUTGOING
#define PACKET_IGNORE_OUTGOING 23
#endif

/* Link-layer header we strip in raw mode: Ethernet + IPv4(IHL=5) + UDP. */
#define RAW_HDR_LEN (14u + 20u + 8u)

/*
 * Raw AF_PACKET capture mode (--raw <ifname>).
 *
 * Some hardware packet generators emit frames a normal UDP socket can
 * never receive: zero/foreign destination MACs (kernel tags the frame
 * PACKET_OTHERHOST and ip_rcv() discards it) or invalid IP header
 * checksums (ip_rcv() discards those too).  Capture tools still see
 * such frames because packet sockets tap the interface *before* the IP
 * stack.  This backend takes the same tap, applies an in-kernel cBPF
 * filter (IPv4, IHL=5, UDP, not fragmented, matching dst port) so only
 * relevant frames cross into userspace, and strips the 42-byte header
 * for free by splitting each message across two iovecs: iov[0] lands
 * the header in scratch, iov[1] lands the payload directly in its ring
 * slot.  Still exactly one copy, done by the kernel inside recvmmsg().
 */
static int open_raw_socket(const struct iwfg_cfg *cfg)
{
    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IP));
    if (fd < 0) {
        if (errno == EPERM || errno == EACCES)
            iwfg_fatal("raw mode needs root or CAP_NET_RAW "
                       "(try: sudo ./iwfg_c2h ...)");
        iwfg_fatal("socket(AF_PACKET): %s", strerror(errno));
    }

    unsigned ifindex = if_nametoindex(cfg->raw_ifname);
    if (ifindex == 0)
        iwfg_fatal("raw: no such interface '%s'", cfg->raw_ifname);

    /* Filter in the kernel so only our flow is copied to userspace:
     *   ethertype IPv4, version/IHL == 0x45, proto UDP,
     *   fragment offset 0, UDP dst port == cfg->port.            */
    struct sock_filter code[] = {
        /* idx                                    (drop is idx 11)     */
        /*  0 */ { 0x28, 0, 0, 12 },           /* ldh  ethertype       */
        /*  1 */ { 0x15, 0, 9, ETH_P_IP },     /* jne  -> drop         */
        /*  2 */ { 0x30, 0, 0, 14 },           /* ldb  ver/ihl         */
        /*  3 */ { 0x15, 0, 7, 0x45 },         /* jne  -> drop         */
        /*  4 */ { 0x30, 0, 0, 23 },           /* ldb  ip proto        */
        /*  5 */ { 0x15, 0, 5, IPPROTO_UDP },  /* jne  -> drop         */
        /*  6 */ { 0x28, 0, 0, 20 },           /* ldh  frag field      */
        /*  7 */ { 0x45, 3, 0, 0x1fff },       /* jset -> drop (frag)  */
        /*  8 */ { 0x28, 0, 0, 36 },           /* ldh  udp dst port    */
        /*  9 */ { 0x15, 0, 1, cfg->port },    /* jne  -> drop         */
        /* 10 */ { 0x06, 0, 0, 0xffffffff },   /* ret  accept          */
        /* 11 */ { 0x06, 0, 0, 0 },            /* ret  drop            */
    };
    struct sock_fprog prog = {
        .len = (unsigned short)(sizeof(code) / sizeof(code[0])),
        .filter = code,
    };
    if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER,
                   &prog, sizeof(prog)) != 0)
        iwfg_fatal("raw: SO_ATTACH_FILTER: %s", strerror(errno));

    /* Don't tap our own transmitted frames (kernel >= 4.20). */
    int one = 1;
    (void)setsockopt(fd, SOL_PACKET, PACKET_IGNORE_OUTGOING,
                     &one, sizeof(one));

    /* Accept frames regardless of destination MAC. */
    struct packet_mreq mr;
    memset(&mr, 0, sizeof(mr));
    mr.mr_ifindex = (int)ifindex;
    mr.mr_type    = PACKET_MR_PROMISC;
    if (setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP,
                   &mr, sizeof(mr)) != 0)
        iwfg_log("raw: promisc membership failed: %s (set "
                 "'ip link set %s promisc on' manually)",
                 strerror(errno), cfg->raw_ifname);

    int sz = (int)cfg->rcvbuf;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &sz, sizeof(sz)) != 0)
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
    int got = 0; socklen_t gl = sizeof(got);
    (void)getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &got, &gl);
    iwfg_log("rx: SO_RCVBUF requested %u, effective %d "
             "(raise net.core.rmem_max if smaller)", cfg->rcvbuf, got);

    if (setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof(one)) != 0)
        iwfg_log("rx: SO_RXQ_OVFL unavailable: %s", strerror(errno));

    if (cfg->busy_poll_us) {
        int us = (int)cfg->busy_poll_us;
        if (setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &us, sizeof(us)) != 0)
            iwfg_log("rx: SO_BUSY_POLL failed: %s", strerror(errno));
    }

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family   = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_IP);
    sll.sll_ifindex  = (int)ifindex;
    if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) != 0)
        iwfg_fatal("raw: bind %s: %s", cfg->raw_ifname, strerror(errno));

    struct timeval tv = { .tv_sec = 0, .tv_usec = 100 * 1000 };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    iwfg_log("rx: raw capture on %s (ifindex %u), udp dst port %u, "
             "header strip %u bytes", cfg->raw_ifname, ifindex,
             cfg->port, RAW_HDR_LEN);
    return fd;
}

static int open_socket(const struct iwfg_cfg *cfg)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        iwfg_fatal("socket: %s", strerror(errno));

    int one = 1;

    /* Large receive buffer to absorb bursts while the ring drains.
     * SO_RCVBUFFORCE bypasses rmem_max when running with CAP_NET_ADMIN;
     * fall back to SO_RCVBUF (clamped by the kernel) otherwise. */
    int sz = (int)cfg->rcvbuf;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &sz, sizeof(sz)) != 0)
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
    int got = 0; socklen_t gl = sizeof(got);
    (void)getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &got, &gl);
    iwfg_log("rx: SO_RCVBUF requested %u, effective %d "
             "(raise net.core.rmem_max if smaller)", cfg->rcvbuf, got);

    /* Per-socket drop counter delivered as ancillary data. */
    if (setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof(one)) != 0)
        iwfg_log("rx: SO_RXQ_OVFL unavailable: %s", strerror(errno));

    /* Optional kernel busy polling - trades one core for latency. */
    if (cfg->busy_poll_us) {
        int us = (int)cfg->busy_poll_us;
        if (setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &us, sizeof(us)) != 0)
            iwfg_log("rx: SO_BUSY_POLL failed: %s", strerror(errno));
    }

    /* Timestamp-free, connectionless; bind and go. */
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(cfg->port);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (cfg->bind_addr &&
        inet_pton(AF_INET, cfg->bind_addr, &sa.sin_addr) != 1)
        iwfg_fatal("invalid bind address '%s'", cfg->bind_addr);

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0)
        iwfg_fatal("bind %s:%u: %s",
                   cfg->bind_addr ? cfg->bind_addr : "0.0.0.0",
                   cfg->port, strerror(errno));

    /* Bounded blocking so the loop can notice shutdown requests. */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100 * 1000 };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return fd;
}

struct batch {
    struct mmsghdr *msgs;
    struct iovec   *iovs;     /* 2 per message: [hdr][payload]       */
    uint8_t        *ctrl;     /* cmsg space for SO_RXQ_OVFL          */
    uint8_t        *hdrs;     /* raw mode: 42-byte header sinks      */
    uint32_t        n;
    size_t          ctrl_one; /* CMSG_SPACE(sizeof(uint32_t))        */
    bool            raw;
};

static void batch_init(struct batch *b, uint32_t n, bool raw)
{
    b->n        = n;
    b->raw      = raw;
    b->ctrl_one = CMSG_SPACE(sizeof(uint32_t));
    b->msgs = calloc(n, sizeof(*b->msgs));
    b->iovs = calloc((size_t)n * 2, sizeof(*b->iovs));
    b->ctrl = calloc(n, b->ctrl_one);
    b->hdrs = raw ? calloc(n, RAW_HDR_LEN) : NULL;
    if (!b->msgs || !b->iovs || !b->ctrl || (raw && !b->hdrs))
        iwfg_fatal("rx: batch allocation failed");
}

static void batch_free(struct batch *b)
{
    free(b->msgs); free(b->iovs); free(b->ctrl); free(b->hdrs);
}

/* Point message i at payload buffer p and reset per-call fields that
 * the kernel overwrites (msg_len, controllen, flags).  In raw mode a
 * leading iovec sinks the 42-byte link/IP/UDP header so the payload
 * still lands contiguously in its final destination. */
static inline void batch_arm(struct batch *b, uint32_t i, uint8_t *p,
                             size_t len)
{
    struct iovec *iv = &b->iovs[(size_t)i * 2];
    size_t niov;
    if (b->raw) {
        iv[0].iov_base = b->hdrs + (size_t)i * RAW_HDR_LEN;
        iv[0].iov_len  = RAW_HDR_LEN;
        iv[1].iov_base = p;
        iv[1].iov_len  = len;
        niov = 2;
    } else {
        iv[0].iov_base = p;
        iv[0].iov_len  = len;
        niov = 1;
    }
    struct msghdr *mh = &b->msgs[i].msg_hdr;
    mh->msg_name       = NULL;
    mh->msg_namelen    = 0;
    mh->msg_iov        = iv;
    mh->msg_iovlen     = niov;
    mh->msg_control    = b->ctrl + (size_t)i * b->ctrl_one;
    mh->msg_controllen = b->ctrl_one;
    mh->msg_flags      = 0;
    b->msgs[i].msg_len = 0;
}

/* Extract the cumulative SO_RXQ_OVFL counter, if present. */
static inline bool msg_ovfl(const struct msghdr *mh, uint32_t *out)
{
    for (struct cmsghdr *c = CMSG_FIRSTHDR((struct msghdr *)mh); c;
         c = CMSG_NXTHDR((struct msghdr *)mh, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_RXQ_OVFL) {
            memcpy(out, CMSG_DATA(c), sizeof(*out));
            return true;
        }
    }
    return false;
}

void *rx_thread(void *arg)
{
    struct rx_ctx *ctx = arg;
    const struct iwfg_cfg *cfg = ctx->cfg;
    struct ring *r = ctx->ring;

    iwfg_pin_thread(cfg->rx_cpu, "iwfg-rx");

    const bool raw = (cfg->raw_ifname != NULL);
    int fd = raw ? open_raw_socket(cfg) : open_socket(cfg);
    ctx->sock_fd = fd;

    struct batch b;
    batch_init(&b, cfg->batch, raw);

    /* Scratch area used to drain-and-drop when the ring is full, so the
     * socket queue never silently overflows while we are stalled and the
     * application can account for every lost datagram itself. */
    uint8_t *scratch = malloc((size_t)cfg->batch * cfg->payload);
    if (!scratch)
        iwfg_fatal("rx: scratch allocation failed");

    const uint32_t payload  = cfg->payload;
    /* recvmmsg reports the full message length; in raw mode that
     * includes the 42-byte header we sink into the first iovec. */
    const uint32_t want_len = payload + (raw ? RAW_HDR_LEN : 0);
    uint32_t ovfl_last = 0;
    bool     ovfl_seen = false;

    while (!g_stop) {
        uint64_t free_slots = ring_free_slots(r, cfg->batch);
        uint32_t want = cfg->batch;
        bool     drop_mode = false;

        if (free_slots == 0) {
            /* Ring full (FIFO reader stalled or absent): drain the
             * socket into scratch and count the loss explicitly. */
            drop_mode = true;
            for (uint32_t i = 0; i < want; i++)
                batch_arm(&b, i, scratch + (size_t)i * payload, payload);
        } else {
            if (free_slots < want)
                want = (uint32_t)free_slots;
            uint64_t tail = ring_tail(r);
            for (uint32_t i = 0; i < want; i++)
                batch_arm(&b, i, ring_slot_ptr(r, tail + i), payload);
        }

        int n = recvmmsg(fd, b.msgs, want, MSG_WAITFORONE | MSG_TRUNC,
                         NULL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == EINTR)
                continue;                       /* timeout / signal */
            if (g_stop)
                break;
            iwfg_fatal("recvmmsg: %s", strerror(errno));
        }
        if (n == 0)
            continue;

        /* Harvest the kernel drop counter (cumulative per socket). */
        uint32_t ovfl;
        if (msg_ovfl(&b.msgs[n - 1].msg_hdr, &ovfl)) {
            if (ovfl_seen) {
                uint32_t delta = ovfl - ovfl_last;   /* mod-2^32 safe */
                if (delta)
                    atomic_fetch_add_explicit(&g_stats.sock_drops, delta,
                                              memory_order_relaxed);
            }
            ovfl_last = ovfl;
            ovfl_seen = true;
        }

        if (drop_mode) {
            atomic_fetch_add_explicit(&g_stats.ring_drops, (uint64_t)n,
                                      memory_order_relaxed);
            continue;
        }

        /* Validate sizes; compact out malformed datagrams (rare path).
         * msg_len is the true datagram length thanks to MSG_TRUNC. */
        uint64_t tail = ring_tail(r);
        uint32_t good = 0;
        uint64_t bytes = 0;
        for (int i = 0; i < n; i++) {
            if (b.msgs[i].msg_len == want_len) {
                if ((uint32_t)i != good)
                    memcpy(ring_slot_ptr(r, tail + good),
                           ring_slot_ptr(r, tail + (uint32_t)i), payload);
                good++;
                bytes += payload;
            } else {
                atomic_fetch_add_explicit(&g_stats.bad_size, 1,
                                          memory_order_relaxed);
            }
        }

        if (good) {
            ring_publish(r, good);
            atomic_fetch_add_explicit(&g_stats.rx_pkts, good,
                                      memory_order_relaxed);
            atomic_fetch_add_explicit(&g_stats.rx_bytes, bytes,
                                      memory_order_relaxed);
        }
    }

    batch_free(&b);
    free(scratch);
    close(fd);
    iwfg_log("rx: thread exiting");
    return NULL;
}
