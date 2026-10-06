# Resource monitoring, recording safety, and H2C controls

## Platform note (important)

The specification is written against Windows performance counters. **This
application runs on Ubuntu.** The equivalent primary sources are used and
named; the *formulas* are identical, only the APIs differ. Where the spec says
"validate against Task Manager", the equivalent here is `top`, `free` and
`df`, and that cross-check was performed (§Validation).

| metric | Windows (spec) | this build (Linux) |
|---|---|---|
| CPU | `PDH` / `GetSystemTimes` | `/proc/stat` aggregate deltas |
| RAM | `GlobalMemoryStatusEx` | `/proc/meminfo` MemTotal/MemAvailable |
| GPU | WDDM counters | `nvidia-smi` query |
| Disk | `GetDiskFreeSpaceEx` | `statvfs()` on the recording directory |
| Process | Working Set | `/proc/<pid>/statm` resident pages |

## 1. Menu-bar resource monitor

Occupies the previously empty menu-bar space beside the H2C button:

```
CPU 65%    RAM 9%  375.1 MiB/3.9 GiB    GPU n/a    [H2C CONTROL]
```

No new colours, fonts or shapes — existing `hintLabel` typography, and state
carried by `theme::textDim / warn / danger` from the existing palette.

## 2. Formulas, and why

**CPU** — delta between successive `/proc/stat` samples, never cumulative
totals:

```
busy  = (total − idleAll) − (totalPrev − idleAllPrev)
total = Σfields − ΣfieldsPrev
cpu%  = 100 × busy / total          idleAll = idle + iowait
```

Already normalised across logical processors: the kernel's aggregate line sums
jiffies over every CPU and the divisor scales with core count, so the classic
"sum of per-core percentages" error cannot occur. The first sample only
establishes a baseline and reports `valid=false` rather than a fake 0 %.

**RAM** — uses `MemAvailable`, not `MemFree`:

```
used% = 100 × (MemTotal − MemAvailable) / MemTotal
```

`MemFree` excludes reclaimable page cache, which overstates consumption badly.
`MemAvailable` is the kernel's own estimate of what a new allocation could
obtain, and is what `free` reports. **Used + Available == Total holds by
construction**, and the percentage is computed from the same bytes it is shown
beside, so the UI cannot display inconsistent values.

**Disk** — `statvfs()` on the **recording directory**, and `f_bavail` rather
than `f_bfree`: the difference is the root reserve, which a recording process
running as a normal user cannot write into.

**Process memory** — `statm` resident pages (the Linux analogue of Working
Set) and virtual size are reported as **separate fields** and never summed or
interchanged.

**GPU** — per-adapter utilisation and memory from `nvidia-smi`. When no driver
is present the metric is simply absent and the bar shows `GPU n/a`. **No value
is ever inferred from CPU or process activity.**

## 3. Validation against system tools

```
this build : RAM 10.5%   used 420.2 MiB / total 3.9 GiB   avail 3.5 GiB
free(1)    : 10.3%       (sampled moments apart)
  consistency used+avail==total : PASS
  pct matches displayed bytes   : PASS

this build : /tmp free 9.8 GiB of 252.0 GiB
df(1)      : avail 10561777664 B = 9.8 GiB          MATCH

GPU        : 0 adapters -> "n/a", not fabricated
```

The small RAM difference is sampling latency between two independent reads,
not a formula error — documented rather than "fixed" by fudging.

## 4. Overhead

Per 1 s tick: two small `/proc` reads and one `statvfs`. `nvidia-smi` is a
process spawn, so it runs **every 5th tick only**. The poll interval is
floored at 250 ms so a caller cannot spin the event loop.

## 5. Recording storage limit

Prompted **before** recording starts, in MB, defaulting to half the safe
capacity of the chosen volume. Validated against the destination's real free
space:

```
safe capacity = free − max(2 GiB, 5 % of volume)
```

The reserve is the greater of a fixed floor and a proportion, so the rule
behaves on both a 32 GiB stick and a 16 TiB array.

Tested (100 GiB volume, 20 GiB free → 15.0 GiB safe):

| input | result |
|---|---|
| 5000 MB | **accepted** |
| 0 MB | CRITICAL — invalid limit |
| −100 MB | CRITICAL — invalid limit |
| 10,000,000 MB | CRITICAL — insufficient storage |
| 13,000 MB | WARNING — close to available capacity |
| bad directory | CRITICAL — directory unavailable |
| unreadable disk | WARNING — free space undeterminable |

Critical findings **block** recording; warnings offer Ok/Cancel. Every message
states what is wrong, why it matters, and the recommended fix.

## 6. Predictive warnings

```
growth      = Δbytes / Δtime, exponentially smoothed (0.7/0.3)
remaining   = limit − session
eta         = min(remaining, safeCapacity) / growth
```

States: **Normal → Warning (80 %) → Critical (95 % or < 60 s) → Limit
Reached**. Hitting the limit emits `limitReached`, which stops the recording
cleanly rather than letting it consume the disk.

Warnings are **stateful**: an id is emitted once when it becomes active and
again only if its severity changes, so a 1 Hz poll cannot spam. Verified — each
warning appeared exactly once across a 12-step run.

**ETA honesty:** no estimate is shown until the smoothing has settled and the
rate is real. Before that the UI shows `Estimating…`; a stalled recording
shows `Insufficient data`, never an infinite time.

## 7. Two bugs found and fixed during testing

* **"Estimating…" never cleared.** The reliability gate compared
  `m_clock.elapsed()` against a 3 s wall-clock threshold, but the first delta
  covers the whole elapsed period, so a fast poll had a valid rate long
  before. Worse, a slow first write could make a long session look unreliable
  forever. Now gated on **sample count** (≥ 3), which is what the smoothing
  actually needs. Verified: ETA now counts down 2.8 s → 0.4 s.
