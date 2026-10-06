# Why nvtop showed no GPU use — and the fix

## The honest diagnosis first

**The GPU code was never called.** I owe you that plainly.

In the previous package I built `GpuIqPipeline` and added `SourceConfig::useGpu`,
but a grep of the source shows:

```
GpuIqPipeline used by:  ConfigDialog.cpp   (#include only — never instantiated)
useGpu read by:         nothing            (written once, never consumed)
```

So the "GPU offload" toggle set a flag that no code read, and the pipeline
existed but was never constructed. It is the same decorative-toggle defect I
had fixed earlier in this project — reintroduced by me. nvtop was telling the
truth: the application never created a CUDA context.

## A second reason, even with the code wired

`tools/iqring_sim` publishes a **host** ring (`IQRINGG1` is only produced by a
real `rdma_rx_gpu`). The old design only engaged the GPU for `IQRINGG1`, so
with the simulator it would have stayed on the CPU regardless.

## What is fixed

### 1. GPU offload now works for every source

`GpuFft` runs inside `DspEngine`, on the decoded samples it already has. So GPU
offload engages for PCIe, the RoCEv2 host ring, the GPUDirect ring, files, and
**the simulator** — which means you can verify it with no network hardware.

### 2. It moves exactly the heavy step, and nothing else

```
window multiply -> FFT -> |X|^2   ->   dBFS, fftshift, averaging
\________ on the GPU ________/         \______ unchanged, CPU ______/
```

The GPU returns `|X|^2` only. Everything after — dBFS, coherent-gain
correction, fftshift, all five averaging modes — is the same code as CPU mode.
**Toggling CPU/GPU cannot move a reading.** The window coefficients come from
`DspEngine`, so there is one definition of the window, not two.

An earlier design computed dBFS on the GPU with a fixed Hann window. That would
have been a second implementation of calibration the GUI already has — and
drift between the two shows up as the display changing on toggle.

### 3. The GPUDirect ring is opened, not refused

`IQRINGG1` used to produce *"use the Holoscan visualiser"*. It now opens the
VRAM ring through CUDA IPC and reads it, using the exact layout your visualiser
uses:

| | |
|---|---|
| host header for slot | `4096 + slot * 64` |
| VRAM payload for slot | `slot * slot_stride` (payload only) |
| torn-read guard | accept if `c2 - c1 <= slots - 4` (write-count distance) |

`cudaIpcMemLazyEnablePeerAccess` is set, so the handle opens in the
peer-to-peer case.

### 4. The status bar tells the truth

A **CPU / GPU** badge now shows where the FFT *actually* ran, driven by
`DspEngine`, not by the toggle. If GPU init fails — no device, no CUDA in this
build — it falls back to CPU **and says so**, rather than looking identical.
Hover it for the reason.

## Peer to peer

"Peer to peer" here means **GPUDirect RDMA**: the NIC DMAs straight into VRAM,
never touching system RAM. It needs the `nvidia_peermem` kernel module. The GUI
does not switch it on — `rdma_rx_gpu` registers VRAM with the NIC; the GUI
opens that VRAM by CUDA IPC.

`tools/gpu_p2p_check.sh` verifies every prerequisite and prints the fix for
each failure: GPU, `nvcc`, `nvidia_peermem`, RDMA device, NIC↔GPU PCIe
topology, ring type, ring ownership, and whether you are root.

## Verified here

| Check | Result |
|---|---|
| every changed file, CUDA **off** and **on** (real headers), `-Wall -Wextra -Wshadow` | 0 errors, 0 warnings in my code |
| `GpuFft` links against real `libcudart` + `libcufft` | PASS |
| runs with no GPU → clean fallback message | `No CUDA device: no CUDA-capable device is detected` |
| full CMake build | 1.8 MB binary |
| `gpu_selftest` compiles, links, fails cleanly here | PASS |
| `gpu_p2p_check.sh` syntax and run | PASS |

Three `-Wshadow` warnings in `DspEngine.cpp` (lines ~795, 838) are **pre-existing**
in your original code — confirmed by compiling the original — and are unrelated
to this change, so I left them.

## Not verified here — and how you verify it

No `nvcc`, no GPU in this environment, so **`GpuKernels.cu` has never been
compiled or run.** The self-test closes that gap on your A4000:

```bash
tools/build_gpu_selftest.sh
./gpu_selftest
```

It runs identical data through the CPU path and the GPU path and compares them:

```
N=4096   max |CPU-GPU| = 0.0000x dB   peak bin CPU 256 GPU 256   PASS
RESULT: PASS — GPU offload matches CPU
```

Leave it running and watch nvtop — `gpu_selftest` must appear. **If it does not
pass, do not enable GPU offload**, and send me its output.

## Order of operations

```bash
tools/gpu_p2p_check.sh                          # 1. prerequisites
cmake -B build -DSDR_ENABLE_CUDA=ON             # 2. build WITH CUDA
cmake --build build -j
tools/build_gpu_selftest.sh && ./gpu_selftest   # 3. prove the GPU path
./build/sdr_workstation                         # 4. GUI, NOT sudo
```

In the GUI choose **GPU offload** and start. The badge must read **GPU**, and
nvtop must list `sdr_workstation`. If the badge reads CPU, hover it for why.

**Do not run the GUI with sudo** for the GPU path — CUDA IPC cannot cross
users. Grant PCIe BAR access with `sudo setcap cap_sys_rawio+ep` instead.
