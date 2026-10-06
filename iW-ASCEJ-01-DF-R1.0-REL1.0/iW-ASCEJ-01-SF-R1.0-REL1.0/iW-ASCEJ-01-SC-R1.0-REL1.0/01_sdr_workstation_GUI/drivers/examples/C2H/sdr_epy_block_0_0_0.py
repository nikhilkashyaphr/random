#!/usr/bin/env python3
"""
GNU Radio Embedded Python Block — IQ Configurable Source  v9
=============================================================
CHANGES vs v8
  - Thread-safety: all mutable parameters protected by a threading.Lock
    so that GUI-Range callbacks (running in the Qt thread) cannot race
    with work() (running in the scheduler thread).
  - set_sample_rate() now also recomputes _phase_step (was missing the
    lock and the recompute in v8 when called at runtime).
  - Output is still always complex64 normalised ±1.0 — no change to
    the data format.
  - All set_* callbacks are fully hot-swap safe (no phase reset, no
    glitch).

GRC BLOCK PARAMETERS  (Embedded Python Block → Properties)
──────────────────────────────────────────────────────────
  ID              Default       Type   Label
  sample_rate     32000         float  Sample Rate (Hz)
  frequency       1000          float  Frequency (Hz)
  amplitude       0.9           float  Amplitude (0.0–1.0)
  waveform        0             int    Waveform (0=Sine 1=Cos 2=Sq 3=Saw 4=Tri 5=Noise)
  phase_offset    0.0           float  Phase Offset (rad)
  iq_phase_diff   -1.5707963    float  IQ Phase Diff (rad)  (-π/2 = standard IQ)

HOW TO WIRE A GUI RANGE TO FREQUENCY (same pattern for all params)
───────────────────────────────────────────────────────────────────
  Step 1: Variable block    id=freq_var,  value=1000
  Step 2: QT GUI Range      variable=freq_var, start=100, stop=500000, step=100
  Step 3: Variable block    id=freq_cb,   value=iq_source_0.set_frequency(freq_var)

  Repeat for:
    Amplitude   → iq_source_0.set_amplitude(amp_var)      range 0.0–1.0
    Waveform    → iq_source_0.set_waveform(wf_var)        chooser 0–5
    Phase diff  → iq_source_0.set_iq_phase_diff(pd_var)   range -π to π
    Phase off   → iq_source_0.set_phase_offset(po_var)    range 0 to 2π
"""

import threading
import numpy as np
from gnuradio import gr

TWO_PI = 2.0 * np.pi

_WAVEFORM_NAME = {
    0: "Sine",
    1: "Cosine",
    2: "Square",
    3: "Sawtooth",
    4: "Triangle",
    5: "Noise",
}


