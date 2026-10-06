# Sampling rate "reflecting the wrong way" — root cause and fix

## The cause

The RFDC PLL cannot synthesise an arbitrary frequency. It produces

```
Fs = RefClk × FBDIV / OutDiv
```

with `RefClk = 300 MHz` on this design (BSP: ADC/DAC master tiles,
`Refclk_Freq 300.000`), integer `FBDIV` in 13..160 and `OutDiv` in {1,2,4,8,16}.
So most requests are **quantised to a nearby achievable value**:

| Requested | Achieved | Error | FBDIV/OutDiv |
|---|---|---|---|
| **2000 MHz** | **2006.25** | **+6.25** | 107/16 |
| 2400 MHz | 2400.00 | 0 | 16/2 |
| 3000 MHz | 3000.00 | 0 | 20/2 |
| 4000 MHz | 4012.50 | +12.5 | 107/8 |
| 4800 MHz | 4800.00 | 0 | 16/1 |
| 5000 MHz | 4987.50 | −12.5 | 133/8 |
| 9600 MHz | 9600.00 | 0 | 32/1 |

The GUI's own default of **2000 MHz is not achievable** and lands on 2006.25.

That quantisation alone is normal silicon behaviour. **The bug was what the
firmware did next:**

```c
if (is_adc) global_adc_gsps = new_sampling_rate_mhz / 1000.0;   // the REQUEST
```

It stored the *requested* rate, not the achieved one. Everything downstream is
derived from that number:

* stream-clock target `Fs / factor`
* the Clocking Wizard output divider programmed from it
* the DDS phase increment, `f × 2³² / stream_clk`
* the automatic Nyquist zone, `NCO / (Fs/2)`

So after a rate change the PL clock, the DDS output frequency and the Nyquist
zone were all computed against a rate the silicon was not running at — small,
consistent, and confusing, because the console cheerfully echoed the number you
typed.

## The fix

`Change_Sampling_Rate_All_Tiles()` now reads the achieved rate back from the
PLL and uses that for all downstream maths:

```c
XRFdc_PLL_Settings pll;
if (XRFdc_GetPLLConfig(RFdcInstPtr, Type, master_tile, &pll) == XST_SUCCESS
    && pll.OutputDivider != 0U) {
    achieved_mhz = (pll.RefClkFreq * pll.FeedbackDivider) / pll.OutputDivider;
}
...
if (is_adc) global_adc_gsps = achieved_mhz / 1000.0;
new_sampling_rate_mhz = achieved_mhz;      /* Nyquist sees it too */
```

When the request is quantised it now says so:

```
NOTE: PLL quantised the request. asked 2000.000 MHz, achieved 2006.250 MHz.
      Downstream math uses the achieved value.
```

The application already had `Verify_Actual_Sampling_Rate()`, which computed
exactly this number — it printed it and then threw it away.

**Practical advice:** prefer rates that are exact multiples of 300/OutDiv —
2400, 3000, 3600, 4800, 6000, 9600 MHz — and the quantisation disappears.

## Second bug: the GUI's "defaults" were not the firmware's defaults

`onRfdcResetDefaults()` restored widget values that did not match the boot
configuration at all:

| Control | GUI "default" | Firmware boot | |
|---|---|---|---|
| Decimation | ×1 | ×24 | ✗ |
| Interpolation | ×1 | ×24 | ✗ |
| DAC VOP | 0 mA | 32 mA | ✗ |
| AXIS routing | 0,1,2,3 | 7,3,2,1 | ✗ |
| PL GPIO | Mode 1 | Mode 3 | ✗ |
| Sampling rate | 2000 MHz | 4800 / 9600 | ✗ (and unachievable) |

So "reset to defaults" produced a state a power cycle would never produce.

Both sides now take their values from `PCIE_DEF_*` in the shared
`pcie_regs.h`, and the button issues **one** firmware command
(`PCIE_EVT_DEFAULTS`) that restores the whole boot configuration. The device is
the authority on what "default" means, rather than the GUI replaying a dozen
writes and hoping they add up to the same thing.

