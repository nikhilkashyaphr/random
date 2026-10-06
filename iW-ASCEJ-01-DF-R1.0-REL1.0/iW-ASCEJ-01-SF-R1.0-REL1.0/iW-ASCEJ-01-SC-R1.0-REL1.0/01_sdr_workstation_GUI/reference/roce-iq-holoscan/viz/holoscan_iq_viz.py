#!/usr/bin/env python3
"""
holoscan_iq_viz.py -- interactive GPU visualization of the RoCEv2 IQ stream.

  RingSource ──> SpectrumDSP ──> HolovizOp
   newest frame    cuFFT, PSD,     Vulkan window: labeled axes,
   from the ring   averaging,      zoom/pan, markers, cursor,
   (host shm OR    resampling,     live RF readout
   GPU memory)     RF estimation

Works with BOTH receivers and auto-detects which one is running:
  rdma_rx      -- frames in host shared memory; one host->GPU copy per frame
  rdma_rx_gpu  -- frames already IN GPU memory (GPUDirect); opened zero-copy
                  through CUDA IPC. Run it and this app as the SAME
                  (non-root) user -- see scripts/05_gpudirect_setup.sh.

INTERACTIVE CONTROLS (window focused; the same commands can be typed into
the console -- 'h' or 'help' lists them):

  Time domain   arrows pan   +/- zoom   a/z amplitude   c cursor   , . move
  Spectrum      s/S span     n/m pan    g center-on-peak
                [/] reference level    {/} dB range
                mouse: scroll zoom, drag pan, click places marker
                k marker at peak       K clear marker
  DSP           f/F FFT size    v averaging    d/D decimation    e/E interp
  Transmitter   console: pace <Gb/s> | fs <Hz> | fc <Hz>      (live!)
  Misc          space pause   0 reset views   h help   q quit

Console values accept suffixes: fs 122.88M, fc 3.5G, span 10M, pace 25.
"""
import argparse
import math
import mmap
import os
import resource
import struct
import sys
import threading
import time

# Raise the stack soft limit BEFORE Holoscan spawns worker threads: Ubuntu's
# 8 MB default is below Holoscan's 32 MB minimum and can kill the process
# outright (the bare "Killed" with no traceback). Unprivileged processes may
# raise soft up to the hard limit, which is normally unlimited.
try:
    _soft, _hard = resource.getrlimit(resource.RLIMIT_STACK)
    _want = 32 * 1024 * 1024
    if _soft != resource.RLIM_INFINITY and _soft < _want:
        _new = _want if _hard == resource.RLIM_INFINITY else min(_want, _hard)
        resource.setrlimit(resource.RLIMIT_STACK, (_new, _hard))
except (ValueError, OSError):
    pass

import numpy as np

try:
    import cupy as cp
except ImportError:
    sys.exit("CuPy is required:  pip install cupy-cuda12x")
try:
    import cupyx.scipy.signal as cpsig
    HAVE_FFTCONV = hasattr(cpsig, "fftconvolve")
except Exception:
    HAVE_FFTCONV = False

try:
    from holoscan.core import Application, Operator, OperatorSpec
    from holoscan.operators import HolovizOp
except ImportError:
    sys.exit("Holoscan is required:  pip install holoscan "
             "(needs CUDA 12 runtime + NVIDIA driver >= 535)")

# ---------------------------------------------------------------------------
# Ring / control ABI -- byte offsets locked in src/rdma_common.h
# ---------------------------------------------------------------------------
RING_MAGIC      = 0x495152494E473031          # "IQRING01"  host ring
RING_MAGIC_GPU  = 0x495152494E474731          # "IQRINGG1"  GPUDirect ring
FRAME_MAGIC     = 0x49515246524D4131
CTRL_BYTES, HDR_BYTES, CTRL_IPC_OFF = 4096, 64, 1024
HDR_FMT = "<QQQIIQq"
OFF_NSLOTS, OFF_STRIDE, OFF_NSAMP = 12, 16, 20
OFF_FS, OFF_FC, OFF_WCOUNT, OFF_BYTES, OFF_GAPS = 24, 32, 40, 48, 56

IQCTL_MAGIC = 0x495143544C303031             # "IQCTL001"

# Viz-side control block (/dev/shm/iqviz): an external GUI panel writes the
# display parameters here and bumps 'seq'; the visualizer polls it each
# frame. Layout (little-endian), all at fixed offsets:
#   0  u64  magic = IQVIZ_MAGIC
#   8  u32  version
#  12  u32  seq            (bump after writing)
#  16  f64  span_frac      (0..1 of fs_eff)
#  24  f64  center_frac    (-0.5..0.5)
#  32  f64  amp
#  40  f64  t_len_frac     (fraction of frame shown in time domain)
#  48  f64  db_top
#  56  f64  db_range
#  64  u32  fft_n
#  68  u32  avg_n
#  72  u32  interp
#  76  u32  decim
#  80  u32  paused
#  84  u32  fit_tone       (bump-style trigger: panel increments to auto-fit)
IQVIZ_MAGIC = 0x495156495A303031             # "IQVIZ001"
IQVIZ_FMT_HEAD = "<QII"
IQVIZ_OFF = dict(span=16, center=24, amp=32, t_len=40, db_top=48,
                 db_range=56, fft_n=64, avg_n=68, interp=72, decim=76,
                 paused=80, fit=84)


class RingState:
    """One mmap of the receiver's shm; understands host and GPU layouts."""

    def __init__(self, path):
        fd = os.open(path, os.O_RDONLY)
        try:
            self.mm = mmap.mmap(fd, 0, prot=mmap.PROT_READ)
        finally:
            os.close(fd)
        magic = self._u64(0)
        if magic == RING_MAGIC:
            self.gpu = False
        elif magic == RING_MAGIC_GPU:
            self.gpu = True
        else:
            raise RuntimeError("bad ring magic (is a receiver running?)")
        self.num_slots     = self._u32(OFF_NSLOTS)
        self.slot_stride   = self._u32(OFF_STRIDE)
        self.frame_samples = self._u32(OFF_NSAMP)
        self._gpu_mem = None
        if self.gpu:
            handle = bytes(self.mm[CTRL_IPC_OFF:CTRL_IPC_OFF + 64])
            try:
                ptr = cp.cuda.runtime.ipcOpenMemHandle(handle)
            except Exception as e:
                raise RuntimeError(
                    f"CUDA IPC open failed: {e}\n"
                    "      rdma_rx_gpu and this app must run as the SAME "
                    "user\n      (see scripts/05_gpudirect_setup.sh)") from e
            total = self.num_slots * self.slot_stride
            self._gpu_mem = cp.cuda.UnownedMemory(ptr, total, owner=self)
            self._ipc_ptr = ptr

    def close(self):
        if self._gpu_mem is not None:
            try:
                cp.cuda.runtime.ipcCloseMemHandle(self._ipc_ptr)
            except Exception:
                pass
            self._gpu_mem = None

    def _u32(self, off):
        return struct.unpack_from("<I", self.mm, off)[0]

    def _u64(self, off):
        return struct.unpack_from("<Q", self.mm, off)[0]

    @property
    def write_count(self): return self._u64(OFF_WCOUNT)
    @property
    def bytes_total(self): return self._u64(OFF_BYTES)
    @property
    def seq_gaps(self):    return self._u64(OFF_GAPS)
    @property
    def sample_rate(self): return self._u64(OFF_FS) or 1
    @property
    def center_freq(self):
        return struct.unpack_from("<q", self.mm, OFF_FC)[0]

    def _hdr(self, slot):
        off = (CTRL_BYTES + slot * 64) if self.gpu \
              else (CTRL_BYTES + slot * self.slot_stride)
        return struct.unpack_from(HDR_FMT, self.mm, off), off

    def read_newest(self):
        """Torn-read-safe newest frame -> (seq, cupy int16 array) or None.

        write_count is published with release semantics only after the NIC
        finished DMA. If fewer than (slots - 4) frames arrived while we
        copied, our slot cannot have been overwritten mid-copy."""
        for _ in range(8):
            c1 = self.write_count
            if c1 == 0:
                return None
            slot = (c1 - 1) % self.num_slots
            (magic, seq, _t, nsamp, _fl, _fs, _fc), hoff = self._hdr(slot)
            if magic != FRAME_MAGIC or nsamp == 0:
                return None
            if self.gpu:
                mp = cp.cuda.MemoryPointer(self._gpu_mem,
                                           slot * self.slot_stride)
                view = cp.ndarray((2 * nsamp,), dtype=cp.int16, memptr=mp)
                payload = view.copy()            # device->device
            else:
                host = np.frombuffer(self.mm, dtype=np.int16,
                                     count=2 * nsamp,
                                     offset=hoff + HDR_BYTES).copy()
                payload = cp.asarray(host)       # the single H->D copy
            c2 = self.write_count
            if c2 - c1 <= self.num_slots - 4:
                return seq, payload
        return None


