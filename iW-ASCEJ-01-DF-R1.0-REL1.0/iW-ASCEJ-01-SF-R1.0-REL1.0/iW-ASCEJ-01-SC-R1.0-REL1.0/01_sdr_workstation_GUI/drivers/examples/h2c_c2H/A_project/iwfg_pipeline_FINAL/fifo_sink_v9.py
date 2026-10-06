#!/usr/bin/env python3
"""
GNU Radio Embedded Python Block — IQ FIFO Sink  v4  (H2C path)

Accepts complex64 (±1.0) from IQ Source, converts to the format the
card DAC expects, writes interleaved int16 [I Q I Q ...] to the FIFO.

DATA FORMAT — RFSoC / IWFG DAC (14-bit LEFT-SHIFTED into 16-bit word)
----------------------------------------------------------------------
  The card DAC (and ADC) uses the Xilinx/RFSoC standard packing:
    bits[15:2]  = 14-bit signed sample value  (MSB-aligned)
    bits[1:0]   = 00  (always zero, LSB padding)

  v4 FIX (format-invariant violation in v3): v3 scaled floats straight
  to ±32764 and rounded, which produces arbitrary integers — ~75 % of
  samples had NONZERO bits[1:0], violating the packing contract above.
  The correct construction is: quantize to int14 (±8191) FIRST, then
  left-shift by 2. Every emitted word now has bits[1:0] == 00 by
  construction, peak = 8191 << 2 = ±32764.

v4 WRITER REWRITE (inode-swap deadlock + stop responsiveness):
----------------------------------------------------------------------
  v3 used a blocking os.open(O_WRONLY). A blocking FIFO open resolves
  the path to an inode ONCE and then waits on THAT inode; if the C app
  restarted and recreated the FIFO (unlink+mkfifo = new inode), the
  writer thread stayed wedged on the dead inode forever and the sink
  never reconnected — data flow silently stopped (empirically
  reproduced). v3's blocking open was also uninterruptible by stop().

  v4 opens with O_NONBLOCK in a retry loop:
    - ENXIO  (no reader yet)      -> sleep 0.1 s, retry — each retry
                                     RE-RESOLVES the path, so a new
                                     inode is picked up automatically;
    - ENOENT (path vanished)      -> recreate the FIFO, retry;
    - not a FIFO (path squatted)  -> warn, retry;
  and keeps the fd non-blocking, pacing writes with select() on a
  0.1 s timeout. Every wait point (open retry, queue wait, select) is
  bounded, so stop() is honored within ~0.25 s in ANY state — including
  a frozen reader that stops draining a full pipe (v3 blocked in
  write() forever in that case).

  On a broken/failed connection the partial chunk is DROPPED, never
  resumed on the next connection: a new reader starts exactly on a
  chunk boundary, preserving I/Q int16-pair framing.

THROTTLE NOTE
-------------
  Always place a Throttle block between IQ Source and this sink.
  IQ Source → Throttle (samp_rate) → FIFO Sink H2C
"""

import os
import stat
import errno
import select
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

