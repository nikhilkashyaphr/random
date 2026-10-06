# RoCEv2 in the GUI — processing, test coverage and four defects

## What was asked

Test RoCEv2 in the GUI, with all data processing done in the GUI rather than
handed to an external visualiser.

## What was already true

The CPU path was complete and wired. `SourceMode::Roce` → `RoceShmSource` →
`DspEngine` → plots, with the Ethernet/RoCEv2 selection in `ConfigDialog`, a
`--roce <path>` option in `main.cpp`, and `RoceShmSource` reading **both**
producers' ABIs (`IQRING01` from `rdma_rx`, `RTAP` from the roce-extractor
tap), adopting rate and centre from the ring header, taking the newest slot of
an overwriting ring and rejecting torn frames.

GPU acceleration of the display maths was also already wired, but **not**
through `GpuIqPipeline`. It runs in `DspEngine` via `GpuFft`, selected by
`cfg.src.useGpu`. `GpuFft` returns `|X|²` and nothing else; the dBFS scaling,
coherent-gain correction, fftshift and all five averaging modes stay on the
one code path shared with CPU mode. So toggling CPU/GPU cannot move a
reading, and it works for every source — PCIe, RoCEv2 host ring, RoCEv2 GPU
ring, files and the simulator.

## Why `GpuIqPipeline` was not wired into the display

`GpuIqPipeline` computes a *finished spectrum* on the device with a fixed Hann
window. `GpuFft.h` records, in its header, why that design was replaced:

> An earlier design computed dBFS on the GPU with a fixed Hann window. The
> GUI's DspEngine supports several windows and five averaging modes with their
> own calibration. Duplicating all of that in CUDA would give two
> implementations to keep in step — and when they drift the display changes on
> toggle, which looks like hardware and gets investigated as hardware.

Routing the display through `GpuIqPipeline` would reintroduce exactly that,
and would also lose the constellation, the metrics, the time-domain plot, the
other windows and the averaging modes, because it returns only a spectrum.

The GPU ring is therefore processed the same way every other source is:
`RoceShmSource` opens the VRAM ring by CUDA IPC, brings in the display slice
(capped at 65,536 samples — 256 KiB, not the full 1 MiB frame), and publishes
it into `DspEngine`, where `GpuFft` does the window/FFT/magnitude on the
device. All processing is in the GUI, with one implementation of the maths.

`GpuIqPipeline` is left in place and now has test coverage of its ABI
constants and the arithmetic its kernels implement. Making the path fully
zero-copy would mean teaching `DspEngine` to accept device pointers; that is a
larger change than this work, and it is the only remaining gap between this
and the Holoscan pipeline.

## The GUI starts the receiver

`rdma_rx` owns the network side and publishes the ring, so until it runs the
GUI has nothing to read. Selecting RoCEv2 and pressing Start therefore worked
only if the operator had already started it in a terminal; otherwise the GUI
reported `cannot open /dev/shm/iqring` and left them to work out why.

`BackendLauncher` already manages the C2H, H2C and UDP helpers with process
lifecycle, log capture, ordered stop and orphan prevention, so the RoCEv2
receiver is now one more managed process:

* `BackendMode::RoceRx` launches `reference/roce-iq-holoscan/rdma_rx`, or
  `rdma_rx_gpu` when the GPU backend is selected, building it on first use
  exactly as the UDP receiver already was.
* `roceDirectory()` finds the vendored tree beside the executable or in the
  source tree. It is looked up separately from `backendDirectory()` because
  the reference stack is vendored under `reference/`, not built into
  `backend/`.
* The ring path is **not** configurable for a launched receiver: `rdma_rx`
  hard-codes `RING_SHM_NAME "/iqring"` and takes no option for it, so
  `roceRingPath()` returns `/dev/shm/iqring` and the source is pointed there
  whatever the device field held.
* `becameReady()` carries the ring path rather than a capture FIFO, which is
  what the existing handler already needs to point the source at.
* Readiness means the ring exists. The receiver creates it before it listens,
  so that is the thing the GUI actually waits for.
* The C2H capture watchdog skips this mode, as it already skips the UDP
  receiver: neither is a capture helper to be restarted.

### A ring that already exists is used, not duplicated

`rdma_rx` creates `/dev/shm/iqring` and dies on a ring it cannot own, and a
second receiver would also find the `rdma_cm` port taken. So when a ring is
already present the GUI launches nothing and reads it as it is, and a direct
`start()` is refused by naming the ring.

The ring check deliberately runs **before** the "is the binary built" check.
`roce_launch_test` caught the original order: with a receiver already
publishing but no binary built in this tree, the operator was told to
`run make` — advice for a problem they did not have, instead of being told a
receiver was already running.

## Defects found and fixed

### 1. The CUDA path could not be built at all

`CMakeLists.txt` evaluated `if(SDR_HAVE_CUDA)` *before* the block that sets
it, and appended `src/core/GpuKernels.cu` to `PROJECT_SOURCES` *after*
`add_executable()` had already consumed that variable. Both mistakes are
invisible in a default build because the option is OFF.

With `-DSDR_ENABLE_CUDA=ON` and a toolkit present, the consequences were:

* `SDR_ENABLE_CUDA` was never defined, so `RoceShmSource` compiled its
  non-CUDA branch and **refused every GPUDirect ring** — in a build that had
  been explicitly asked for CUDA support.
* `GpuKernels.cu` was never compiled, so every `sdr_gpu_*` kernel would have
  failed to link.

