import numpy as np
import os
import errno
import select
from gnuradio import gr


class blk(gr.sync_block):
    """
    IWave DMA FIFO: interleaved int16 I/Q (SC16) -> complex64.

    Live      : reads ONLY what it emits, so surplus stays in the kernel pipe
                and the C writer backpressures -> latency bounded to pipe depth.
    No glitch : single-phase, frame-aligned, contiguous emit; output written
                exactly once; the stream is never chopped mid-flight.
    Safety    : if surplus ever piles up in RAM anyway (writer not blocking),
                ONE rare frame-aligned flush trims latency -> at most a single
                FFT window disturbed, not every one.
    """
    IQ = 4  # bytes per complex sample (int16 I + int16 Q)

    def __init__(self, samp_rate=150.25e6, fifo_path="/tmp/iwfg_fifo",
                 normalize=True, frame=8192, max_latency_ms=8.0):
        gr.sync_block.__init__(self, name="IWave DMA FIFO",
                               in_sig=None, out_sig=[np.complex64])
        self.samp_rate      = float(samp_rate)
        self.fifo_path      = fifo_path
        self.normalize      = bool(normalize)
        self.frame          = int(frame)          # emit/drop granularity (>= sink fftsize)
        self.max_latency_ms = float(max_latency_ms)
        self.fd  = None
        self.buf = bytearray()
        self.set_output_multiple(self.frame)

    def _high_water(self):
        hw  = int(self.samp_rate * self.max_latency_ms * 1e-3)
        hw -= hw % self.frame
        return max(hw, self.frame)

    def _reopen(self):
        try:
            if self.fd is not None:
                os.close(self.fd)
        except OSError:
            pass
        try:
            self.fd = os.open(self.fifo_path, os.O_RDONLY | os.O_NONBLOCK)
        except OSError:
            self.fd = None

    def work(self, input_items, output_items):
        out   = output_items[0]
        n_out = len(out)
        if self.fd is None:
            return 0

        need = n_out * self.IQ
        have = len(self.buf)

        # --- read ONLY the shortfall; never drain the pipe -----------------
        # Leaving surplus in the kernel pipe keeps the writer backpressured,
        # which is exactly what bounds latency AND keeps the stream continuous.
        if have < need:
            r, _, _ = select.select([self.fd], [], [], 0.050)
            if r:
                try:
                    chunk = os.read(self.fd, need - have)
                    if chunk:
                        self.buf += chunk
                    elif have < self.IQ:
                        self._reopen()             # writer closed -> reattach
                        return 0
                except OSError as e:
                    if getattr(e, "errno", None) not in (errno.EAGAIN,
                                                          errno.EWOULDBLOCK):
                        print(f"[IWave] read error: {e}")

        # --- safety valve: rare, single, frame-aligned latency trim --------
        hw = self._high_water()
        if len(self.buf) // self.IQ > hw:
            keep  = self.frame * 2
            drop  = (len(self.buf) // self.IQ) - keep
            drop -= drop % self.frame
            if drop > 0:
                del self.buf[:drop * self.IQ]

        # --- emit ONE contiguous, frame-aligned block (written once) -------
        n_avail = len(self.buf) // self.IQ
        n_emit  = min(n_avail, n_out)
        n_emit -= n_emit % self.frame
        if n_emit == 0:
            return 0

        nb  = n_emit * self.IQ
        raw = np.frombuffer(bytes(self.buf[:nb]), dtype=np.int16)
        i = raw[0::2].astype(np.float32)
        q = raw[1::2].astype(np.float32)
        if self.normalize:
            i *= 1.0 / 32768.0
            q *= 1.0 / 32768.0
        out[:n_emit] = i + 1j * q

        del self.buf[:nb]
        return n_emit

    def start(self):
        print(f"[IWave] Opening FIFO: {self.fifo_path}")
        try:
            self.fd = os.open(self.fifo_path, os.O_RDONLY | os.O_NONBLOCK)
        except FileNotFoundError:
            print(f"[IWave] FIFO not found: {self.fifo_path} (start C app first)")
            return False
        except Exception as e:
            print(f"[IWave] open error: {e}")
            return False
        self.buf = bytearray()
        print("[IWave] FIFO opened (non-blocking)")
        return True

    def stop(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None
        return True
