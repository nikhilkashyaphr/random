/* blast.c - sendmmsg UDP traffic generator for throughput testing.
 * usage: blast <npkts> [port] [payload] */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <stdint.h>

#define B 64

int main(int argc, char **argv)
{
    long npkts   = argc > 1 ? atol(argv[1]) : 1000000;
    int  port    = argc > 2 ? atoi(argv[2]) : 50000;
    int  payload = argc > 3 ? atoi(argv[3]) : 1472;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET,
                              .sin_port = htons((uint16_t)port) };
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    int sz = 64 << 20;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));

    static uint8_t bufs[B][65536];
    struct iovec iov[B];
    struct mmsghdr msgs[B];
    memset(msgs, 0, sizeof(msgs));
    for (int i = 0; i < B; i++) {
        memset(bufs[i], 0xAB, (size_t)payload);
        iov[i].iov_base = bufs[i];
        iov[i].iov_len  = (size_t)payload;
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long sent = 0;
    uint64_t seq = 0;
    while (sent < npkts) {
        int n = (int)(npkts - sent < B ? npkts - sent : B);
        for (int i = 0; i < n; i++)
            memcpy(bufs[i], &seq, sizeof(seq)), seq++;
        int r = sendmmsg(fd, msgs, (unsigned)n, 0);
        if (r < 0) { perror("sendmmsg"); return 1; }
        sent += r;
        seq -= (uint64_t)(n - r);   /* resend unsent seqs next round */
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (double)(t1.tv_sec - t0.tv_sec) +
                (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("blast: %ld pkts in %.3fs = %.0f pps, %.3f Gbps payload\n",
           sent, dt, sent / dt, sent * (double)payload * 8 / dt / 1e9);
    return 0;
}
