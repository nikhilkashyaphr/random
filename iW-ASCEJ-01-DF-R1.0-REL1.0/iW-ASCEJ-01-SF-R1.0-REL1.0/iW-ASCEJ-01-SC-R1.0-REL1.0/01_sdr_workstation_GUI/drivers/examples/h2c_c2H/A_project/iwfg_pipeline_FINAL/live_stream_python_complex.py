import fcntl
import numpy as np
import os
import select
import time
from gnuradio import gr

_DEFAULT_FIFO   = "/tmp/iwfg_c2h.fifo"
_OPEN_TIMEOUT   = 30.0
_SELECT_TIMEOUT = 0.05

# Confirmed from hardware measurement:
# ADC returns values with effective peak ~4096
# Dividing by 4096 gives ±1.0 matching H2C amplitude
PEAK_INT16 = 4096.0


class blk(gr.sync_block):
    """
    C2H source: reads int16 IQ pairs from FIFO, outputs complex64 ±1.0.
    PEAK_INT16 = 4096.0 confirmed from hardware loopback measurement.
    """

    def __init__(self, samp_rate=184.32e6, fifo_path=_DEFAULT_FIFO):
        gr.sync_block.__init__(self, name="IWave DMA FIFO",
                               in_sig=None, out_sig=[np.complex64])
        self.samp_rate = samp_rate
        self.fifo_path = fifo_path
        self.fd   = None
        self._buf = b""

    def start(self):
        print(f"[C2H] Opening FIFO: {self.fifo_path}")
        deadline = time.monotonic() + _OPEN_TIMEOUT
        while True:
            try:
                fd = os.open(self.fifo_path, os.O_RDONLY | os.O_NONBLOCK)
                flags = fcntl.fcntl(fd, fcntl.F_GETFL)
                fcntl.fcntl(fd, fcntl.F_SETFL, flags & ~os.O_NONBLOCK)
                # Increase read-side pipe buffer to 1MB
                try:
                    fcntl.fcntl(fd, fcntl.F_SETPIPE_SZ, 1024 * 1024)
                except Exception:
                    pass
                self.fd   = fd
                self._buf = b""
                print(f"[C2H] FIFO opened. PEAK_INT16={PEAK_INT16}")
                return True
            except FileNotFoundError:
                if time.monotonic() > deadline:
                    print(f"[C2H] FIFO not found: {self.fifo_path}")
                    return False
                time.sleep(0.25)
            except OSError as e:
                print(f"[C2H] Error: {e}"); return False

    def stop(self):
        if self.fd is not None:
            try: os.close(self.fd)
            except OSError: pass
            self.fd = None
        return True

    def work(self, input_items, output_items):
        out      = output_items[0]
        n_output = out.shape[0]
        produced = 0

        if self.fd is None:
            return 0

        if self._buf:
            produced, self._buf = self._consume(self._buf, out, 0, n_output)
            if produced == n_output:
                return produced

        try:
            readable, _, _ = select.select([self.fd], [], [], _SELECT_TIMEOUT)
        except (ValueError, OSError):
            return produced

        if not readable:
            return produced

        to_read = max((n_output - produced) * 4, 4096)
        to_read = (to_read // 4) * 4

        try:
            chunk = os.read(self.fd, to_read)
        except OSError:
            return produced

        if not chunk:
            self.stop(); self.start()
            return produced

        combined = self._buf + chunk
        produced, self._buf = self._consume(combined, out, produced, n_output)
        return produced

    @staticmethod
    def _consume(data, out, already, n_output):
        raw     = np.frombuffer(data, dtype=np.int16)
        n_pairs = min(len(raw) // 2, n_output - already)
        if n_pairs > 0:
            iq = raw[:n_pairs * 2].astype(np.float32) / PEAK_INT16
            out[already:already + n_pairs] = (
                iq[0::2] + 1j * iq[1::2]).astype(np.complex64)
        return already + n_pairs, data[n_pairs * 4:]
