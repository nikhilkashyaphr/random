# Mellanox RoCEv2 → GPU → GUI: architecture assessment

Status: **Inspect / Verify / Design complete. Implementation gated on hardware
evidence that can only be gathered on the target machine.**

Per your rule 27 — where something cannot be verified, it is named as such
rather than faked.

---

## 1. The decision that matters most (Section 22)

### Is Rivermax the right transport for RoCEv2? **No.**

This is the single most consequential finding, and your brief explicitly
warned against assuming otherwise.

| | Rivermax | RDMA verbs (libibverbs) |
|---|---|---|
| transport | UDP unicast/multicast media streams | RoCEv2 (RDMA over UDP:4791) |
| designed for | SMPTE ST 2110 / ST 2059 video-audio | RDMA queue pairs, one/two-sided ops |
| receives | raw packet payloads into a ring | messages/RDMA WRITEs into registered MRs |
| GPU path | GPUDirect via Rivermax GPU buffers | GPUDirect RDMA via `ibv_reg_mr` on CUDA memory |
| RoCEv2 support | **none — it is not an RDMA library** | native |

**If the traffic is genuinely RoCEv2, Rivermax cannot receive it.** RoCEv2 is
RDMA: the payload is placed by the NIC into a registered memory region under
queue-pair semantics. Rivermax is a kernel-bypass *media* stack for UDP; it has
no concept of QPs, MRs or CQs.

The correct architecture is therefore:

```
ConnectX port  →  RDMA verbs (QP/CQ/MR)  →  ibv_reg_mr over cudaMalloc
               →  GPUDirect RDMA         →  GPU memory  →  CUDA/Holoscan
```

Rivermax becomes the right answer **only** if what is actually on the wire is
plain UDP media rather than RoCEv2. That is an observation, not an assumption
— confirm with `tcpdump -i <netdev> udp port 4791` (RoCEv2) versus your media
stream's port.

### Remaining Section 22 answers

| question | answer | basis |
|---|---|---|
| A. Is the traffic truly RoCEv2? | **must be confirmed on target** | check UDP:4791 and the port's GID types |
| B. Rivermax required? | **No**, for RoCEv2 | see table above |
| C. GPUDirect RDMA supported? | **must be confirmed on target** | needs `nvidia_peermem` or dma-buf MR |
| D. Lowest-copy path? | NIC → GPU MR, zero host copies | GPUDirect RDMA |
| E. CPU-side? | control plane, stats, GUI | not the data plane |
| F. GPU-side? | decimate, window, FFT, magnitude | reduces 200 MSPS → ~2 k points |
| G. NumPy/SciPy genuinely needed? | **only on the reduced result** | see §4 |
| H. Replaceable by CUDA? | FFT (cuFFT), filtering, decimation | already GPU-resident |

---

## 2. What was built this round

`src/core/GpuNetProbe.{h,cpp}` — a read-only capability preflight, plus
`--preflight` on the existing binary. It is the evidence-gathering step your
loop demands before any implementation.

It reports, from sysfs (no libibverbs/CUDA link dependency, so it runs on a
machine that has neither):

* every RDMA device/port, **with its real Linux netdev** — read from
  `/sys/class/infiniband/<dev>/device/net/`, because `mlx5_0` is an RDMA
  device name and never an interface name;
* link layer (Ethernet = RoCE vs InfiniBand), state, rate, firmware;
* **GID types per port**, so RoCE v1 vs **v2** is established from evidence;
* GPUDirect peer-memory: `nvidia_peermem` loaded, or a kernel new enough for
  dma-buf MRs;
* CUDA, Rivermax, Holoscan, NumPy, SciPy, CuPy versions;
* a dependency matrix and an **evidence-based transport decision**.

Verified output on this development host:

```
RDMA devices     : none (/sys/class/infiniband empty)
nvidia_peermem   : not loaded
NumPy            : 2.4.4   yes
SciPy            : 1.17.1  yes
GPU / CUDA / NIC : NO
transport        : Not determinable on this machine
GPU path viable  : NO      (exit code 2)
```

That is the correct result here — **there is no ConnectX adapter and no NVIDIA
GPU in this environment**, so no claim about GPUDirect throughput could be
made honestly. Run it on the iWave machine and it will fill in.

---

## 3. Integration point — why the GUI is untouched

The application already abstracts acquisition behind `ISignalSource`
(`src/core/Sources.h`), which `SimulatedSource`, `FileSource` and `DmaSource`
implement. A network receive path is a **fourth implementation**:

