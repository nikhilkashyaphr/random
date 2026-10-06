# iwfg pipeline v23 — migration to the current driver

Source of truth for every change: the driver source (`iwfg_main.c`,
`iwfg_dma.c`, `iwfg.h`) and its reference userspace app (`fifo_pipe.c`).

## The new driver contract in one paragraph

The driver creates `/dev/iwfg0` (C2H, qid 0) and `/dev/iwfg1` (H2C, qid 1);
`/dev/iwfg` no longer exists and transfer direction is derived in-kernel
from which node was opened. `open()` initializes the queue (and starts the
C2H hardware stream); `close()` tears everything down and unpins all
buffers — the old `STREAM_START`/`STREAM_STOP` ioctls were removed and now
return `-ENOTTY`. `ADD_BUF` requires `buf_type = IWFG_BUF_TYPE_USERPTR` and
`import_fd = -1`, pins pages, prebuilds the direction's descriptor list,
and returns `buf_index`. `DMA_DATA` is fully **blocking**: H2C returns when
the card has consumed the whole buffer (never `EAGAIN`; errors are `EINTR`
/ `EBUSY` / `ETIMEDOUT`), C2H returns when the buffer is completely filled
(`EAGAIN` = in-kernel FPGA fifo-overflow recovery ran, just retry) and
accepts an optional `struct iwfg_c2h_prearm` appended after the request in
the same ioctl argument so the next buffer's descriptors are posted before
the current one drains. Because the driver holds page pins until the
device fd is closed, userspace teardown order is: close FIFOs → close
device fd → `free()` buffers.

## File-by-file changes

### iwfg_user.h — REPLACED
Old ABI header. Now mirrors the kernel `iwfg.h` bit-for-bit and adds:
`IWFG_IOCTL_QUERY_STREAM_PARAMS` + `struct iwfg_stream_params`, and
`struct iwfg_c2h_prearm`. `IWFG_IOCTL_STREAM_START/STOP` are deliberately
**not defined** so any stale call site fails at compile time. Full
contract documented in the header comment.

### iwfg_h2c.c — REWRITTEN (v7)
Zero-gap looping architecture preserved (reader thread + replay loop).
- `/dev/iwfg1` default; STREAM_START/STOP removed.
- `ADD_BUF` with `buf_type`/`import_fd`; DMA by `buf_index`.
- Blocking-DMA model: dead `EAGAIN` loop removed; `EINTR` = shutdown,
  `EBUSY` = overflow-recovery reset (bounded backoff), `ETIMEDOUT`
  bounded retry.
- The two ping-pong buffers are registered once and DMA'd **directly**
  (driver dedupes registrations and re-posts descriptors per call); the
  old per-iteration ~700 MB/s memcpy is gone — one 16 KB copy per GNU
  Radio update (~30 MB/s) remains.
- `sigaction` without `SA_RESTART` (Ctrl+C actually interrupts the
  in-kernel wait), SIGPIPE ignored, reader unblocked via
  `pthread_kill` + `pthread_tryjoin_np` loop.
- Teardown use-after-free fixed: join reader → unlink FIFO →
  `close(device)` (unpins) → `free()`.
- New: reader reconnects on FIFO EOF while the DMA loop replays the last
  chunk — zero-gap holds across a GNU Radio restart (matches
  `fifo_sink_v2.py`'s own reconnect loop).
- Chunk size is argv[3] (default 16384); bump it if per-ioctl overhead
  caps throughput (TX ring holds 16384 descriptors, one per 16 KB).

### iwfg_c2h.c — REWRITTEN (v2)
Two-thread pipeline (DMA thread + FIFO writer, lock-protected ring)
preserved.
- `/dev/iwfg0` default; STREAM_START/STOP removed (C2H hardware stream
  starts on `open()`).
- `ADD_BUF` with `buf_type`/`import_fd`; DMA by `buf_index`.
- **Prearm added**: each `DMA_DATA` carries `{req, prearm}` naming the
  next ring slot, so the QDMA descriptor ring never starves between
  ioctls (the mechanism `fifo_pipe.c` relies on for gap-free capture).
- Buffer sizing adapted to the one-ioctl-per-full-buffer model: ring is
  now 4 × 4 MB (argv[3] tunes MB per buffer, 1–64) instead of 16 × 16 KB,
  which would have meant an ioctl round-trip every ~22 µs at 737 MB/s.
- `EAGAIN` = overflow recovery → brief backoff, retry same slot;
  `EINTR` = clean shutdown.
- `sigaction` without `SA_RESTART`, SIGPIPE ignored, robust
  re-signal-until-join shutdown for both threads.
- Teardown use-after-free fixed (same ordering as H2C).
- `F_SETPIPE_SZ` 1 MB retained.

### run.sh — UPDATED
Launches H2C on `/dev/iwfg1` and C2H on `/dev/iwfg0` (was: one
non-existent `/dev/iwfg` for both). Args: `run.sh [c2h_dev] [h2c_dev]`.

### Makefile — UNCHANGED
Targets and flags already correct; both apps build clean with
`-Wall -Wextra -O3 -pthread`.

### fifo_sink_v2.py, iq_source_v9.py, live_stream_python_complex.py,
### GNU_RADIO_SETUP.txt — UNCHANGED (verified compatible)
The GNU Radio blocks talk only to the named FIFOs (raw int16-IQ byte
streams) and have zero driver coupling. The DAC packing (14-bit
left-shifted) and the ADC scaling (PEAK_INT16 = 4096, hardware-
calibrated) are properties of the FPGA data path, not the driver.
`fifo_sink_v2.py`'s reconnect loop now pairs with the H2C app's new
reader-reconnect for seamless GR restarts.

## Behaviors that could not be mapped 1:1

- **Explicit stream start/stop**: no equivalent exists; the driver ties
  stream lifecycle to `open()`/`close()` of the device node. Both apps
  therefore hold the fd for the whole session and rely on `close()` for
  teardown — this is the intended model (see `iwfg_release()`).
- **H2C `offset`/`flags` chunked semantics**: the current driver ignores
  both on the H2C path (`tx_desc_served` is never advanced; every
  `DMA_DATA` transmits the buffer's entire descriptor list). The apps
  pass `offset = 0`, `flags = IWFG_FLAG_NORM_PACKET` for forward
  compatibility only.
- **Output mux caveat**: `IWFG_H2C_CTRL_VID_OUT_SEL` is set only by the
  C2H open path (`iwfg_qdma_c2h_start`). H2C-only playback with no C2H
  node open may leave the output mux unselected — if the DAC is silent
  in that configuration, that register bit is the first thing to check
  (a driver-side decision, not an app fix). `run.sh` always opens both,
  so the combined pipeline is unaffected.

## Verification performed

- Both apps compile clean: `gcc -Wall -Wextra -O3 -pthread` (no warnings).
- Full logic exercised with an `LD_PRELOAD` ioctl shim:
  - H2C: first-chunk prime, zero-gap replay during GR disconnect,
    reader reconnect, update accounting, SIGINT teardown, FIFO unlinked.
  - C2H: ring flow under a fast fake DMA, prearm size validation on
    every request, reader-disconnect (EPIPE) shutdown, SIGINT shutdown,
    data pattern integrity through ring → FIFO → file.
