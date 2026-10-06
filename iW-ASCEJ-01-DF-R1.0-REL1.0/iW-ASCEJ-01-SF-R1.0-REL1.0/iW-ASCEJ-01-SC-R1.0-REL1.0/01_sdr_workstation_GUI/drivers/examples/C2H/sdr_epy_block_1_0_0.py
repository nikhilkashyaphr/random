#!/usr/bin/env python3
"""
GNU Radio Embedded Python Block — IQ FIFO Sink  v3  (H2C path)

Accepts complex64 (±1.0) from IQ Source, converts to the format the
card DAC expects, writes interleaved int16 [I Q I Q ...] to the FIFO.

DATA FORMAT — RFSoC / IWFG DAC (14-bit LEFT-SHIFTED into 16-bit word)
----------------------------------------------------------------------
  The card DAC (and ADC) uses the Xilinx/RFSoC standard packing:
    bits[15:2]  = 14-bit signed sample value  (MSB-aligned)
    bits[1:0]   = 00  (always zero, LSB padding)

  This is the same format as the captured .bin files from the RFSoC ADC.
  It is NOT the same as right-aligned 14-bit (bits[13:0]).

  Conversion:
    float  ±1.0  →  int14  ±8191  →  int16  ±32764  (left-shift by 2)
    peak = (2^13 - 1) << 2 = 8191 * 4 = 32764

  The original fifo_sink used PEAK_14BIT=8191 without the left-shift,
  producing values 4× too small in the wrong bit position → near-zero
  signal at the DAC → nothing visible on C2H loopback.

THROTTLE NOTE
-------------
  Always place a Throttle block between IQ Source and this sink.
  IQ Source → Throttle (samp_rate) → FIFO Sink H2C
"""

import os
import stat
import threading
import collections
import time
import numpy as np

try:
    from gnuradio import gr
except ImportError:
    class _F:
        def __init__(self, name, in_sig, out_sig): pass
    gr = type("gr", (), {"sync_block": _F})()

# 14-bit value left-shifted by 2 into a 16-bit word (RFSoC/IWFG DAC format)
# ±1.0 → ±8191 → left-shift 2 → ±32764
PEAK_INT16  = float((1 << 13) - 1) * 4   # 32764.0
IQ_BYTES    = 4

_DEFAULT_FIFO = "/tmp/iwfg_h2c.fifo"