## Third gap: the GUI could not see hardware state

The register map has no read path, so the panel could only ever display what
had last been *typed* — misleading exactly when the PLL quantises.

Added `PCIE_EVT_READBACK` (0x0F1). The host writes a tagged request into
`RFDC_configuration`; the firmware **overwrites that register with the answer**
before acknowledging. No new register address is invented.

It is self-detecting: the request carries the tag `0x5242` in `[31:16]`. If the
host reads its own tag back unchanged, the PL cannot let the PS write that
register, and the GUI says readback is unsupported instead of believing a stale
value.

Readable: sampling rate, decimation, interpolation, NCO (signed kHz), DDS, DSA,
VOP, QMC gain, Nyquist zone, DAC source, PL GPIO mode, AXIS routing, and the
derived ADC/DAC stream clocks.

New **"Read from hardware"** button populates every field from the device.

## How to confirm the fix

1. **Read from hardware** → note the sampling rate.
2. Set 2000 MHz, **Apply rate**. Console prints the quantisation note.
3. **Read from hardware** again → the field now shows **2006**, not 2000.
4. Check the DDS output on a scope: it should sit at the requested frequency,
   because the phase increment is now derived from the true stream clock.
5. **Reset all to defaults** → 4800/9600, ×24, 32 mA, routing 7/3/2/1, and the
   values agree with a power cycle.

## Verification

| Check | Result |
|---|---|
| Firmware, 17 modules × 2 flows, `-Wall -Wextra` | 0 errors, 0 warnings |
| GUI, `moc` + `-Wall -Wextra -Wshadow`, Qt 5.15 | 0 errors, 0 warnings |
| PLL quantisation table computed from the Gen 3 divider ranges | matches the BSP's own 4800/9600 exact cases |

---

# Update 2 — GUI freeze, and why "reset to defaults" was refused

## The board is running old firmware

The status line in the screenshot decodes precisely:

```
ring = 0x2A400023
  [31]    NEW_CMD = 0
  [30:24] marker  = 0x2A   -> a valid ACK status word
  [23:20] error   = 0x4    -> UNKNOWN EVENT CODE
  [19:0]  event   = 0x023  -> PCIE_EVT_DEFAULTS
```

The device **rejected** "Reset all to defaults" because its firmware predates
event `0x023`. That is why defaults never applied — nothing was wrong with the
GUI's request.

**Flash the updated `main.c` / `pcie_cfg.c` / `pcie_regs.h`.** Until then the
readback and defaults commands will keep returning error 4, and the sampling
rate will keep using the requested value instead of the achieved one.

## The freeze was my design error

`RfdcControl::sendCommand()` busy-polls the ring register on the **calling**
thread. Every button called it directly from the GUI thread, so the Qt event
loop stopped for the whole transaction:

| Action | Worst case frozen |
|---|---|
| Apply rate | 15 s |
| Reset all to defaults | 20 s |
| Read from hardware | 13 round trips, up to 26 s |

No repaints, no input, window greys out, the WM reports "not responding". A
rejected sampling-rate command is the slow path, which is exactly when you saw
it.

### Fix: the transaction runs on its own thread

* `RfdcControl` is created **without a parent** and moved to a dedicated
  `QThread`. Every call site now uses `rfdcInvoke()`, which queues a lambda
  onto that thread — the GUI thread never blocks.
* `attached()` reads a `QAtomicInt`, so the GUI can test it safely.
* `busyChanged(bool)` drives an indeterminate progress bar and disables the
  device-touching controls, so a second command cannot be queued behind a 15 s
  PLL reprogram.
* **Abort** button appears while busy and calls `requestAbort()`, which breaks
  the poll loop early. The device may still finish the command — the message
  says so rather than pretending otherwise.
* Readback is now **one** aggregated worker-thread pass emitting
  `snapshotReady(QVariantMap)`, instead of thirteen blocking round trips.