Reproduced in isolation before fixing. With the original ordering, a
`-DSDR_ENABLE_CUDA=ON` configure yields no compile definition on the target
and no extra source; with the block moved above `add_executable()`, both
appear. The block now carries a comment saying why its position matters.

### 2. The configured ring path was ignored

`ConfigDialog` populates the "Device / FIFO" field with `/dev/shm/iqring` when
the RoCEv2 transport is selected and lets the operator edit it;
`main.cpp --roce <path>` sets the same field and calls `peekRingInfo` on it to
fill the axes. `RoceShmSource` then **never read `cfg.src.devicePath`** and
always opened `/dev/shm/iqring`, or `/dev/shm/roce_tap` if that existed.

So `--roce /dev/shm/other` took its sample rate and centre frequency from the
named ring and then streamed samples from a different one — one ring's axes
over another ring's data when both existed — and failed with
`cannot open /dev/shm/iqring` when only the named ring existed.

The path is now resolved at `start()` (not construction, because
`applyConfig()` arrives later): the configured path wins when set, and the
previous autodetection is the fallback for an empty field, so the historical
default and its error message are unchanged.

`roce_emu/roce_ring_test` covers this. Against the unfixed reader it fails
with `cannot open /dev/shm/iqring` while pointed at another path.

### 3. `gpuring::kFrameMagic` disagreed with the ABI

`GpuIqPipeline.h` declared `0x4651524d30303031`. `rdma_common.h` defines
`IQ_FRAME_MAGIC` as `0x49515246524d4131` ("IQRFRMA1"), which is what
`RoceShmSource` had always used and what the producers write. The constant was
unused, so nothing was broken — but a frame-header check written against it
would have rejected every frame on the GPU path. Corrected, with a note, and
now asserted by `gpu_math_test`.

### 4. The RoCEv2 ingest path had no automated coverage

TS-01..TS-10 cover firmware, PCIe, the GUI in simulation and the H2C chain.
Nothing touched `RoceShmSource` or either ring ABI. The only way to exercise
RoCEv2 was to cable a ConnectX port to itself, bring up the RDMA stack and
look at the screen — which cannot be a regression gate, and which mixes
several unknowns when nothing appears.

`tools/iqring_sim.c` and `tools/fake_iqring.c` existed as manual aids, but
neither is a test: they publish and leave the verdict to the operator's eye.

## TS-11 — `01_Host_Test_Suite/roce_emu`

122 checks in four binaries, needing no NIC, no RDMA stack, no GPU and no
board — only a writable `/dev/shm`.

| Binary | Checks | Covers |
|---|---|---|
| `roce_ring_test` | 33 | `IQRING01` through the real `RoceShmSource`: production geometry, header adoption, a CW tone measured through `SampleCodec`, newest-slot-wins, and five refusal paths including the GPUDirect ring |
| `roce_tap_test` | 28 | `RTAP`: magic-based detection, all five dtypes, header-supplied rate/centre/channels/full-scale, seqlock tearing, and four malformed-header refusals |
| `gpu_math_test` | 35 | `GpuMathRef.h` against an independent DFT, plus the ring ABI constants |
| `roce_launch_test` | 26 | Starting the receiver from the GUI: discovery of the vendored tree, the unbuilt-binary refusal naming `make`/`make gpu`, the already-publishing refusal, and the watchdog exclusion |

The synthetic producers in `fake_ring.h` are written from the ABI constants
**independently of `rdma_common.h`**, deliberately. `rdma_common.h` guards its
own layout with `_Static_assert`; `fake_ring.h` is the second opinion. If the
two ever disagree a test fails and names the field, whereas an `#include`
would make both sides wrong together and silently. Every synthetic ring writes
its magic last, as the real producers do, so a reader mapping mid-setup sees
an invalid ring rather than a half-built one.

Two properties of the shipping readers are worth knowing before writing more
tests here: `mapRing()` and `mapRoceTap()` both baseline the publish counter
at map time, deliberately, so that attaching to a long-running receiver does
not report its entire history as drops. Frames must therefore be published
**after** `start()` to be seen.

Note also that slot counts in `fake_ring.h` are named `nslots`, never `slots`:
Qt defines `slots` as a macro expanding to nothing, so a parameter with that
name silently vanishes and the errors point at the assignment rather than the
name.

## Still requiring hardware

Everything above runs with no hardware. These remain bench steps, unchanged
from `ROCE_INTEGRATION.md`:

1. Cable ConnectX Port A to Port B and run `scripts/02_setup_network.sh`.
   Both ports previously read `flags=4099` (no `RUNNING`), which is the
   cabling, not a software defect.
2. `scripts/07_selftest.sh` to prove the RDMA stack independently.
3. Point the GUI at the real `rdma_rx` ring and compare the measured tone
   against `rdma_tx`'s configured `fc`.
4. For the GPU ring: `scripts/05_gpudirect_setup.sh` (nvidia_peermem), then
   `rdma_rx_gpu`, with the GUI built `-DSDR_ENABLE_CUDA=ON` — which is what
   defect 1 made impossible. `rdma_rx_gpu` and the GUI must run as the **same
   user**; a CUDA IPC handle cannot cross users. The GUI can now start
   `rdma_rx_gpu` itself, which satisfies that automatically.

Steps 1 and 2 remain manual because they need root and change the host's
network configuration. Step 3 no longer needs a terminal: select RoCEv2 and
press Start, and the GUI builds and starts the receiver if nothing is
publishing.