* **ETA displayed "0 s" while nearly a second remained.** `int()` truncation.
  Sub-minute values now carry one decimal.

Also fixed: `qproperty-alignment` on `QRadioButton`, which Qt logged as a
warning on every launch (visible in the user's terminal) — `QRadioButton` has
no `alignment` property.

## 8. H2C control window — full parameter set

Previously only the three live `iq_ctl` fields were exposed. Now two tabs:

**Transmit (live)** — written to `/dev/shm/iqctl`, applied by `rdma_tx` on its
next poll: sample rate, centre frequency, pacing (with explicit *Unlimited*).
A derived line shows the resulting output rate, transmit centre and span.

**Signal chain (applied on restart)** — NCO/DUC (signed), interpolation,
decimation, channels, wire format, chunk size.

The split is deliberate and labelled: `rdma_tx` accepts only three fields
live, so presenting the rest as instantaneous would imply an immediacy the
backend does not provide. The signal-chain values are emitted via
`signalChainChanged()` for the session to apply at (re)start.

## 9. Regression

Clean build, zero warnings. Verified unchanged: main window, offline CW
replay, H2C gating across all three data-flow modes, hardware validation (9
cases), per-board channel lists, frequency accuracy.

## 10. Known limitations

* GPU telemetry is NVIDIA-only (`nvidia-smi`). AMD/Intel adapters report
  `n/a` rather than a fabricated figure.
* Per-process CPU% is not yet populated — `/proc/<pid>/stat` deltas need a
  second sample per process, and the per-process view is on-demand. RSS and
  virtual size are correct and separated.
* File rollover / multiple output files are not modelled; the guard tracks a
  single session's byte count as reported by the recorder.

---

## 11. Threshold highlighting and crossing alerts

### Visibility

The bar was dim grey (`textDim`) and too faint to read at a glance, which
defeated the point of adding it. Each metric is now a coloured pill with bold
text, drawn **from the existing palette only** — no new theme:

| state | colour (Theme.h) | meaning |
|---|---|---|
| Normal | `theme::ok` green | within thresholds |
| Warning | `theme::warn` amber | crossed the warn line |
| Critical | `theme::danger` red | crossed the critical line |
| Unavailable | `theme::textDim` grey | no telemetry — **not** an alarm |

Each metric is coloured by **its own** level, so a CPU spike does not recolour
memory. GPU with no driver stays dim grey rather than green, because "no
telemetry" is not "healthy".

### Documented thresholds

| metric | warn | critical |
|---|---|---|
| CPU | 85 % | 95 % |
| RAM | 85 % | 95 % |
| GPU | 90 % | 98 % |
| Disk | 85 % | 95 % |

Held in `ResourceThresholds`, adjustable via `setThresholds()`, not scattered
as magic numbers.

### Crossing alerts — with hysteresis

Crossing a line raises a status-bar message naming the metric, the value, the
threshold and a recommended action.

The important detail is **hysteresis**: a metric must fall `releaseMargin`
(3 %) *below* a threshold before its level may drop again. Without it, a value
hovering on the boundary flaps and emits an alert every single poll — the
classic cause of alert spam.

Verified against an oscillating value:

```
thresholds: warn 85%  crit 95%  release margin 3%

  84.0%  -> NORMAL
  85.5%  -> WARNING   <-- ALERT
  84.5%  -> WARNING            (held: above 82% release line)
  85.2%  -> WARNING
  84.8%  -> WARNING
  85.9%  -> WARNING
  83.0%  -> WARNING
  81.5%  -> NORMAL    <-- ALERT

  alerts emitted: 2      without hysteresis: 6      PASS
```

Escalation and de-escalation both transition cleanly:
`NORMAL → WARNING → CRITICAL → WARNING → NORMAL`, one alert each, none
repeated while a level is held.

### Bug fixed while testing

The pill styling added horizontal padding that the label's size hint did not
account for, so "3.9 GiB" clipped to "3.9 Gi". Minimum width is now computed
from the rendered text via `QFontMetrics` on each refresh.

---

## 12. Relocated to the toolbar row

The resource metrics and the H2C button were moved out of the menu bar and
onto the toolbar, immediately left of the run-state pill:

```
[Start] [Pause] [Stop] │ [Record] [Open capture…] │ QDMA FPGA · PCIe
                            … │ CPU 74% │ RAM 10% 387.6 MiB/3.9 GiB │ GPU n/a │ [H2C CONTROL] │ [RUNNING]
```

Reasons this is the better placement:

* The menu bar is a text row sized for menu items; a pill button and a metrics
  strip crowded it and were vertically cramped.
* The right-hand end of the toolbar already carries the run-state pill, so the
  metrics, the H2C control and RUNNING now read as **one status cluster**
  rather than being split across two rows.
* The menu bar returns to File / Acquisition / View / Help only.

### Bug fixed during the move

`buildToolBar()` runs at construction line 91, but the widgets were being
created at line 115 — so they were still null when the toolbar was assembled
and simply never appeared. Creation now happens **before** `buildToolBar()`.
Two fragments of the old menu-bar code (a stray closing brace and an orphaned
comment block) were also removed rather than left as dead code.

### Verified after the move

| check | result |
|---|---|
| metrics + H2C on toolbar | present, grouped with RUNNING |
| menu bar | File / Acquisition / View / Help only |
| H2C gating, ADC Only | **disabled** |
| H2C gating, DAC Only / ADC + DAC | enabled |
| offline CW replay | pass |
| build | clean, zero warnings |