* The destructor aborts any in-flight poll, quits the thread and waits, so
  shutdown cannot tear the thread down mid-transaction.

### Self-correcting sampling rate

After a successful `Apply rate` the panel automatically re-reads the device, so
the field settles on the **achieved** rate. Ask for 2000 MHz and it will show
2006 — visibly, instead of silently disagreeing with the hardware.

## Verification

| Check | Result |
|---|---|
| `moc` + compile, `RfdcControl` and `ControlWindow3D`, Qt 5.15 | 0 errors, 0 warnings at `-Wall -Wextra -Wshadow` |
| moc-generated sources compile | clean |
| Firmware, 17 modules × 2 flows | 0 errors, 0 warnings |

## Order of operations

1. **Flash the updated firmware first.** Without it, defaults and readback
   return error 4 and the rate bug remains.
2. Rebuild the GUI.
3. **Ping** → confirms the path.
4. **Read from hardware** → fields show live values.
5. **Reset all to defaults** → 4800/9600, ×24, 32 mA, routing 7/3/2/1.

---

# Update 3 — full audit: the freeze was only half fixed

## What the audit found

Grepping every `m_rfdc->` call site in the UI showed the previous pass had
missed most of them.

**Still blocking the GUI thread:**

| Line | Call | Why it mattered |
|---|---|---|
| 393 | `setDecimation` + `realignMts` | MTS is slow on its own |
| 399 | `setInterpolation` + `realignMts` | same |
| 405 | `setNcoFrequency` | freeze/unfreeze cycle per block |
| 410 | `setNcoPhase` | |
| 415 | `setAdcDsa` | |
| 420 | `setQmcGain` | |

**Data races** — the worker thread owns the descriptor and mapping, but the GUI
thread was touching both:

| Line | Call | |
|---|---|---|
| 490 | `attachSysfs()` | `open()` + `mmap()` from the GUI thread |
| 660 | `snapshot()` | two register reads, on **every** `busyChanged(false)` |

**UI churn:** `readSnapshot()` ran 13 transactions, each emitting
`busyChanged(true/false)` — 13 progress-bar toggles and 13 enable/disable
storms per refresh.

**Wasted round trips:** `readParameter()` still issued a full transaction per
parameter even after readback had been proven unsupported — 13 pointless
transactions every refresh on such a build.

## Fixes

1. **All six handlers queued.** Decimation and interpolation now bundle their
   MTS re-align into the *same* queued job, so the two cannot be interleaved
   with another command.
2. **Attach moved to the worker** (`attachSysfsAsync`), with the result
   arriving via `attachResult(bool, QString)`. `open()`/`mmap()` now happen on
   the thread that owns the descriptor.
3. **Register readout moved to the worker** (`pollRegisters()` →
   `registersRead(ring, cfg)`). No BAR access from the GUI thread at all.
4. **Busy is refcounted.** `readSnapshot()` raises it once for the whole batch:
   one busy period, not thirteen.
5. **Early-out on unsupported readback** — 13 transactions saved per refresh.
6. **Status line decodes the ACK**, so `ACK err=0x4` is visible in the panel
   rather than only in the log.

Verified by grep: **no `m_rfdc->` call remains outside a queued lambda**, and
the only cross-thread read left is the GUI's own `m_rfAttached` mirror.

## Verification

| Check | Result |
|---|---|
| `ControlWindow3D.cpp`, `RfdcControl.cpp`, `-Wall -Wextra -Wshadow` | 0 errors, 0 warnings |
| Both moc-generated sources compile | clean |
| Grep audit for GUI-thread device access | none remaining |
| Firmware, 17 modules × 2 flows | 0 errors, 0 warnings |

## Still required on your side

The board is running firmware that predates events `0x023` and `0x0F1`. Until
it is flashed, **Reset all to defaults** and **Read from hardware** will return
`ACK err=0x4`, and the sampling-rate quantisation bug remains in the firmware
regardless of what the GUI does.