class blk(gr.sync_block):

    def __init__(self, fifo_path=_DEFAULT_FIFO, chunk_size=16*1024,
                 queue_depth=64, verbose=True):

        if chunk_size % IQ_BYTES != 0:
            raise ValueError(f"chunk_size must be multiple of {IQ_BYTES}")

        gr.sync_block.__init__(self, name="FIFO Sink H2C",
                               in_sig=[np.complex64], out_sig=[])

        self._fifo_path   = fifo_path
        self._chunk_size  = chunk_size
        self._queue_depth = queue_depth
        self._verbose     = verbose

        self._accum      = bytearray()
        self._accum_lock = threading.Lock()
        self._queue      = collections.deque()
        self._queue_cv   = threading.Condition(threading.Lock())

        self._stat_lock        = threading.Lock()
        self._drops            = 0
        self._writes           = 0
        self._total_bytes_sent = 0
        self._stop_event       = threading.Event()
        self._thread           = None

        if not os.path.exists(fifo_path):
            os.mkfifo(fifo_path, 0o666)
            if verbose:
                print(f"[fifo_sink] Created FIFO: {fifo_path}")
        elif not stat.S_ISFIFO(os.stat(fifo_path).st_mode):
            raise RuntimeError(f"{fifo_path} is not a FIFO")

        if verbose:
            print(f"\n[fifo_sink H2C v3]")
            print(f"  FIFO       : {fifo_path}")
            print(f"  Format     : 14-bit LEFT-SHIFTED in int16 (RFSoC DAC format)")
            print(f"  ±1.0 maps to ±{int(PEAK_INT16)} (= 8191 << 2)")
            print(f"  Chunk size : {chunk_size} bytes\n")

        self._start_writer()

    def start(self):
        self._stop_event.clear()
        if self._thread is None or not self._thread.is_alive():
            self._start_writer()
        return True

    def stop(self):
        self._stop_event.set()
        with self._queue_cv:
            self._queue_cv.notify_all()
        if self._thread is not None:
            self._thread.join(timeout=5.0)
        if self._verbose:
            with self._stat_lock:
                d, w, b = self._drops, self._writes, self._total_bytes_sent
            print(f"[fifo_sink] sent={w} chunks ({b/1048576:.1f} MB) drops={d}")
        return True

    def work(self, input_items, output_items):
        items = input_items[0]
        raw   = self._to_int16(items)

        with self._accum_lock:
            self._accum.extend(raw)
            chunks = []
            while len(self._accum) >= self._chunk_size:
                chunks.append(bytes(self._accum[:self._chunk_size]))
                del self._accum[:self._chunk_size]

        for chunk in chunks:
            self._enqueue(chunk)

        return len(items)

    @staticmethod
    def _to_int16(samples: np.ndarray) -> bytes:
        """
        Convert complex64 ±1.0  →  int16 left-shifted 14-bit (RFSoC DAC format)

        Step 1: scale  ±1.0  →  ±32764   (= 8191 * 4)
        Step 2: clip to ±32764
        Step 3: cast to int16
                The resulting values fit in int16 (max 32764 < 32767).
                bits[15:2] hold the 14-bit value, bits[1:0] are always 0
                because 32764 = 0x7FFC and any value ≤ 32764 has bits[1:0]=0
                after the left-shift is already baked into the scaling.
        """
        i_f = samples.real.astype(np.float64)
        q_f = samples.imag.astype(np.float64)

        i_int = np.clip(np.round(i_f * PEAK_INT16),
                        -PEAK_INT16, PEAK_INT16).astype(np.int16)
        q_int = np.clip(np.round(q_f * PEAK_INT16),
                        -PEAK_INT16, PEAK_INT16).astype(np.int16)

        out       = np.empty(2 * len(samples), dtype=np.int16)
        out[0::2] = i_int
        out[1::2] = q_int
        return out.tobytes()

    def _enqueue(self, chunk):
        with self._queue_cv:
            if len(self._queue) >= self._queue_depth:
                self._queue.popleft()
                with self._stat_lock:
                    self._drops += 1
                if self._verbose and self._drops % 10 == 0:
                    print(f"[fifo_sink] drops={self._drops} — check Throttle block")
            self._queue.append(chunk)
            self._queue_cv.notify()

    def _start_writer(self):
        self._thread = threading.Thread(target=self._writer_loop,
                                        name="fifo-h2c-writer", daemon=True)
        self._thread.start()

    def _writer_loop(self):
        while not self._stop_event.is_set():
            if self._verbose:
                print("[fifo_sink] Waiting for iwfg_stream to open FIFO...")
            try:
                fd      = os.open(self._fifo_path, os.O_WRONLY | os.O_CLOEXEC)
                fifo_fh = os.fdopen(fd, "wb", buffering=0)
            except OSError as e:
                print(f"[fifo_sink] open error: {e}")
                time.sleep(1.0)
                continue

            if self._verbose:
                print("[fifo_sink] Connected — streaming to card.")

            try:
                self._stream_until_broken(fifo_fh)
            finally:
                try: fifo_fh.close()
                except OSError: pass

            if not self._stop_event.is_set():
                print("[fifo_sink] Disconnected — reconnecting...")
                time.sleep(0.5)

    def _stream_until_broken(self, fifo_fh):
        while not self._stop_event.is_set():
            with self._queue_cv:
                while not self._queue and not self._stop_event.is_set():
                    self._queue_cv.wait(timeout=0.1)
                if not self._queue:
                    continue
                chunk = self._queue.popleft()
            try:
                view, w, total = memoryview(chunk), 0, len(chunk)
                while w < total:
                    n = fifo_fh.write(view[w:])
                    if not n: raise BrokenPipeError("write=0")
                    w += n
                with self._stat_lock:
                    self._writes           += 1
                    self._total_bytes_sent += len(chunk)
            except (BrokenPipeError, OSError):
                raise
