"""
RF Metrics Sink — GNU Radio Python Embedded Block (v8, final)
IEEE 1241 / 1057 metrics. Compact monochrome top-right panel.

Inputs : 1 x complex64
Outputs: none (sink)
"""

import numpy as np
import gnuradio.gr as gr

try:
    from PyQt5.QtWidgets import (QApplication, QWidget, QGridLayout,
                                 QVBoxLayout, QHBoxLayout, QLabel)
    from PyQt5.QtCore import QTimer, Qt
    HAS_QT = True
except ImportError:
    HAS_QT = False


# ── DSP helpers ────────────────────────────────────────────────────────────
def make_window(name, n):
    name = name.lower()
    if name == "hann":
        w = np.hanning(n)
    elif name == "blackman":
        w = np.blackman(n)
    elif name in ("flat", "flattop"):
        a = [0.21557895, 0.41663158, 0.277263158, 0.083578947, 0.006947368]
        k = np.arange(n)
        w = sum((-1)**i * a[i] * np.cos(2*np.pi*i*k/n) for i in range(5))
    else:
        w = np.hanning(n)
    nenbw = n * np.sum(w**2) / np.sum(w)**2          # noise bandwidth, bins
    return (w / np.sum(w)).astype(np.float32), float(nenbw)


def welch_psd(frames, w):
    acc = np.zeros(frames.shape[1], dtype=np.float64)
    for f in frames:
        acc += np.abs(np.fft.fft(f * w)) ** 2
    return np.fft.fftshift(acc / frames.shape[0])     # DC at centre bin N//2


def freq_axis(n, fs):
    return np.fft.fftshift(np.fft.fftfreq(n, d=1.0/fs))


def _parabolic(psd, k):
    """Sub-bin peak refinement (cosmetic; affects displayed freq only)."""
    if k <= 0 or k >= len(psd) - 1:
        return float(k)
    a = np.log(max(psd[k-1], 1e-30))
    b = np.log(max(psd[k],   1e-30))
    c = np.log(max(psd[k+1], 1e-30))
    d = a - 2*b + c
    return k + (0.5 * (a - c) / d if abs(d) > 1e-20 else 0.0)


# ── metric engine ──────────────────────────────────────────────────────────
def compute_metrics(psd, freqs, fs, n_harmonics, nenbw,
                    dc_guard_bins, return_debug=False):
    N = len(psd)
    eps = 1e-30
    guard = int(np.ceil(1.5 * nenbw)) + 1            # honest, window-derived
    dcc = N // 2                                     # DC bin after fftshift
    lo = max(0, dcc - dc_guard_bins)
    hi = min(N, dcc + dc_guard_bins + 1)

    # ── exclude DC region, THEN locate the fundamental ────────────────────
    search = psd.copy()
    search[lo:hi] = 0.0
    fund_bin = int(np.argmax(search))
    fund_hz = float(np.interp(_parabolic(psd, fund_bin),
                              np.arange(N), freqs))

    def _mask(c):
        m = np.zeros(N, dtype=bool)
        m[max(0, c-guard):min(N-1, c+guard)+1] = True
        return m

    mask_sig = _mask(fund_bin)
    mask_dc = np.zeros(N, dtype=bool)
    mask_dc[lo:hi] = True

    # ── harmonics folded into [-fs/2, +fs/2] ──────────────────────────────
    harm_masks, harm_pwrs = [], []
    for h in range(2, n_harmonics + 2):
        hf = (fund_hz * h + fs/2) % fs - fs/2
        hb = int(np.argmin(np.abs(freqs - hf)))
        m = _mask(hb)
        harm_masks.append(m)
        harm_pwrs.append(float(psd[m].sum()))
    mask_harm = (np.logical_or.reduce(harm_masks)
                 if harm_masks else np.zeros(N, dtype=bool))
    mask_noise = ~(mask_sig | mask_harm | mask_dc)

    P_sig = float(psd[mask_sig].sum())
    P_harm = float(sum(harm_pwrs))
    P_noise = float(psd[mask_noise].mean()) * int(mask_noise.sum())

    spur = psd.copy()
    spur[mask_sig | mask_dc] = 0.0                   # SFDR spur may be a harmonic
    P_spur = float(spur.max())

    snr = 10*np.log10(max(P_sig, eps) / max(P_noise, eps))
    sfdr = 10*np.log10(max(P_sig, eps) / max(P_spur, eps))
    sinad = 10*np.log10(max(P_sig, eps) / max(P_noise + P_harm, eps))

    out = {
        "fundamental_hz": round(fund_hz, 1),
        "snr_db": round(snr, 2),
        "sfdr_dbc": round(sfdr, 2),
        "sinad_db": round(sinad, 2),
        "enob": round((sinad - 1.76) / 6.02, 3),
        "thd_dbc": round(10*np.log10(max(P_harm, eps)/max(P_sig, eps)), 2),
        "noise_floor_dbfs": round(10*np.log10(max(P_noise/N, eps)), 2),
    }
    if return_debug:
        order = np.argsort(psd)[::-1][:5]
        out["_peaks"] = [(round(freqs[i]/1e6, 4),
                          round(10*np.log10(max(psd[i], eps)), 1)) for i in order]
    return out


