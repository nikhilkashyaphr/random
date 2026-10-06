# SDR Signal Analysis Workstation

A Qt-based RF signal analysis workstation with two fully interchangeable
acquisition paths — **live streaming from a PCIe DMA device node or FIFO**,
and **offline replay of recorded captures** — feeding one shared DSP and
visualisation pipeline.

```
┌────────────────────┐   SampleBlock    ┌───────────┐  FrameResult  ┌────────────┐
│ ISignalSource      │ ───────────────► │ DspEngine │ ────────────► │ MainWindow │
│  ├ SimulatedSource │  (acq thread)    │(dsp thread)│ (queued sig) │  4 plots   │
│  ├ FileSource      │        │         └───────────┘               │  4 docks   │
│  └ DmaSource       │        ▼                                     └────────────┘
└────────────────────┘   ┌──────────┐
         │               │ Recorder │  raw wire-format bytes + .meta sidecar
         └─────────────► │(rec thread)
                         └──────────┘
```

## Building

Interpolation and decimation both default to 1.

Requires CMake ≥ 3.16 and Qt 5.15+ or Qt 6 (Widgets only — no QCustomPlot,
Qwt, or FFTW dependencies).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/sdr_workstation
```

Builds warning-free with `-Wall -Wextra -Wpedantic` on both Qt major
versions. Live DMA acquisition uses POSIX I/O; on non-POSIX platforms the
DMA source reports a clear error and the other two modes work normally.

## Running

Launching with no arguments shows a configuration dialog. All of it can be
bypassed from the command line:

```sh
./build/sdr_workstation --skip-config                  # simulator, defaults
./build/sdr_workstation --file cap.bin --format cs16 --channels 2
./build/sdr_workstation --device /dev/iwfg0 --format cs16 --channels 2 --rate 122.88
./build/sdr_workstation --skip-config --capture ui.png --capture-delay 5000
```

`--capture` renders the window to a PNG after settling and exits — combined
with `QT_QPA_PLATFORM=offscreen` it gives reproducible headless screenshots
for CI.

## Live mode (PCIe DMA)

Select **Live — PCIe DMA / FIFO**, point *Device / FIFO* at your DMA
character device or a named pipe, and press **Start**. The source:

- opens with `O_NONBLOCK` (a FIFO with no writer would otherwise block
  `open()` and freeze the UI),
- `poll()`s while idle and falls back to blocking reads on character
  devices that return `POLLNVAL`,
- accumulates partial reads into fixed-size blocks, so short DMA
  completions never corrupt sample framing,
- services Stop/Pause/config changes between read slices by re-posting its
  pump through the thread's event loop rather than spinning.

Try it without hardware:

```sh
mkfifo /tmp/iwfg0
python3 tools/make_test_capture.py /tmp/sig.bin --channels 2 --format cs16
cat /tmp/sig.bin > /tmp/iwfg0 &          # stand-in for the DMA engine
./build/sdr_workstation --device /tmp/iwfg0 --format cs16 --channels 2
```

### Adapting to real hardware

`DmaSource` in `src/core/Sources.cpp` reads a plain byte stream. If your
fabric prepends per-transfer descriptors or uses ioctl-driven ring buffers,
that file is the only place to change; everything downstream consumes
`SampleBlock`s and does not care where the bytes came from.

## Offline mode

Select **Offline — capture file**, browse to a recording, Start. Features:
loop-at-end, paced replay at nominal rate (or any speed multiple, or
free-run), sample-aligned seeking from the position slider, and a header
byte skip for files with a fixed prefix.

Opening a file that has a `.meta` sidecar restores its sample rate, wire
format, channel count, centre frequency, NCO, decimation and interpolation
automatically. This applies to **every** route into offline mode — the browse
button, the File menu, and the `--file` command line — so a capture always
replays with the settings it was taken with. Without it, a file recorded at a
decimated rate is replayed and down-converted against whatever happens to be
on screen, which smears the constellation into concentric rings.
Recordings made by the app always carry one; `tools/make_test_capture.py`
writes one too.

## Data types

Named after the GNU Radio stream types they correspond to. Complex formats
interleave `ch0.I ch0.Q ch1.I ch1.Q …` per sample-time, little-endian; real
formats carry one scalar per sample and are lifted into the complex pipeline
with Q = 0 (their spectrum is conjugate-symmetric, which is a property of the
data, not a display artefact).

| name | tag | bytes / sample |
|------|-----|----------------|
| Complex Float 32 | `fc32` | 8 |
| Complex Float 64 | `fc64` | 16 |
| Complex Int 32 | `sc32` | 8 |
| Complex Int 16 | `sc16` | 4 |
| Complex Int 12 (packed) | `sc12` | 3 |
| Complex Int 8 | `sc8` | 2 |
| Complex Byte (uint8) | `uc8` | 2 |
| Float 32 | `f32` | 4 |
| Float 64 | `f64` | 8 |
| Int 32 | `s32` | 4 |
| Short 16 | `s16` | 2 |
| Byte (int8) | `s8` | 1 |
| Byte (uint8) | `u8` | 1 |

Format combos bind the enum as item *data*, not row index, so the list can be
reordered or extended without silently reinterpreting a saved configuration.

Captures hold **post-DDC samples at the decimated (display) rate**; the
configured ADC rate and decimation exist so the axes and paced replay know
what that rate is. The recorder writes the same layout it received, so a
recording replays byte-identically.

## Signal processing

- **DDC**: complex NCO mix, phase derived from the absolute sample index so
  display-throttled (dropped) blocks cannot rotate the constellation.
- **Spectrum**: radix-2 FFT, 7 windows (Rect/Hann/Hamming/Blackman/
  Blackman-Harris/Flat-top/Kaiser) with per-window coherent-gain correction
  so a full-scale tone reads 0 dBFS under any of them.
- **Averaging**: off, exponential, linear-N, max hold, min hold. Linear-N
  averages in *linear power* — averaging decibels biases the result low.
- **Measurements** per channel: peak level/frequency (parabolic sub-bin
  interpolation), median-based noise floor, SNR, 99 % occupied bandwidth,
  channel power, RMS, peak amplitude, PAPR, EVM, phase error, DC offset,
  I/Q imbalance, and an N-deep peak table with delta-to-P1.
- **Constellation**: envelope-gated symbol capture at the symbol instant,
  static 4th-power phase alignment, density-graded persistence display.

## Plots

All four plots share one interaction model:

| action | effect |
|---|---|
| wheel | zoom X (Ctrl = Y, Shift = both) |
| left-drag | pan |
| Shift-drag / middle-drag | box zoom |
| Ctrl+click | drop marker (up to 4, with Δ readout) |
| double-click | reset view |
| right-click | context menu (markers, crosshair, reset) |

Views are held in data units, so a zoomed region stays put while frames
stream underneath. Spectrum and waterfall X axes are linked. The waterfall
scrolls newest-first with the history axis counting downward.

## Responsiveness

The pipeline is designed so the window stays interactive when the machine is
saturated:

- **Render gate.** The DSP stage checks a shared atomic before posting a
  frame and skips while the GUI still owes a paint. Without it the engine
  outruns the compositor, and the queued-connection backlog monopolises the
  GUI thread's event loop and starves its timers.
- **Frame coalescing.** Frame arrival only marks state dirty; painting runs
  on a GUI-owned timer at the configured refresh rate, so a burst of frames
  costs one repaint rather than one repaint each.
- **Bounded analysis window.** A block at 122.88 MSPS holds a quarter-million
  samples per channel, but the display consumes one FFT and a 2048-point
  trace. Only a window is down-converted and measured, as a real analyser
  does. Throughput accounting and recording still see the full stream.
- **Waterfall ring buffer.** Scrolling advances a cursor and writes one row;
  paint stitches the ring with two blits. Previously every frame memmoved the
  whole image.
- **Worker threads run below the GUI thread**, so a saturated pipeline loses
  frames rather than the UI.
- **Envelope rendering for dense traces.** When a time-domain trace carries
  more than ~2 samples per pixel column, it is drawn as a per-column min/max
  envelope — antialias-free vertical lines, exactly as an oscilloscope
  renders. The naive antialiased polyline is catastrophically slow for
  high-frequency content (AA cost scales with the area each segment covers;
  a fast waveform makes every segment span the full plot height — measured
  at 0.6–2.4 s *per paint* in the field). The envelope is O(plot width)
  regardless of density and is more faithful: a decimated polyline aliases,
  min/max preserves the true extremes. The spectrum trace similarly takes
  the loudest bin per pixel column, which is what a swept analyser shows.

### Simulator and sample rate

Sample rate drives span, RBW, the time base, and file replay pacing. The
built-in **simulator** tracks the configured rate only up to a 16 k-sample
block ceiling — above roughly 1 MSPS a software generator cannot synthesise
in real time, and generating more than the DSP analyses only starves the GUI.
The status panel therefore reports **measured** rate next to **configured**
rate rather than pretending they match. File and DMA sources carry the true
rate and are unaffected.

## Hardware pre-flight and driver management

Selecting **GPU** in the launcher probes NVIDIA availability on the spot
(driver, CUDA, model, memory) and falls back to CPU with a clear notice when
no usable GPU is present.

Selecting **PCIe** for a live session runs a pre-flight before the main
window opens: iwfg module presence and version, device-node existence and
readability, PCI enumeration (vendor/device ID, current and maximum link
speed and width, BARs), presented as a brief link-discovery screen that
auto-continues on success. On failure the launcher stays open with the exact
reason, and a "Fix driver" action drives the integrated driver manager:
dependency check → out-of-tree `make` in `drivers/` → `pkexec insmod` →
node re-validation. The module is inserted by exec'ing `insmod` directly
under polkit — never via a shell with an interpolated path, and never
silently. Drop the `iwfg_SDR_Driver` sources into `drivers/` to enable the
self-contained build (see `drivers/README.md`).

## Streaming-rate architecture

Sources publish **undecoded wire bytes**; the DSP stage decodes only after
its display throttle and render gate, so at 800 MB/s roughly 3 % of blocks
ever pay for conversion — previously the decode of every block pinned a full
core. The recorder writes those raw bytes straight to disk, making
recordings byte-identical to the stream at memcpy cost. Dense time-domain
traces render as a per-column min/max envelope (O(width), antialias-free,
as an oscilloscope draws) instead of an antialiased path whose cost scales
with the area every segment covers; the displayed window is aligned to a
rising zero-crossing each frame, so the trace holds still like a triggered
scope instead of re-phasing every update.

## Driver lifecycle (PCIe)

The application manages the iwfg module automatically and leaves the system
clean on exit. Drop the validated `iwfg.ko` into `drivers/`; on PCIe launch
the pre-flight inserts it directly (`insmod iwfg.ko`, source build only as a
fallback), verifies load, and confirms the `/dev/iwfg*` node before enabling
streaming. On exit the pipeline is stopped and descriptors closed first, then
the module is removed (`rmmod iwfg`) — but **only if this session loaded it**.
A module you inserted by hand is left untouched. No Xilinx memory-controller
node is created or expected; it is absent by design in this architecture.

## Channel selection (1-16)

The live panel exposes a channel matrix: set the wire channel count (1-16),
then tick any subset to process and display — a single channel, a pair, a
contiguous range (1-8), or a scattered set (CH1, CH4, CH7, CH10). Presets
cover All / None / Even / Odd / 1-8. The selection is a bitmask carried in
`SourceConfig::channelMask`; the DSP processes only the selected channels, so
picking 1 of 16 costs a sixteenth of the work rather than all-then-discard.
The mask is saved in the record sidecar and restored on replay.

## Ethernet (Mellanox)

Selecting the Ethernet interface detects the first Mellanox adapter (PCI
vendor 0x15b3) and applies the lab configuration — 192.168.1.1, MTU 9000,
link up — via `ip` under polkit, then reports interface, MAC, link state,
MTU and IPv4 in the launcher. A system without a Mellanox NIC gets a clear
status line instead of an error.

## Plot navigation

Every plot (time domain, spectrum, waterfall, constellation) supports:

| action | effect |
|---|---|
| wheel | zoom X (Ctrl = Y, Shift = both) — **unlimited in both directions** |
| left-drag | pan |
| Shift-drag / middle-drag | box zoom |
| double-click | auto fit (resume following the data) |
| right-click | auto fit · zoom in · zoom out · **set axis range…** · markers · crosshair |
| Ctrl+click | drop marker (up to 4, with delta readout) |

Zoom is bounded only by double precision (span 1e-9 … 1e15), so an arbitrary
span can be held indefinitely. Earlier builds snapped the view back to the
data extent on zoom-out *and* re-armed auto-fit, which dragged the user back
to fit-to-screen on the next frame; the override is now set explicitly by the
act of zooming and is only cleared by an explicit auto-fit.

"Set axis range…" accepts an exact `min, max` pair for work that needs a
specific window rather than whatever scrolling lands on.

## Channel selection

The **Select** field in the source panel shows the current selection
(`1,2,5,8` or `All (16)`) and opens a modal chooser with one checkbox per
channel, plus Select All / Clear All / OK / Cancel. Cancel preserves the
previous selection; OK is disabled while nothing is ticked. The selection is
a bitmask (`SourceConfig::channelMask`) applied in the DSP, so only the
chosen channels are processed and plotted, and it is saved to the record
sidecar and restored on replay.

## Display scaling

High-DPI scaling is enabled before `QApplication` is constructed (set
afterwards, the attributes are silently ignored) with `PassThrough`
fractional rounding. Row heights, stepper buttons and status widgets are
sized from font metrics rather than literal pixel counts, so text does not
clip at scaling factors above 1.0.

## Backend integration (live acquisition)

Live mode runs the supplied driver applications **unmodified** and attaches
the workstation's existing `DmaSource` to their FIFO. The workstation does
not re-implement DMA, buffer registration or streaming synchronisation — the
validated applications do that exactly as they do under GNU Radio.

Pressing **Start** in live mode shows a mode popup:

| mode | processes launched | data path |
|------|--------------------|-----------|
| `C2H` | `c2h_stream <dev> <fifo>` | card → FIFO → workstation |
| `H2C_C2H` | `iwfg_h2c` then `c2h_stream` | FIFO → card → FIFO → workstation |

Playback starts before capture in loopback mode, so the card is consuming
before the capture side attaches.

### Architecture

`src/core/BackendLauncher.{h,cpp}` owns the process lifecycle and contains
**no UI code** — it reports everything through Qt signals (`statusMessage`,
`logLine`, `becameReady`, `startFailed`, `processFailed`, `stopped`), so the
UI layer renders diagnostics however it likes and the backend is testable
headlessly. The only frontend change is the mode popup and the Start/Stop
bridge; every existing screen, widget and interaction is untouched.

* **Non-blocking.** `start()` returns immediately; readiness arrives by signal.
* **Graceful stop.** SIGINT first — both applications install handlers that
  unwind the DMA path and close the device cleanly — escalating to SIGTERM
  then SIGKILL only on timeout.
* **No orphans.** The destructor stops everything; an orphaned capture app
  would keep `/dev/iwfg0` open and break the next run.
* **Failure recovery.** A stage dying unexpectedly is reported with its exit
  code and the app's own stderr text, and the rest of the pipeline is torn
  down rather than left half-running.
* **Auto-build.** Helpers are built on first use via `backend/Makefile`.

`backend/` holds the application sources (`C2H/fifo_pipe.c`,
`H2C/iwfg_h2c.c`) and their headers; `make` there produces `bin/c2h_stream`
and `bin/iwfg_h2c`. The GNU Radio reference flows are kept alongside for
comparison. The RF-parameter module is untouched.

## Back-pressure and integrity

Source and DSP share an atomic in-flight block counter. When it reaches
capacity the source **drops the block and counts the dropped samples** —
the same overrun semantics a real DMA ring gives you — and the status panel
shows buffer depth, drops, and display frames throttled separately, so "the
GUI is skipping frames" is never conflated with "samples were lost".

## Layout

Four dockable panels (source & signal chain, display & analysis,
acquisition status, measurements) around a 2×2 plot grid; every dock
floats, moves, or closes, plot visibility is toggleable, four layout
presets are built in, and geometry persists across runs via QSettings.

## Hardware integration

### GPU detection
Selecting **GPU offload** in the launcher probes the machine immediately:
`nvidia-smi` (2.5 s hard timeout) for model, memory, driver version and
compute capability, `/proc/driver/nvidia/version` for the driver, and the
CUDA runtime library/toolkit on disk. Success shows the full summary; any
failure states the reason and falls back to CPU automatically, so a mode
that cannot work is never launched.

### PCIe pre-flight and link discovery
Whenever a session starts in PCIe mode, a ~1 s initialization screen
validates the stack before the workstation opens: PCIe link status, current
vs max link speed and width, device enumeration (BDF), vendor/device IDs,
BAR regions, iwfg kernel module + version, device node accessibility, and
DMA engine readiness — all read from sysfs/procfs without privileges. On
success it transitions automatically; on failure it stays up with a one-line
reason instead of letting acquisition start against a broken configuration.
A named FIFO passes with only the node check, so loopback testing needs no
hardware. Headless capture of this screen: set `SDR_CAPTURE_DISCOVERY=1`
with `--capture`.

### Integrated driver management (`drivers/`)
The package carries the iwfg driver in `drivers/` (see the README there for
what to drop in). When PCIe pre-flight finds the module missing and sources
are bundled, the discovery screen offers **Build & load driver**, which runs
the chain: dependency check (make, compiler, kernel headers — each missing
one reported with its install command) → out-of-tree `make` → `pkexec
insmod` → node re-validation, with per-step diagnostics on failure. Without
a polkit agent the exact `sudo insmod` command is shown instead — the
application never escalates privileges silently.

## Tools

`tools/make_test_capture.py` — synthesises multi-channel QPSK/16-QAM/64-QAM
or noise captures in any supported wire format, with burst gating, carrier
offset, AWGN and a matching `.meta` sidecar. Needs NumPy.
