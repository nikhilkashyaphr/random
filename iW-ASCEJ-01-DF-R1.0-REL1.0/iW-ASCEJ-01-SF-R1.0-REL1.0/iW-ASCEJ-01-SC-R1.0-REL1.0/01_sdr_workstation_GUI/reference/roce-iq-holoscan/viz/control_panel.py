#!/usr/bin/env python3
"""
control_panel.py -- on-screen GUI to drive the live IQ pipeline.

A small always-on-top control window with sliders and entry boxes for every
runtime parameter. It writes two shared-memory blocks:

  /dev/shm/iqctl  -> the running transmitter (rdma_tx): sample rate, centre
                     frequency, link pacing
  /dev/shm/iqviz  -> the visualizer (holoscan_iq_viz.py): FFT size, span,
                     averaging, amplitude, dB scale, time zoom, pause,
                     interpolation/decimation, auto-fit

Run it alongside the visualizer (same user, any time):

    python viz/control_panel.py

No arguments needed. It auto-creates the shm blocks if absent and seeds the
transmitter defaults; once rdma_tx / the viz are up they pick up changes
within one poll tick (~1 s for the tx, one frame for the viz).
"""
import argparse
import mmap
import os
import struct
import tkinter as tk
from tkinter import ttk

IQCTL_MAGIC = 0x495143544C303031
IQVIZ_MAGIC = 0x495156495A303031
IQVIZ_OFF = dict(span=16, center=24, amp=32, t_len=40, db_top=48,
                 db_range=56, fft_n=64, avg_n=68, interp=72, decim=76,
                 paused=80, fit=84)
FRAME = 262144


def open_shm(path, magic, seed_fn):
    try:
        fd = os.open(path, os.O_RDWR)
    except FileNotFoundError:
        fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o666)
        try:
            os.fchmod(fd, 0o666)
        except OSError:
            pass
    os.ftruncate(fd, 4096)
    mm = mmap.mmap(fd, 4096)
    os.close(fd)
    if struct.unpack_from("<Q", mm, 0)[0] != magic:
        seed_fn(mm)
    return mm


def fmt_si(v, unit="Hz"):
    a = abs(v)
    for s, m in (("G", 1e9), ("M", 1e6), ("k", 1e3)):
        if a >= m:
            return f"{v / m:.4g} {s}{unit}"
    return f"{v:.4g} {unit}"