# ── compact top-right panel ────────────────────────────────────────────────
_BG = "#1b1b1b"; _DIM = "#777"; _VAL = "#e0e0e0"; _ACC = "#4caf50"

class RFPanel(QWidget):
    _ROWS = [("fundamental_hz", "Fund", "MHz"), ("snr_db", "SNR", "dB"),
             ("sfdr_dbc", "SFDR", "dBc"), ("sinad_db", "SINAD", "dB"),
             ("enob", "ENOB", "b"), ("thd_dbc", "THD", "dBc"),
             ("noise_floor_dbfs", "NF", "dBFS")]

    def __init__(self, fs):
        super().__init__(None, Qt.Window | Qt.FramelessWindowHint |
                         Qt.WindowStaysOnTopHint)
        self.setFixedWidth(175)
        self.setStyleSheet(f"background:{_BG};")
        self._drag = None
        lay = QVBoxLayout(self); lay.setContentsMargins(10, 8, 10, 8); lay.setSpacing(4)

        top = QHBoxLayout()
        t = QLabel("RF METRICS")
        t.setStyleSheet(f"color:{_ACC};font:bold 10px Consolas;letter-spacing:1px;")
        x = QLabel("×"); x.setStyleSheet(f"color:{_DIM};font:bold 12px Consolas;")
        x.mousePressEvent = lambda e: self.close()
        top.addWidget(t); top.addStretch(); top.addWidget(x)
        lay.addLayout(top)

        f = QLabel(f"Fs {fs/1e6:.2f} MHz")
        f.setStyleSheet(f"color:{_DIM};font:8px Consolas;")
        lay.addWidget(f)

        g = QGridLayout(); g.setSpacing(3); self._v = {}
        for r, (k, l, u) in enumerate(self._ROWS):
            a = QLabel(l); a.setStyleSheet(f"color:{_DIM};font:10px Consolas;")
            b = QLabel("—"); b.setStyleSheet(f"color:{_VAL};font:bold 11px Consolas;")
            b.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
            c = QLabel(u); c.setStyleSheet(f"color:{_DIM};font:8px Consolas;")
            g.addWidget(a, r, 0); g.addWidget(b, r, 1); g.addWidget(c, r, 2)
            self._v[k] = b
        lay.addLayout(g)

        self._cnt = QLabel("● 0")
        self._cnt.setStyleSheet(f"color:{_ACC};font:8px Consolas;")
        lay.addWidget(self._cnt)
        self._n = 0

    def refresh(self, m):
        self._n += 1
        for k, w in self._v.items():
            val = m.get(k)
            if val is None:
                continue
            if k == "fundamental_hz":
                w.setText(f"{abs(val)/1e6:.3f}")
            elif k == "enob":
                w.setText(f"{val:.2f}")
            else:
                w.setText(f"{val:.1f}")
        self._cnt.setText(f"● {self._n}")

    def showEvent(self, e):
        s = QApplication.primaryScreen().availableGeometry()
        self.move(s.right() - self.width() - 16, s.top() + 16)
        super().showEvent(e)

    def mousePressEvent(self, e):
        if e.button() == Qt.LeftButton:
            self._drag = e.globalPos() - self.frameGeometry().topLeft()
    def mouseMoveEvent(self, e):
        if self._drag is not None and e.buttons() & Qt.LeftButton:
            self.move(e.globalPos() - self._drag)
    def mouseReleaseEvent(self, e):
        self._drag = None


