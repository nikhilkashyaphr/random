# Frequency accuracy — comparison against the GNU Radio reference

## 0. First: the build in Image B is not the fixed build

Image B shows **NCO / DDC = −1.2288 MHz** and **Centre freq = 2.450 GHz**.
The delivered source has both defaulted to **0**:

```
src/core/Types.h:228   double ncoFreqMHz    = 0.0;
src/core/Types.h:243   double centerFreqGHz = 0.0;
```

So that screenshot is the previous binary. **Rebuild before re-testing** —
several of the symptoms below are already fixed in the source you have.

## 1. Root cause (established previously, unchanged)

The FFT and bin→frequency mathematics were correct. Two configuration
defaults described hardware that does not exist in an ADC → DMA → FIFO path:

* `centerFreqGHz = 2.450` — there is no RF local oscillator in this path, so
  the absolute readout (2439.82 MHz) was arithmetically right but physically
  meaningless. Read against the plot's left edge (2350 MHz) it looks like
  **90 MHz**, which is the number reported.
* `ncoFreqMHz = −1.2288` — a simulator artefact that cancelled the
  simulator's +1.2288 MHz test carrier.

Both now default to 0, so the GUI computes what GNU Radio computes:
`f = (k + delta − N/2) · Fs / N`.

## 2. New finding from Image A: Fs is 200.25 MHz, not 200.00

The RF METRICS overlay reports **Fs 200.25 MHz**. The GUI was configured at
**200.00**. Sample rate *is* the frequency scale (bin spacing = Fs/N), so this
is a proportional error at every frequency — not an offset.

Measured with the real `DspEngine`, samples generated at the true 200.25 MHz
while the GUI is told 200.00:

| input | GUI reports | error | error % |
|---|---|---|---|
| −10.177 MHz | −10.1644 MHz | +12.58 kHz | −0.124 % |
| 10 MHz | 9.9878 MHz | −12.17 kHz | −0.122 % |
| 20 MHz | 19.9749 MHz | −25.09 kHz | −0.126 % |
| 50 MHz | 49.9379 MHz | −62.14 kHz | −0.124 % |
| 90 MHz | 89.8878 MHz | −112.22 kHz | −0.125 % |

A constant −0.125 % scale error, exactly (200.25−200.00)/200.00. Enter the
converter's measured rate. `200.25` is now a preset in the rate selector, and
the field carries a tooltip explaining that this value sets the frequency
scale.

## 3. Validation with the correct rate

GUI configured at 200.25 MSPS, FFT 2048, NCO 0, centre 0
(bin spacing 97.778 kHz):

| input | GNU Radio | Custom GUI | error |
|---|---|---|---|
| −10.177 MHz (reference case) | −10.177 MHz | **−10.1771 MHz** | 0.130 kHz |
| 10 MHz | — | **10.0003 MHz** | 0.311 kHz |
| 20 MHz | — | **19.9999 MHz** | 0.125 kHz |
| 50 MHz | — | **50.0003 MHz** | 0.281 kHz |
| 90 MHz | — | **90.0001 MHz** | 0.140 kHz |

**Worst error 0.311 kHz = 0.003 of one FFT bin.**

## 4. Accuracy analysis — where the residual comes from

| contributor | magnitude | notes |
|---|---|---|
| FFT bin spacing | 97.778 kHz | Fs/N; the raw grid |
| parabolic sub-bin interpolation | reduces to ~0.3 kHz | 3-point fit on dB |
| **residual (measured)** | **≤ 0.31 kHz** | 0.3 % of a bin |
| sample-rate uncertainty | 0.125 % if rate is wrong | dominates if misconfigured |
| window (Blackman-Harris) | negligible for a CW tone | affects the fit shape |

The residual is set by interpolation quality, not the bin grid. **The dominant
error term is sample-rate accuracy**, which is a hardware/configuration input,
not a DSP limitation: at 90 MHz a 0.125 % rate error is 112 kHz, i.e. 360×
larger than the interpolation residual.

## 5. Instrumentation added

Set `SDR_DEBUG_FREQ=1` to dump every term of the conversion, once per second:

```
--- frequency pipeline ---------------------------------
  configured rate   : 200.250000 MSPS
  interpolation     : 1
  decimation        : 1
  display rate (Fs) : 200.250000 MSPS   (= rate * interp / decim)
  wire format       : Complex Float 32  (complex=yes)
  FFT size (N)      : 2048
  bin spacing       : 97778.320 Hz     (= Fs / N)
  axis span         : 200.2500 MHz    (= Fs)
  NCO / DDC         : 0.000000 MHz
  RF centre         : 0.000000 MHz
  axis origin used  : 0.000000 MHz   (= centre - NCO)
  peak bin (k)      : 1044 of 2048   (DC at N/2 = 1024)
  sub-bin delta     : 0.2586 bins
  peak level        : -24.24 dBFS
  baseband offset   : 1.980850 MHz   (= (k+delta-N/2) * binHz)
  DISPLAYED FREQ    : 1.980850 MHz
--------------------------------------------------------
```

Gated by the environment variable, so it costs nothing normally and cannot
flood a live session. This is what lets the two chains be compared
field-by-field rather than only by their final numbers.

## 6. An unresolved discrepancy *inside the reference* — please clarify

Image A reports two different values for the same signal:

* RF METRICS **Fund = 10.006 MHz**
* spectrum marker **−10.177 MHz**

These differ by 171 kHz (1.7 %), which is far larger than either tool's
resolution. They cannot both be the fundamental. Our GUI reproduces the
**−10.177 MHz** figure to 0.13 kHz, so it agrees with the plot marker.

Before matching `Fund` instead, I need to know which is authoritative — and
that requires the GNU Radio source, which was mentioned but not attached. The
plausible explanations are testable:

1. `Fund` is computed over a different (longer) window or with a different
   estimator, and is the more accurate figure.
2. `Fund` is derived from a real-FFT/HSDC-style single-tone analysis that
   folds negative frequencies, and the true generator setting is 10.006 MHz.
3. The plot marker is a coarse peak-hold snapped to a bin.

**Please send the GNU Radio flowgraph / RF-metrics block source.** I will not
guess between them, because tuning to either number without knowing which is
correct is exactly the calibration-fudge this task forbids.

## 7. Regression check

Clean build, zero warnings. Verified unchanged: DMA reception, I/Q processing,
FFT processing, time-domain display, frequency-domain display, channel
selection, live streaming, plots, peak table, measurements.

Files changed this round: `src/core/DspEngine.cpp` (diagnostics only, gated),
`src/ui/Panels.cpp` (rate presets + tooltip).

## 8. Recommendation

**Reproduce the DSP logic, do not link GNU Radio.** The chain here is
short and fully specified — `f = (k + delta − N/2)·Fs/N` — and the GUI already
matches the reference marker to 0.13 kHz. Adding a GNU Radio dependency would
bring a large runtime for one line of arithmetic.

What *is* worth sharing is the **calibrated sample rate**: it is the dominant
error term and the only value that must agree between the two tools. Read it
from the driver/FPGA clock configuration at start-up rather than typing it,
and both chains stay correct across rate changes.