class IqCtl:
    """Writer for /dev/shm/iqctl -- live control of the running rdma_tx."""

    def __init__(self, path):
        self.ok = False
        try:
            # Open an existing block first: Ubuntu's fs.protected_regular
            # rejects O_CREAT opens of files another user (e.g. a sudo'd
            # rdma_tx) left in the sticky /dev/shm. Create only if absent.
            try:
                fd = os.open(path, os.O_RDWR)
            except FileNotFoundError:
                fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o666)
                try:
                    os.fchmod(fd, 0o666)      # best effort for cross-user RW
                except OSError:
                    pass
            os.ftruncate(fd, 4096)
            self.mm = mmap.mmap(fd, 4096)
            os.close(fd)
            if struct.unpack_from("<Q", self.mm, 0)[0] != IQCTL_MAGIC:
                struct.pack_into("<QII", self.mm, 0, IQCTL_MAGIC, 1, 0)
                struct.pack_into("<dQq", self.mm, 16, 10.0,
                                 100_000_000, 2_450_000_000)
            self.ok = True
        except OSError as e:
            print(f"[ctl] no live tx control ({e})")

    def read(self):
        pace, = struct.unpack_from("<d", self.mm, 16)
        fs,   = struct.unpack_from("<Q", self.mm, 24)
        fc,   = struct.unpack_from("<q", self.mm, 32)
        return pace, fs, fc

    def write(self, pace=None, fs=None, fc=None):
        if not self.ok:
            print("[ctl] control block unavailable")
            return
        cpa, cfs, cfc = self.read()
        struct.pack_into("<dQq", self.mm, 16,
                         cpa if pace is None else float(pace),
                         cfs if fs is None else int(fs),
                         cfc if fc is None else int(fc))
        seq, = struct.unpack_from("<I", self.mm, 12)
        struct.pack_into("<I", self.mm, 12, (seq + 1) & 0xFFFFFFFF)
        p, f, c = self.read()
        print(f"[ctl] -> tx  pace={p:g} Gb/s  fs={f:g} Hz  fc={c:g} Hz "
              "(applied at tx's next 1 s tick)")


class VizCtl:
    """Reads /dev/shm/iqviz (written by the GUI control panel) and applies
    display params to UIState. Polled by RingSource every frame."""

    def __init__(self, path, ui):
        self.ui = ui
        self.ok = False
        self.seq_seen = -1
        self.fit_seen = 0
        try:
            try:
                fd = os.open(path, os.O_RDWR)
            except FileNotFoundError:
                fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o666)
                try:
                    os.fchmod(fd, 0o666)
                except OSError:
                    pass
            os.ftruncate(fd, 4096)
            self.mm = mmap.mmap(fd, 4096)
            os.close(fd)
            # seed from current UI so the panel opens consistent
            if struct.unpack_from("<Q", self.mm, 0)[0] != IQVIZ_MAGIC:
                self._seed()
            self.ok = True
        except OSError as e:
            print(f"[viz] no GUI control block ({e}); keys/console still work")

    def _seed(self):
        u = self.ui.snap()
        struct.pack_into(IQVIZ_FMT_HEAD, self.mm, 0, IQVIZ_MAGIC, 1, 0)
        struct.pack_into("<dddddd", self.mm, IQVIZ_OFF["span"],
                         u["span"], u["center"], u["amp"], u["t_len"],
                         u["db_top"], u["db_range"])
        struct.pack_into("<IIIIII", self.mm, IQVIZ_OFF["fft_n"],
                         u["fft_n"], u["avg_n"], u["interp"], u["decim"],
                         1 if u["paused"] else 0, 0)

    def poll(self):
        if not self.ok:
            return
        if struct.unpack_from("<Q", self.mm, 0)[0] != IQVIZ_MAGIC:
            return
        seq, = struct.unpack_from("<I", self.mm, 12)
        fit, = struct.unpack_from("<I", self.mm, IQVIZ_OFF["fit"])
        if seq == self.seq_seen and fit == self.fit_seen:
            return
        self.seq_seen, do_fit = seq, (fit != self.fit_seen)
        self.fit_seen = fit
        (span, center, amp, t_len, db_top, db_range) = \
            struct.unpack_from("<dddddd", self.mm, IQVIZ_OFF["span"])
        (fft_n, avg_n, interp, decim, paused) = \
            struct.unpack_from("<IIIII", self.mm, IQVIZ_OFF["fft_n"])
        with self.ui.lock:
            self.ui.span = min(1.0, max(1 / 256, span))
            self.ui.center = max(-0.5, min(0.5, center))
            self.ui.amp = max(0.05, min(1e4, amp))
            self.ui.t_len = min(1.0, max(8 / 262144, t_len))
            self.ui.db_top = db_top
            self.ui.db_range = max(20.0, min(160.0, db_range))
            self.ui.fft_n = min(65536, max(1024, int(fft_n)))
            self.ui.avg_n = min(64, max(1, int(avg_n)))
            self.ui.interp = min(8, max(1, int(interp)))
            self.ui.decim = min(8, max(1, int(decim)))
            self.ui.paused = bool(paused)
        if do_fit:
            fs = self.ui.snap()["fs_eff"]
            ft = self.ui.snap()["peak_baseband_hz"]
            if fs > 0 and ft > 0:
                samples = max(32.0, min(8192.0, 10.0 * fs / ft))
                with self.ui.lock:
                    self.ui.t_len = samples / 262144.0
                    self.ui.t_start = 0.0


# ---------------------------------------------------------------------------
# Shared interactive state (callbacks + console REPL write; operators read)
# ---------------------------------------------------------------------------
class UIState:
    def __init__(self, fft0):
        self.lock = threading.Lock()
        # time-domain view
        self.t_start = 0.0          # fraction of frame [0,1)
        self.t_len = 256 / 262144   # fraction of frame (~few cycles, readable)
        self.amp = 1.0              # amplitude zoom
        self.cursor_on = False
        self.cursor_pos = 0.5       # fraction of current view
        # spectrum view
        self.span = 1.0             # fraction of fs_eff
        self.center = 0.0           # baseband center, fraction of fs_eff
        self.db_top = 5.0
        self.db_range = 115.0
        self.marker_hz = None       # absolute Hz
        # dsp
        self.fft_n = fft0
        self.avg_n = 1
        self.interp = 1
        self.decim = 1
        # misc / runtime
        self.paused = False
        self.quit = False
        self.win_w, self.win_h = 1500.0, 900.0
        self.mouse_xy = (0.0, 0.0)
        self.drag = None            # (region, x0_norm, anchor_value)
        self.moved = 0.0
        # published by DSP each frame (for click->marker, g key)
        self.cur_f_lo = 0.0
        self.cur_f_hi = 1.0
        self.last_peak_hz = 0.0
        self.fs_eff = 1.0
        self.peak_baseband_hz = 1.0

    def snap(self):
        with self.lock:
            d = dict(self.__dict__)
        d.pop("lock", None)
        return d


def fmt_si(v, unit="Hz", digits=4):
    a = abs(v)
    for s, m in (("G", 1e9), ("M", 1e6), ("k", 1e3)):
        if a >= m:
            return f"{v / m:.{digits}g} {s}{unit}"
    if a >= 1 or v == 0:
        return f"{v:.{digits}g} {unit}"
    for s, m in (("m", 1e-3), ("u", 1e-6), ("n", 1e-9)):
        if a >= m:
            return f"{v / m:.{digits}g} {s}{unit}"
    return f"{v:.3g} {unit}"


