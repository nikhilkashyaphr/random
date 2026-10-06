# RF Data Converter Control — User Guide

iWave SDR Analysis Tool · **H2C CONTROL** window · **RFDC** tab

---

## 1. What this tab controls, and what it does not

The window carries **two independent control paths**. Knowing which is which
saves a lot of confusion:

| Tab | Talks to | Over | Controls |
|---|---|---|---|
| **Transmit** | `rdma_tx` on **this PC** | `/dev/shm/iqctl` | pacing, sample rate, centre frequency |
| **Signal chain** | this PC | in-process | display/session values |
| **RFDC** | bare-metal firmware on the **ZU47DR** | PCIe BAR registers | the converters themselves |

The Transmit tab paces the host-side stream. **The RFDC tab configures the
silicon that stream lands in.** Changing one does not change the other.

---

## 2. First-time setup

### 2.1 Prerequisites

1. **Firmware must be current.** The board needs the build containing events
   `0x023` (defaults) and `0x0F1` (readback). If it doesn't, the status line
   shows `ring = 0x2A4xxxxx` — error `4`, *unknown event code* — and Reset and
   Read will not work.
2. **Run the GUI as root**, or grant `CAP_SYS_RAWIO`. Mapping a PCIe BAR
   through sysfs is privileged.
3. The bare-metal application must be at its **main menu**. If the console is
   sitting at a sub-prompt such as `Enter ADC Tile (0-3):`, the CPU is blocked
   in `scanf` and will not poll PCIe. Press Enter on the serial console.

### 2.2 Attach

| Field | Value | Why |
|---|---|---|
| **Endpoint (BDF)** | e.g. `0000:3c:00.0` | Auto-populated with Xilinx (`10ee`) devices |
| **BAR** | **2** | BAR2 is the 4 KB control window. **BAR0 is QDMA control space** — writing register offsets there can disturb the `iwfg` driver |
| **Register base** | `0x0` | Only non-zero if the block sits inside a larger window |

Press **Attach**. Green means mapped.

| Message | Meaning |
|---|---|
| `Cannot open … (needs root or CAP_SYS_RAWIO)` | Not privileged |
| `a driver holds this BAR exclusively` | Use the character-device backend |
| `No such BAR` | Wrong BDF — check `lspci -D` |

### 2.3 Prove the path — press **Ping**

Ping changes **no hardware**, so success proves the whole chain: host write →
PCIe → firmware poll → decode → acknowledge.

```
ring = 0x2A0000F0   cfg = 0x00000000   NEW_CMD=0   ACK ok
```

If Ping fails, nothing else on this tab will work. Fix it first.

---

## 3. Reading the status line

The line under the buttons is the live register state and is worth learning:

```
ring = 0x2A0000F0   cfg = 0x00989680   NEW_CMD=0   ACK ok
        ▲▲                  ▲                ▲        ▲
        ││                  │                │        └ decoded result
        ││                  │                └ 0 = idle, 1 = command pending
        ││                  └ payload of the last command
        │└ [23:20] error code
        └ 0x2A = this is a firmware acknowledgement
```

| You see | Meaning |
|---|---|
| `NEW_CMD=1` persisting | Firmware is not polling — check it is at the main menu |
| `ACK ok` | Last command applied |
| `ACK err=0x4` | **Unknown event — firmware is out of date** |
| `ACK err=0x5` | Payload out of range |
| `ACK err=0x6` | Event not valid for the selected converter |
| `ACK err=0x7` | That tile/block is not built in this design |

---

## 4. Target selection — read this before changing anything

**Converter / Tile / Channel apply to most commands below them.** Setting NCO
with *Tile 2, Channel 0* affects only that block.

| Control | Meaning |
|---|---|
| Converter | ADC, DAC, or both |
| Tile | All tiles, or 0–3 |
| Channel | Both, 0, or 1. ADC → blocks 0/1; DAC → blocks 0/2 |