# ── GNU Radio block ────────────────────────────────────────────────────────
class blk(gr.sync_block):
    """RF Metrics Sink: 1x complex64 in, small top-right dashboard."""

    def __init__(self, sample_rate=150.25e6, fft_size=4096, avg_frames=16,
                 n_harmonics=5, window_type="flat", gui_refresh_ms=500,
                 dc_guard_bins=8, debug_peaks=False):
        gr.sync_block.__init__(self, name="RF Metrics Sink",
                               in_sig=[np.complex64], out_sig=[])
        assert fft_size > 0 and not (fft_size & (fft_size-1)), \
            "fft_size must be a power of 2"
        self._fs = float(sample_rate)
        self._N = int(fft_size)
        self._K = int(avg_frames)
        self._nh = int(n_harmonics)
        self._rms = int(gui_refresh_ms)
        self._dcg = int(dc_guard_bins)
        self._dbg = bool(debug_peaks)

        self._win, self._nenbw = make_window(window_type, self._N)
        self._freqs = freq_axis(self._N, self._fs)

        self._buf = np.zeros((self._K, self._N), dtype=np.complex64)
        self._bhead = 0
        self._acc = np.empty(0, dtype=np.complex64)      # cross-call carry (< N)

        self.metrics = {k: None for k in
                        ("fundamental_hz", "snr_db", "sfdr_dbc", "sinad_db",
                         "enob", "thd_dbc", "noise_floor_dbfs")}
        self._panel = None
        if HAS_QT:
            QTimer.singleShot(600, self._build_panel)

    def _build_panel(self):
        try:
            if not QApplication.instance():
                return
            self._panel = RFPanel(self._fs)
            self._panel.setWindowTitle("RF Metrics")
            self._panel.show()
            t = QTimer(self._panel)
            t.setInterval(self._rms)
            t.timeout.connect(lambda: self._panel.refresh(self.metrics))
            t.start()
        except Exception as e:
            print(f"[RF Metrics Sink] GUI error: {e}")

    def work(self, input_items, output_items):
        raw = input_items[0]
        s = np.concatenate((self._acc, raw)) if self._acc.size else raw
        n = len(s)
        idx = 0
        # drain ALL complete frames -> carry stays < one frame (no leak)
        while idx + self._N <= n:
            self._buf[self._bhead] = s[idx:idx + self._N]
            self._bhead += 1
            idx += self._N
            if self._bhead == self._K:                    # full ring -> compute once
                self._bhead = 0
                psd = welch_psd(self._buf, self._win)
                m = compute_metrics(psd, self._freqs, self._fs, self._nh,
                                    self._nenbw, self._dcg, return_debug=self._dbg)
                if self._dbg:
                    print(f"[RF] fund={m['fundamental_hz']/1e6:.4f} MHz  "
                          f"top5(MHz,dB)={m.pop('_peaks')}")
                self.metrics = m
        self._acc = s[idx:].copy()                        # coherent tail
        return len(raw)

    def get_metrics(self): return dict(self.metrics)
    def get_snr(self):     return self.metrics["snr_db"] or 0.0
    def get_sfdr(self):    return self.metrics["sfdr_dbc"] or 0.0
    def get_enob(self):    return self.metrics["enob"] or 0.0
