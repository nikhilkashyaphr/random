# You are blocked on one thing: the board's firmware

## What the amber message means

```
Device is reachable, but NOT ONE register could be read back.
```

That is accurate. Your serial log shows why:

```
[pcie] REJECTED event 0x000F1: unknown EVENT code     <- readback   (x13)
[pcie] REJECTED event 0x00023: unknown EVENT code     <- defaults
```

Error code `0x4` is the firmware saying *"I have never heard of this command."*
The board is running a build that predates both events. **No change on the host
side can fix that** — the commands do not exist in the running firmware.

Everything else in your log worked: `ping`, `dac-source`, `dds-freq` at 5 and
25 MHz, `start`, `stop`.

## My design error, now corrected

Locking the panel read-only protected you from writing defaults — and stopped
you testing anything at all. A safety interlock that blocks all work is a bad
interlock.

The real danger was never "the operator edits a field". It was **the panel
auto-sending a value nobody chose** — QMC 0.0000 (silence), DAC VOP 0 mA
(rejected), decimation ×1 against a device running ×24.

So the two concerns are now separated:

| Readback | Panel | On edit |
|---|---|---|
| **available** | populated from the device | sent immediately, as before |
| **unavailable** | fields enabled, marked as defaults | **recorded, not sent** |

In the second mode each edit is highlighted amber and counted. Nothing reaches
the hardware until you press **"Apply changed values"**, and only the fields
you actually edited are transmitted. A field you never touched is now
*structurally incapable* of being sent.

**So you can work on today's firmware.** The panel is fully usable; it simply
will not act on your behalf.

---

## Flashing — the thing that actually unblocks you

You need `main.c`, `pcie_cfg.c`, `pcie_cfg.h`, `pcie_regs.h` from
`baremetal_uart_fix.zip`.

### 1. Copy the sources

```bash
cp baremetal_src/*.c baremetal_src/*.h \
   <workspace>/app_component/src/
```

Do **not** copy `rfdc_ctl.c`, `pcie_access.*` or the `Makefile` — those are the
Linux host tool and will not compile for the A53.

### 2. Build

Right-click `app_component` → **Build Project**. Expect 0 errors.

Two warnings about `IntrHandler`/`Intc` being unused are normal — the GIC block
is compiled out by `ENABLE_LMK_LOCK_IRQ=0`.

### 3. Run it

Whichever route you already use: JTAG for a quick test, or rebuild `BOOT.BIN`
with `boot/make_boot.sh` for standalone boot.

### 4. Confirm on the serial console

You are looking for **three** things:

```
[pcie] config manager ready @ 0xA00A0000 (ring +0xa8, cfg +0xac)
```

then, from the GUI's **Read from hardware**:

```
[pcie] cmd ring=0x8F2000F1 cfg=0x52420001 | ... evt=0x000F1
[pcie] applied.                                   <- NOT "REJECTED"
```

and from **COMPLETE RESET**:

```
[pcie] restoring boot defaults...
[pcie] defaults restored.
```

If you still see `REJECTED ... 0x000F1`, the old ELF is still running — the
build did not reach the board.

### 5. Confirm in the GUI

Press **Attach**. The amber message should become green:

```
Synchronised with device.
```

At that point the fields hold real device values, apply-on-edit resumes, and
**Apply changed values** disappears because it is no longer needed.

---

## What the new firmware also fixes

Worth knowing, because two of these explain behaviour you have already seen:

**PCIe starvation.** `scanf()` left the CR/LF in the UART FIFO, so the guard
loop exited without polling and the CPU blocked in libc. PCIe was only serviced
when you typed — exactly what you noticed. `MenuReadLine()` now owns the FIFO
and polls between keystrokes, including inside sub-menus.

**Sampling-rate quantisation.** The PLL synthesises `RefClk × FBDIV / OutDiv`
and cannot reach every request — 2000 MHz lands on 2006.25. The firmware stored
the *requested* value and derived the stream clock, Clocking Wizard divider,
DDS phase increment and Nyquist zone from it. It now reads the achieved rate
back and uses that throughout.

**Prefer exact rates:** 2400 / 3000 / 3600 / 4800 / 6000 / 9600 MHz.

---

## Testing H2C today, without flashing

The transmit path does not depend on the RFDC readback at all:

1. **Transmit** tab → set sample rate to the DAC stream rate (200 MSPS).
2. Start the H2C backend so `iwfg_h2c` is reading the FIFO.
3. **Start generator.** Watch the chunk counter increase — that is proof bytes
   are leaving the application and being consumed.
4. **RFDC** tab → set **DAC input source** to *Host / GNU Radio stream*, then
   press **Apply changed values**.

Step 4 works on your current firmware: `dac-source` (event `0x016`) is one of
the commands your log shows succeeding. Left on *DDS compiler* the converter
plays its internal oscillator and ignores everything you send.