Some commands are **global** and ignore tile/channel: sampling rate, Nyquist
zone, AXIS routing, PL GPIO routing, DAC source, MTS, Start, Stop, Reset.

---

## 5. The controls

### 5.1 Immediate controls — apply on edit

Leaving the field (or picking from the combo) sends the command at once.

| Control | Range | Notes |
|---|---|---|
| Decimation | 1–40 (listed) | ADC only. **Also triggers MTS re-align** |
| Interpolation | 1–40 (listed) | DAC only. **Also triggers MTS re-align** |
| NCO frequency | ±10 GHz | Sent in kHz. Freezes/unfreezes TSCB around the change |
| NCO phase | −180…180° | Integer degrees |
| ADC DSA | 0–27 dB, 0.5 steps | **0 dB = maximum gain.** Watch for clipping |
| QMC gain | 0–1.9999 | |
| DAC VOP | 2.25–40.5 mA | Boot default 32 mA |
| DAC input source | DDS / host stream | Which feeds the DAC |
| DDS frequency | 0–75 MHz | PL DDS compiler |

### 5.2 Deliberate controls — need their own Apply

These are heavy or structural, so they are not sent on every keystroke.

**Sampling rate** — reprograms the PLL, re-aligns MTS, restores NCOs. Takes
several seconds. See §6, this one has a trap.

**Nyquist zone** — forces zone 1 or 2, overriding automatic selection.

**AXIS routing** — four values, each 0–7, mapping an input stream to each
output. Boot default is **7, 3, 2, 1**.

**PL GPIO routing** — modes 1/2/3 = 1/2/4 channels in PL logic. Boot default
**Mode 3**.

### 5.3 Actions

| Button | Effect |
|---|---|
| **Ping** | No-op handshake. Always safe |
| **Read registers** | Refresh the status line only |
| **Read from hardware** | Populate **every field** from the device |
| **Start** / **Stop** | Enable / disable all converter FIFOs. **Stop means no data at all** until Start |
| **Re-align MTS** | Re-run multi-tile sync and restore NCOs |
| **Soft reset** | MTS + NCO + routing restore. **Not** a converter power cycle |
| **Reset all to defaults** | Restore the firmware's boot configuration, then read it back |
| **Abort current command** | Appears while busy; stops waiting for the ACK |

---

## 6. Sampling rate — the one real trap

**The PLL cannot produce arbitrary frequencies.** It synthesises
`Fs = 300 MHz × FBDIV / OutDiv`, so most requests are quantised:

| You ask for | You get | Error |
|---|---|---|
| 2000 MHz | **2006.25** | +6.25 |
| **2400 MHz** | 2400.00 | exact |
| **3000 MHz** | 3000.00 | exact |
| 4000 MHz | 4012.50 | +12.5 |
| **4800 MHz** | 4800.00 | exact |
| 5000 MHz | 4987.50 | −12.5 |
| **6000 MHz** | 6000.00 | exact |
| **9600 MHz** | 9600.00 | exact |

**Prefer 2400 / 3000 / 3600 / 4800 / 6000 / 9600 MHz** and the error vanishes.

After a successful Apply rate the panel **re-reads the device automatically**,
so the field settles on the *achieved* value. Ask for 2000 and you will see it
change to 2006 — that is correct behaviour, not a glitch. The serial console
prints the same thing:

```
NOTE: PLL quantised the request. asked 2000.000 MHz, achieved 2006.250 MHz.
```

Older firmware stored the *requested* rate and derived the stream clock, the
Clocking Wizard divider, the DDS phase increment and the Nyquist zone from it —
so everything downstream was quietly off. Current firmware uses the achieved
rate throughout.

---

## 7. Recommended order of operations

Commands are not commutative. This order leaves the datapath consistent:

```
1. Sampling rate        ← changes everything downstream
2. Decimation / Interpolation
3. Re-align MTS         ← automatic after 1 and 2
4. NCO frequency / phase
5. DSA, QMC, VOP
6. AXIS / GPIO routing, DAC source
7. DDS frequency        ← LAST: derived from the DAC stream clock
```

