import numpy as np
import os
import select
from gnuradio import gr

class blk(gr.sync_block):
    def __init__(self, samp_rate=200e6, fifo_path="/tmp/iwfg_fifo"):
        gr.sync_block.__init__(
            self,
            name="IWave DMA FIFO",
            in_sig=None,
            out_sig=[np.complex64]  # Complex samples as complex64
        )
        self.samp_rate = samp_rate
        self.fifo_path = fifo_path
        self.fd = None
        self.buffer = b''  # Internal buffer for partial reads

    def work(self, input_items, output_items):
        out = output_items[0]
        n_output = out.shape[0]
        if self.fd is None:
            return 0

        # First, use any leftover bytes from previous read
        if len(self.buffer) > 0:
            raw = np.frombuffer(self.buffer, dtype=np.int16)
            # Pair up I/Q samples and convert to complex64
            n_pairs = min(len(raw) // 2, n_output)
            if n_pairs > 0:
                iq = raw[:n_pairs * 2].astype(np.float32)
                out[:n_pairs] = (iq[0::2] + 1j * iq[1::2]).astype(np.complex64)
            consumed_bytes = n_pairs * 4  # 2 int16s (4 bytes) per complex sample
            self.buffer = self.buffer[consumed_bytes:]
            if n_pairs == n_output:
                return n_pairs

        # Need more data - check if FIFO has data
        readable, _, _ = select.select([self.fd], [], [], 0.010)  # 10ms timeout
        if not readable:
            return n_pairs if 'n_pairs' in dir() else 0

        try:
            # Each complex sample = 2 x int16 = 4 bytes
            to_read = (n_output * 4) - len(self.buffer)  # bytes needed
            to_read = max(to_read, 4096)          # at least 4KB for efficiency
            to_read = (to_read // 4) * 4          # align to complex sample boundary

            chunk = os.read(self.fd, to_read)
            if len(chunk) == 0:
                # FIFO closed?
                print("FIFO closed by writer")
                return n_pairs if 'n_pairs' in dir() else 0

            combined = self.buffer + chunk
            raw = np.frombuffer(combined, dtype=np.int16)

            # Convert interleaved int16 I/Q → complex64
            n_pairs = min(len(raw) // 2, n_output)
            iq = raw[:n_pairs * 2].astype(np.float32)
            out[:n_pairs] = (iq[0::2] + 1j * iq[1::2]).astype(np.complex64)

            # Save remainder (keep only full int16 bytes)
            consumed = n_pairs * 4
            self.buffer = combined[consumed:]
            return n_pairs

        except (BlockingIOError, OSError):
            return n_pairs if 'n_pairs' in dir() else 0

    def start(self):
        print(f"Opening FIFO: {self.fifo_path}")
        print("Make sure C app is running: sudo ./iwfg_live_stream /dev/iwfg0 &")
        # Open non-blocking initially to avoid deadlock if C app not running
        try:
            self.fd = os.open(self.fifo_path, os.O_RDONLY | os.O_NONBLOCK)
            # Switch to blocking for normal operation
            # (fcntl would be needed to change flags, or just reopen)
            print("✅ FIFO opened (non-blocking mode)")
            return True
        except FileNotFoundError:
            print(f"❌ FIFO not found: {self.fifo_path}")
            print("Start the C app first!")
            return False
        except Exception as e:
            print(f"❌ Error opening FIFO: {e}")
            return False

    def stop(self):
        if self.fd:
            os.close(self.fd)
            self.fd = None
        return True
