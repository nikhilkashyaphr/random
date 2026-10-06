# You flashed main.c but not pcie_cfg.c

## The evidence

Your boot log contains, side by side:

**From the NEW `main.c`:**
```
Release 2025.2   Sep  3 2026 - 09:03:55        <- built today
[init] 2/9 GIC + LMK lock IRQ SKIPPED (ENABLE_LMK_LOCK_IRQ=0)
[init] PL peripherals OK - bitstream is loaded.
--- Step 2.7: PCIe Configuration Interface ---
17. PCIe interface diagnostics
18. Set PCIe control base address (debug)
"> " prompt redraw                              <- MenuReadLine, the UART fix
```

**From the OLD `pcie_cfg.c`:**
```
[pcie] REJECTED event 0x000F1: unknown EVENT code
```

`main.c` is current. `pcie_cfg.c` and `pcie_regs.h` are not.

The readback (`0x0F1`) and defaults (`0x023`) handlers live in **`pcie_cfg.c`**,
not `main.c`. So you got the UART starvation fix and the new menu, but not the
two commands the GUI needs.

## My gap, now closed

Nothing on the board announced which build it was running, so new-main +
old-pcie_cfg looked identical to a fully updated board. That ambiguity cost
bench time and it was my omission.

`pcie_cfg` now states its own identity at startup:

```
[pcie] config manager ready @ 0xA00A0000 (ring +0xa8, cfg +0xac)
[pcie] pcie_cfg ABI 3, built Sep  3 2026 09:14:22, caps 0x3F:
       basic extended start/stop READBACK(0x0F1) DEFAULTS(0x023)
```

**If that second line is missing, `pcie_cfg.c` is stale.** No ambiguity left.

Menu **17** repeats it, and `PCIE_CFG_ABI` bumps whenever an event is added.

## Flash these four files together

| File | md5 (first 12) |
|---|---|
| `main.c` | `e7216eda5817` |
| `pcie_cfg.c` | `abfa586df5f3` |
| `pcie_cfg.h` | `6053021fe0a1` |
| `pcie_regs.h` | `abb2d53bdc6b` |

```bash
cp baremetal_src/main.c baremetal_src/pcie_cfg.c \
   baremetal_src/pcie_cfg.h baremetal_src/pcie_regs.h \
   <workspace>/app_component/src/

md5sum <workspace>/app_component/src/pcie_cfg.c   # must match above
```

Then Build Project and run. Confirm on the console:

```
[pcie] pcie_cfg ABI 3, ... READBACK(0x0F1) DEFAULTS(0x023)
```

## Two other defects your log exposed

### Diagnostics decoded an ACK as a command

Menu 17 printed:

```
ring (+0xA8) : 0x2A4000F1
    TARGET  [30:28]= 2
    TILE    [26:24]= 2
    CHANNEL [23:20]= 4
```

Nonsense — `0x2A4000F1` is an **acknowledgement**, not a command. Bits `[30:24]`
are the marker `0x2A`, not TARGET/TILE. Diagnostics now detects the marker and
decodes it correctly:

```
      (this is an ACK word, not a command)
      marker  [30:24]= 0x2a
      error   [23:20]= 0x4  unknown EVENT code
      event   [19:0] = 0x000f1  (the command reported on)
```

### The GUI retried all 13 readbacks every refresh

Your counters showed it: `seen/applied/rejected : 56 / 4 / 52`. Error `0x4` on a
readback is a **capability answer**, not a transient failure — the firmware does
not implement `0x0F1`. The host now latches that on the first rejection:

| | rejected transactions per refresh |
|---|---|
| before | 13 |
| after | 1, then 12 skipped locally |

It also names the cause directly: *"pcie_cfg.c is older than main.c — reflash
BOTH."*

## Verification

| Check | Result |
|---|---|
| Firmware, 17 modules × 2 flows, `-Wall -Wextra` | 0 errors, 0 warnings |
| GUI, 3 sources + 3 moc outputs, `-Wall -Wextra -Wshadow` | 0 errors, 0 warnings |

## What already works on the board you have

From your own log — these succeeded, so the link, base address and handshake
are all proven:

```
[pcie] ping                       -> applied
DAC Input routed to Host / GNU Radio stream
DDS Frequency set to 25.000 MHz
[pcie] datapath STARTED / STOPPED
```

Only readback and defaults are missing, and both live in the one file that did
not get flashed.
