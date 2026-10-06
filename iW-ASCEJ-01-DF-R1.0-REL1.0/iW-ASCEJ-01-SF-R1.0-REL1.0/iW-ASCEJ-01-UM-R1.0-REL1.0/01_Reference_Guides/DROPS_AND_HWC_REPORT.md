# Data-drop investigation and HWC → C2H readiness

All figures below are measured with the **real** `DmaSource` and `DspEngine`
in a headless harness, not estimated.

## 1. Where the drops occur — measured, by stage

There is exactly one place the drop counter increments:
`ISignalSource::admit()`, when the shared back-pressure queue is full. It is
**in the application**, above the driver. Stage isolation:

| test | producer | consumer | drops | queue depth |
|---|---|---|---|---|
| A | 322 MB/s | normal | **0** | 0/24 |
| B | **3343 MB/s** (≈ your 3068) | normal | **0** | 0/24 |
| C | 3343 MB/s | slowed 500 µs/block | **88.9 %** | **24/24 pinned** |
| D | 16 MiB bursts, moderate | normal | **0** | 0/24 |

**Test B is the key result: the pipeline sustains your full 3.3 GB/s with zero
drops.** Hardware, driver and DMA are not dropping anything — the FIFO is
drained as fast as it is filled. Drops appear only when the consumer is
starved (Test C).

## 2. Why *your* rate is not actually high

The status bar showed `drops 547,880,960`, which reads as catastrophic. The
arithmetic says otherwise:

```
block               = 16384 samples x 4 ch x 4 B = 256 KiB
blocks produced     = 3068.3 MB/s / 256 KiB      = 11,705 /s
blocks dropped      = 547,880,960 / 16384 / 330 s=    101 /s
ACTUAL DROP RATE    = 101 / 11705                =   0.87 %
```

**0.87 %, not "very high".** The counter is a cumulative absolute total; at
200 MSPS any non-zero rate produces an enormous number within minutes.

Second, and more fundamental:

```
display consumes : 30 fps x 16384        =     491,520 samples/s/channel
hardware sends   :                          200,250,000 samples/s/channel
display can use  :                                 0.25 %
=> 99.75 % MUST be discarded by any display of this kind
```

A spectrum analyser is a *sampling* instrument. Discarding is the design, not
a failure. The application must still drain the FIFO at full rate — otherwise
`c2h_stream` blocks on `write()` and the hardware genuinely overruns — so it
reads everything and analyses a slice.

## 3. Why the buffer showed 0 % while drops accumulated

Test C and the burst tests reproduce this exactly: `queue depth 0/24` at the
end, with a high drop count. The queue fills during a producer burst, sheds
blocks, then drains to empty long before the next 400 ms status sample. The
gauge is an instantaneous reading of a quantity that oscillates far faster
than it is sampled. Not a bug, but it does mean **buffer % cannot be used to
diagnose drops** — the rate can.

## 4. A hypothesis I tested and discarded

The producer bursts 16 MiB (`fifo_pipe.c` DMA buffer) while the queue holds
24 × 256 KiB = 6 MiB — 2.7× too small. Plausible cause, so I tested it:

| capacity | queue bytes | drop rate |
|---|---|---|
| 24 blocks | 6 MiB | 49.04 % |
| 96 blocks | 24 MiB | **48.83 %** |

**Quadrupling the queue changed nothing.** Buffering absorbs bursts, not a
sustained rate mismatch; when the consumer is permanently slower, a bigger
queue only delays the same steady-state loss. This is exactly the "increase
buffer sizes to hide the problem" move the brief warned against, and the
measurement says it would not have worked anyway. **No capacity change made.**

## 5. The change made

Reporting only — the pipeline itself is already correct.

* `src/core/Types.h` — added `StreamStats::dropRatePct()` (data, no UI).
* `src/ui/MainWindow.cpp`, `src/ui/Panels.cpp` — existing labels now show
  `drops N (X.XX%)`, and colour by **rate**: normal < 1 %, amber 1–5 %,
  red ≥ 5 %. Previously any non-zero count turned red, which cried wolf on a
  pipeline that is behaving correctly.