class Panel:
    def __init__(self, ctl_path, viz_path):
        # ---- shm: transmitter control ----
        def seed_ctl(mm):
            struct.pack_into("<QII", mm, 0, IQCTL_MAGIC, 1, 0)
            struct.pack_into("<dQq", mm, 16, 10.0, 100_000_000, 2_450_000_000)
        self.ctl = open_shm(ctl_path, IQCTL_MAGIC, seed_ctl)

        # ---- shm: viz control ----
        def seed_viz(mm):
            struct.pack_into("<QII", mm, 0, IQVIZ_MAGIC, 1, 0)
            struct.pack_into("<dddddd", mm, IQVIZ_OFF["span"],
                             1.0, 0.0, 1.0, 256 / FRAME, 5.0, 115.0)
            struct.pack_into("<IIIIII", mm, IQVIZ_OFF["fft_n"],
                             8192, 1, 1, 1, 0, 0)
        self.viz = open_shm(viz_path, IQVIZ_MAGIC, seed_viz)

        # current tx values
        pace, = struct.unpack_from("<d", self.ctl, 16)
        fs, = struct.unpack_from("<Q", self.ctl, 24)
        fc, = struct.unpack_from("<q", self.ctl, 32)

        self.build_gui(pace, fs, fc)

    # ---- writers -----------------------------------------------------------
    def push_ctl(self, pace, fs, fc):
        struct.pack_into("<dQq", self.ctl, 16, float(pace), int(fs), int(fc))
        seq, = struct.unpack_from("<I", self.ctl, 12)
        struct.pack_into("<I", self.ctl, 12, (seq + 1) & 0xFFFFFFFF)

    def push_viz(self):
        struct.pack_into("<dddddd", self.viz, IQVIZ_OFF["span"],
                         self.v_span.get(), self.v_center.get(),
                         self.v_amp.get(), self.v_tlen.get(),
                         self.v_dbtop.get(), self.v_dbrange.get())
        struct.pack_into("<IIIII", self.viz, IQVIZ_OFF["fft_n"],
                         int(self.v_fft.get()), int(self.v_avg.get()),
                         int(self.v_interp.get()), int(self.v_decim.get()),
                         1 if self.v_pause.get() else 0)
        seq, = struct.unpack_from("<I", self.viz, 12)
        struct.pack_into("<I", self.viz, 12, (seq + 1) & 0xFFFFFFFF)

    def trigger_fit(self):
        fit, = struct.unpack_from("<I", self.viz, IQVIZ_OFF["fit"])
        struct.pack_into("<I", self.viz, IQVIZ_OFF["fit"],
                         (fit + 1) & 0xFFFFFFFF)

    # ---- GUI ---------------------------------------------------------------
    def build_gui(self, pace, fs, fc):
        self.root = tk.Tk()
        self.root.title("IQ Pipeline Control")
        self.root.attributes("-topmost", True)
        self.root.configure(bg="#15171c")

        # Create ALL control variables up front, before any widget whose
        # callback calls push_viz() -- otherwise the first slider fires
        # push_viz() before v_pause exists and crashes.
        self.v_pace = tk.DoubleVar(value=pace)
        self.v_span = tk.DoubleVar(value=1.0)
        self.v_center = tk.DoubleVar(value=0.0)
        self.v_fft = tk.DoubleVar(value=8192)
        self.v_avg = tk.DoubleVar(value=1)
        self.v_dbtop = tk.DoubleVar(value=5.0)
        self.v_dbrange = tk.DoubleVar(value=115.0)
        self.v_tlen = tk.DoubleVar(value=256 / FRAME)
        self.v_amp = tk.DoubleVar(value=1.0)
        self.v_interp = tk.DoubleVar(value=1)
        self.v_decim = tk.DoubleVar(value=1)
        self.v_pause = tk.BooleanVar(value=False)

        style = ttk.Style(self.root)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure(".", background="#15171c", foreground="#dfe3ea",
                        fieldbackground="#23262e")
        style.configure("TLabelframe", background="#15171c",
                        foreground="#9fb0c8")
        style.configure("TLabelframe.Label", background="#15171c",
                        foreground="#7fd6ff")
        style.configure("TButton", background="#2a2f3a")

        pad = dict(padx=6, pady=3, sticky="we")

        # ============ TRANSMITTER ============
        tx = ttk.LabelFrame(self.root, text="TRANSMITTER  (rdma_tx, live)")
        tx.grid(row=0, column=0, padx=8, pady=6, sticky="we")
        tx.columnconfigure(1, weight=1)

        self.e_fs = self._entry(tx, 0, "Sample rate Fs", fmt_si(fs))
        self.e_fc = self._entry(tx, 1, "Centre freq Fc", fmt_si(fc))
        ttk.Label(tx, text="Link pacing (Gb/s, 0=max)").grid(
            row=2, column=0, **pad)
        self.l_pace = ttk.Label(tx, text=f"{pace:.0f}")
        self.l_pace.grid(row=2, column=2, padx=6)
        s = ttk.Scale(tx, from_=0, to=60, variable=self.v_pace,
                      command=lambda e: self._pace_live())
        s.grid(row=2, column=1, **pad)
        ttk.Button(tx, text="Apply Fs / Fc",
                   command=self._tx_apply).grid(row=3, column=0,
                                                columnspan=3, pady=5)
        # quick presets
        pf = ttk.Frame(tx)
        pf.grid(row=4, column=0, columnspan=3, sticky="we")
        for i, (lbl, f, c) in enumerate([
                ("100M/2.45G", 100e6, 2.45e9),
                ("122.88M/3.5G", 122.88e6, 3.5e9),
                ("61.44M/1.8G", 61.44e6, 1.8e9),
                ("200M/5.8G", 200e6, 5.8e9)]):
            ttk.Button(pf, text=lbl,
                       command=lambda f=f, c=c: self._preset(f, c)
                       ).grid(row=0, column=i, padx=2, pady=2)

        # ============ SPECTRUM ============
        sp = ttk.LabelFrame(self.root, text="SPECTRUM  (display)")
        sp.grid(row=1, column=0, padx=8, pady=6, sticky="we")
        sp.columnconfigure(1, weight=1)
        self._slider(sp, 0, "Span (frac of Fs)", self.v_span,
                     1 / 64, 1.0, "{:.3f}")
        self._slider(sp, 1, "Centre (frac)", self.v_center, -0.5, 0.5,
                     "{:+.3f}")
        self._slider(sp, 2, "FFT size", self.v_fft, 1024, 65536, "{:.0f}",
                     snap_pow2=True)
        self._slider(sp, 3, "Averaging", self.v_avg, 1, 32, "{:.0f}")
        self._slider(sp, 4, "Ref level dBFS", self.v_dbtop, -60, 20,
                     "{:.0f}")
        self._slider(sp, 5, "dB range", self.v_dbrange, 30, 160, "{:.0f}")

        # ============ TIME DOMAIN ============
        td = ttk.LabelFrame(self.root, text="TIME DOMAIN  (display)")
        td.grid(row=2, column=0, padx=8, pady=6, sticky="we")
        td.columnconfigure(1, weight=1)
        # show samples count, store fraction
        ttk.Label(td, text="Window (samples)").grid(row=0, column=0, **pad)
        self.l_tlen = ttk.Label(td, text="256")
        self.l_tlen.grid(row=0, column=2, padx=6)
        self.s_tlen = ttk.Scale(td, from_=5, to=14,  # log2(samples)
                                command=self._tlen_live)
        self.s_tlen.set(8)
        self.s_tlen.grid(row=0, column=1, **pad)
        self._slider(td, 1, "Amplitude x", self.v_amp, 0.1, 20, "{:.2f}")
        self._slider(td, 2, "Interpolation", self.v_interp, 1, 8, "{:.0f}")
        self._slider(td, 3, "Decimation", self.v_decim, 1, 8, "{:.0f}")
        ttk.Button(td, text="Auto-fit to tone (~10 cycles)",
                   command=lambda: (self.trigger_fit(),)
                   ).grid(row=4, column=0, columnspan=3, pady=5)

        # ============ GLOBAL ============
        gf = ttk.Frame(self.root)
        gf.grid(row=3, column=0, padx=8, pady=8, sticky="we")
        ttk.Checkbutton(gf, text="Pause display", variable=self.v_pause,
                        command=self.push_viz).grid(row=0, column=0, padx=6)
        ttk.Button(gf, text="Reset display",
                   command=self._reset_viz).grid(row=0, column=1, padx=6)
        self.status = ttk.Label(self.root, text="ready", foreground="#7fd6ff")
        self.status.grid(row=4, column=0, padx=10, pady=(0, 8), sticky="w")

        self.push_viz()

    # ---- widget helpers ----------------------------------------------------
    def _entry(self, parent, row, label, initial):
        ttk.Label(parent, text=label).grid(row=row, column=0, padx=6,
                                           pady=3, sticky="w")
        var = tk.StringVar(value=initial)
        e = ttk.Entry(parent, textvariable=var, width=12)
        e.grid(row=row, column=1, columnspan=2, padx=6, pady=3, sticky="we")
        return var

    def _slider(self, parent, row, label, var, lo, hi, fmt,
                snap_pow2=False):
        ttk.Label(parent, text=label).grid(row=row, column=0, padx=6,
                                           pady=3, sticky="w")
        lab = ttk.Label(parent, text=fmt.format(var.get()))
        lab.grid(row=row, column=2, padx=6)

        def on(_=None):
            v = var.get()
            if snap_pow2:
                v = 1 << max(10, min(16, round(__import__("math").log2(v))))
                var.set(v)
            lab.config(text=fmt.format(var.get()))
            self.push_viz()
        ttk.Scale(parent, from_=lo, to=hi, variable=var,
                  command=on).grid(row=row, column=1, padx=6, pady=3,
                                   sticky="we")

    # ---- actions -----------------------------------------------------------
    def _tlen_live(self, _=None):
        n = int(round(2 ** self.s_tlen.get()))
        self.v_tlen.set(n / FRAME)
        self.l_tlen.config(text=str(n))
        self.push_viz()

    def _pace_live(self):
        self.l_pace.config(text=f"{self.v_pace.get():.0f}")
        pace = self.v_pace.get()
        fs, = struct.unpack_from("<Q", self.ctl, 24)
        fc, = struct.unpack_from("<q", self.ctl, 32)
        self.push_ctl(pace, fs, fc)
        self.status.config(text=f"pace -> {pace:.0f} Gb/s")

    def _parse(self, s):
        s = s.strip()
        mult = 1.0
        if s and s[-1] in "kKmMgG":
            mult = {"k": 1e3, "K": 1e3, "m": 1e6, "M": 1e6,
                    "g": 1e9, "G": 1e9}[s[-1]]
            s = s[:-1]
        return float(s) * mult

    def _tx_apply(self):
        try:
            fs = self._parse(self.e_fs.get())
            fc = self._parse(self.e_fc.get())
        except ValueError:
            self.status.config(text="bad Fs/Fc value", foreground="#ff6b6b")
            return
        self.push_ctl(self.v_pace.get(), fs, fc)
        self.status.config(text=f"tx -> Fs {fmt_si(fs)}, Fc {fmt_si(fc)} "
                                "(applies within ~1 s)", foreground="#7fd6ff")

    def _preset(self, f, c):
        self.e_fs.set(fmt_si(f))
        self.e_fc.set(fmt_si(c))
        self._tx_apply()

    def _reset_viz(self):
        self.v_span.set(1.0)
        self.v_center.set(0.0)
        self.v_fft.set(8192)
        self.v_avg.set(1)
        self.v_dbtop.set(5.0)
        self.v_dbrange.set(115.0)
        self.v_amp.set(1.0)
        self.v_interp.set(1)
        self.v_decim.set(1)
        self.s_tlen.set(8)
        self.v_tlen.set(256 / FRAME)
        self.v_pause.set(False)
        self.l_tlen.config(text="256")
        self.push_viz()
        self.status.config(text="display reset", foreground="#7fd6ff")

    def run(self):
        self.root.mainloop()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ctl", default="/dev/shm/iqctl")
    ap.add_argument("--vizctl", default="/dev/shm/iqviz")
    args = ap.parse_args()
    Panel(args.ctl, args.vizctl).run()


if __name__ == "__main__":
    main()
