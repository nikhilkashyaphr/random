# iwfg driver: the C2H-overflow recovery also breaks H2C (1.2.5 findings)

This note is for whoever maintains the `iwfg` kernel driver
(`SC/01_sdr_workstation_GUI/drivers`). The GUI in 1.2.5 works around the problem. It was
**not** fixed in the driver, because driver changes need a rebuilt and
reloaded module and were outside the scope of the GUI work.

## What the bench showed

Setup: 4-channel capture at 200 MSPS, with H2C transmit from the GUI's
generator. The failures were:

- the transmitted tone carried broadband disturbances;
- later, no tone at all;
- the C2H capture died (*"exit 0"*) or stalled at 0.0 MB/s;
- the transmit helper reported stalls and cycled the device.

The capture needs 4 × 200 MSPS × 4 B = **3052 MiB/s**. The host drained
about **2400 MiB/s**. So the FPGA's C2H FIFO overflows continuously.

## What the driver does on every overflow

`iwfg_dma.c`, `iwfg_rx_poll()`, the `if (is_usr_ovf)` branch at about line
876:

1. **Tries to take H2C exclusively** by moving `priv->h2c_busy` 0 → 2. It
   busy-waits 1 ms, then retries up to 100 × ~100 µs.
2. **Forces the lock if that fails.** At about line 918 it logs *"Failed to
   acquire reset lock … forcing reset"* and sets `h2c_busy = 2` **while an
   H2C transfer is still in `iwfg_tx_roll()` / `iwfg_wait_for_tx()`**. That
   happens whenever a transfer is longer than about 11 ms, for example a
   large buffer, or any transfer to a card that is not consuming (the wait
   runs to its 2 s timeout).
3. **Frees every queue**, the TX queue included (`iwfg_clear_tx_queue(priv, i)`,
   about line 928). That function frees the ring and `kfree(q)`. The waiting
   H2C ioctl still holds `q`:
   - `iwfg_wait_for_tx()` calls `iwfg_tx_reclaim(q)` every 100 ms slice
     (line 298) and reads `ring->wb` for its timeout message;
   - `iwfg_tx_roll()` goes on writing descriptors into the freed ring and
     ringing the doorbell of the **new** queue with a stale pidx.

   This is a **use-after-free**, and a plausible cause of a device that
   stops delivering C2H data until the module is reloaded.
4. **Soft-resets the QDMA** (`iwfg_reset_hardware()` → register 0xA0, then
   `udelay(17000)` inside a work item). It re-initialises all queues, then
   rewrites `0x08` and `C2H_CTRL = 0x3`.
   - It does **not** re-apply `H2C_CTRL.VID_OUT_SEL` (offset 0x04), which
     `iwfg_qdma_h2c_start()` sets at H2C open.
   - If the soft reset clears it, the card stops consuming H2C data after
     the first overflow, until `/dev/iwfg1` is reopened. That matches the
     *"card not consuming"* stalls.
5. **The DAC feed stops** for at least 17 ms per overflow. The DAC input has
   no TREADY, so it plays stale data. At a continuous overflow rate this is
   the broadband junk seen in 1.2.1.

## A second source of wedged state

`iwfg_release()` soft-resets the QDMA only when the node being closed is
C2H **and** no other stream is active. When capture and transmit were
stopped at the same time (the GUI before 1.2.5), the capture could close
first while H2C was still open. Then no reset happened at all, and the next
session started on whatever state the previous one left.

## What the GUI does about it (1.2.5)

- **Transmit guard.** When the generator starts, a capture above 1600 MiB/s
  is cut to 2 channels (or 1) via PL GPIO mode. The measured rate is
  checked afterwards, and the setting is restored when the generator stops.
  The goal is that the FIFO does not overflow in the first place.
- **Ordered stop.** Transmit closes first, then capture, so the capture's
  close is the last one and the driver's reset runs.
- **Device reset.** If capture delivers nothing for about 5 s, the GUI stops
  transmit, then capture (the driver resets), then starts both again. This
  happens at most twice in 3 minutes, and after that the GUI tells you to
  reload the driver.
- **Same-size reopen.** A transmit helper whose transfers stop completing is
  reopened at the same buffer size before any step-down. The reopen calls
  `iwfg_qdma_h2c_start()` again.
- **Larger capture FIFO pipe** (`F_SETPIPE_SZ` to `fs.pipe-max-size`). The
  host drains faster, so there are fewer overflows at a given rate.

## Suggested driver fixes

In order of value:

1. **Do not tear down the TX queue in the C2H overflow recovery.** An
   overflow is a C2H-side event. Recover the RX queue only. If the QDMA
   soft reset really does require all queues, abort the in-flight H2C
   transfer first: complete its wait with an error and wait until
   `iwfg_xmit_data()` has returned, and only then free anything.
2. **Never free a queue that an ioctl may still reference.** Take a reference
   in `iwfg_xmit_data()`, or hold `q` under a lock that `iwfg_clear_tx_queue()`
   also takes. Re-read `priv->queue[qid]` after every wait instead of
   caching `q`.
3. **Re-apply `H2C_CTRL.VID_OUT_SEL`** after `iwfg_reset_hardware()` for every
   open H2C node, just as `C2H_CTRL` is re-applied.
4. **Replace `udelay(17000)` in the work item with `msleep()`.** It spins a
   CPU for 17 ms per overflow, and those CPU cycles are then missing from
   the capture drain.
5. **Surface the overflow to userspace**, for example as a counter readable
   through an ioctl or sysfs. Today `c2h_stream` sees only a silent `EAGAIN`,
   and the user sees nothing.
