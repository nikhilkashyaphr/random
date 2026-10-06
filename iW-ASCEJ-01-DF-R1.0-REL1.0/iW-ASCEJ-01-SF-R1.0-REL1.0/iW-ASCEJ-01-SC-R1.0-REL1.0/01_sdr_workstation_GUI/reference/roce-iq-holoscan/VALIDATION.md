# VALIDATION.md — GPU Pipeline Depth & 100-Link RoCEv2 Certification

This document answers two engineering questions for this rig (Ubuntu 22.04,
ConnectX-5 100 GbE in PCIe Gen3 x8, RTX A4000):

1. How much of the pipeline can genuinely run on the GPU, and where is the
   hard floor for CPU involvement on this exact hardware?
2. How do we validate 100 simultaneous RoCEv2 links — throughput, integrity,
   loss, latency, GPU memory bandwidth, sustained stability — with the tools
   in this repository?

---

## Part 1 — How "pure GPU" can this pipeline get?

Split the system into a **data plane** (where sample bytes travel) and a
**control plane** (who tells the NIC what to do). They have very different
answers.

### Data plane: fully GPU-resident today

With the GPUDirect receiver (`rdma_rx_gpu`, see README section 11), sample
data takes this path:

```
ConnectX-5 ──PCIe DMA──> A4000 VRAM ring ──cuFFT/CuPy──> Holoviz (Vulkan)
```

The bytes never touch system RAM. The NIC's DMA engine writes payloads
directly into a `cudaMalloc`'d ring (enabled by the `nvidia-peermem` kernel
module, which teaches `ibv_reg_mr` to pin and map CUDA memory), the DSP
operates on them in place, and Holoviz renders from the same GPU. The only
per-frame host traffic is the 64-byte header, which the receive WR scatters
into host memory on purpose so the CPU control loop and the Python app can
read metadata without touching VRAM. On this machine that is the optimum:
**zero host copies of sample data, end to end.**

One honest caveat about *why* it works here — or doesn't: GPUDirect RDMA
is a PCIe peer-to-peer transaction between the NIC and the GPU, and P2P
routing is a **motherboard property**, not a driver one. Two devices under
the same PCIe switch or downstream port: fine. Two devices under
**sibling root ports** of a desktop chipset (this rig:
`01.0 → A4000`, `01.1 → ConnectX-5`): frequently not routed — and the
failure is *silent*. `nvidia-peermem` loads, `ibv_reg_mr` succeeds, the
NIC's completions fire and the receiver reports full line rate with zero
gaps, because completion semantics don't prove the chipset delivered the
posted writes to VRAM. The mandatory acceptance test is therefore
**payload-level**: the visualizer must show the known test signal (tone at
fc − 0.25·fs, chirp). Empty or noise-shaped spectrum while the CPU-staging
path shows the signal = P2P not routed = use `rdma_rx`; `iommu=pt` and
ACS settings are worth one attempt, but the real fixes are a
server/workstation board or a PCIe switch between the two devices. On
boards without P2P, CPU staging *is* the optimal architecture, and its
cost is small: one host→GPU copy per displayed frame (~60 MB/s at 60 fps)
against a ~25 GB/s PCIe Gen3 x8 host-to-GPU budget.
Also note the bandwidth reality: both slots are Gen3 x8, so GPUDirect on
this box buys **latency and CPU offload**, not throughput — the ~6.5 GB/s
ceiling is the same whether data stages through RAM or not.

### Control plane: the CPU floor on a ConnectX-5

Someone must post receive WQEs, ring doorbells, and poll the completion
queue. On a ConnectX-5 that someone is the CPU — the `rdma_rx_gpu` data
loop is exactly that and nothing else (validate header, bump counters,
repost). Budget roughly one core at full rate; it does no memcpy.

