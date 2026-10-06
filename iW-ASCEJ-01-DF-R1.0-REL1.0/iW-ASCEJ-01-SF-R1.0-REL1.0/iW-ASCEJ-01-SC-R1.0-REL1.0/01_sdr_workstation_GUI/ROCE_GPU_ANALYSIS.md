# RoCEv2 + GPU in the GUI — analysis

What it would take to make **RoCEv2 + GPU offload** a real mode, based on
reading the Holoscan modules and the current GUI rather than on assumption.

---

## 1. Where things actually stand

Three findings, each verifiable in the source:

**The "GPU offload" toggle is decorative.** `ConfigDialog.cpp:243` creates it,
`exclusive(m_cpu, m_gpu)` makes it look like a choice — but there is **no GPU
field in `SessionConfig`** (`src/core/Types.h`). The selection never leaves the
dialog. Selecting it today changes nothing about how data is acquired or
processed.

**The GUI deliberately refuses GPU rings.** `RoceShmSource.cpp:178`:

```cpp
if (magic == kRingMagicGpu) {
    ... "sample payloads live in GPU VRAM and are not host-readable.
         Use the Holoscan visualiser for the GPU path" ...
    return false;
}
```

That message is presently the entire GPU story: the GUI detects the ring,
explains why it cannot read it, and stops.

**The GUI links no CUDA.** Nothing in `CMakeLists.txt` references cuda, cufft
or similar.

So this is not "wire up a half-finished feature". It is new capability — but
much smaller than it first appears, for the reason in §3.

---

## 2. What the Holoscan path actually does

```
rdma_rx_gpu                       holoscan_iq_viz.py
────────────                      ──────────────────
NIC DMAs payload ──► VRAM ring    opens VRAM ring by CUDA IPC (zero copy)
NIC DMAs 64 B hdr ─► host shm     reads headers from host shm
publishes cudaIpcMemHandle_t      cuFFT + log magnitude
  at host shm offset 1024         Holoviz renders
```

### The ring ABI is small and fully specified

From `rdma_common.h`:

| | |
|---|---|
| `RING_MAGIC_GPU` | `0x495152494e474731` = `"IQRINGG1"` |
| `CTRL_IPC_OFF` | `1024` — 64-byte `cudaIpcMemHandle_t` lives here |
| host shm | `ring_ctrl` (4096 B) + `RING_SLOTS × 64 B` frame headers |
| GPU ring | `RING_SLOTS × FRAME_PAYLOAD`, opened via CUDA IPC |

### Its torn-read guard differs from the RTAP one

`holoscan_iq_viz.py:175`:

```python
c1 = self.write_count
...copy the slot...
c2 = self.write_count
if c2 - c1 <= self.num_slots - 4:   # slot cannot have been recycled
```

This is a **write-count distance** test, not a seqlock. The roce-extractor tap
uses a per-slot seqlock instead. Both are correct for their ring; a GPU
implementation must use *this* one. Getting it wrong produces frames that are
half old and half new — which look like signal artefacts, not bugs.

### The DSP is smaller than the surrounding machinery

Every GPU primitive the visualiser uses:

```
cp.fft.fft · cp.fft.fftshift · cp.hanning · cp.abs · cp.log10 · cp.mean
cpsig.fftconvolve   (one call, for smoothing)
```

That is a windowed FFT, log magnitude, and a smoothing convolution. **The GUI's
`DspEngine::computeSpectrum()` already computes exactly this shape on the CPU**,
backed by `src/core/Fft.h`.

---

## 3. The consequence that matters

**The hard part is not the DSP. It is about six CUDA calls.**

To read the GPU ring the GUI needs:

```
cudaIpcOpenMemHandle      open the VRAM ring published by rdma_rx_gpu
cudaMemcpy (D→D or D→H)   fetch the newest slot
cufftPlan1d / cufftExecC2C   if the FFT is to stay on the GPU
cudaIpcCloseMemHandle
```

Everything downstream — windowing, averaging, the waterfall, markers,
measurements, recording — already exists and is transport-agnostic, because
`RoceShmSource` publishes raw bytes through the same `SampleCodec` as the PCIe
path.

---

## 4. Four options, honestly costed

### A. Launch the Python visualiser from the GUI
Add a button that spawns `holoscan_iq_viz.py` (the GUI already has
`BackendLauncher` for spawning backends).

| | |
|---|---|
| Effort | ~1 day |
| Risk | very low |
| Result | **a second window**, with its own axes, markers and controls |

Honest assessment: this is integration in name only. The user gets two
applications that do not share configuration, recording, or measurements. It
does not answer "plot RoCEv2+GPU *in my GUI*".

