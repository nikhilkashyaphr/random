# H2C CONTROL — User Guide

A step-by-step guide to the **H2C CONTROL** window: opening it, what each
control does, and how to drive the ZU47DR RF Data Converter (RFDC) entirely
from the GUI. No terminal commands are needed at any point.

If you ever get lost, click **Reset all to defaults** on the RFDC tab — it puts
every field back to a known baseline in one click.

---

## 1. Start the application

Launch the app. You do **not** pass anything on the command line.

1. The launcher dialog appears. Pick your source (Simulated / File / PCIe /
   Ethernet), the data-flow mode (ADC Only / DAC Only / ADC + DAC) and press
   the launch button.
2. The main workstation window opens.

That is all the setup there is. Everything below happens inside the GUI.

---

## 2. Open the H2C CONTROL window

On the main window toolbar, click **H2C CONTROL**.

- The window opens as a **separate, floating panel** — the main window stays
  fully usable behind it.
- It is available in **every** data-flow mode. In *ADC Only* mode the Transmit
  tab is inactive (there is no transmit path), but the **RFDC** tab still works,
  because converter configuration is valid regardless of data flow.
- Closing the window only hides it; your settings are kept and reappear when you
  open it again.

The window can be **resized** by dragging any edge or the corner grip, and each
tab **scrolls**, so the lower controls are always reachable even on a small
screen.

The window has three tabs: **Transmit**, **Signal chain**, and **RFDC**.

---

## 3. The RFDC tab (main converter control)

This tab talks to the bare-metal application on the board over the PCIe control
registers. It is organised top-to-bottom in the order you normally use it.

### 3.1 Connect — "PCIe control interface"

| Field | What it is |
|-------|-----------|
| **Endpoint (BDF)** | The PCIe device address, e.g. `0000:3c:00.0`. Xilinx cards (vendor `10ee`) are listed automatically — pick one, or type your own. |
| **BAR** | The memory window that holds the control block. Default **2**. |
| **Register base** | Offset of the control block inside the BAR. Leave at `0x0` unless your design differs. |
| **Attach** | Maps the registers and turns on all the controls below. |

Press **Attach**.

- On success the status line turns green (`Attached: …`) and every control on the
  tab becomes usable.
- On failure the status line turns red with the reason. Common cases are in the
  Troubleshooting table (§6).

Until you attach successfully, all the parameter and action controls stay
disabled — so you can never send a command into thin air.

### 3.2 Choose the target — "Target"

These three selectors decide **what** the next parameter you edit applies to:

- **Converter** — ADC, DAC, or ADC + DAC.
- **Tile** — All tiles, or a specific Tile 0–3.
- **Channel** — Both channels, or Channel 0 / Channel 1.

Set these **first**, then edit a parameter.

### 3.3 Set parameters — "Parameters — applied on edit"

**Important:** each value is sent to the board **the moment you finish editing
it** — press *Enter* or click away from a number box, or pick an item in a
drop-down. There is no "apply everything" button, so **changing one field
changes only that one thing** on the hardware.

| Control | Meaning | Applies to |
|---------|---------|-----------|
| **Decimation** | ADC decimation factor (only the accepted factors are listed). | ADC |
| **Interpolation** | DAC interpolation factor. | DAC |
| **NCO frequency** | Digital mixer frequency, in MHz (may be negative). | Target |
| **NCO phase** | Mixer phase offset, −180…180°. | Target |
| **ADC DSA** | ADC step attenuator, 0…27 dB. `0` = leave unchanged. | ADC |
| **QMC gain** | Gain correction. `0` = leave unchanged. | Target |
| **DAC VOP** | DAC output current, in mA. `0` = leave unchanged. | DAC |
| **DAC input source** | DDS compiler, or the host / GNU Radio stream. | DAC |
| **DDS frequency** | On-chip DDS tone frequency, in MHz. | DDS |

A change to Decimation or Interpolation automatically re-aligns MTS afterwards,
because it moves the stream clock.

### 3.4 Heavy / structural operations — "PLL, Nyquist & routing"

These are **not** applied on edit. You set the value, then press that row's own
**Apply** button, so they are only issued deliberately:

- **Sampling rate** → *Apply rate*. Reprograms the PLL and re-aligns MTS — this
  one takes a few seconds, that is normal.
- **Nyquist zone** → *Apply zone*. Zone 1 or Zone 2 for the target converter.
- **AXIS routing** (four boxes) → *Apply*. Source stream for each AXIS output.
- **PL GPIO routing** → *Apply*. Routing mode 1–3.

### 3.5 Actions