# Correct DAC quantization: int14 first, THEN << 2 (see header).
PEAK_14BIT  = float((1 << 13) - 1)       # 8191.0
PEAK_INT16  = int(PEAK_14BIT) << 2       # 32764  (banner/reference only)
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
        self._reconnects       = 0
        self._stop_event       = threading.Event()
        self._thread           = None

        self._ensure_fifo()

        if verbose:
            print(f"\n[fifo_sink H2C v4]")
            print(f"  FIFO       : {fifo_path}")
            print(f"  Format     : int14 quantize -> <<2 (RFSoC DAC, bits[1:0]=00)")
            print(f"  ±1.0 maps to ±{PEAK_INT16} (= 8191 << 2)")
            print(f"  Chunk size : {chunk_size} bytes\n")

        self._start_writer()

    # ------------------------------------------------------------------ #
    # GNU Radio lifecycle                                                #
    # ------------------------------------------------------------------ #

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
            if self._thread.is_alive() and self._verbose:
                print("[fifo_sink] WARNING: writer thread did not exit in 5 s")
        if self._verbose:
            with self._stat_lock:
                d, w, b, r = (self._drops, self._writes,
                              self._total_bytes_sent, self._reconnects)
            print(f"[fifo_sink] sent={w} chunks ({b/1048576:.1f} MB) "
                  f"drops={d} reconnects={r}")
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

    # ------------------------------------------------------------------ #
    # Sample conversion (v4 fix: format-correct quantization)             #
    # ------------------------------------------------------------------ #

    @staticmethod
    def _to_int16(samples: np.ndarray) -> bytes:
        """
        complex64 ±1.0  →  int16 in RFSoC DAC packing (bits[1:0] == 00).

        Step 1: quantize  ±1.0 → ±8191  (true 14-bit signed value)
        Step 2: left-shift by 2         (MSB-align into the 16-bit word)

        Because the shift happens AFTER integer quantization, bits[1:0]
        are structurally zero for every sample (v3 rounded a ×32764
        scaling instead, leaving ~75 % of samples with nonzero LSBs).
        Peak: 8191 << 2 = 32764 < 32767, so no int16 overflow.
        """
        i_f = samples.real.astype(np.float64)
        q_f = samples.imag.astype(np.float64)

        i14 = np.clip(np.round(i_f * PEAK_14BIT),
                      -PEAK_14BIT, PEAK_14BIT).astype(np.int16)
        q14 = np.clip(np.round(q_f * PEAK_14BIT),
                      -PEAK_14BIT, PEAK_14BIT).astype(np.int16)

        out       = np.empty(2 * len(samples), dtype=np.int16)
        out[0::2] = i14
        out[1::2] = q14
        np.left_shift(out, 2, out=out)
        return out.tobytes()

    # ------------------------------------------------------------------ #
    # Chunk queue (drop-oldest, bounded)                                  #
    # ------------------------------------------------------------------ #

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

    # ------------------------------------------------------------------ #
    # Writer thread (v4: non-blocking, inode-swap safe, stop-responsive) #
    # ------------------------------------------------------------------ #

    def _ensure_fifo(self):
        """Create the FIFO if absent; error out if the path is squatted."""
        try:
            st = os.stat(self._fifo_path)
        except FileNotFoundError:
            os.mkfifo(self._fifo_path, 0o666)
            if self._verbose:
                print(f"[fifo_sink] Created FIFO: {self._fifo_path}")
            return
        if not stat.S_ISFIFO(st.st_mode):
            raise RuntimeError(f"{self._fifo_path} exists and is not a FIFO")

    def _start_writer(self):
        self._thread = threading.Thread(target=self._writer_loop,
                                        name="fifo-h2c-writer", daemon=True)
        self._thread.start()

    def _open_fifo_wr(self):
        """
        Non-blocking, retrying FIFO open for writing. Returns an fd (left
        in non-blocking mode) or -1 if stop was requested. Each retry
        re-resolves the path, so a replaced inode (C-app restart doing
        unlink+mkfifo) is picked up automatically — the v3 deadlock.
        """
        warned = False
        while not self._stop_event.is_set():
            try:
                fd = os.open(self._fifo_path,
                             os.O_WRONLY | os.O_NONBLOCK | os.O_CLOEXEC)
            except OSError as e:
                if e.errno == errno.ENXIO:          # FIFO exists, no reader yet
                    self._stop_event.wait(0.1)
                    continue
                if e.errno == errno.ENOENT:         # path vanished: self-heal
                    try:
                        os.mkfifo(self._fifo_path, 0o666)
                        if self._verbose:
                            print(f"[fifo_sink] FIFO vanished — recreated "
                                  f"{self._fifo_path}")
                    except FileExistsError:
                        pass
                    except OSError as e2:
                        if not warned:
                            print(f"[fifo_sink] cannot recreate FIFO: {e2}")
                            warned = True
                        self._stop_event.wait(0.5)
                    continue
                if not warned:
                    print(f"[fifo_sink] open error: {e} — retrying")
                    warned = True
                self._stop_event.wait(0.5)
                continue

            try:
                if not stat.S_ISFIFO(os.fstat(fd).st_mode):
                    os.close(fd)
                    if not warned:
                        print(f"[fifo_sink] {self._fifo_path} is not a FIFO "
                              f"— retrying")
                        warned = True
                    self._stop_event.wait(0.5)
                    continue
            except OSError:
                os.close(fd)
                continue

            return fd
        return -1

    def _writer_loop(self):
        while not self._stop_event.is_set():
            fd = self._open_fifo_wr()
            if fd < 0:
                break                                # stop requested
            if self._verbose:
                print("[fifo_sink] Connected — streaming to card.")
            try:
                self._stream_until_broken(fd)
            except (BrokenPipeError, ConnectionError):
                pass                                 # reader went away
            except OSError as e:
                print(f"[fifo_sink] write error: {e}")
            except Exception as e:                   # never die silently
                print(f"[fifo_sink] unexpected writer error: {e!r}")
            finally:
                try:
                    os.close(fd)
                except OSError:
                    pass

            if not self._stop_event.is_set():
                with self._stat_lock:
                    self._reconnects += 1
                if self._verbose:
                    print("[fifo_sink] Disconnected — reconnecting...")
                self._stop_event.wait(0.25)

    def _stream_until_broken(self, fd):
        """
        Pop chunks and write them through the non-blocking fd, pacing
        with select(0.1 s) so a frozen reader can never wedge the thread
        (v3's blocking write() hung forever once the pipe filled). A
        chunk interrupted by a connection error is dropped whole; the
        next connection starts on a chunk boundary (framing preserved).
        """
        while not self._stop_event.is_set():
            with self._queue_cv:
                while not self._queue and not self._stop_event.is_set():
                    self._queue_cv.wait(timeout=0.1)
                if not self._queue:
                    continue
                chunk = self._queue.popleft()

            view, off, total = memoryview(chunk), 0, len(chunk)
            while off < total:
                if self._stop_event.is_set():
                    return
                _, wr, _ = select.select([], [fd], [], 0.1)
                if not wr:
                    continue                         # pipe full: reader slow
                try:
                    n = os.write(fd, view[off:])
                except BlockingIOError:
                    continue
                except OSError as e:
                    if e.errno == errno.EPIPE:
                        raise BrokenPipeError from e
                    raise
                if n == 0:
                    raise BrokenPipeError("write returned 0")
                off += n

            with self._stat_lock:
                self._writes           += 1
                self._total_bytes_sent += total