def parse_si(tok):
    tok = tok.strip()
    mult = 1.0
    if tok and tok[-1] in "kKmMgG":
        mult = {"k": 1e3, "K": 1e3, "m": 1e6, "M": 1e6,
                "g": 1e9, "G": 1e9}[tok[-1]]
        tok = tok[:-1]
    return float(tok) * mult


def nice_ticks(lo, hi, target=6):
    rng = hi - lo
    if rng <= 0:
        return [lo]
    raw = rng / target
    mag = 10 ** math.floor(math.log10(raw))
    step = mag
    for mlt in (1, 2, 5, 10):
        if mag * mlt >= raw:
            step = mag * mlt
            break
    first = math.ceil(lo / step) * step
    out, v = [], first
    while v <= hi + step * 1e-6:
        out.append(0.0 if abs(v) < step * 1e-6 else v)
        v += step
    return out


# ---------------------------------------------------------------------------
# Plot layout (normalized window coords, origin top-left, y points DOWN --
# Holoviz geometry layers use exactly this [0,1] convention)
# ---------------------------------------------------------------------------
TD = dict(x0=0.075, x1=0.965, y0=0.075, y1=0.42)
SP = dict(x0=0.075, x1=0.695, y0=0.535, y1=0.895)
RF_X, FOOT_Y = 0.725, 0.962
TD_PTS, SP_PTS = 2048, 1600


# ---------------------------------------------------------------------------
# Operator 1: ring -> GPU (newest-frame sampler, fps paced)
# ---------------------------------------------------------------------------
class RingSource(Operator):
    def __init__(self, fragment, *args, ring_path, fps, ui, vizctl=None,
                 **kwargs):
        self.ring_path = ring_path
        self.period = 1.0 / max(fps, 1)
        self.ui = ui
        self.vizctl = vizctl
        self.ring = None
        self.last_emit = 0.0
        self.cached = None
        self.prev_stats = None
        super().__init__(fragment, *args, **kwargs)

    def setup(self, spec: OperatorSpec):
        spec.output("out")

    def start(self):
        print(f"[viz] waiting for ring {self.ring_path} ...")
        while True:
            try:
                self.ring = RingState(self.ring_path)
                break
            except FileNotFoundError:
                time.sleep(0.25)
            except RuntimeError as e:
                if "IPC" in str(e):
                    sys.exit(f"[viz] {e}")
                time.sleep(0.25)
        mode = "GPUDirect, zero-copy" if self.ring.gpu else "host shm"
        print(f"[viz] ring open [{mode}]: {self.ring.num_slots} slots, "
              f"{self.ring.frame_samples} IQ samples/frame")

    def stop(self):
        if self.ring:
            self.ring.close()

    def compute(self, op_input, op_output, context):
        now = time.monotonic()
        wait = self.period - (now - self.last_emit)
        if wait > 0:
            time.sleep(wait)
        self.last_emit = time.monotonic()

        if self.vizctl is not None:
            self.vizctl.poll()
        if self.ui.snap()["quit"]:
            # Graceful stop: close the ring (releases CUDA IPC handle in
            # GPUDirect mode), then ask the GXF scheduler to halt this
            # operator. If the runtime offers no clean stop, fall back to a
            # hard exit so the process never hangs.
            try:
                if self.ring:
                    self.ring.close()
            except Exception:
                pass
            try:
                # Newer Holoscan: raising from compute stops the graph
                # cleanly; if not, the except below catches the hard path.
                self.stop()
            except Exception:
                pass
            os._exit(0)
        paused = self.ui.snap()["paused"]

        got = None
        if not paused:
            got = self.ring.read_newest()
            deadline = time.monotonic() + 0.5
            while got is None and time.monotonic() < deadline:
                time.sleep(0.01)
                got = self.ring.read_newest()
            if got is not None:
                self.cached = got
        if self.cached is None:
            return                            # nothing has ever arrived
        seq, payload = self.cached

        t = time.monotonic()
        gbps = gaps_s = 0.0
        bt, gp = self.ring.bytes_total, self.ring.seq_gaps
        if self.prev_stats:
            dt = t - self.prev_stats[0]
            if dt > 0:
                gbps = (bt - self.prev_stats[1]) * 8.0 / dt / 1e9
                gaps_s = (gp - self.prev_stats[2]) / dt
        self.prev_stats = (t, bt, gp)

        op_output.emit({
            "iq": payload,
            "meta": {"seq": seq,
                     "fs": float(self.ring.sample_rate),
                     "fc": float(self.ring.center_freq),
                     "gbps": gbps, "gaps_per_s": gaps_s,
                     "gpu_direct": self.ring.gpu,
                     "stale": got is None and not paused,
                     "paused": paused},
        }, "out")


# ---------------------------------------------------------------------------
# GPU resampler: interpolate L / decimate D with a windowed-sinc FIR
# ---------------------------------------------------------------------------
class Resampler:
    def __init__(self):
        self.key = None
        self.h = None

    def _design(self, L, D):
        m = max(L, D)
        ntaps = min(16 * m + 1, 257)
        n = np.arange(ntaps) - (ntaps - 1) / 2
        cutoff = 0.5 / m                      # of post-interpolation rate
        h = 2 * cutoff * np.sinc(2 * cutoff * n) * np.hamming(ntaps) * L
        return cp.asarray(h.astype(np.complex64))

    def run(self, c, L, D):
        if L == 1 and D == 1:
            return c
        if self.key != (L, D):
            self.h = self._design(L, D)
            self.key = (L, D)
        if L > 1:
            up = cp.zeros(c.size * L, dtype=cp.complex64)
            up[::L] = c
            c = up
        if HAVE_FFTCONV:
            c = cpsig.fftconvolve(c, self.h, mode="same")
        else:
            c = cp.convolve(c, self.h, mode="same")
        return cp.ascontiguousarray(c[::D].astype(cp.complex64))