| Button | What it does |
|--------|-------------|
| **Ping** | Handshake test. Changes nothing on the hardware, so a success proves the whole path end-to-end. Use it first after Attach. |
| **Read registers** | Refreshes the `ring` / `cfg` readout at the bottom. |
| **Start** | Enables the converter FIFOs (data flows). |
| **Stop** | Disables the FIFOs (no data until Start). |
| **Re-align MTS** | Re-establishes multi-tile sync. |
| **Soft reset** | *Firmware* reset: MTS re-align + NCO/routing restore. Does not power-cycle the converters. |
| **Reset all to defaults** | *Panel* reset — see §4. |

### 3.6 The register readout

The bottom line shows the two live control registers:

```
ring = 0x........   cfg = 0x........   NEW_CMD=0
```

`NEW_CMD=0` means the board has consumed the last command and is ready for the
next. If it sticks at `1`, see Troubleshooting.

---

## 4. "Reset all to defaults" — recover a known baseline

This button is the quick way out of a messy configuration, so you never have to
re-enter every field by hand.

When you click it:

1. **Every control on the RFDC tab** is set back to its default value
   (Target = ADC, Tile = All, Channel = Both, Decimation/Interpolation = x1,
   NCO = 3100 MHz, phase = 0, DSA/QMC/VOP = 0, DAC source = DDS, DDS = 10 MHz,
   Sampling rate = 2000 MHz, Nyquist = Zone 1, AXIS = 0/1/2/3, PL GPIO = Mode 1).
2. **If a device is attached**, it also applies the default signal chain in one
   go — decimation, interpolation, MTS re-align, NCO frequency, NCO phase, DAC
   source and DDS — so the board itself returns to that baseline.
   - The 0-valued gain/attenuator fields (DSA, QMC, VOP) mean "leave unchanged",
     so they are not pushed.
   - Sampling rate, Nyquist and routing are heavy/structural, so those are reset
     in the panel but you apply them yourself with their own buttons if you want.
3. **If no device is attached**, it just resets the on-screen values, and the log
   says so.

**Soft reset vs. Reset all to defaults:**

- *Soft reset* asks the **firmware** to restore its own converter defaults.
- *Reset all to defaults* restores the **panel's** default values (and pushes the
  default signal chain). Use this one when you want the GUI and the board back to
  the baseline you started from.

---

## 5. The other two tabs

### Transmit
Live control of the host transmitter. Three fields — **Sample rate**, **Centre
frequency**, **Pacing** — plus:

- **Apply** — writes the values and bumps the sequence; the transmitter picks
  them up on its next poll.
- **Revert** — reloads whatever is currently in the control block.

Tick **Unlimited** to send unpaced. This tab is inactive in ADC-Only mode.

### Signal chain
Describes the intended H2C transmit feed (NCO/DUC, interpolation, decimation,
channels, wire format, chunk size). In the current build these are informational
— the values the transmitter actually honours live are the three on the
**Transmit** tab.

---

## 6. Troubleshooting

| Symptom | Cause / fix |
|---------|-------------|
| **"No Xilinx PCIe endpoint found"** | No card enumerated. Check the board is up and the link is trained. |
| **"needs root or CAP_SYS_RAWIO"** on Attach | The BAR needs privilege to map. Launch the app with the rights your setup uses for hardware access. |
| **"a driver holds this BAR exclusively"** | Another driver owns BAR 2. Detach it, or use the character-device backend for that stack. |
| **Controls are greyed out** | You are not attached. Fill in the endpoint and press **Attach**. |
| **`NEW_CMD` stays 1** | The previous command was not consumed — the firmware may be sitting in a UART sub-menu, or not polling. Press **Read registers** to re-check. |
| **A value "did not take"** | Make sure you committed it: press *Enter* or click away from the box, or re-pick the drop-down item. Watch the log line for the confirmation. |
| **Sampling-rate change seems to hang** | It reprograms the PLL and can take several seconds. That is expected. |
| **Everything looks wrong** | Click **Reset all to defaults**. |

---

## 7. A first-time walkthrough

1. Open **H2C CONTROL** → **RFDC** tab.
2. Pick the **Endpoint (BDF)**, leave BAR = 2, press **Attach** → wait for green.
3. Press **Ping** → the log should say it applied. That proves the path works.
4. Set **Converter = ADC**, **Tile = All tiles**, **Channel = Both channels**.
5. Change **NCO frequency**, press *Enter* → the log confirms it was applied, and
   the `ring`/`cfg` line updates.
6. Press **Start** to enable the FIFOs.
7. When you want to begin again from scratch, press **Reset all to defaults**.
