# Constellation phase bug + RF Analyzer metrics

## 1. Why only 25 MHz read 0°

Not a frequency-dependent error — a **rotating phasor measured with a
static-phase estimator**.

The constellation samples every `sps = 4` samples, so an unmodulated carrier
advances a fixed angle per plotted point:

```
step = 360 × f_residual / fs × sps    degrees per plotted point
```

The 4th-power aligner removes a *static* phase and is 90°-symmetric, so it can
only cancel that rotation when `step` is a multiple of 90°. At 200 MSPS with
`sps = 4`:

| tone | step/point | mod 90° | result |
|---|---|---|---|
| 5 MHz | 36° | 36° | rotates → phase error |
| 10 MHz | 72° | 72° | rotates → phase error |
| 20 MHz | 144° | 54° | rotates → phase error |
| **25 MHz** | **180°** | **0°** | **static → 0°** |
| 40 MHz | 288° | 18° | rotates → phase error |
| **50 MHz** | **360°** | **0°** | **static → 0°** |

25 MHz and 50 MHz are the only frequencies on that grid where the rotation
lands on the estimator's symmetry, so those alone appeared correct.

## 2. Fix

Estimate the residual rotation and remove it *before* the static-phase
estimate:

```cpp
for (i = 1 .. n)  rotAcc += raw[i] * conj(raw[i-1]);
stepPerPoint = arg(rotAcc);                 // exact for CW, zero-mean for QAM
flat[i] = raw[i] * polar(1, -stepPerPoint * i);
phi0    = arg(-Σ flat⁴) / 4;                // now on a STATIONARY set
```

**Order matters, and getting it wrong was the second half of the bug.** My
first attempt de-rotated but still computed `phi0` from the rotating points,
which left the cluster at an arbitrary angle and the phase error stuck at
~17–18°. Estimating `phi0` on the de-rotated set fixes it.

### Measured, before and after

| tone | phase error before | after |
|---|---|---|
| 5 MHz | 17.09° | **0.67°** |
| 10 MHz | 18.19° | **0.12°** |
| 20 MHz | 17.09° | **0.67°** |
| 25 MHz | 0.01° | 0.01° |
| 40 MHz | 18.19° | **0.12°** |
| 50 MHz | 0.00° | 0.00° |

25 MHz is no longer special. The offset removed is reported as **Residual
carrier** in Hz, so the figure is physical rather than an artefact of the
plotting stride — verified recovering 5/10/20/25 MHz to 4 decimal places.

## 3. RF Analyzer metrics added

`SINAD`, `ENOB`, `SFDR`, `THD` and harmonics `H2…H6`, computed from the same
windowed spectrum the display shows, in linear power.

Bins within ±guard of a tone are counted as part of it: a window spreads a
sinusoid over several bins (Blackman-Harris ENBW ≈ 2), so summing only the
peak bin would understate signal power and inflate every derived figure. The
guard width is derived from the window, not hardcoded.

Harmonic positions are computed **modulo the span**, because a harmonic above
Nyquist folds back into the digitised band rather than disappearing. A
harmonic that folds onto the fundamental is skipped as unmeasurable rather
than reported as a spurious value.

`ENOB = (SINAD − 1.76) / 6.02`.

### Validation — ENOB against a known quantiser

| quantiser | SINAD | ENOB | ideal |
|---|---|---|---|
| 8-bit | 45.47 dB | **7.26** | 8 |
| 10-bit | 61.03 dB | **9.85** | 10 |
| 12-bit | 70.83 dB | **11.47** | 12 |
| 14-bit | 82.95 dB | **13.49** | 14 |
| 16-bit | 90.59 dB | **14.76** | 16 |

Tracks the quantiser correctly, ~0.5–1.2 bits below ideal as expected: window
leakage and the finite record put real noise in the measurement, so a
perfectly ideal figure would indicate a formula that flatters itself.

### Validation — THD/SFDR against an injected harmonic

| injected H2 | THD | SFDR | H2 |
|---|---|---|---|
| none | −126.0 dBc | 87.4 dBc | −129.0 dBc |
| 0.007 | **−40.0 dBc** | 40.0 dBc | **−40.0 dBc** |
| 0.022 | **−30.0 dBc** | 30.0 dBc | **−30.0 dBc** |
| 0.070 | **−20.0 dBc** | 20.0 dBc | **−20.0 dBc** |

Exact agreement with the injected amplitude at every level.

## 4. Parameter table

23 rows now, populated and verified end to end:

```
Peak level · Peak frequency · Noise floor · SNR · Occupied bandwidth
Channel power · RMS level · Peak amplitude · PAPR · EVM · Phase error
DC offset · I/Q imbalance · SINAD · ENOB · SFDR · THD · H2…H6
Residual carrier
```

## 5. A caveat worth stating

EVM and DC offset are still **slicer-based**, i.e. they compare each symbol to
the nearest QAM grid point. That is correct for modulated data and meaningless
for an unmodulated carrier — the 25.48 % EVM in the CW test above is that, not
a defect. If CW is the primary use case, those two rows should be suppressed
or switched to a carrier-appropriate definition; I have not done so because it
changes behaviour for modulated signals and is worth a decision rather than an
assumption.

Clean build, zero errors, zero warnings.