**DDS goes last** because its phase increment is computed from the DAC stream
clock, which any interpolation or sampling-rate change moves.

---

## 8. Getting back to a known state

Press **Reset all to defaults**. One firmware command restores the boot
configuration and the panel then reads it back:

| Parameter | Default |
|---|---|
| ADC / DAC sampling rate | 4800 / 9600 MHz |
| Decimation / Interpolation | ×24 / ×24 |
| ADC & DAC NCO | 3100 MHz |
| DDS | 10 MHz |
| ADC DSA | 0 dB |
| DAC VOP | 32 mA |
| DAC source | DDS compiler |
| AXIS routing | 7, 3, 2, 1 |
| PL GPIO | Mode 3 |
| Nyquist | automatic |

These match a power cycle exactly — the firmware is the authority, the GUI just
displays it.

> If this returns `ACK err=0x4`, the board's firmware predates the defaults
> command. Flash the current build.

---

## 9. Responsiveness

Every device transaction runs on a **worker thread**; the GUI never blocks —
plots keep updating and the window stays usable throughout.

- A thin progress bar appears while a command is in flight.
- Device controls disable so a second command cannot queue behind a 15 s PLL
  reprogram.
- **Abort** stops waiting. The device may still finish the command; the log
  says so rather than pretending otherwise.
- **Read from hardware** is a single batched pass, not 13 round trips.

If the whole application freezes for seconds at a time, you are running an
older build — that was a real defect and it is fixed.

---

## 10. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| Everything times out | Firmware not polling | Press Enter on the serial console to leave any sub-menu |
| `ACK err=0x4` | Firmware out of date | Flash the current build |
| Fields don't update on Read | Readback unsupported by that PL build | Log says so; values shown are the last sent |
| `NEW_CMD=1` stuck | Previous command never consumed | Check the board is running; reattach |
| Rate reads back different | **Normal** — PLL quantisation | Use an exact rate (§6) |
| No data after Stop | Working as intended | Press Start |
| `EBUSY` on attach | A driver owns the BAR | Use the character-device backend |
| Attach fails, not root | sysfs BAR needs privilege | Run with `sudo` |

### Cross-checking against the CLI

`rfdc_ctl` speaks the same protocol and is useful for isolating GUI from
firmware:

```bash
sudo ./rfdc_ctl -b 0000:3c:00.0 -r 2 -v ping
sudo ./rfdc_ctl -b 0000:3c:00.0 -r 2 dump
```

If the CLI works and the GUI does not, the problem is the GUI. If both fail,
it is the link or the firmware.

---

## 11. Safety notes

- **ADC DSA 0 dB is maximum gain**, applied to every block at boot. Combined
  with DAC VOP at 32 mA this maximises ENOB — and clipping risk on an unknown
  input.
- **Soft reset is not a power cycle.** It deliberately does not re-run the
  LMK/LMX clock-tree sequence, because dropping the clock tree from a remote
  host is not something a button should do.
- **Stop really stops.** No samples at all until Start.
- Sampling-rate changes tear down and rebuild the datapath. Do not change them
  while recording.

---

## 12. Device-state synchronisation (how the panel now behaves)

The panel follows one rule:

> **Read the device first, then update the software and the UI to match it.**

### 12.1 On connect — automatic, and the UI waits

Pressing **Attach** runs a single sequence on the worker thread:

```
Attach (open + mmap)
      ↓
Ping                    ← prove the link before trusting anything read over it
      ↓
Read ALL registers
      ↓
Validate + update internal state
      ↓
Populate the panel
      ↓
Enable user interaction
```

**Every control stays disabled until this finishes.** That is deliberate: it is
the only way a widget's default value cannot be written over a live device
setting before you have seen what the device actually holds.

While it runs you see *"Synchronising with device…"* and a progress bar.