Removing *that* is what **DOCA GPUNetIO** does: CUDA kernels post WQEs and
poll CQs themselves, so packets arrive while the CPU sleeps. It requires a
**ConnectX-6 Dx or newer** (or a BlueField DPU) — the CX-5 lacks the
hardware doorbell/CQ mapping model it needs, so on this rig it is a future
upgrade path, not an option. The same applies to NVIDIA **Rivermax** +
Holoscan's **Advanced Network Operator** (holohub) in GPU-direct mode:
excellent production path, newer NIC required for the full GPU-driven mode
(the ANO's DPDK/host modes do run on CX-5 and are worth a look when you
outgrow this demo's receiver).

So the honest scorecard for this machine: RoCEv2 ingestion **into GPU
memory** — yes, today, with `rdma_rx_gpu`. Buffer handling, IQ DSP, FFT,
spectral analysis, rendering — all GPU (CuPy/cuFFT/Holoviz). WQE posting
and CQ polling — CPU, irreducibly so on CX-5, by NIC generation rather
than by software choice. Python orchestration — CPU by nature, negligible
load.

### Verifying the claim

Run the GPUDirect path and watch three things: `rdma_rx_gpu` prints
"Gb/s -> GPU" (payload DMA target is VRAM); the visualizer banner says
`[GPUDirect, zero-copy]`; and `htop` shows the receiver pinned at ~1 core
with **zero** memory-bandwidth-shaped load, while `python viz/gpu_bw_probe.py`
(Part 2, metric 5) proves GPU memory headroom is untouched.

---

## Part 2 — Validating 100 simultaneous RoCEv2 links

### What "100 links" means here, and what this setup can certify

A RoCEv2 "link" in this methodology is an independent **Reliable
Connection queue pair** with its own rdma_cm connection, sequence space,
and flow control — which is precisely what 100 remote senders would
present to the receiving NIC and host. `scale_tx -n 100` creates 100 of
them over the one physical cable; `scale_rx` terminates them.

This faithfully exercises everything on the receive side that matters at
scale: 100 QP contexts in NIC cache, completion aggregation onto one CQ,
WQE replenishment under pressure, memory registration size, PCIe DMA
scheduling across QPs, and the host's ability to keep up. What it does
**not** exercise is a switched fabric — congestion management (PFC/ECN/DCQCN)
between multiple physical ports is a different test that needs a switch.
For one box validating its ingest path, link multiplication over one cable
is the industry-standard honest proxy.

### The five metrics and exactly how each is measured

**1. Aggregate throughput.** `scale_rx` counts received bytes per second
across all connections and prints Gb/s live; the final JSON reports the
whole-run average. Expectation on this rig: unpaced (`-g 0`), large frames
(≥ 256 KiB) should sit at **50–55 Gb/s** — the PCIe Gen3 x8 ceiling —
*independent of link count* from 1 to 100. If adding links *reduces*
aggregate, something real is wrong (QP cache thrash, CPU saturation).

**2. Packet integrity.** Every 8-byte lane of every payload is a pure
function of (link, sequence, lane) — `sc_lane()` in `scale_common.h` — so
the receiver can verify any byte without checksums in the protocol. By
default it samples head, tail and every 4 KiB of each frame (catches
truncation, slot mix-ups, misdirected DMA at ~zero CPU cost); `-V` verifies
**every lane** — use it at paced rates or with fewer links, since full
verification of 6+ GB/s costs real CPU. Pass criterion: `payload_errors == 0`,
always, at any rate. A single corrupt lane is a hardware/driver bug, not
noise.

**3. Loss.** Per-link sequence-gap counting. This is the subtle one: RC
transport **retransmits** lost packets itself, so by the time a frame is
delivered to software, link-level drops have already been repaired. A
sequence gap at the application layer therefore means frames were lost
*above* the transport — receiver out of WQEs beyond RNR-retry patience, or
a software bug — and the pass criterion is an unconditional `gaps == 0`.
Link-level retransmission activity is visible separately in the NIC
counters the harness snapshots (`out_of_sequence`, `packet_seq_err`,
`rnr_nak_retry_err` under `/sys/class/infiniband/mlx5_*/ports/1/hw_counters/`):
small RNR NAK counts under unpaced load are *flow control working as
designed*; `out_of_sequence` movement on a direct cable should be zero.

**4. Latency consistency.** Each frame header carries the sender's
`CLOCK_REALTIME` at post time; the receiver subtracts on arrival. Because
sender and receiver are the same machine, this is a **true one-way
latency** with no clock-sync error — it covers payload fill, send-side
PCIe, the wire, receive-side PCIe DMA, and completion delivery.
`scale_rx` builds a histogram and reports min/avg/p50/p99/max per second
and per run (per-link p99 in the JSON). Read it as *consistency*, not as a
microbenchmark: at paced rates expect p50 in the tens of microseconds and
a tight p99; unpaced, queues are deliberately full, so latency = queue
depth ÷ drain rate (hundreds of µs to a few ms) and the criterion becomes
"p99 stable over time and across links", not "p99 small". A drifting p99
during a soak is an early warning (thermals, memory pressure) that
throughput numbers alone hide.

**5. GPU memory bandwidth utilization.** Two numbers. Arithmetic: worst
case ingest is ~6.8 GB/s of writes into VRAM (GPUDirect) plus the DSP's
reads — single-digit GB/s against the A4000's ~448 GB/s, i.e. **< 3 %**.
Measurement: run `python viz/gpu_bw_probe.py --secs 60` *during* the load
test; it saturates device-to-device copies and prints achieved GB/s. If
the probe under load is within a few percent of the probe idle, GPU memory
bandwidth is provably not a constraint — which is the result you should
get, and worth having on record.

**6 (implicit). Sustained stability.** Not a separate metric but the same
metrics over time: a soak is `DUR=3600 STEPS="100"` and the pass criterion
is that the per-second CSV shows flat throughput, zero cumulative
gaps/errors, and a non-drifting p99 from first minute to last, with no
NIC error-counter movement.

### Running it

```bash
make                                   # builds scale_tx / scale_rx too
sudo ./scripts/04_scale_validate.sh    # sweep 1 -> 10 -> 50 -> 100, 30 s each
sudo DUR=3600 STEPS="100" ./scripts/04_scale_validate.sh      # 1 h soak
sudo GBPS=40 STEPS="100" ./scripts/04_scale_validate.sh       # paced run
```

The harness snapshots NIC counters before/after every step, stores
per-second CSVs and JSON summaries in `scale_results/`, prints a PASS/FAIL
verdict per step (gates: zero gaps, zero payload errors) and an overall
verdict. Manual runs for experimentation:

```bash
sudo ./scale_rx -a 192.168.100.2 -n 100 -t 60 -C ts.csv -O sum.json
sudo ip netns exec ns_tx ./scale_tx -s 192.168.100.2 -n 100 -g 0 -t 60
```

### Pass criteria summary

| Metric | Gate |
|---|---|
| Aggregate throughput (unpaced, ≥256 KiB frames) | ≥ 50 Gb/s, flat from 1→100 links |
| Payload integrity | 0 corrupt lanes (sampled always; `-V` spot-checks full) |
| Sequence gaps (loss above RC) | 0 |
| NIC `out_of_sequence` / ethtool discards delta | 0 on direct cable |
| One-way latency | p99 stable across links and over the whole run |
| GPU memory bandwidth | probe under load ≈ probe idle (ingest < 3 % of 448 GB/s) |
| Soak (1 h+) | all of the above, no drift in the CSV |

### Reading failures

Throughput below 50 Gb/s with 100 links but fine with 1: check CPU — one
core saturated polling means try larger `-b` (fewer completions/s) or pin
`scale_rx` with `taskset -c <core-on-NIC-NUMA-node>`. Gaps under unpaced
load: receiver WQE starvation; raise `-o` on the rx side or check memlock.
Payload errors: stop, capture `scale_results/`, re-run with `-V` and one
link — if it reproduces, suspect RAM (memtest), firmware (`mlxup`), or an
overheating riser before suspecting this code. Latency p99 exploding only
at high link counts: classic CQ overflow territory — confirm
`cq_depth = links × slots` headroom and that the 1 s prints aren't being
starved. `rnr_nak_retry_err` climbing fast: receiver can't repost in time;
same fixes as gaps.

### Scaling the methodology beyond this box

When real remote senders replace `scale_tx`, keep the tools: `scale_rx` is
already a multi-initiator sink. What changes is the network: once a switch
sits in the path you must configure lossless RoCE (PFC on the RoCE
priority, ECN/DCQCN) or accept RC retransmissions doing the work, and
latency measurement needs PTP instead of the same-host clock trick. Memory
math for the receiver scales as `links × slots_per_link × frame_bytes`
(100 × 8 × 256 KiB = 200 MiB at the defaults) and the single shared CQ
sizing as `links × slots + margin` — both already parameterized.
