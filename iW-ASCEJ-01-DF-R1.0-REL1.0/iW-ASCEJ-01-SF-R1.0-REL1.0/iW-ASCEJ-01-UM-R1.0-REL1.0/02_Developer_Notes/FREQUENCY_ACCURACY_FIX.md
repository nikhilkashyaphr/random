# Frequency accuracy — root cause, fix, and validation

## 1. Root cause

`DspEngine::applyDdc()` multiplies the sample stream by `exp(+j·2π·f_nco·t)`,
which **translates the entire spectrum up by f_nco**. A component whose true
baseband offset is `f_b` therefore lands in the FFT at `f_b + f_nco`.

`DspEngine::processBlock()` then published the frequency-axis origin as:

```cpp
frame.centerFreqHz = m_cfg.acq.centerFreqHz();      // RF centre, un-compensated
```

Every consumer of the frequency axis derives its scale from this single value
while treating it as the true RF centre:

| consumer | file |
|---|---|
| "Peak frequency" metric | `DspEngine::computeMetrics()` |
| Detected-peaks table (P1..Pn) | `dsp::findPeaks()` |
| Spectrum plot X axis | `Plots.cpp:260` |
| Waterfall X axis | `Plots.cpp:516` |

So all of them reported `f_b + f_nco` instead of `f_b`. With the default NCO
of −1.2288 MHz that is a constant −1.2288 MHz bias, and it scales directly
with whatever NCO the operator sets — precisely the 1–3 MHz discrepancy
observed against a known carrier.

**Second, smaller defect:** `computeMetrics()` reported the bare `argmax`
bin, quantising the reading to the bin centre (±binHz/2 = ±48.8 kHz at
200 MSPS / 2048). `findPeaks()` already interpolated, so the "Peak frequency"
field and P1 in the peak table disagreed for the same carrier. The ±48.8 kHz
quantisation riding on the 1.2288 MHz bias is what made the error appear to
*vary* rather than sit at a fixed offset.

## 2. The mathematical correction (not a fudge factor)

Undoing a frequency translation is a subtraction of that exact translation:

```
f_measured = f_b + f_nco          (what the FFT sees)
f_b        = f_measured − f_nco
f_RF       = centre + f_b = (centre − f_nco) + f_measured
```

so the axis origin moves by −f_nco:

```cpp
frame.centerFreqHz = m_cfg.acq.centerFreqHz() - m_cfg.acq.ncoFreqMHz * 1e6;
```

No constant, no tuned offset, no scaling. The term is the NCO frequency the
DDC itself applied. Correcting it at this one point keeps the plot, the peak
table and the measurement panel consistent by construction, since all four
consumers read this field.

Sub-bin refinement uses 3-point parabolic interpolation on the dB spectrum —
the same estimator `findPeaks()` already used. Interpolating in dB is
deliberate: the log of a windowed main lobe is near-parabolic, so a 3-point
fit on dB is markedly more accurate than the same fit on linear magnitude.
Residual error is then set by window shape and SNR, not the bin grid.

## 3. Files modified

**`src/core/DspEngine.cpp` only.** Two hunks:

1. `processBlock()` — NCO compensation of `frame.centerFreqHz`.
2. `computeMetrics()` — parabolic sub-bin interpolation of `peakFreqHz`.

No other file in the project differs from the supplied baseline. No UI, DMA,
driver, channel, plotting, configuration or architecture change.

## 4. Validation

Synthetic tones at known offsets driven through the **real `DspEngine`**,
reading the same `metrics.peakFreqHz` the GUI displays.
Conditions: 200 MSPS, decimation 1, FFT 2048, NCO −1.2288 MHz,
centre 2.450 GHz. Bin spacing 97.656 kHz.

| tone | expected RF | BEFORE | error | AFTER | error |
|---|---|---|---|---|---|
| 5 MHz | 2455.000000 | 2453.808594 | **−1191.4 kHz** | 2454.999743 | **−0.257 kHz** |
| 10 MHz | 2460.000000 | 2458.789062 | **−1210.9 kHz** | 2459.999743 | **−0.257 kHz** |
| 20 MHz | 2470.000000 | 2468.750000 | **−1250.0 kHz** | 2470.000285 | **+0.285 kHz** |
| 45 MHz | 2495.000000 | 2493.750000 | **−1250.0 kHz** | 2495.000285 | **+0.285 kHz** |
| 50 MHz | 2500.000000 | 2498.730469 | **−1269.5 kHz** | 2500.000206 | **+0.206 kHz** |

Worst error: **1269.5 kHz → 0.285 kHz** — a 4450× improvement, and 0.003 of
one FFT bin.

Note the *spread* in the BEFORE column (1191→1270 kHz): that is the ±48.8 kHz
bin quantisation on top of the 1.2288 MHz NCO bias, i.e. the "variation" in
the original report.

### Generality sweep

162 combinations — NCO {0, ±1.2288, −5, +12.5, −30} MHz × rate {61.44, 200,
245.76} MSPS × FFT {1024, 2048, 8192} × tone {5, 10, 20} MHz, excluding
combinations where `tone + NCO` exceeds post-DDC Nyquist:

**worst error across all valid combinations: 0.765 kHz.**

Including NCO = 0 confirms the correction is not a tuned constant: with no
DDC translation the term vanishes and the reading is still correct.

### One legitimate non-result

NCO +12.5 MHz with a 20 MHz tone at 61.44 MSPS reads −61.44 MHz off — exactly
one full sample rate. This is real aliasing, not an error: 20 + 12.5 =
32.5 MHz exceeds the ±30.72 MHz complex Nyquist limit and wraps. The DSP is
reporting the frequency that is genuinely present in the sampled data.
Masking it would violate the "no artificial correction" requirement.

## 5. Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Builds warning-free. Simulated and offline modes regression-tested: plots,
channel selection, peak table and measurements all unchanged.

## 6. Performance

Zero measurable cost. The NCO compensation is one subtraction per frame; the
interpolation is three array reads and a divide per frame. No added latency,
no extra buffering, no change to the threading model or the pipeline.
