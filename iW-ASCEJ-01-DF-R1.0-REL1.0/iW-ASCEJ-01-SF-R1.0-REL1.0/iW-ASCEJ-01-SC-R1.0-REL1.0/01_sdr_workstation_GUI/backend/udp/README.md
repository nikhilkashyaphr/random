# iwfg_c2h — 100G UDP payload → FIFO streamer

Continuously receives UDP datagrams from a Mellanox (ConnectX) 100 GbE
interface, strips all protocol headers, and writes the raw 1472-byte
payloads back-to-back into `/tmp/iwfg_c2h.fifo` as a continuous sample
stream for GNU Radio.

```
NIC ──► kernel UDP ──► [RX thread] ──► lock-free SPSC ring ──► [writer] ──► FIFO
                        recvmmsg()        368 MiB, hugepage-      write()
                        batch = 128       backed, mlocked         ≤4 MiB chunks
                                   [stats thread] 1 Hz
```

## Build

```sh
make                 # release: -O3 -march=native -flto, warning-free
make debug           # ThreadSanitizer build
make asan            # AddressSanitizer + UBSan build
```

Requires only glibc and a C11 compiler; no external dependencies.
For a binary that must run on a *different* CPU than the build host,
build with `make OPT="-O3 -flto"` (drops `-march=native`).

## Run

```sh
./iwfg_c2h                       # defaults: port 50000, /tmp/iwfg_c2h.fifo
./iwfg_c2h -p 50000 -b 192.0.2.10 \
           --rx-cpu 2 --wr-cpu 3 \
           --hugepages --busy-poll 50 --spin
```