# ---------------------------------------------------------------------------
# Operator 2: DSP + scene building
# ---------------------------------------------------------------------------
class SpectrumDSP(Operator):
    def __init__(self, fragment, *args, ui, dynamic_text, **kwargs):
        self.ui = ui
        self.dyn = dynamic_text
        self.resampler = Resampler()
        self.psd_avg = None
        self.avg_alpha_n = 1
        self._win = None            # cached Hann window
        self._win_n = 0
        self.last_print = 0.0
        super().__init__(fragment, *args, **kwargs)

    def setup(self, spec: OperatorSpec):
        spec.input("in")
        spec.output("tensors")
        if self.dyn:
            spec.output("specs")

    @staticmethod
    def _strip(xy):
        return cp.ascontiguousarray(xy.astype(cp.float32)[cp.newaxis, ...])

    @staticmethod
    def _lines(pairs):
        a = np.asarray(pairs, dtype=np.float32).reshape(1, -1, 2)
        return cp.ascontiguousarray(cp.asarray(a))

    @staticmethod
    def _textpts(pts):
        a = np.asarray(pts, dtype=np.float32).reshape(1, -1, 3)
        return cp.ascontiguousarray(cp.asarray(a))

    def compute(self, op_input, op_output, context):
        msg = op_input.receive("in")
        if msg is None:
            return
        ui = self.ui.snap()
        iq, meta = msg["iq"], msg["meta"]
        fs0, fc = meta["fs"], meta["fc"]

        f = iq.astype(cp.float32) / 32768.0
        c = (f[0::2] + 1j * f[1::2]).astype(cp.complex64)
        c = self.resampler.run(c, ui["interp"], ui["decim"])
        fs = fs0 * ui["interp"] / ui["decim"]
        nfrm = int(c.size)

        # ---------------- time domain ----------------
        v_len = max(16, int(ui["t_len"] * nfrm))
        v_st = max(0, min(int(ui["t_start"] * nfrm), nfrm - v_len))
        seg_i = cp.real(c[v_st:v_st + v_len])
        seg_q = cp.imag(c[v_st:v_st + v_len])

        # decimate-to-pixels preserving min/max (oscilloscope envelope)
        if v_len > TD_PTS:
            g = int(math.ceil(v_len / (TD_PTS // 2)))

            def env(seg):
                pad = (-int(seg.size)) % g
                if pad:
                    seg = cp.pad(seg, (0, pad), mode="edge")
                s = seg.reshape(-1, g)
                return cp.stack([s.max(1), s.min(1)], 1).ravel()
            seg_i, seg_q = env(seg_i), env(seg_q)
        npts = int(seg_i.size)

        a = ui["amp"]
        mid = (TD["y0"] + TD["y1"]) / 2
        half = (TD["y1"] - TD["y0"]) / 2 * 0.94
        xs = cp.linspace(TD["x0"], TD["x1"], npts, dtype=cp.float32)
        t_i = self._strip(cp.stack(
            [xs, mid - cp.clip(seg_i * a, -1, 1) * half], 1))
        t_q = self._strip(cp.stack(
            [xs, mid - cp.clip(seg_q * a, -1, 1) * half], 1))

        # ---------------- spectrum ----------------
        fft_n = min(ui["fft_n"],
                    1 << int(math.log2(max(nfrm, 1024))))
        if self._win is None or self._win_n != fft_n:
            self._win = cp.hanning(fft_n).astype(cp.float32)
            self._win_n = fft_n
        x = c[:fft_n] * self._win
        X = cp.fft.fftshift(cp.fft.fft(x))
        p_lin = (cp.abs(X) / fft_n) ** 2

        if (self.psd_avg is None or self.psd_avg.size != fft_n
                or self.avg_alpha_n != ui["avg_n"]):
            self.psd_avg = p_lin.copy()
            self.avg_alpha_n = ui["avg_n"]
        else:
            al = 1.0 / ui["avg_n"]
            self.psd_avg = (1 - al) * self.psd_avg + al * p_lin
        psd = 10.0 * cp.log10(self.psd_avg + 1e-14)

        span = ui["span"]
        ctr = max(-0.5 + span / 2, min(0.5 - span / 2, ui["center"]))
        lo = int((ctr - span / 2 + 0.5) * fft_n)
        hi = max(lo + 8, int((ctr + span / 2 + 0.5) * fft_n))
        sl = psd[lo:hi]
        if int(sl.size) > SP_PTS:
            g = int(math.ceil(int(sl.size) / SP_PTS))
            pad = (-int(sl.size)) % g
            if pad:
                sl = cp.pad(sl, (0, pad), mode="edge")
            sl = sl.reshape(-1, g).max(1)
        db_top, db_bot = ui["db_top"], ui["db_top"] - ui["db_range"]
        sl = cp.clip(sl, db_bot, db_top)
        sy = SP["y0"] + (db_top - sl) / (db_top - db_bot) \
            * (SP["y1"] - SP["y0"])
        sx = cp.linspace(SP["x0"], SP["x1"], int(sl.size),
                         dtype=cp.float32)
        t_sp = self._strip(cp.stack([sx, sy], 1))

        # ---------------- RF parameters (full-res averaged PSD) ----------
        pl = self.psd_avg
        tot = cp.sum(pl)                  # stays a 0-d CuPy array on GPU
        cs = cp.cumsum(pl)
        # CuPy's searchsorted (unlike NumPy's) rejects Python scalars for
        # 'v' -- pass both OBW thresholds as one small device array.
        edges = cp.searchsorted(cs, cp.stack((0.005 * tot, 0.995 * tot)))
        b_lo, b_hi = int(edges[0]), int(edges[1])
        obw = (b_hi - b_lo) * fs / fft_n
        pk = int(cp.argmax(psd))
        peak_db = float(psd[pk])
        peak_f = fc + (pk / fft_n - 0.5) * fs
        floor_db = float(cp.median(psd))
        pwr = cp.abs(c) ** 2
        rms_db = float(10 * cp.log10(float(cp.mean(pwr)) + 1e-12))
        papr = float(10 * cp.log10(float(cp.max(pwr)) /
                                   (float(cp.mean(pwr)) + 1e-12) + 1e-12))
        rbw = fs / fft_n * 1.5                       # Hann ENBW

        # ---------------- decorations: grid, ticks, cursor, marker -------
        f_lo = fc + (ctr - span / 2) * fs
        f_hi = fc + (ctr + span / 2) * fs
        with self.ui.lock:                    # publish for mouse handlers
            self.ui.cur_f_lo, self.ui.cur_f_hi = f_lo, f_hi
            self.ui.last_peak_hz = peak_f
            self.ui.fs_eff = fs
            self.ui.peak_baseband_hz = abs(peak_f - fc) or fs / 100.0

        segs, texts, tpts = [], [], []

        def box(b):
            segs.extend([(b["x0"], b["y0"]), (b["x1"], b["y0"]),
                         (b["x1"], b["y0"]), (b["x1"], b["y1"]),
                         (b["x1"], b["y1"]), (b["x0"], b["y1"]),
                         (b["x0"], b["y1"]), (b["x0"], b["y0"])])
        box(TD)
        box(SP)

        # time axis (seconds into the frame, units auto-scale)
        t0s, t1s = v_st / fs, (v_st + v_len) / fs
        for tv in nice_ticks(t0s, t1s):
            fx = TD["x0"] + (tv - t0s) / (t1s - t0s) * (TD["x1"] - TD["x0"])
            if TD["x0"] - 1e-6 <= fx <= TD["x1"] + 1e-6:
                segs.extend([(fx, TD["y1"]), (fx, TD["y1"] + 0.012)])
                texts.append(fmt_si(tv, "s", 3))
                tpts.append((fx - 0.02, TD["y1"] + 0.016, 0.0155))
        # amplitude axis (full-scale units, follows amplitude zoom)
        for av in (1 / a, 0.0, -1 / a):
            fy = mid - max(-1.0, min(1.0, av * a)) * half
            segs.extend([(TD["x0"] - 0.012, fy), (TD["x0"], fy)])
            texts.append(f"{av:+.3g}" if av else "0")
            tpts.append((0.012, fy - 0.011, 0.0155))

        # frequency axis (absolute Hz)
        for fv in nice_ticks(f_lo, f_hi):
            fx = SP["x0"] + (fv - f_lo) / (f_hi - f_lo) \
                * (SP["x1"] - SP["x0"])
            if SP["x0"] - 1e-6 <= fx <= SP["x1"] + 1e-6:
                segs.extend([(fx, SP["y1"]), (fx, SP["y1"] + 0.012)])
                texts.append(fmt_si(fv, "Hz", 5))
                tpts.append((fx - 0.025, SP["y1"] + 0.016, 0.0145))
        # dB axis labels + horizontal gridlines
        for dv in nice_ticks(db_bot, db_top, 6):
            fy = SP["y0"] + (db_top - dv) / (db_top - db_bot) \
                * (SP["y1"] - SP["y0"])
            segs.extend([(SP["x0"], fy), (SP["x1"], fy)])
            texts.append(f"{dv:.0f}")
            tpts.append((0.024, fy - 0.011, 0.0155))

        # time cursor
        cur_txt = ""
        csegs = []
        if ui["cursor_on"]:
            ci = min(v_len - 1, max(0, int(ui["cursor_pos"] * v_len)))
            fx = TD["x0"] + ci / max(1, v_len - 1) * (TD["x1"] - TD["x0"])
            csegs.extend([(fx, TD["y0"]), (fx, TD["y1"])])
            iv = float(cp.real(c[v_st + ci]))
            qv = float(cp.imag(c[v_st + ci]))
            cur_txt = (f"Cursor  : t={fmt_si((v_st + ci) / fs, 's', 4)}  "
                       f"I={iv:+.4f}  Q={qv:+.4f}  "
                       f"|x|={math.hypot(iv, qv):.4f}")

        # spectrum marker
        mk_txt = ""
        if ui["marker_hz"] is not None and f_lo <= ui["marker_hz"] <= f_hi:
            mb = min(fft_n - 1, max(0, int(round(
                ((ui["marker_hz"] - fc) / fs + 0.5) * fft_n))))
            mdb = float(psd[mb])
            fx = SP["x0"] + (ui["marker_hz"] - f_lo) / (f_hi - f_lo) \
                * (SP["x1"] - SP["x0"])
            csegs.extend([(fx, SP["y0"]), (fx, SP["y1"])])
            mk_txt = (f"Marker  : {fmt_si(ui['marker_hz'], 'Hz', 6)}  "
                      f"{mdb:.1f} dBFS  (peak{peak_db - mdb:+.1f} dB)")

        rf_lines = [
            f"Fs in   : {fmt_si(fs0)}",
            f"Fs eff  : {fmt_si(fs)}  (L{ui['interp']}/D{ui['decim']})",
            f"Fc      : {fmt_si(fc)}",
            f"Span    : {fmt_si(f_hi - f_lo)}",
            f"RBW     : {fmt_si(rbw)}",
            f"Peak    : {peak_db:6.1f} dBFS",
            f"Peak f  : {fmt_si(peak_f, 'Hz', 6)}",
            f"Floor   : {floor_db:6.1f} dBFS",
            f"SNR     : {peak_db - floor_db:6.1f} dB",
            f"OBW 99% : {fmt_si(obw)}",
            f"RMS     : {rms_db:6.1f} dBFS",
            f"PAPR    : {papr:6.1f} dB",
            f"Link    : {meta['gbps']:6.2f} Gb/s"
            + ("  [GPUDirect]" if meta["gpu_direct"] else ""),
            f"Seq     : {meta['seq']}   gaps/s {meta['gaps_per_s']:.0f}",
        ]
        if mk_txt:
            rf_lines.append(mk_txt)
        if cur_txt:
            rf_lines.append(cur_txt)
        if meta["stale"]:
            rf_lines.append("** NO NEW DATA **")

        foot = (f"fft {fft_n}   avg {ui['avg_n']}   "
                f"view {fmt_si(t1s - t0s, 's', 3)}   amp x{a:.3g}   "
                f"span {fmt_si(f_hi - f_lo)}"
                f"{'   PAUSED' if ui['paused'] else ''}"
                "       w = fit to tone    h = help / live fs,fc")

        tensors = {"grid": self._lines(segs),
                   "iq_i": t_i, "iq_q": t_q, "spec": t_sp}
        if csegs:
            tensors["cursors"] = self._lines(csegs)

        if self.dyn:
            heads = [(TD["x0"], 0.03, 0.0205),
                     (SP["x0"], 0.494, 0.0205),
                     (RF_X, 0.494, 0.0205)]
            heads_txt = ["TIME DOMAIN     I (cyan)   Q (orange)",
                         "SPECTRUM  (dBFS)", "RF / LINK"]
            rf_pts = [(RF_X, SP["y0"] + 0.0255 * k, 0.0175)
                      for k in range(len(rf_lines))]
            tensors["heads"] = self._textpts(heads)
            tensors["rfpanel"] = self._textpts(rf_pts)
            tensors["footer"] = self._textpts([(TD["x0"], FOOT_Y, 0.0165)])
            if tpts:
                tensors["ticks"] = self._textpts(tpts)
            op_output.emit(self._make_specs(
                heads_txt, rf_lines, [foot], texts,
                have_cursors=bool(csegs), have_ticks=bool(tpts)), "specs")

        op_output.emit(tensors, "tensors")

        if time.monotonic() - self.last_print >= 1.0:
            self.last_print = time.monotonic()
            print("[rf ] " + "  |  ".join(
                s.replace("  ", " ") for s in rf_lines[:14]))

    def _make_specs(self, heads_txt, rf_lines, foot, ticks_txt,
                    have_cursors, have_ticks):
        S = HolovizOp.InputSpec
        specs = []

        g = S("grid", "lines")
        g.color = [0.28, 0.32, 0.38, 1.0]
        g.line_width = 1.0
        g.priority = 0
        specs.append(g)

        if have_cursors:
            cu = S("cursors", "lines")
            cu.color = [1.0, 0.25, 0.45, 1.0]
            cu.line_width = 1.5
            cu.priority = 2
            specs.append(cu)

        for name, color, lw in (("iq_i", [0.10, 0.85, 1.00, 1.0], 1.8),
                                ("iq_q", [1.00, 0.62, 0.15, 1.0], 1.8),
                                ("spec", [0.30, 1.00, 0.45, 1.0], 2.2)):
            s = S(name, "line_strip")
            s.color = color
            s.line_width = lw
            s.priority = 1
            specs.append(s)

        hd = S("heads", "text")
        hd.text = heads_txt
        hd.color = [0.85, 0.88, 0.95, 1.0]
        hd.priority = 3
        specs.append(hd)

        rf = S("rfpanel", "text")
        rf.text = rf_lines
        rf.color = [0.95, 0.95, 0.60, 1.0]
        rf.priority = 3
        specs.append(rf)

        ft = S("footer", "text")
        ft.text = foot
        ft.color = [0.65, 0.70, 0.78, 1.0]
        ft.priority = 3
        specs.append(ft)

        if have_ticks:
            tk = S("ticks", "text")
            tk.text = ticks_txt
            tk.color = [0.62, 0.66, 0.74, 1.0]
            tk.priority = 3
            specs.append(tk)
        return specs


# ---------------------------------------------------------------------------
# Input handling: window callbacks (GLFW conventions) + console REPL.
# Both funnel into the same actions on UIState.
# ---------------------------------------------------------------------------
HELP = """
-------------------------------- CONTROLS ---------------------------------
Time domain   <- -> pan (Shift = fine)   + - zoom   a/z amplitude
              w auto-fit to tone (~10 cycles)   c cursor   , . move cursor
Spectrum      s/S span in/out   n/m pan   g center on peak
              [ ] reference level   { } dB range
              k marker at peak   K clear   (mouse click also sets marker)
Mouse         scroll = zoom (over a plot), drag = pan, click = marker/cursor
              drag the window edge/corner to resize -- plots reflow to fit
DSP           f/F FFT 1k..64k   v cycle averaging 1..32   V reset
              d/D decimation 1..8   e/E interpolation 1..8
Misc          space/p pause   0 reset views   q quit   h this help
LIVE TUNING (type in this console, then Enter):
  fs 122.88M    change sample rate on the running transmitter
  fc 3.5G       change centre frequency on the running transmitter
  pace 25       change link rate to 25 Gb/s   (pace 0 = unlimited)
  fft 16384 | avg 8 | span 10M | center 3.5G | marker 2.46G
  (values take k/M/G suffixes, e.g. fc 3.5G, span 10M)
----------------------------------------------------------------------------
"""


class InputHandler:
    def __init__(self, ui: UIState, ctl: IqCtl):
        self.ui = ui
        self.ctl = ctl

    # ---- shared actions ----------------------------------------------------
    def act(self, ch):
        u = self.ui
        with u.lock:
            if ch in "+=":
                rel = u.cursor_pos if u.cursor_on else 0.5
                mid = u.t_start + rel * u.t_len
                u.t_len = max(1e-5, u.t_len / 2)
                u.t_start = min(max(0.0, mid - rel * u.t_len), 1 - u.t_len)
            elif ch == "-":
                mid = u.t_start + 0.5 * u.t_len
                u.t_len = min(1.0, u.t_len * 2)
                u.t_start = min(max(0.0, mid - 0.5 * u.t_len), 1 - u.t_len)
            elif ch == "a":
                u.amp = min(1e4, u.amp * 1.25)
            elif ch == "z":
                u.amp = max(0.1, u.amp / 1.25)
            elif ch == "c":
                u.cursor_on = not u.cursor_on
            elif ch == "w":
                pass            # handled below using DSP-published fs/peak
            elif ch == ",":
                u.cursor_pos = max(0.0, u.cursor_pos - 0.02)
            elif ch == ".":
                u.cursor_pos = min(1.0, u.cursor_pos + 0.02)
            elif ch == "s":
                u.span = max(1 / 64, u.span / 2)
            elif ch == "S":
                u.span = min(1.0, u.span * 2)
            elif ch == "n":
                u.center -= u.span / 8
            elif ch == "m":
                u.center += u.span / 8
            elif ch == "g":
                pass            # handled below using DSP-published values
            elif ch == "[":
                u.db_top -= 5
            elif ch == "]":
                u.db_top += 5
            elif ch == "{":
                u.db_range = max(30, u.db_range - 10)
            elif ch == "}":
                u.db_range = min(160, u.db_range + 10)
            elif ch == "k":
                u.marker_hz = u.last_peak_hz
            elif ch == "K":
                u.marker_hz = None
            elif ch == "f":
                u.fft_n = max(1024, u.fft_n // 2)
            elif ch == "F":
                u.fft_n = min(65536, u.fft_n * 2)
            elif ch == "v":
                u.avg_n = u.avg_n * 2 if u.avg_n < 32 else 1
            elif ch == "V":
                u.avg_n = 1
            elif ch == "d":
                u.decim = min(8, u.decim + 1)
            elif ch == "D":
                u.decim = max(1, u.decim - 1)
            elif ch == "e":
                u.interp = min(8, u.interp + 1)
            elif ch == "E":
                u.interp = max(1, u.interp - 1)
            elif ch in " p":
                u.paused = not u.paused
            elif ch == "0":
                u.t_start, u.t_len, u.amp = 0.0, 256 / 262144, 1.0
                u.span, u.center = 1.0, 0.0
                u.db_top, u.db_range = 5.0, 115.0
            elif ch == "q":
                u.quit = True
            elif ch == "h":
                print(HELP)
        if ch == "g":   # outside lock: uses values DSP published
            s = self.ui.snap()
            fspan = s["cur_f_hi"] - s["cur_f_lo"]
            if fspan > 0:
                fs_full = fspan / s["span"]
                fc_abs = (s["cur_f_lo"] + s["cur_f_hi"]) / 2 \
                    - s["center"] * fs_full
                with self.ui.lock:
                    self.ui.center = (s["last_peak_hz"] - fc_abs) / fs_full
        elif ch == "w":   # auto-fit time view to ~10 cycles of the tone
            s = self.ui.snap()
            fs = s["fs_eff"]
            f_tone = s["peak_baseband_hz"]
            if fs > 0 and f_tone > 0:
                # samples for ~10 cycles = 10 * fs / f_tone
                samples = max(32.0, min(8192.0, 10.0 * fs / f_tone))
                with self.ui.lock:
                    self.ui.t_len = samples / 262144.0
                    self.ui.t_start = 0.0

    # ---- GLFW window callbacks (registered on HolovizOp) -------------------
    def key(self, *args):
        try:
            key, _sc, action, mods = (list(args) + [0, 0, 0, 0])[:4]
            key, action, mods = int(key), int(action), int(mods)
        except Exception:
            return
        if action not in (1, 2):              # press / repeat
            return
        u = self.ui
        fine = 50 if (mods & 1) else 5        # Shift = fine
        with u.lock:
            if key == 263:                    # left
                u.t_start = max(0.0, u.t_start - u.t_len / fine)
            elif key == 262:                  # right
                u.t_start = min(1 - u.t_len, u.t_start + u.t_len / fine)
            elif key == 265:                  # up
                u.amp = min(1e4, u.amp * 1.25)
            elif key == 264:                  # down
                u.amp = max(0.1, u.amp / 1.25)
            elif key == 32:                   # space
                if action == 1:
                    u.paused = not u.paused

    def char(self, *args):
        try:
            self.act(chr(int(args[0])))
        except Exception:
            pass

    def cursor(self, *args):
        try:
            x, y = float(args[0]), float(args[1])
        except Exception:
            return
        u = self.ui
        with u.lock:
            nx, ny = x / u.win_w, y / u.win_h
            u.mouse_xy = (nx, ny)
            if u.drag:
                region, x0, anchor = u.drag
                dx = nx - x0
                u.moved += abs(dx)
                if region == "td":
                    u.t_start = min(max(0.0, anchor - dx /
                                        (TD["x1"] - TD["x0"]) * u.t_len),
                                    1 - u.t_len)
                elif region == "sp":
                    u.center = anchor - dx / (SP["x1"] - SP["x0"]) * u.span

    @staticmethod
    def _region(nx, ny):
        if TD["x0"] <= nx <= TD["x1"] and TD["y0"] <= ny <= TD["y1"]:
            return "td"
        if SP["x0"] <= nx <= SP["x1"] and SP["y0"] <= ny <= SP["y1"]:
            return "sp"
        return None

    def mouse(self, *args):
        try:
            button, action = int(args[0]), int(args[1])
        except Exception:
            return
        if button != 0:
            return
        u = self.ui
        if action == 1:                       # press: maybe start drag
            with u.lock:
                nx, ny = u.mouse_xy
                r = self._region(nx, ny)
                if r == "td":
                    u.drag = ("td", nx, u.t_start)
                elif r == "sp":
                    u.drag = ("sp", nx, u.center)
                u.moved = 0.0
        else:                                 # release: click if no drag
            with u.lock:
                nx, ny = u.mouse_xy
                drag, moved = u.drag, u.moved
                u.drag = None
            if drag and moved < 0.004:
                r = drag[0]
                if r == "sp":
                    s = self.ui.snap()
                    rel = (nx - SP["x0"]) / (SP["x1"] - SP["x0"])
                    hz = s["cur_f_lo"] + rel * (s["cur_f_hi"]
                                                - s["cur_f_lo"])
                    with self.ui.lock:
                        self.ui.marker_hz = hz
                elif r == "td":
                    rel = (nx - TD["x0"]) / (TD["x1"] - TD["x0"])
                    with self.ui.lock:
                        self.ui.cursor_on = True
                        self.ui.cursor_pos = min(1.0, max(0.0, rel))

    def scroll(self, *args):
        try:
            dy = float(args[-1])
        except Exception:
            return
        u = self.ui
        with u.lock:
            nx, ny = u.mouse_xy
            r = self._region(nx, ny)
            fac = 0.8 ** dy
            if r == "td":
                rel = (nx - TD["x0"]) / (TD["x1"] - TD["x0"])
                mid = u.t_start + rel * u.t_len
                u.t_len = min(1.0, max(1e-5, u.t_len * fac))
                u.t_start = min(max(0.0, mid - rel * u.t_len), 1 - u.t_len)
            elif r == "sp":
                rel = (nx - SP["x0"]) / (SP["x1"] - SP["x0"])
                fmid = u.center + (rel - 0.5) * u.span
                u.span = min(1.0, max(1 / 64, u.span * fac))
                u.center = fmid - (rel - 0.5) * u.span

    def winsize(self, *args):
        try:
            with self.ui.lock:
                self.ui.win_w = max(1.0, float(args[0]))
                self.ui.win_h = max(1.0, float(args[1]))
        except Exception:
            pass

    # ---- console REPL -------------------------------------------------------
    def repl(self):
        print(HELP)
        while not self.ui.snap()["quit"]:
            try:
                line = input()
            except (EOFError, KeyboardInterrupt):
                return
            t = line.strip().split()
            if not t:
                continue
            cmd = t[0].lower()
            try:
                if cmd == "help":
                    print(HELP)
                elif cmd == "pace" and len(t) > 1:
                    self.ctl.write(pace=parse_si(t[1]))
                elif cmd == "fs" and len(t) > 1:
                    self.ctl.write(fs=parse_si(t[1]))
                elif cmd == "fc" and len(t) > 1:
                    self.ctl.write(fc=parse_si(t[1]))
                elif cmd == "fft" and len(t) > 1:
                    val = max(1024.0, min(65536.0, parse_si(t[1])))
                    n = 1 << int(round(math.log2(val)))
                    with self.ui.lock:
                        self.ui.fft_n = min(65536, max(1024, n))
                elif cmd == "avg" and len(t) > 1:
                    with self.ui.lock:
                        self.ui.avg_n = min(64, max(1, int(parse_si(t[1]))))
                elif cmd in ("span", "center", "marker") and len(t) > 1:
                    hz = parse_si(t[1])
                    s = self.ui.snap()
                    fs_full = (s["cur_f_hi"] - s["cur_f_lo"]) / s["span"]
                    fcen = (s["cur_f_lo"] + s["cur_f_hi"]) / 2 \
                        - s["center"] * fs_full
                    with self.ui.lock:
                        if cmd == "span" and fs_full > 0:
                            self.ui.span = min(1.0, max(1 / 64,
                                                        hz / fs_full))
                        elif cmd == "center" and fs_full > 0:
                            self.ui.center = (hz - fcen) / fs_full
                        else:
                            self.ui.marker_hz = hz
                elif cmd in ("quit", "exit"):
                    with self.ui.lock:
                        self.ui.quit = True
                elif len(cmd) == 1:
                    self.act(line.strip()[0])
                else:
                    print("[?] unknown command -- 'help' lists them")
            except (ValueError, IndexError) as e:
                print(f"[?] {e}")


# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
class IQMonitorApp(Application):
    def __init__(self, args, ui, handler, vizctl=None):
        self.args = args
        self.ui = ui
        self.handler = handler
        self.vizctl = vizctl
        super().__init__()

    def compose(self):
        src = RingSource(self, ring_path=self.args.ring,
                         fps=self.args.fps, ui=self.ui, vizctl=self.vizctl,
                         name="ring_source")
        dsp = SpectrumDSP(self, ui=self.ui,
                          dynamic_text=not self.args.no_dynamic_text,
                          name="spectrum_dsp")

        base = dict(name="holoviz", width=1500, height=900,
                    window_title="RoCEv2 -> GPU IQ Monitor")
        if self.args.no_dynamic_text:
            base["tensors"] = [
                dict(name="grid", type="lines",
                     color=[0.28, 0.32, 0.38, 1.0], line_width=1.0),
                dict(name="cursors", type="lines",
                     color=[1.0, 0.25, 0.45, 1.0], line_width=1.5),
                dict(name="iq_i", type="line_strip",
                     color=[0.10, 0.85, 1.00, 1.0], line_width=1.8),
                dict(name="iq_q", type="line_strip",
                     color=[1.00, 0.62, 0.15, 1.0], line_width=1.8),
                dict(name="spec", type="line_strip",
                     color=[0.30, 1.00, 0.45, 1.0], line_width=2.2),
            ]

        h = self.handler
        cbs = dict(key_callback=h.key, unicode_char_callback=h.char,
                   mouse_button_callback=h.mouse, scroll_callback=h.scroll,
                   cursor_pos_callback=h.cursor,
                   window_size_callback=h.winsize)
        try:
            viz = HolovizOp(self, **base, **cbs)
        except TypeError:
            print("[viz] this Holoscan build has no window callbacks -- "
                  "mouse/keys disabled, console commands still work.")
            viz = HolovizOp(self, **base)

        self.add_flow(src, dsp, {("out", "in")})
        self.add_flow(dsp, viz, {("tensors", "receivers")})
        if not self.args.no_dynamic_text:
            self.add_flow(dsp, viz, {("specs", "input_specs")})


class EmbeddedPanel:
    """In-process Tk control panel. Writes display params straight into the
    shared UIState (no shm round-trip) and transmitter params through IqCtl.
    Runs on the main thread; Holoscan runs in a daemon thread."""

    def __init__(self, ui, ctl):
        import tkinter as tk
        from tkinter import ttk
        self.tk, self.ttk = tk, ttk
        self.ui = ui
        self.ctl = ctl
        FRAME = 262144

        pace, fs, fc = (ctl.read() if ctl.ok else (10.0, 100e6, 2.45e9))

        root = tk.Tk()
        self.root = root
        root.title("IQ Pipeline Control")
        root.configure(bg="#15171c")
        st = ttk.Style(root)
        try:
            st.theme_use("clam")
        except tk.TclError:
            pass
        st.configure(".", background="#15171c", foreground="#dfe3ea",
                     fieldbackground="#23262e")
        st.configure("TLabelframe", background="#15171c")
        st.configure("TLabelframe.Label", background="#15171c",
                     foreground="#7fd6ff")
        st.configure("TButton", background="#2a2f3a")

        # vars first (avoids any callback firing before they exist)
        self.v_pace = tk.DoubleVar(value=pace)
        self.v_span = tk.DoubleVar(value=1.0)
        self.v_center = tk.DoubleVar(value=0.0)
        self.v_fft = tk.DoubleVar(value=ui.snap()["fft_n"])
        self.v_avg = tk.DoubleVar(value=1)
        self.v_dbtop = tk.DoubleVar(value=5.0)
        self.v_dbrange = tk.DoubleVar(value=115.0)
        self.v_amp = tk.DoubleVar(value=1.0)
        self.v_interp = tk.DoubleVar(value=1)
        self.v_decim = tk.DoubleVar(value=1)
        self.v_pause = tk.BooleanVar(value=False)
        self.FRAME = FRAME

        pad = dict(padx=6, pady=3, sticky="we")

        # ---- transmitter ----
        tx = ttk.LabelFrame(root, text="TRANSMITTER  (rdma_tx, live)")
        tx.grid(row=0, column=0, padx=8, pady=6, sticky="we")
        tx.columnconfigure(1, weight=1)
        ttk.Label(tx, text="Sample rate Fs").grid(row=0, column=0, **pad)
        self.e_fs = tk.StringVar(value=self._si(fs))
        ttk.Entry(tx, textvariable=self.e_fs, width=12).grid(
            row=0, column=1, columnspan=2, **pad)
        ttk.Label(tx, text="Centre freq Fc").grid(row=1, column=0, **pad)
        self.e_fc = tk.StringVar(value=self._si(fc))
        ttk.Entry(tx, textvariable=self.e_fc, width=12).grid(
            row=1, column=1, columnspan=2, **pad)
        ttk.Label(tx, text="Link pacing Gb/s (0=max)").grid(
            row=2, column=0, **pad)
        self.l_pace = ttk.Label(tx, text=f"{pace:.0f}")
        self.l_pace.grid(row=2, column=2, padx=6)
        ttk.Scale(tx, from_=0, to=60, variable=self.v_pace,
                  command=lambda e: self._pace()).grid(row=2, column=1, **pad)
        ttk.Button(tx, text="Apply Fs / Fc", command=self._txapply).grid(
            row=3, column=0, columnspan=3, pady=5)
        pf = ttk.Frame(tx)
        pf.grid(row=4, column=0, columnspan=3, sticky="we")
        for i, (lbl, f, c) in enumerate([
                ("100M/2.45G", 100e6, 2.45e9),
                ("122.88M/3.5G", 122.88e6, 3.5e9),
                ("61.44M/1.8G", 61.44e6, 1.8e9),
                ("200M/5.8G", 200e6, 5.8e9)]):
            ttk.Button(pf, text=lbl, width=12,
                       command=lambda f=f, c=c: self._preset(f, c)).grid(
                row=0, column=i, padx=2, pady=2)

        # ---- spectrum ----
        sp = ttk.LabelFrame(root, text="SPECTRUM  (display)")
        sp.grid(row=1, column=0, padx=8, pady=6, sticky="we")
        sp.columnconfigure(1, weight=1)
        self._sl(sp, 0, "Span (frac Fs)", self.v_span, 1/64, 1.0, "{:.3f}",
                 self._apply_disp)
        self._sl(sp, 1, "Centre (frac)", self.v_center, -0.5, 0.5, "{:+.3f}",
                 self._apply_disp)
        self._sl(sp, 2, "FFT size", self.v_fft, 1024, 65536, "{:.0f}",
                 self._apply_disp, pow2=True)
        self._sl(sp, 3, "Averaging", self.v_avg, 1, 32, "{:.0f}",
                 self._apply_disp)
        self._sl(sp, 4, "Ref level dBFS", self.v_dbtop, -60, 20, "{:.0f}",
                 self._apply_disp)
        self._sl(sp, 5, "dB range", self.v_dbrange, 30, 160, "{:.0f}",
                 self._apply_disp)

        # ---- time domain ----
        td = ttk.LabelFrame(root, text="TIME DOMAIN  (display)")
        td.grid(row=2, column=0, padx=8, pady=6, sticky="we")
        td.columnconfigure(1, weight=1)
        ttk.Label(td, text="Window (samples)").grid(row=0, column=0, **pad)
        self.l_tlen = ttk.Label(td, text="256")
        self.l_tlen.grid(row=0, column=2, padx=6)
        self.s_tlen = ttk.Scale(td, from_=5, to=14, command=self._tlen)
        self.s_tlen.set(8)
        self.s_tlen.grid(row=0, column=1, **pad)
        self._sl(td, 1, "Amplitude x", self.v_amp, 0.1, 20, "{:.2f}",
                 self._apply_disp)
        self._sl(td, 2, "Interpolation", self.v_interp, 1, 8, "{:.0f}",
                 self._apply_disp)
        self._sl(td, 3, "Decimation", self.v_decim, 1, 8, "{:.0f}",
                 self._apply_disp)
        ttk.Button(td, text="Auto-fit to tone (~10 cycles)",
                   command=self._fit).grid(row=4, column=0, columnspan=3,
                                           pady=5)

        # ---- global + live readout ----
        gf = ttk.Frame(root)
        gf.grid(row=3, column=0, padx=8, pady=6, sticky="we")
        ttk.Checkbutton(gf, text="Pause", variable=self.v_pause,
                        command=self._apply_disp).grid(row=0, column=0,
                                                       padx=6)
        ttk.Button(gf, text="Reset display", command=self._reset).grid(
            row=0, column=1, padx=6)
        self.readout = ttk.Label(root, text="", foreground="#9fe6b0",
                                 justify="left", font=("monospace", 9))
        self.readout.grid(row=4, column=0, padx=10, pady=(0, 8), sticky="w")
        root.protocol("WM_DELETE_WINDOW", self._close)
        self._apply_disp()
        self._tick()

    # ---- helpers ----
    @staticmethod
    def _si(v, unit="Hz"):
        a = abs(v)
        for s, m in (("G", 1e9), ("M", 1e6), ("k", 1e3)):
            if a >= m:
                return f"{v/m:.4g} {s}{unit}"
        return f"{v:.4g} {unit}"

    @staticmethod
    def _parse(s):
        s = s.strip()
        mult = 1.0
        if s and s[-1] in "kKmMgG":
            mult = {"k": 1e3, "K": 1e3, "m": 1e6, "M": 1e6,
                    "g": 1e9, "G": 1e9}[s[-1]]
            s = s[:-1]
        return float(s) * mult

    def _sl(self, parent, row, label, var, lo, hi, fmt, cb, pow2=False):
        ttk = self.ttk
        ttk.Label(parent, text=label).grid(row=row, column=0, padx=6,
                                           pady=3, sticky="w")
        lab = ttk.Label(parent, text=fmt.format(var.get()))
        lab.grid(row=row, column=2, padx=6)

        def on(_=None):
            if pow2:
                import math
                var.set(1 << max(10, min(16, round(math.log2(var.get())))))
            lab.config(text=fmt.format(var.get()))
            cb()
        ttk.Scale(parent, from_=lo, to=hi, variable=var, command=on).grid(
            row=row, column=1, padx=6, pady=3, sticky="we")

    # ---- actions: display -> UIState directly ----
    def _apply_disp(self):
        u = self.ui
        with u.lock:
            u.span = min(1.0, max(1/256, self.v_span.get()))
            u.center = max(-0.5, min(0.5, self.v_center.get()))
            u.fft_n = min(65536, max(1024, int(self.v_fft.get())))
            u.avg_n = min(64, max(1, int(self.v_avg.get())))
            u.db_top = self.v_dbtop.get()
            u.db_range = max(20.0, min(160.0, self.v_dbrange.get()))
            u.amp = max(0.05, min(1e4, self.v_amp.get()))
            u.interp = min(8, max(1, int(self.v_interp.get())))
            u.decim = min(8, max(1, int(self.v_decim.get())))
            u.paused = bool(self.v_pause.get())

    def _tlen(self, _=None):
        n = int(round(2 ** self.s_tlen.get()))
        self.l_tlen.config(text=str(n))
        with self.ui.lock:
            self.ui.t_len = n / self.FRAME

    def _fit(self):
        s = self.ui.snap()
        fs, ft = s["fs_eff"], s["peak_baseband_hz"]
        if fs > 0 and ft > 0:
            n = max(32.0, min(8192.0, 10.0 * fs / ft))
            with self.ui.lock:
                self.ui.t_len = n / self.FRAME
                self.ui.t_start = 0.0
            # slider is log2(samples); n is a float so use log2, not bit_length
            self.s_tlen.set(max(5, min(14, int(round(math.log2(n))))))
            self.l_tlen.config(text=str(int(n)))

    def _pace(self):
        self.l_pace.config(text=f"{self.v_pace.get():.0f}")
        if self.ctl.ok:
            self.ctl.write(pace=self.v_pace.get())

    def _txapply(self):
        try:
            fs = self._parse(self.e_fs.get())
            fc = self._parse(self.e_fc.get())
        except ValueError:
            self.readout.config(text="bad Fs/Fc value")
            return
        if self.ctl.ok:
            self.ctl.write(fs=fs, fc=fc)

    def _preset(self, f, c):
        self.e_fs.set(self._si(f))
        self.e_fc.set(self._si(c))
        self._txapply()

    def _reset(self):
        self.v_span.set(1.0); self.v_center.set(0.0); self.v_fft.set(8192)
        self.v_avg.set(1); self.v_dbtop.set(5.0); self.v_dbrange.set(115.0)
        self.v_amp.set(1.0); self.v_interp.set(1); self.v_decim.set(1)
        self.v_pause.set(False); self.s_tlen.set(8)
        self.l_tlen.config(text="256")
        with self.ui.lock:
            self.ui.t_len = 256 / self.FRAME
            self.ui.t_start = 0.0
        self._apply_disp()

    def _tick(self):
        # live RF readout mirrored from what the DSP publishes
        s = self.ui.snap()
        self.readout.config(text=(
            f"peak {self._si(s['last_peak_hz'])}   "
            f"fs_eff {self._si(s['fs_eff'])}   "
            f"{'PAUSED' if s['paused'] else 'running'}"))
        self.root.after(500, self._tick)

    def _close(self):
        with self.ui.lock:
            self.ui.quit = True
        self.root.destroy()

    def run(self):
        self.root.mainloop()


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ring", default="/dev/shm/iqring")
    ap.add_argument("--ctl", default="/dev/shm/iqctl")
    ap.add_argument("--vizctl", default="/dev/shm/iqviz")
    ap.add_argument("--fps", type=int, default=60)
    ap.add_argument("--fft", type=int, default=8192)
    ap.add_argument("--no-dynamic-text", action="store_true",
                    help="fallback if input_specs is rejected: plots only, "
                         "numbers on the console")
    ap.add_argument("--no-repl", action="store_true")
    ap.add_argument("--panel", action="store_true",
                    help="open the built-in GUI control panel (single app: "
                         "Tk panel + the Holoscan window together)")
    ap.add_argument("--no-panel", dest="panel", action="store_false")
    ap.set_defaults(panel=True)
    args = ap.parse_args()

    print(f"[viz] GPU: "
          f"{cp.cuda.runtime.getDeviceProperties(0)['name'].decode()}")
    ui = UIState(args.fft)
    ctl = IqCtl(args.ctl)
    handler = InputHandler(ui, ctl)

    # External GUI panel support: VizCtl reads /dev/shm/iqviz so the
    # standalone control_panel.py works too. The embedded panel writes
    # UIState directly; both can run at once (last writer wins, harmless).
    vizctl = VizCtl(args.vizctl, ui)

    # Try the built-in GUI panel. It must own the main thread (Tk rule), so
    # Holoscan runs in a daemon thread. If Tk is unavailable we fall back to
    # the keyboard/console-only path with Holoscan on the main thread.
    panel = None
    if args.panel:
        try:
            import tkinter            # noqa: F401  (probe availability)
            panel = EmbeddedPanel(ui, ctl)
        except Exception as e:
            print(f"[viz] GUI panel unavailable ({e}); using keys/console. "
                  "Install with: sudo apt-get install -y python3-tk")
            panel = None

    if not args.no_repl and sys.stdin.isatty():
        threading.Thread(target=handler.repl, daemon=True).start()

    app = IQMonitorApp(args, ui, handler, vizctl)
    if panel is not None:
        threading.Thread(target=app.run, daemon=True).start()
        panel.run()             # blocks on the main thread until closed
        with ui.lock:           # window closed -> tell the graph to stop
            ui.quit = True
        time.sleep(0.3)         # let RingSource see quit and close the ring
    else:
        app.run()


if __name__ == "__main__":
    main()
