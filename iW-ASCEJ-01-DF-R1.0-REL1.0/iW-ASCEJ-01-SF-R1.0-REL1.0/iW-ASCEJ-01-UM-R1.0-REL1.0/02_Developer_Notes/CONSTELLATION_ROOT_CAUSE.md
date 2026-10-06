# Constellation phase: root cause, the wrong fix, and the correct one

You asked whether this is actually right and how it "suddenly worked". Fair
question — the first fix I shipped was **wrong**, and I only found that by
testing it. Here is the full chain, with the evidence for each step.

---

## 1. The mechanism

The constellation plots every `sps`-th sample (`sps = 4`). An unmodulated
carrier at offset *f* advances a fixed angle between plotted points:

```
step = 360 × f / fs × sps        degrees per plotted point
```

Alignment used the **fourth-power (Viterbi & Viterbi) estimator**, which
removes a *static* phase and is 90°-symmetric. It therefore cancels the
rotation only when `step` is a multiple of 90°.

Verified in NumPy, independently of the application:

| tone | predicted step | measured step | mod 90° |
|---|---|---|---|
| 5 MHz | 36.000° | 36.000° | 36° |
| 10 MHz | 72.000° | 72.000° | 72° |
| 20 MHz | 144.000° | 144.000° | 54° |
| **25 MHz** | **180.000°** | **180.000°** | **0°** |
| 40 MHz | 288.000° | 288.000° | 18° |
| **50 MHz** | **360.000°** | **360.000°** | **0°** |

And the consequence, measured as the angular spread of the cluster after
alignment (0° = a stationary point):

| tone | spread, old code | spread, fixed |
|---|---|---|
| 5 MHz | 72.000° | **0.000°** |
| 10 MHz | 72.000° | **0.000°** |
| 20 MHz | 72.000° | **0.000°** |
| 25 MHz | 0.000° | 0.000° |
| 50 MHz | 0.000° | 0.000° |

So 25 MHz was never "correct" — it was the one frequency on your grid where a
rotating phasor happened to land on the estimator's symmetry.

**Nothing was broken by a recent change.** This has always been true of a
static-phase estimator applied to a rotating input; you noticed it because you
swept frequency.

---

## 2. My first fix was wrong — and testing caught it

I first estimated the rotation with **delay-and-multiply**,
`Σ r[i]·conj(r[i-1])`. That is exact for CW, and it made the phase numbers
look perfect. Then I tested it on 16-QAM:

| carrier offset | EVM before | EVM after my "fix" |
|---|---|---|
| 0 MHz | 0.67 % | **26.29 %** |
| 0.5 MHz | 25.82 % | 25.95 % |

It **destroyed** a clean constellation. The reason:

```
r[i]·conj(r[i-1]) = s[i]·conj(s[i-1]) · e^{jΔ}
```

For CW the data term is `|s|²` and sums coherently. For QAM it is zero-mean
and sums as a **random walk**. Measured coherence of that term:

```
CW      : 1.0000     (fully coherent — estimate is exact)
16-QAM  : 0.0097     (noise — estimate is meaningless)
```

So the estimate was garbage on modulated data and de-rotating by it wrecked
the display. Had I shipped only the CW test, this would have reached you.

---

## 3. The correct fix

Strip the modulation **first**, then difference — the standard fourth-power
frequency estimator:

```cpp
rotAcc += (r[i]²)² · conj((r[i-1]²)²);     // r⁴ · conj(prev⁴)
step    = arg(rotAcc) / 4;                  // ÷4 undoes the fourth power
```

Raising to the fourth power removes the QPSK/QAM symmetry, leaving a tone at
`4·step`. Compared side by side:

| signal | offset | delay-multiply | **4th-power** |
|---|---|---|---|
| CW | 0.0 MHz | 0.0000 | **0.0000** |
| CW | 2.0 MHz | 2.0000 | **2.0000** |
| 16-QAM | 0.0 MHz | 18.6467 ✗ | **−0.0607** |
| 16-QAM | 0.5 MHz | −12.0355 ✗ | **0.2887** |
| 16-QAM | 2.0 MHz | −7.4793 ✗ | **2.0623** |

Order also matters: the static phase `phi0` must be estimated **after**
de-rotation. My first attempt de-rotated but still computed `phi0` from the
rotating points, which left the phase error stuck at ~17°.

Result — phase error across frequency:

| tone | before | after |
|---|---|---|
| 5 MHz | 17.09° | **0.04°** |
| 10 MHz | 18.19° | **0.14°** |
| 20 MHz | 17.09° | **0.04°** |
| 25 MHz | 0.01° | 0.00° |
| 40 MHz | 18.19° | **0.14°** |

---

## 4. A limitation I am reporting rather than hiding

The fourth power multiplies the phase step by four, so the *reported* offset
is unambiguous only while `|offset| < fs/(8·sps)` — **6.25 MHz** at 200 MSPS.
Beyond that it aliases, exactly as predicted:

```
10 MHz -> 4·step wraps to  −72° -> reports −2.500 MHz
20 MHz -> 4·step wraps to −144° -> reports −5.000 MHz
```

**The constellation is still correct beyond that range**, because the aliased
step differs from the true one by a multiple of 90° — precisely the symmetry
the display and slicer ignore. Only the *number* is ambiguous.

A range check alone is not sufficient (−2.5 MHz sits inside ±6.25 MHz and
would pass), so validity is decided by **cross-checking against the FFT peak**,
which is independent and unambiguous across the whole span. When they
disagree the table prints:

```
Residual carrier    — (beyond estimator range)
```

`Peak frequency`, from the FFT, remains correct at all frequencies and is the
figure to read.

---

## 5. References

* **A. J. Viterbi and A. M. Viterbi**, "Nonlinear estimation of PSK-modulated
  carrier phase with application to burst digital transmission",
  *IEEE Trans. Information Theory*, 29(4), 1983 — the fourth-power
  modulation-stripping method used for both the phase and the frequency
  estimate.
* **S. Kay**, "A fast and accurate single frequency estimator", *IEEE Trans.
  ASSP*, 37(12), 1989 — the delay-and-multiply estimator and its variance;
  explains why it needs a constant-envelope input.
* **IEEE Std 1241** (ADC terminology and test methods) and **IEEE Std 1057**
  (digitising waveform recorders) — definitions of SINAD, SFDR, THD and the
  `ENOB = (SINAD − 1.76)/6.02` relation.
* **Xilinx/AMD PG269**, *Zynq UltraScale+ RF Data Converter LogiCORE IP* —
  the RF Analyzer metric set this table now mirrors.
* **Analog Devices MT-003**, *Understand SINAD, ENOB, SNR, THD, THD+N and
  SFDR* — the 1.76 dB term and the distinction between SNR and SINAD.
* `SDR_RF_Parameter.py` (your own reference) — window ENBW handling and the
  `dc_guard_bins` idea, both carried into this implementation.

---

## 6. What I would still change

`EVM` and `DC offset` remain **slicer-based** — each symbol is compared to the
nearest QAM grid point. That is correct for modulated data and meaningless for
CW; the ~25 % EVM on a pure tone is that, not a defect. Suppressing or
redefining those two rows for CW is a behaviour change for modulated signals,
so it is worth a decision rather than my assumption.