| Option | Default | Meaning |
|---|---|---|
| `-p, --port` | 50000 | UDP listen port |
| `-b, --bind` | any | local IPv4 to bind (use the NIC's address) |
| `-f, --fifo` | /tmp/iwfg_c2h.fifo | output FIFO (created if absent) |
| `-s, --payload` | 1472 | expected payload; other sizes are dropped + counted |
| `-r, --ring` | 262144 | ring slots (pow2), 262144 × 1472 B = 368 MiB |
| `-B, --batch` | 128 | recvmmsg batch |
| `--rcvbuf` | 256 MiB | SO_RCVBUF (uses SO_RCVBUFFORCE when root) |
| `--pipe-sz` | max | FIFO pipe buffer via F_SETPIPE_SZ |
| `--busy-poll` | off | SO_BUSY_POLL µs (burns the RX core for latency) |
| `--rx-cpu / --wr-cpu` | unpinned | CPU affinity |
| `--stats` | 1 | report interval seconds, 0 = silent |
| `--spin` | off | writer busy-spins when idle (lowest latency) |
| `--hugepages` | off | back the ring with explicit 2 MiB hugepages |
| `--raw IFNAME` | off | AF_PACKET capture mode (see below) |

### `--raw` capture mode — for non-compliant senders

Some hardware packet generators emit frames a normal UDP socket can
never receive: a zero/foreign **destination MAC** (the kernel tags the
frame `PACKET_OTHERHOST` and `ip_rcv()` discards it before UDP) or an
**invalid IP header checksum** (also discarded by `ip_rcv()`). Capture
tools like Wireshark still show such traffic because they tap the
interface *before* the IP stack — which is exactly why "Wireshark sees
it but the app gets nothing" happens.

`--raw <ifname>` takes the same pre-stack tap: an `AF_PACKET` socket
bound to the interface with an in-kernel cBPF filter (IPv4, IHL=5, UDP,
unfragmented, matching dst port) so only the wanted flow is copied to
userspace. The 42-byte Ethernet+IPv4+UDP header is stripped for free by
splitting each `recvmmsg` message across two iovecs — header into
scratch, payload directly into its ring slot — so it is still exactly
one copy. MAC and IP-checksum validity are irrelevant on this path.

```sh
sudo ./iwfg_c2h -p 16384 --raw enp1s0f0np0
```

Needs root or `CAP_NET_RAW`. The socket joins promiscuous mode on the
interface itself. `-b` is ignored in this mode (filtering is by port).
Validated against replayed frames with dst MAC `00:00:00:00:00:00` and
a deliberately wrong IP checksum: 100 % delivery, byte-exact, while a
normal UDP socket received zero from the same stream.

The app runs until SIGINT/SIGTERM, prints per-second stats
(`pps / Gbps / ring fill / drops`), and a final counter summary.

Start order doesn't matter: the writer waits for a FIFO reader, and if
the reader disconnects it buffers into the ring, reopens, and resumes
**on an exact 1472-byte packet boundary** (a torn packet is discarded so
the new reader never starts mid-sample).

## Threading architecture — why 3 threads, not 8

* **RX thread** — sole owner of the socket. `recvmmsg()` in batches of
  128 with each message's iovec pointing *directly into a ring slot*, so
  the single unavoidable kernel→user copy lands the payload in its final
  resting place. Header stripping is free: the kernel UDP stack already
  removed Ethernet/IP/UDP before the copy.
* **Writer thread** — sole owner of the FIFO. Drains the largest
  physically contiguous run of slots per `write()` (capped at 4 MiB so
  slot release stays frequent), non-blocking + `poll()` so a stalled
  reader can never wedge it.
* **Stats thread** — 1 Hz reporting from relaxed atomic counters, each
  on its own cache line (no false sharing with the datapath).

More datapath threads were considered and rejected on measurement-backed
grounds: the FIFO is a strictly ordered byte stream, so exactly one
thread may write it, and fanning RX across `SO_REUSEPORT` sockets would
reorder samples. Any thread inserted between RX and writer adds a copy
or a queue hop for zero gain. Two pinned datapath threads on adjacent
cores of the NIC's NUMA node beat a 6–8-thread pipeline for this
workload.

### The ring

Classic cached-index SPSC: `head`/`tail` are monotonically increasing
64-bit counters (slot = counter & mask), producer publishes with a
release store, consumer releases with a release store, and each side
keeps a cached copy of the other's index so the hot path touches a
shared cache line only when its cached view says full/empty. No mutex,
no CAS, no syscalls on the queue itself. The buffer is mmap'd, pre-
faulted, `mlock`ed, and optionally hugepage-backed (`MADV_HUGEPAGE`
fallback) to eliminate TLB pressure and page faults on the hot path.

### Overload behaviour (all loss is counted, never silent)

* Ring full (reader stalled): RX drains the socket into scratch and
  counts every discarded datagram in `ring drops` — validated to be
  exact (19 473 + 527 delivered = 20 000 offered).
* Socket queue overflow: per-datagram kernel counter via `SO_RXQ_OVFL`
  ancillary data → `sock drops`.
* Wrong-size datagrams (`MSG_TRUNC` reveals true length): dropped and
  counted in `bad size`; the good stream is compacted in place so
  framing never breaks.

## Performance tuning

### CPU affinity

Find the NIC's NUMA node: `cat /sys/class/net/<if>/device/numa_node`.
Pick three cores on that node, e.g. 2, 3, 4:

```sh
# steer the NIC RX queue interrupt for your flow to core 2
echo 4 > /proc/irq/<irq>/smp_affinity          # bitmask for CPU 2
./iwfg_c2h --rx-cpu 2 --wr-cpu 3
taskset -c 4 <gnuradio flowgraph>              # FIFO reader nearby
```

Best results with the softirq core, RX thread, writer, and FIFO reader
all on the same NUMA node, each on its own physical core. Isolate them
from the scheduler with `isolcpus=`/`nohz_full=` kernel args for jitter-
free long runs.

### Kernel sysctls

```sh
sysctl -w net.core.rmem_max=536870912
sysctl -w net.core.rmem_default=16777216
sysctl -w net.core.netdev_max_backlog=250000
sysctl -w net.core.netdev_budget=600
sysctl -w net.core.netdev_budget_usecs=8000
sysctl -w fs.pipe-max-size=67108864        # let --pipe-sz grow to 64 MiB
sysctl -w vm.swappiness=1
# hugepages for --hugepages (368 MiB ring → 192 pages)
echo 256 > /proc/sys/vm/nr_hugepages
```

### Mellanox / ConnectX NIC tuning

```sh
ethtool -G <if> rx 8192                        # max RX descriptors
ethtool -C <if> adaptive-rx off rx-usecs 8 rx-frames 64   # coalescing
ethtool -K <if> gro off lro off                # datagrams, not streams
ethtool -N <if> flow-type udp4 dst-port 50000 action <q>  # steer flow
mlnx_tune -p HIGH_THROUGHPUT                   # if MLNX-OFED installed
```

Pin the chosen queue's IRQ to the core adjacent to `--rx-cpu`. Disable
`irqbalance` for that IRQ. Set the CPU governor to `performance`.

### Honest throughput expectations

The wire-format ceiling at 1472-byte payloads is ~8.13 Mpps for
100 Gbps. Three distinct ceilings apply to this pipeline, in order:

1. **The FIFO itself** — a kernel pipe moves roughly 3–8 GB/s
   (24–64 Gbps) per core depending on CPU, because the reader side
   copies too. This is a property of named pipes, not of this program.
2. **Kernel UDP receive** — a single tuned `recvmmsg()` core sustains
   roughly 1–3 Mpps (12–35 Gbps at this size).
3. **This application's own overhead** — near zero: one copy
   (kernel→ring, done by `recvmmsg`), one copy (ring→pipe, done by
   `write`), no locks, no per-packet syscalls.

On the validation host (one shared vCPU for sender + app + reader) the
pipeline moved 5,000,000 packets / 7.36 GB byte-exact with zero loss;
the traffic generator, not the app, was the limiter. For true sustained
100 Gbps you must replace the *transport on both ends*: an `AF_XDP`
zero-copy RX backend and shared-memory output (e.g. a gr-hpudp-style
block reading the ring directly) instead of a pipe. The SPSC ring and
writer in this codebase are designed so an AF_XDP RX thread can be
dropped in without touching the consumer side.

## Validation performed

* 5 M-packet sustained run: byte-exact (7,360,000,000 B), 0 drops,
  clean SIGINT shutdown.
* Sequenced-payload verification: 0 corruption, 0 reordering, 0 partial
  packets across all runs.
* Fault injection: 1170 interleaved wrong-size datagrams — all counted,
  stream framing intact.
* Reader kill/reattach: resumes on exact packet boundary, `reopen`
  counter accurate.
* Ring overflow under stalled reader: drop accounting exact to the
  packet.
* ThreadSanitizer: 0 warnings. ASan+UBSan+LeakSanitizer: 0 issues.
* `-Wall -Wextra -Wshadow -Wconversion`: 0 warnings.

## Files

```
include/iwfg.h      common config/stats definitions
include/ring.h      lock-free SPSC ring (hot path, header-only)
include/rx.h        RX thread interface
include/writer.h    writer thread interface
include/stats.h     stats thread interface
src/main.c          CLI, signals, thread orchestration
src/rx.c            socket setup + recvmmsg receive loop
src/writer.c        FIFO lifecycle + batched drain
src/stats.c         periodic reporting
src/ring.c          ring allocation (hugepages, mlock, prefault)
src/util.c          logging, pinning, clocks
test/verify2.py     sequenced end-to-end correctness harness
test/faults.py      fault-injection tests (bad sizes, reconnect)
test/blast.c        sendmmsg traffic generator
```
