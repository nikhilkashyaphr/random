# Comparison against the GNU Radio reference (SDR_RF_Parameter.py)

## Headline: the two numbers in Image A are not equally valid

Image A showed **Fund = 10.006 MHz** (RF METRICS panel) and a spectrum marker
at **−10.177 MHz**. I flagged that 171 kHz gap last round and declined to
tune to either. The reference source resolves it.

**−10.177 MHz is an uninterpolated FFT bin.** It lands on an exact bin of the
grid at every FFT size:

| plot FFT | bin spacing | nearest bin to −10.177 | that bin's frequency |
|---|---|---|---|
| 1024 | 195.557 kHz | −52 | **−10.1689 MHz** |
| 2048 | 97.778 kHz | −104 | **−10.1689 MHz** |
| 4096 | 48.889 kHz | −208 | **−10.1689 MHz** |

It is the coarse, uninterpolated peak-hold marker. **10.006 MHz from the RF
METRICS panel is the accurate figure**, because `compute_metrics()` applies
`_parabolic()` sub-bin refinement.

**Consequence: our GUI was already correct.** Image B showed 2.4400 GHz
against a 2.450 GHz centre — an offset of −10.0 MHz, i.e. ~10.006, not the
−10.177 marker. The DSP was right; only the fictional 2.450 GHz display
origin made it look wrong, and that is already fixed.

## Stage-by-stage comparison

| stage | reference (`SDR_RF_Parameter.py`) | this GUI | verdict |
|---|---|---|---|
| samples | complex64 | complex64 | same |
| Fs | `sample_rate` (200.25e6) | configurable, 200.25 preset added | same |
| frame length | `fft_size` = 4096 | `fftSize` (256..16384) | same |
| averaging | Welch, `avg_frames`=16 | exponential / linear-N / hold | equivalent |
| window | **flat-top** | Blackman-Harris | **ours better for frequency** |
| FFT | `np.fft.fft` + `fftshift` | radix-2 + `fftShift` | same |
| axis | `fftshift(fftfreq(N,1/fs))` | `(k − N/2)·Fs/N` | same |
| DC handling | **zeroes psd[dc ± 8] before argmax** | *was missing* | **adopted** |
| peak refine | `_parabolic` on `log(psd)` | parabolic on dB | same estimator |
| sign | keeps sign; panel shows `abs()` | keeps sign | ours shows sign |

Only two real differences: the window, and the DC guard.

## The flat-top window costs the reference accuracy

Flat-top exists for *amplitude* accuracy — its main lobe is deliberately flat,
which is exactly what makes parabolic frequency refinement poor. Measured with
the reference code itself, true tone −10.006 MHz, N=4096, 16-frame Welch:

| window | NENBW (bins) | reported | error |
|---|---|---|---|
| **flat** (as configured) | 3.77 | −10.0005 MHz | **+5.538 kHz** |
| hann | 1.50 | −10.0052 MHz | +0.756 kHz |
| blackman | 1.73 | −10.0057 MHz | **+0.312 kHz** |

The reference's own frequency error across tones is **±6.5 kHz**; this GUI's
is **±0.16 kHz**. Copying the flat-top window would make our frequency
readout ~35× worse. Recommendation: keep Blackman-Harris for frequency. If
IEEE-1241 amplitude metrics (SFDR/THD/ENOB) are ever added, use flat-top for
*those* and a narrow window for frequency — the reference conflates the two.

## What was adopted: the DC guard (a real defect we had)

The reference zeroes `psd[dc ± dc_guard_bins]` before `argmax`. We had no such
guard, and it mattered — measured, true tone −10.006 MHz, growing DC offset:

| DC offset | before | after |
|---|---|---|
| 0.0 | −10.0058 MHz | −10.0058 MHz |
| 0.5 | −10.0058 MHz | −10.0058 MHz |
| **1.0** | **0.0000 MHz — locked onto DC** | −10.0058 MHz |
| **2.0** | **0.0000 MHz — locked onto DC** | −10.0058 MHz |

LO leakage and ADC offset routinely produce a DC spur taller than the carrier,
which would have made the GUI report 0.000 MHz. The guard width is derived
from the window's ENBW (`ceil(1.5·ENBW)+1`, the same rule the reference uses
for its signal mask) rather than hardcoded to 8 bins, so it scales correctly
if the window is changed.

## Validation (Fs = 200.25 MSPS, N = 4096, Blackman-Harris)

| input | GUI reports | error |
|---|---|---|
| −10.006 MHz (the real signal) | −10.0058 MHz | **0.150 kHz** |
| −10.000 MHz | −9.9999 MHz | 0.063 kHz |
| +10.000 MHz | 9.9999 MHz | 0.063 kHz |
| +20.000 MHz | 20.0001 MHz | 0.070 kHz |
| +50.000 MHz | 49.9998 MHz | 0.156 kHz |
| +90.000 MHz | 89.9999 MHz | 0.079 kHz |

**Worst 0.156 kHz** = 0.003 of a bin, and ~40× better than the reference
metrics block.

## Accuracy limits

| contributor | magnitude |
|---|---|
| bin spacing (Fs/N, N=4096) | 48.889 kHz |
| parabolic refinement residual | **≤ 0.16 kHz** |
| window choice (flat-top vs Blackman-Harris) | 5.4 kHz penalty if flat-top |
| **sample-rate error (dominant)** | **0.125 % → 112 kHz at 90 MHz** |

Sample-rate accuracy dominates everything else by two orders of magnitude. It
is a configuration input, not a DSP limit — enter the converter's measured
clock (200.25, now a preset), not the nominal 200.00.

## Changes this round

* `src/core/DspEngine.cpp` — DC guard before fundamental detection (window-
  derived width); gated `SDR_DEBUG_FREQ=1` pipeline diagnostics.
* `src/ui/Panels.cpp` — 200.00 / 200.25 rate presets + tooltip.

No UI layout, DMA, driver, channel, plotting or architecture change. Clean
build, zero warnings; simulated and offline modes regression-tested.

## Recommendation

**Reproduce the logic, don't share the implementation.** The frequency path is
one equation and this GUI already exceeds the reference's accuracy. Do adopt
the reference's *robustness* ideas — DC guard (done) and Welch averaging
(already available as averaging modes). Do **not** adopt its flat-top window
for frequency work.

The one value that must be shared is the **calibrated sample rate**: read it
from the FPGA clock configuration at start-up so both tools agree
automatically.