### B. GPU ingest, CPU DSP  ← **recommended first step**
Open the CUDA IPC ring in C++, copy the newest slot **device → host**, then
publish it into the existing pipeline exactly as the host ring does.

| | |
|---|---|
| Effort | ~2–3 days |
| New deps | CUDA runtime only (**no cuFFT, no Holoscan, no CuPy**) |
| Risk | low — one new source path, everything downstream unchanged |
| Result | GPUDirect capture plots in the **existing** GUI, with existing markers, measurements and recording |

What it buys: the NIC still DMAs straight into VRAM, so the capture path keeps
its CPU offload and latency benefit. What it does not buy: the FFT still runs
on the CPU.

**Worth being clear about the honest limit.** At 1 MiB/frame and display rates
(30–60 fps), a D→H copy is ~30–60 MB/s. That is nothing next to the 50–55 Gb/s
the link carries — the display was *never* going to see every frame. So the
copy is not the bottleneck; the display rate is, and that is equally true of
the Python path.

### C. GPU ingest + GPU DSP
As B, plus cuFFT on the device; only the finished spectrum (a few KB) crosses
to the host.

| | |
|---|---|
| Effort | ~1–2 weeks |
| New deps | CUDA + **cuFFT**, and a CUDA toolchain in the build |
| Risk | moderate — dual CPU/GPU DSP paths must agree numerically, or the display changes when you toggle the button |
| Result | true GPU offload |

Only worth it if the CPU FFT is measurably limiting. **Measure first**: at
4096-point FFTs and 60 fps that is ~250 k points/s, which a modern CPU core
does without noticing. I would not start here.

### D. Embed Holoscan in the GUI
Holoviz rendering inside the Qt window.

| | |
|---|---|
| Effort | weeks |
| Risk | high — Holoviz owns a window/context, and reconciling it with Qt is where such projects stall |
| Result | duplicates plotting the GUI already has |

Not recommended. The GUI's plotting is not the weak part.

---

## 5. Recommendation

**Do B. Then measure before considering C.**

Sequence:

1. **Make the toggle real.** Add `useGpu` to `SessionConfig` and carry it from
   the dialog. Without this, nothing else can be selected.
2. **`RoceGpuSource`** — or a third ABI branch in `RoceShmSource`, consistent
   with how `RTAP` was added. Opens the IPC handle, applies the *write-count
   distance* guard, copies D→H, publishes raw bytes.
3. **Guard the dependency.** Build with `-DSDR_ENABLE_CUDA=ON` optional; when
   off, the GPU ring keeps today's clear refusal message. The GUI must still
   build and run on a machine with no CUDA — that is most developer machines.
4. **Report honestly in the panel:** ring type, whether IPC opened, frames
   dropped between displayed frames. Do not imply every frame is shown.

### Two operational constraints, from the Holoscan source

**CUDA IPC requires the same user.** `holoscan_iq_viz.py:134` catches the
failure and says so explicitly:

> `rdma_rx_gpu and this app must run as the SAME user`

The GUI is often run with `sudo` for PCIe BAR access. **`sudo` GUI + user
`rdma_rx_gpu` will fail to open the handle.** This needs stating in the UI, or
it becomes a support call.

**GPUDirect needs `nvidia_peermem` or dma-buf.** The GUI already probes for
this (`GpuNetProbe.h:53`, `GpuDirectStatus::available()`), so the pre-flight
check exists — it just is not wired to anything yet.

---

## 6. What I would build first, concretely

A ~200-line `RoceGpuSource` that does only this:

```
map host shm ──► magic == IQRINGG1?
                 read cudaIpcMemHandle_t at offset 1024
                 cudaIpcOpenMemHandle
  poll ────────► write_count advanced?
                 slot = (wc-1) % num_slots
                 read 64 B header from host shm
                 cudaMemcpy slot payload D→H
                 re-read write_count, apply the distance guard
                 publish raw bytes  ← same call the host ring makes
```

Everything else in the application is untouched. If that works, C is an
optimisation with a measurable case behind it. If it does not, very little has
been spent finding out.

---

## 7. Questions I need answered before writing it

1. **Which machine runs the GUI** — the same box as `rdma_rx_gpu`? CUDA IPC
   cannot cross hosts, so if the GUI runs elsewhere, only option A works.
2. **Is a CUDA toolkit acceptable as a build dependency**, or must the GUI keep
   building on machines without one? (I would make it optional either way.)
3. **Does the GUI need to run as root** on that machine? If yes, the same-user
   IPC constraint has to be solved first — likely by running `rdma_rx_gpu` as
   the same user rather than by relaxing the GUI.
4. **Is the CPU FFT actually limiting anything today?** If not, C is
   speculative and B is the whole job.
