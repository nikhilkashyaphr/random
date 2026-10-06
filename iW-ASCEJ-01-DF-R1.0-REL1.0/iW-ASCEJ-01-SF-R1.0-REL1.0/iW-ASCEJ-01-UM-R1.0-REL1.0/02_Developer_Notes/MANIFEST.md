# Complete source package — nothing here is a patch

Every file is a **full, standalone source file**. Delete your old copies and
drop these in. There are no diffs, no fragments, no "apply on top of".

## Layout

```
firmware/   37 files  ->  <vitis>/app_component/src/     (bare-metal, A53)
host/        5 files  ->  anywhere on the Linux PC       (CLI tool)
gui/         full Qt project                             (workstation)
docs/       36 documents
```

## firmware/ — 37 files

**Delete everything in `app_component/src/` and copy all 37 in.**

| Changed in this release | Why |
|---|---|
| `main.c` | UART starvation fix, achieved-PLL-rate fix, menus 17/18 |
| `pcie_cfg.c` | **readback 0x0F1, defaults 0x023, capability banner** |
| `pcie_cfg.h` | ABI/capability constants |
| `pcie_regs.h` | event map, readback protocol, boot defaults |
| `rfdc_cmd.c/.h` | DAC tile macro fix, blocking readiness wait |
| `adcSaveCalCoefficients.c` | coefficient region sizing comment |
| `platform_ids.h` | SDT / classic identifier mapping |

The other 30 are unchanged from your baseline and are included so the folder is
complete — you never have to work out which ones to keep.

**`rfdc_ctl.c`, `pcie_access.c` and `pcie_access.h` are deliberately ABSENT.**
They are the Linux host tool and will not compile for the A53. Putting them in
`src/` is what caused the earlier `pcie_access.h: No such file` build failure.

### Checksums of the four critical files

```
  e7216eda581732abe163412d52a4f511  firmware/main.c
  abfa586df5f327efc421cd78442f8450  firmware/pcie_cfg.c
  6053021fe0a1a8caa146abdf98baf0fe  firmware/pcie_cfg.h
  abb2d53bdc6b818b6a95164dfe618808  firmware/pcie_regs.h
```

### Verifying the flash took

The single most useful line on the console:

```
[pcie] pcie_cfg ABI 3, built <date>, caps 0x3F:
       basic extended start/stop READBACK(0x0F1) DEFAULTS(0x023)
```

**Missing that line = `pcie_cfg.c` is stale.** This is what caught the last
mixed build, where `main.c` was current and `pcie_cfg.c` was not.

## host/ — 5 files

```bash
cd host && make          # -> ./rfdc_ctl
sudo ./rfdc_ctl -b 0000:3c:00.0 -r 2 -v ping
```

## gui/ — full Qt project

```bash
cd gui && mkdir -p build && cd build && cmake .. && make
```

## The one file that lives in three places

`pcie_regs.h` is shared by all three, byte-identical (the GUI copy uses LF to
match its tree; content is the same). It is the single definition of the
register map and event codes.

**If you ever edit it, copy it to all three.** A silent divergence means the
host sends commands the firmware misinterprets — far harder to debug than a
build error.

## Build verification performed

| Target | Result |
|---|---|
| Firmware, 17 modules × 2 flows (SDT + classic) | 0 errors, 0 warnings, `-Wall -Wextra` |
| Host tool | 0 warnings, `-Wall -Wextra -Wpedantic -Wconversion` |
| GUI, 3 changed sources + 3 moc outputs | 0 errors, 0 warnings, `-Wall -Wextra -Wshadow` |
| DAC packing, 200 000 samples | 0 nonzero LSBs, peak 32764 |
| Generator tone accuracy | 10.000 MHz requested → 10.000 measured |

## Order of work

1. **Flash all 37 firmware files.** Confirm the ABI banner.
2. Rebuild the GUI.
3. **Attach** → should go green: *"Synchronised with device."*
4. **Read from hardware** → fields populate from the board.
5. **COMPLETE RESET** → 4800/9600, ×24, 32 mA, routing 7/3/2/1.
6. Transmit tab → **Start generator**, then set DAC input source to
   *Host / GNU Radio stream*.