```cpp
class RdmaGpuSource : public ISignalSource {
    // start(): open ibv device, create PD/CQ/QP, cudaMalloc the RX arena,
    //          ibv_reg_mr over it, post RECVs, transition QP to RTR/RTS
    // poll  : ibv_poll_cq -> completed buffer already in GPU memory
    // emit  : publish() a SampleBlock referencing the GPU region
};
```

Nothing above it changes: the DSP stage, plots, channel selection, frequency
measurement and GUI all consume `SampleBlock`/`FrameResult` exactly as today.
This is the smallest correct change and it satisfies rules 1, 2 and 19.

**One design consequence to settle before coding:** `SampleBlock` currently
carries host memory (`QByteArray raw`). Keeping data GPU-resident means adding
a device-pointer variant so the DSP stage can run cuFFT in place rather than
copying down. That is a contained change to one struct plus the DSP entry
point — not a rewrite — but it is the piece that decides whether the pipeline
is genuinely zero-copy or quietly copying through host memory.

---

## 4. Data-path table (Section 8)

| stage | memory | copy | note |
|---|---|---|---|
| NIC RX | NIC DMA engine | — | |
| RDMA placement | **GPU VRAM** (registered MR) | **none** | requires GPUDirect; else falls back to host and **must be reported** |
| decimate / window | GPU VRAM | none | CUDA kernel |
| FFT | GPU VRAM | none | cuFFT, in place |
| magnitude / dB | GPU VRAM | none | |
| reduced result | GPU → host | **one small copy** | ~2 k floats/frame, not 200 MS/s |
| NumPy / SciPy | host | none | operates on the reduced array |
| GUI | host | none | existing plot path |

Only one host copy in the critical path, and it carries the *reduced* result.
At 30 fps × 2048 points × 4 ch × 4 B that is ~1 MB/s crossing PCIe, versus
3.2 GB/s if raw samples were copied down — a ~3000× reduction.

**Rule 10 compliance:** none of this is claimed as verified. It is the design;
the preflight determines whether the machine can support it.

---

## 5. NumPy/SciPy placement (Section 9)

NumPy and SciPy are present (2.4.4 / 1.17.1) and belong **after** the GPU
reduction, never in the packet path (rule 6). The existing DSP is C++ and
already produces exactly the reduced arrays the GUI needs, so the Python layer
is optional: it is the right tool if you want SciPy's filter design or
statistical routines on the reduced result, and the wrong tool for anything
touching the 200 MS/s stream. CuPy would let the same array API run on the GPU
side, avoiding the download entirely for GPU-native steps — worth adding only
if you want the DSP expressed in Python.

---

## 6. What is NOT done, and why

Not implemented: `RdmaGpuSource`, CUDA kernels, cuFFT stage, Holoscan
operators, benchmark mode.

Reason: this environment has **no ConnectX NIC, no NVIDIA GPU, no CUDA, no
Rivermax, no Holoscan**. Writing an RDMA/CUDA receive path here would produce
code that has never been compiled against the real headers, never linked, and
never run — which would violate rules 9, 10, 14 and 27. The preflight exists
precisely so the next step starts from measured facts.

### Next steps, in order

1. Run `./build/sdr_workstation --preflight` on the iWave machine. Send the
   output; it settles questions A and C.
2. Confirm the wire protocol: `tcpdump -i <netdev> -c 20 udp port 4791`.
3. Confirm the payload format — sample format, I/Q order, bit width,
   endianness, channel count, sequence/timestamp fields. **Never guessed**
   (Section 10); the existing `SampleCodec` already handles 13 wire formats
   and will very likely cover it.
4. Minimal NIC → GPU test (`ib_write_bw --use_cuda`) to prove GPUDirect
   before any application code.
5. Then `RdmaGpuSource`, validated against a known tone exactly as the
   existing frequency validation does (currently 0.156 kHz worst error).

---

## 7. Honest capability statement

| requirement | current capability | missing | needed |
|---|---|---|---|
| Mellanox detection | **done** — `detectMellanox()` | — | — |
| RDMA/RoCEv2 discovery | **done** — `--preflight` | — | — |
| GPU/CUDA detection | **done** — `detectGpu()` | — | — |
| GPUDirect verification | **detects capability** | live proof | `ib_write_bw --use_cuda` |
| NIC → GPU receive | **not implemented** | hardware to develop against | ConnectX + GPU + MLNX_OFED |
| Holoscan pipeline | **not implemented** | SDK absent | Holoscan SDK |
| GPU DSP | **not implemented** | CUDA absent | CUDA + cuFFT |
| Existing GUI | **fully functional, untouched** | — | — |
