# Time-domain waveform rendering — root cause and fix

## 1. What caused the jagged waveform

**Visualisation downsampling with no anti-alias filter** (items 16 and 17 of
the checklist). The raw samples were fine.

`DspEngine::processBlock()` built the plot trace by spreading
`kMaxTimePoints` picks across the whole analysis window:

```cpp
const int stride = std::max<int>(1, static_cast<int>(avail) / kMaxTimePoints);
for (std::size_t i = trig; i < m_work.size(); i += stride)
    cf.iq.push_back(m_work[i]);
```

With `analysisN` = 16384 and `kMaxTimePoints` = 2048 that gives **stride = 8**:
every 8th sample was plotted.

| quantity | value |
|---|---|
| Fs | 200.25 MSPS |
| tone | 10 MHz |
| raw samples per cycle | **20.02** |
| stride | 8 |
| **plotted points per cycle** | **2.50** |
| effective display rate | 25.03 MSPS (Nyquist 12.52 MHz) |
| tone as fraction of display Nyquist | **80 %** |

A polyline through 2.5 points per cycle cannot be anything but a zigzag. The
data was correct at 20 points per cycle; the plot threw 7 of every 8 away.

A second, independent factor made it worse at full zoom: 2048 contiguous
samples span 10.2 us, i.e. ~102 cycles of a 10 MHz tone across a ~600 px
plot — about 6 px per cycle, which reads as a solid band no matter how smooth
the underlying curve is. That is a **timebase** problem, not a resolution one.

## 2. Exact code changed

`src/core/DspEngine.cpp` only. Three edits:

1. **Contiguous instead of strided sampling.** Take the first
   `kMaxTimePoints` samples after the trigger rather than striding across the
   window. Restores the full sample rate to the trace.
2. **Time axis follows the plotted samples.** `frame.blockSpanSec` is now
   `plottedPoints / Fs` instead of `analysisN / Fs`; otherwise the axis would
   be stretched 4x and misreport the waveform period.
3. **`kMaxTimePoints` 2048 -> 512.** With stride 1 this value *is* the
   timebase (window = points / Fs). 512 points spans 2.56 us, ~26 cycles,
   ~23 px per cycle at ~1 plotted point per pixel.

No other file touched.

## 3. Why this makes it smoother

| metric | before | after |
|---|---|---|
| points per cycle | 2.50 | **20.03** |
| implied sample spacing on the axis | 39.95 ns (8x the truth) | **4.9938 ns = the true sample period** |
| cycles across the plot | 102 | **25.6** |
| pixels per cycle (~600 px plot) | ~6 | **~23** |
| max step between adjacent points | ~100 % of pk-pk | **15.6 % of pk-pk** |

20 points per cycle with a 15.6 % maximum step between neighbours is a smooth
curve. Nothing is filtered, interpolated or averaged — the fix only changes
**which** samples are drawn, and the axis label that describes them.

Verified visually: a synthetic CW 10 MHz capture at 200.25 MSPS now renders as
continuous sinusoids with the correct 90 degree I/Q phase relationship.

## 4. Confirmation nothing else changed

Sample values are untouched. The spectrum, constellation and all metrics
still consume the full `analysisN` analysis window — only the time trace uses
the shortened slice.

Re-ran the previous validation suites after this change:

| check | result |
|---|---|
| trace peak / mean amplitude | 0.7500 / 0.7500 vs source 0.7500 — **unchanged** |
| measured frequency | −10.0003 MHz — **unchanged** |
| frequency accuracy sweep (−10.006 … 90 MHz) | worst 0.156 kHz — **unchanged** |
| DC-guard immunity (DC offset 0.5 / 1.0 / 2.0) | −10.0058 MHz throughout — **unchanged** |
| simulated mode | OK |
| offline capture mode | OK |
| build | clean, zero warnings |

Unmodified: FFT processing, frequency-domain display, sampling-rate handling,
decimation/interpolation, DMA acquisition, driver interaction, I/Q processing,
channel selection, live streaming, capture, GUI layout, controls, parameters,
backend logic.

## 5. Note on performance

Strictly cheaper than before: 512 points are copied per channel per frame
instead of 2048, and the plot draws a quarter as many line segments.