| Outcome | What you see |
|---|---|
| Success | Green *"Synchronised with device."*, controls enabled |
| Ping failed | Amber, controls stay disabled — the link is not trustworthy |
| Readback unsupported | Amber warning: values shown are **not** confirmed against hardware |

Reconnecting re-runs the whole sequence. So does Complete Reset.

### 12.2 On change — write, read back, verify

Sampling rate uses the full loop:

```
Write → Read back → Compare → Update UI (with the DEVICE's value)
```

* **Match** → green confirmation, the field shows the confirmed value.
* **Quantised within 25 MHz** → still success, and it says so:
  *"Confirmed 2006 MHz (PLL quantised the 2000 MHz request by 6 MHz)."*
* **Outside tolerance, or read-back failed** → a warning dialog listing
  requested vs. actual. **The UI does not claim success.**

The 25 MHz tolerance exists because the PLL synthesises
`RefClk × FBDIV / OutDiv` and legitimately cannot hit every request (§6).
Demanding an exact match would report a failure where the hardware behaved
correctly.

If readback is unsupported by the firmware, the write is reported as
**not verified** rather than as success.

### 12.3 COMPLETE RESET

The red **COMPLETE RESET** button is deliberately separated from the ordinary
controls, because it changes many settings at once.

It asks first:

> **Restore every RF Data Converter parameter to its default state?**
> This will stop the datapath, restore the firmware's boot configuration
> (4800/9600 MHz, ×24, NCO 3100 MHz, DDS 10 MHz, 32 mA VOP, routing 7/3/2/1),
> restart the datapath and re-read every register.

Then it runs:

```
Stop datapath
      ↓
Restore firmware defaults
      ↓
Wait for the device to answer again (up to 2 s, polled by ping)
      ↓
Restart datapath
      ↓
Re-read ALL registers
      ↓
Report
```

**Success is reported only if every stage succeeded.** If defaults fail, the
datapath is restarted anyway — leaving the converters stopped after a failed
reset would be worse than the reset failing — and the dialog says the device may
be in a partially reset state.

### 12.4 What is locked, and when

The panel cannot issue conflicting operations:

| Phase | Controls |
|---|---|
| Before attach | disabled |
| Syncing | disabled, *"Synchronising…"* shown |
| Command in flight | disabled, progress bar, **Abort** available |
| Idle and attached | enabled |

### 12.5 Error handling

Nothing is silently ignored:

| Condition | Behaviour |
|---|---|
| Device disconnected | Attach fails with the reason; controls stay disabled |
| Register read failure | Sync reports failure; values are not shown as confirmed |
| Register write failure | Verification dialog; UI **not** updated as successful |
| Read-back mismatch | Warning dialog with requested vs. actual |
| Reset timeout | *"Device did not become ready after reset."* |
| Readback unsupported | Amber warning; values marked unconfirmed |
| Communication timeout | Logged, plus **Abort** to stop waiting |

---

## 13. Acceptance checklist

**Register synchronisation**
- [x] All registers read during initialisation
- [x] Panel displays actual register values
- [x] Default widget values cannot overwrite device values — UI is gated
- [x] Sync runs again after reconnect
- [x] Sync runs again after reset

**Sampling rate**
- [x] Correct register only (`ring` EVENT `0x010`, payload MHz)
- [x] No unrelated register touched — one write per command
- [x] Unit conversion documented and tested (MHz → payload, kHz for NCO)
- [x] Written value read back and verified
- [x] Panel displays the confirmed hardware value
- [x] Quantisation reported rather than hidden

**Complete reset**
- [x] Dedicated, visually separated control
- [x] Confirmation dialog before executing
- [x] All settings restored to firmware defaults
- [x] Registers re-read afterwards
- [x] UI synchronised with the result
- [x] Success **and** failure clearly reported

**Reliability**
- [x] Communication failures handled
- [x] Never reports success when an operation failed
- [x] Conflicting operations impossible during sync/reset
- [x] Verified mechanically: no device call outside a queued worker lambda
