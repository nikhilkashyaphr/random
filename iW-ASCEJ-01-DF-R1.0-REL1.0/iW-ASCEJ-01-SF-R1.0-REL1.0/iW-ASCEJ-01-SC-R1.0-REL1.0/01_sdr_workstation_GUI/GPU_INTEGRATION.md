# RoCEv2 + GPU — what was built, and what was verified

## Summary

The GPU path is implemented end to end: CUDA IPC ingest from the GPUDirect
ring, cuFFT spectrum on the device, and only the finished spectrum crossing to
the host. Sample data never enters system RAM.

**I could not run it here.** This container has CUDA *headers and libraries*
but no `nvcc` and no GPU. What that allowed, and what it did not, is set out
in §5 — please read that before deploying.

## 1. What was wrong before

| | |
|---|---|
| "GPU offload" toggle | **decorative** — `ConfigDialog.cpp:243` created it, but `SourceConfig` had no field, so the choice never left the dialog |
| GPU rings | **refused** — `RoceShmSource.cpp:178` reported "use the Holoscan visualiser" and stopped |
| CUDA | not linked at all |

## 2. What runs on the GPU now

```
NIC ──DMA──────────► VRAM ring          rdma_rx_gpu, GPUDirect RDMA
VRAM ring ──IPC────► this application   zero copy
int16 I/Q ──kernel─► cufftComplex       windowed, normalised
cufftComplex ─cuFFT► spectrum
|X| ──kernel───────► dBFS + fftshift
dBFS ──D2H─────────► Qt plots           16 KB, not 1 MiB
```

For a 4096-point FFT the host sees **16 KB per displayed frame** instead of the
1 MiB raw slot.

## 3. On Holoviz — a deliberate decision

Holoviz owns a GLFW window and a GL context. Reparenting that into a QWidget
hierarchy means reconciling two event loops and two context owners, and it is
where this kind of integration usually stalls.

**The value of the Holoscan path is the GPU pipeline, not its renderer.** The
GUI already has plotting, markers, measurements and recording that Holoviz
would duplicate — and you asked for *the same GUI*.

So this reproduces the Holoscan **pipeline** exactly — same ring, same
ordering guard, same FFT and log-magnitude maths — and feeds your existing
plots. One window; every stage before the plot on the GPU.

If you specifically want Holoviz rendering rather than GPU compute, say so:
that is a different job, and I would want to prototype the context sharing
before promising it.

## 4. One definition of the arithmetic

Two implementations of the same maths drift, and the symptom is that the
display changes when the operator toggles CPU/GPU — which looks like a
hardware difference and gets investigated as one.

So the arithmetic is defined once in **`GpuMathRef.h`**, unit-tested on the
host, and the CUDA kernels follow it statement for statement.

Verified against known-answer signals:

| Check | Result |
|---|---|
| Hann coherent gain | 0.499878 (periodic Hann ≈ 0.5) |
| full-scale tone peaks at the right bin | PASS — bin 64 |
| full-scale tone reads 0 dBFS | **PASS — 0.000 dBFS** |
| half-scale tone reads −6 dBFS | **PASS — −6.020** (theory −6.021) |
| level independent of FFT size (1k/2k/4k) | **PASS — 0.00 / 0.00 / 0.00** |
| fftshift moves bin 0 to centre | PASS |
| full scale is 8191<<2, not 32767 | PASS |

That last row matters: the RFSoC packs 14-bit samples MSB-aligned into int16,
so full scale is **32764**. Using 32767 puts every reading 0.08 dB low —
small, consistent, and exactly the kind of error that is never noticed.

## 5. What was and was not verified

**Verified here:**

| | |
|---|---|
| host pipeline compiles against **real CUDA headers** | PASS |
| links against **real `libcudart` + `libcufft`** | PASS |
| runs and fails cleanly with no GPU | `No CUDA device: no CUDA-capable device is detected` |
| error paths return real messages | null mapping and bad-magic both verified |
| builds **without** CUDA (`SDR_ENABLE_CUDA=OFF`) | PASS |
| builds with `SDR_ENABLE_CUDA=ON` and **no toolkit** | warns and degrades, does not fail |
| the spectrum maths | 7/7 against known answers |

**Not verified here, because it needs a GPU:**

- `GpuKernels.cu` has never been compiled — there is no `nvcc` in this
  container. **Expect to fix small `nvcc` complaints on first build.**
- `cudaIpcOpenMemHandle` against a live `rdma_rx_gpu` ring.
- End-to-end throughput and frame-skip behaviour.

I would rather say this plainly than let you find it at the bench.

## 6. The constraint that will bite first

**CUDA IPC requires the same user.** Your own visualiser says so
(`holoscan_iq_viz.py:134`), and the error text here repeats it:

> `rdma_rx_gpu and this application must run as the SAME user`

You often run the GUI with **sudo** for PCIe BAR access. **`sudo` GUI + user
`rdma_rx_gpu` will fail to open the handle.** Either run both as the same user,
or start the GUI without sudo when using the GPU path.

## 7. Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSDR_ENABLE_CUDA=ON
cmake --build build -j
```

Default is **OFF**, so the application still builds on a machine with no
toolkit. With it ON but no toolkit found, CMake warns and builds CPU-only
rather than silently producing a binary that claims GPU support.

## 8. Files

```
src/core/GpuIqPipeline.h    API; no CUDA types, so the GUI builds without it
src/core/GpuIqPipeline.cpp  IPC open, cuFFT orchestration, graceful no-CUDA stub
src/core/GpuKernels.cu      the only file needing nvcc
src/core/GpuMathRef.h       the arithmetic, defined once and unit-tested
src/core/Types.h            SourceConfig::useGpu — the field the toggle lacked
src/ui/ConfigDialog.cpp     carries the toggle into the session config
CMakeLists.txt              optional CUDA
```

## 9. Next step

Build with `-DSDR_ENABLE_CUDA=ON` on the box that has the A4000, and send me
whatever `nvcc` says about `GpuKernels.cu`. That is the one part I could not
compile, and fixing its first-build complaints is quick once I can see them.