class blk(gr.sync_block):
    """
    IQ Signal Source — outputs complex64 normalised to ±1.0.
    All parameters are hot-swappable via set_* callbacks.
    Connect GUI Range sliders through Variable (callback) blocks.
    """

    def __init__(
        self,
        sample_rate:   float = 32000.0,
        frequency:     float = 1000.0,
        amplitude:     float = 0.9,
        waveform:      int   = 0,
        phase_offset:  float = 0.0,
        iq_phase_diff: float = -np.pi / 2,
    ):
        gr.sync_block.__init__(
            self,
            name    = "IQ Source (complex64)",
            in_sig  = None,
            out_sig = [np.complex64],
        )

        # FIX: lock protects all mutable fields accessed from GUI callbacks
        self._lock = threading.Lock()

        self.sample_rate   = float(sample_rate)
        self.frequency     = float(frequency)
        self.amplitude     = float(np.clip(amplitude, 0.0, 1.0))
        self.waveform      = int(waveform)
        self.phase_offset  = float(phase_offset)
        self.iq_phase_diff = float(iq_phase_diff)

        # Phase accumulator (float64 for precision); never reset on param change
        self._phase      = float(phase_offset) % TWO_PI
        self._phase_step = TWO_PI * self.frequency / self.sample_rate

        self._print_config()

    # ── Live-control callbacks ────────────────────────────────────────────────

    def set_frequency(self, freq):
        """Live frequency control — phase-continuous, no glitch."""
        with self._lock:
            self.frequency   = float(freq)
            self._phase_step = TWO_PI * self.frequency / self.sample_rate
        print(f"[IQ Source] Frequency  → {float(freq):.2f} Hz")

    def set_amplitude(self, amp):
        """Live amplitude control (0.0 = off, 1.0 = full scale)."""
        with self._lock:
            self.amplitude = float(np.clip(amp, 0.0, 1.0))
        print(f"[IQ Source] Amplitude  → {self.amplitude:.4f}")

    def set_waveform(self, wf):
        """Live waveform select: 0=Sine 1=Cos 2=Square 3=Saw 4=Tri 5=Noise."""
        with self._lock:
            self.waveform = int(wf)
        print(f"[IQ Source] Waveform   → {_WAVEFORM_NAME.get(int(wf), wf)}")

    def set_phase_offset(self, offset):
        """Live I-channel phase offset shift (radians)."""
        with self._lock:
            delta             = float(offset) - self.phase_offset
            self.phase_offset = float(offset)
            self._phase       = (self._phase + delta) % TWO_PI

    def set_iq_phase_diff(self, diff):
        """Live IQ phase difference (radians). -π/2 = standard 90° IQ."""
        with self._lock:
            self.iq_phase_diff = float(diff)
        print(f"[IQ Source] IQ phase Δ → {float(diff):.4f} rad")

    def set_sample_rate(self, sr):
        """Update sample rate (recalculates phase step, no phase reset)."""
        with self._lock:
            self.sample_rate = float(sr)
            self._phase_step = TWO_PI * self.frequency / self.sample_rate

    # ── Waveform synthesis ────────────────────────────────────────────────────

    def _wave(self, n, start_phase, phase_step, waveform):
        """Generate n samples of the selected waveform starting at start_phase."""
        phi = start_phase + phase_step * np.arange(n, dtype=np.float64)
        if   waveform == 0: return np.sin(phi)
        elif waveform == 1: return np.cos(phi)
        elif waveform == 2: return np.where(np.sin(phi) >= 0.0, 1.0, -1.0)
        elif waveform == 3: return 2.0 * ((phi / TWO_PI) % 1.0) - 1.0
        elif waveform == 4:
            pn = (phi / TWO_PI) % 1.0
            return 2.0 * np.abs(2.0 * pn - 1.0) - 1.0
        else:
            return np.random.uniform(-1.0, 1.0, n)

    # ── GNU Radio work() ──────────────────────────────────────────────────────

    def work(self, input_items, output_items):
        out = output_items[0]
        n   = len(out)

        # FIX: snapshot all shared state under the lock so GUI callbacks
        # cannot mutate it mid-computation
        with self._lock:
            phase      = self._phase
            phase_step = self._phase_step
            iq_diff    = self.iq_phase_diff
            amp        = self.amplitude
            waveform   = self.waveform

        i_f = self._wave(n, phase,           phase_step, waveform) * amp
        q_f = self._wave(n, phase + iq_diff, phase_step, waveform) * amp

        # Advance accumulator — must be done under the lock
        with self._lock:
            self._phase = (self._phase + phase_step * n) % TWO_PI

        out[:] = i_f.astype(np.float32) + 1j * q_f.astype(np.float32)
        return n

    # ── Startup banner ────────────────────────────────────────────────────────

    def _print_config(self):
        print(
            f"\n[IQ Source v9] ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            f"  Output      : complex64  (I + jQ, normalised ±1.0)\n"
            f"  Waveform    : {_WAVEFORM_NAME.get(self.waveform, self.waveform)}\n"
            f"  Frequency   : {self.frequency} Hz\n"
            f"  Sample rate : {self.sample_rate} Hz\n"
            f"  Amplitude   : {self.amplitude:.3f}\n"
            f"  IQ phase Δ  : {self.iq_phase_diff:.6f} rad\n"
            f"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            f"  GUI Range wiring example (frequency):\n"
            f"    Variable id=freq_var, value=1000\n"
            f"    GUI Range → variable=freq_var\n"
            f"    Variable id=freq_cb, value=iq_source_0.set_frequency(freq_var)\n"
            f"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        )
