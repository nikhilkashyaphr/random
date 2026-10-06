# RF frequency measurement — root cause, comparison, and correction

## 1. Root cause

**The FFT and bin→frequency mathematics were already correct. The defect was a
frame-of-reference error introduced by two configuration defaults that assert
hardware which does not exist in this data path.**

Driving the real `DspEngine` with a tone at the reference frequency
(−10.177 MHz, 200 MSPS, FFT 2048) under the shipped defaults:

| configuration | reported | offset from centre |
|---|---|---|
| shipped defaults (centre 2.45 GHz, NCO −1.2288 MHz) | 2439.8233 MHz | **−10.1767 MHz** |
| NCO 0, centre 2.45 GHz | 2439.8227 MHz | **−10.1773 MHz** |
| NCO 0, centre 0 (baseband) | **−10.1773 MHz** | −10.1773 MHz |

Every row agrees with GNU Radio's −10.177 MHz to within 0.6 kHz. The DSP was
measuring the signal correctly; it was *labelling* it against a fictional
origin.

### Why the displayed number looked like ~90 MHz

`centerFreqGHz` defaulted to **2.450 GHz** and `ncoFreqMHz` to
**−1.2288 MHz**. Both were simulator-era values: the simulator generated a
carrier at +1.2288 MHz and the NCO exactly cancelled it, so the pair was
invisible in simulation.

On this hardware the path is **ADC → DMA → FIFO**. There is no RF local
oscillator, so a 2.450 GHz centre is an assertion about hardware that is not
present. A tone at −10.177 MHz baseband was therefore reported as
2439.823 MHz — arithmetically right, physically meaningless, impossible to
check against a signal generator. Read against the spectrum plot's left edge
(2350 MHz), 2440 − 2350 = **90 MHz**, which is the number observed.

## 2. Comparison with the reference chains

| stage | GNU Radio | TI HSDC-style capture | This GUI (before) | This GUI (after) |
|---|---|---|---|---|
| samples | complex I/Q from FIFO | raw ADC codes | complex I/Q from FIFO | unchanged |
| sample rate | `samp_rate` = 200 MSPS | ADC Fs from clock tree | 200 MSPS (configured) | unchanged |
| DDC / NCO | none | none (or explicit) | −1.2288 MHz | **0** |
| FFT | N-point complex | N-point | N-point complex | unchanged |
| axis origin | **baseband, 0 Hz** | baseband, or Fs-referenced | **2.450 GHz (fictional)** | **baseband, 0 Hz** |
| axis span | ±Fs/2 = ±100 MHz | ±Fs/2 | ±Fs/2 (correct) | unchanged |
| peak estimate | argmax + interp | argmax | argmax + parabolic interp | unchanged |

The only divergence was the axis origin (and the NCO feeding into it). Span,
FFT, ordering and scaling all already matched — confirmed by the fact that the
*offset from centre* matched the reference in every configuration.

Note the reference peak is at a **negative** baseband frequency. Our I/Q
ordering reproduces that sign, so there is no I/Q inversion or
conjugation error in the chain.

## 3. The equations actually used

FFT bin `k` of an `N`-point complex FFT, after `fftshift` (DC at `N/2`),
sampled at `Fs` with a digital down-conversion of `f_nco`:

```
binHz     = Fs / N
f_meas    = (k + delta - N/2) * binHz          delta = sub-bin interpolation
f_base    = f_meas - f_nco                     undo the DDC translation
f_display = f_centre + f_base
```

with `Fs = sampleRateMsps * 1e6 * interpolation / decimation`, and `delta`
from 3-point parabolic interpolation on the dB spectrum (the log of a windowed
main lobe is near-parabolic, so a dB fit beats a linear-magnitude fit).

With `f_centre = 0` and `f_nco = 0` this reduces to GNU Radio's exact
computation: `f_display = (k + delta - N/2) * Fs / N`.

## 4. Incorrect assumptions found

| assumption | status |
|---|---|
| RF centre 2.450 GHz | **wrong** — no LO in an ADC→DMA→FIFO path. Now 0. |
| NCO −1.2288 MHz | **wrong for hardware** — simulator artefact. Now 0. |
| Sample rate 200 MSPS | correct — GNU Radio's ±100 MHz axis confirms it |
| Decimation / interpolation = 1 | correct |
| Complex (not real) samples | correct |
| I/Q ordering | correct — negative-frequency tone reproduced with right sign |
| FFT size / bin spacing / span | correct |
| Endianness, signedness, scaling | correct — level and SNR agree with reference |

## 5. Changes made

Three files, minimal:

* `src/core/Types.h` — `ncoFreqMHz` 0.0, `centerFreqGHz` 0.0, with rationale.
* `src/ui/Panels.cpp` — centre-frequency spin box lower bound 0 (was
  0.000001 GHz, which made baseband unreachable from the UI); both widgets now
  take their initial value from the config defaults instead of hardcoding
  2.450 / −1.2288.
* `src/core/DspEngine.cpp` — unchanged in this round (carries the previous
  NCO-compensation and sub-bin interpolation fix).

No UI layout, DMA, driver, channel, plotting or architecture change.

## 6. Validation

Real `DspEngine`, reading the same `metrics.peakFreqHz` the GUI displays.
200 MSPS, FFT 2048, bin spacing 97.656 kHz.

**Baseband defaults (centre 0, NCO 0):**

| known (MHz) | displayed (MHz) | error (kHz) |
|---|---|---|
| −10.177 (reference case) | −10.1773 | −0.282 |
| −50.000 | −50.0000 | 0.000 |
| −20.000 | −19.9997 | +0.272 |
| −5.000 | −5.0003 | −0.272 |
| +5.000 | +5.0003 | +0.272 |
| +10.000 | +10.0002 | +0.233 |
| +20.000 | +19.9997 | −0.272 |
| +50.000 | +50.0000 | 0.000 |
| +90.000 | +89.9998 | −0.233 |

**Worst error 0.282 kHz** — 0.003 of one bin, across positive and negative
frequencies out to 90 % of Nyquist.

**With a real front end configured (LO 2.450 GHz, NCO −1.2288 MHz):**

| known baseband | displayed | implied baseband |
|---|---|---|
| −10.177 MHz | 2439.8233 MHz | −10.1767 MHz |
| −20.000 MHz | 2429.9997 MHz | −20.0003 MHz |
| +10.000 MHz | 2459.9997 MHz | +9.9997 MHz |
| +25.000 MHz | 2475.0002 MHz | +25.0002 MHz |

**Worst baseband error 0.276 kHz.** The LO/NCO path stays correct, so an
operator with real up/down-conversion still gets a true RF reading.

## 7. Why this is correct by construction, not by tuning

No constant was added and nothing was calibrated to the 10 MHz case. The two
changes are declarations of what the hardware is:

* `f_nco = 0` — no digital down-conversion is applied, so none is undone.
* `f_centre = 0` — the samples are baseband, so the origin is 0 Hz.

The displayed frequency is then `(k + delta − N/2)·Fs/N`, i.e. exactly the
frequency present in the sampled data, which is what GNU Radio computes.
Because `f_centre` and `f_nco` remain live parameters that enter the equation
symbolically, an operator who *does* have an LO or DDC sets them and the
result stays correct — validated above at four frequencies with both non-zero.

Accuracy is bounded only by bin spacing and interpolation quality; it does not
depend on the particular frequency under test.

## 8. Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Warning-free. Simulated and offline modes regression-tested; plots, channel
selection, peak table and measurements unchanged.