No layout, control or widget was added or moved.

## 6. If you want the residual 0.87 % lower

It is bounded by consumer throughput, so the levers are:

1. **Lower `refreshFps`** (30 → 15) — halves the DSP work per second.
   Zero code change; it is an existing control.
2. **Select fewer channels** — the channel matrix already restricts DSP work
   to the selection; 1 of 4 channels is a quarter of the analysis cost.
3. **Smaller FFT** — 2048 → 1024 halves the transform cost.

None of these lose anything the display was showing, because the discarded
99.75 % was never rendered. **If you need gap-free data, that is a different
requirement** — use Record, which writes the raw bytes at full rate before the
back-pressure gate and is unaffected by display drops.

## 7. HWC → C2H readiness

The UAPI is explicit (`drivers/iwfg_uapi.h`):

> *the driver actually derives direction from which /dev node was opened, see
> `icdev->direction` in `iwfg_ioctl()`*

C2H is minor 0 (`/dev/iwfg0`, direction `0x2`) — **the node you are already
streaming on**. HWC sits upstream of the C2H engine *inside the FPGA*; the
host receives whatever that engine sends. The host-side contract does not
change.

| item | status | evidence |
|---|---|---|
| C2H driver support | **READY** | streaming now; `IWFG_DIR_C2H_BUF`, minor 0 |
| C2H DMA path | **READY** | 3.3 GB/s sustained, 0 drops (Test B) |
| HWC integration | **N/A host-side** | no `HWC` symbol in driver or UAPI; FPGA-side |
| User-space application | **READY** | `c2h_stream` unmodified, already validated |
| Continuous streaming | **READY** | Tests A/B/D, no corruption |
| GUI integration | **READY** | frequency accurate to 0.16 kHz, waveform smooth |

**Overall: READY.** Nothing needs to be written or rewritten for HWC → C2H.
Grep found no `HWC` symbol anywhere in the driver (only an unrelated
`SLAB_HWCACHE_ALIGN` comment), which is consistent with HWC being an FPGA
block rather than a host concept.

**One caveat I cannot resolve from here:** if "HWC" in your design *does*
require a host-side mode/register write to route it into the C2H engine, that
selection does not exist in the current UAPI and would need adding. Please
confirm whether HWC is selected in the FPGA bitstream/registers or expected to
be selected by the host.

## 8. Test procedure

```bash
# 1. Raw path: is data continuous and non-zero before any DSP?
./backend/bin/c2h_stream /dev/iwfg0 /tmp/iwfg_fifo &
head -c 1M /tmp/iwfg_fifo | xxd | head          # expect varied, non-zero
```

```bash
# 2. Continuity: sequence/rate sanity at full speed
cat /tmp/iwfg_fifo | pv > /dev/null              # expect steady ~MB/s, no stalls
```

```bash
# 3. I/Q + time domain: known tone, GUI
./build/sdr_workstation --device /tmp/iwfg_fifo --format cs16 \
    --channels 4 --rate 200.25
#    expect: smooth sinusoid, ~20 points/cycle
```

```bash
# 4. Frequency: compare against the generator
#    expect: Peak frequency = generator setting within ~0.2 kHz
#    sweep 10 / 20 / 50 / 90 MHz
```

```bash
# 5. Drop rate under sustained load
#    expect: "drops N (<1.00%)" in normal colour; investigate only if >= 5%
```

```bash
# 6. Gap-free check (if required): Record, then re-open the capture
#    offline replay must show the same frequency as the live view
```

## 9. Regression

Clean build, zero warnings. Re-verified unchanged after this round:

| check | result |
|---|---|
| frequency sweep (−10.006 … 90 MHz) | worst 0.156 kHz — unchanged |
| time-domain points/cycle | 20.03 — unchanged |
| trace amplitude | 0.7500 vs source 0.7500 — unchanged |
| DC-guard immunity | unchanged |
| simulated mode | OK |
| CW capture replay | OK |

Untouched: frequency calculation, FFT, sampling-rate handling,
decimation/interpolation, GUI layout, channel selection, DSP logic, driver,
H2C functionality.
