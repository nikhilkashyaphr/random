# Creating BOOT.BIN — ZU47DR RFSoC Bare-Metal, Vitis 2025.2

Everything needed to boot the board standalone from SD or QSPI, with no JTAG.

---

## 1. What goes in, and what must not

A ZynqMP/RFSoC bare-metal boot image has exactly **four** partitions:

| # | Partition | Purpose | Required? |
|---|---|---|---|
| 1 | `zynqmp_fsbl.elf` | Runs on A53#0 from OCM. PS init, DDR bring-up, loads everything below. | **Yes** |
| 2 | `zynqmp_pmufw.elf` | PMU firmware — power islands, clocks, resets. | **Yes** (see §1.1) |
| 3 | `<design>.bit` | Configures the PL. | **Yes** |
| 4 | `app_component.elf` | Your application. | **Yes** |

**Do not include** `bl31.elf` (ARM Trusted Firmware), `u-boot.elf`, `system.dtb`, `boot.scr`, or `Image`. Those are Linux boot components. Adding ATF to a bare-metal image is a common cause of "FSBL runs, then nothing" — the app never gets control at the exception level its BSP expects.

### 1.1 Why PMUFW is not optional here

On a generic ZynqMP design you can sometimes get away without PMU firmware. **On an RFSoC you should not.** The PMU manages the power islands and reset sequencing that the RF-ADC/RF-DAC tiles depend on. Without it, `XRFdc_StartUp()` behaviour becomes inconsistent between cold and warm boots — tiles that reach power-on state `0xF` on the bench under JTAG may not from flash. Given `rfdcWaitAllTilesReady()` now blocks on exactly that state, an image built without PMUFW will show up as a startup timeout.

### 1.2 Partition order is not cosmetic

The FSBL loads partitions **in BIF order**. The bitstream **must** come before the application: the RFDC, AXI GPIO, AXIS switch and Quad SPI registers `main()` touches do not exist until the PL is configured. Application-before-bitstream gives an AXI hang or a data abort inside the first `XRFdc_CfgInitialize()`.

---

## 2. Prerequisite: the platform must generate boot artifacts

**This is the step that most often blocks people, and it cannot be fixed after the fact from the GUI.**

FSBL and PMUFW only exist if the platform component was created with **"Generate boot artifacts"** (called "Generate boot components" in some versions) ticked in the *New Platform Component* wizard. Enabling it creates an FSBL domain and a PMU firmware domain alongside your `standalone_psu_cortexa53_0` domain.

Check for them now:

```bash
find <workspace>/platform_ZU47 -name 'zynqmp_fsbl.elf' -o -name 'zynqmp_pmufw.elf'
```

If that returns nothing, **recreate the platform component** with the option enabled, then rebuild it. Your application component points at the platform by name, so it will pick the new one up without changes.

### 2.1 Getting the bitstream

The `.bit` is inside the `.xsa`, which is a zip archive:

```bash
unzip -o <design>.xsa '*.bit' -d <workspace>/platform_ZU47/hw/
```

If there is no `.bit` inside, the XSA was exported **pre-synthesis**. Re-export from Vivado with *File → Export → Export Hardware → **Include bitstream***.

---

## 3. Method A — GUI (Vitis Unified IDE)

1. Open the workspace. In the **Flow** navigator, select your **application component** (`app_component`).
2. Click **Create Boot Image**.
3. **Architecture**: `Zynq MP`. **Output format**: `BIN`. **Output path**: name it exactly `BOOT.BIN` (case matters for the ZynqMP BootROM on FAT32).
4. The wizard pre-populates FSBL and the app. **Add the two it misses**, in this order:
   - **PMU firmware** — `Add` → browse to `zynqmp_pmufw.elf` → *Partition type* = **pmufw**.
   - **Bitstream** — `Add` → browse to your `.bit` → *Partition type* = **datafile**, *Destination device* = **PL**.
5. Reorder so the list reads: **FSBL → PMUFW → bitstream → application**. Drag to reorder if needed.
6. **Create Image**.

The wizard writes a `.bif` alongside the output. Keep it under version control — it is the reproducible record of what you built.

---

## 4. Method B — command line (recommended for repeatability)

Two files are included in `boot/`:

- **`boot.bif`** — hand-written, fully commented, edit the four filenames to match yours.
- **`make_boot.sh`** — locates the artifacts, validates them, generates a BIF and runs bootgen.

```bash
source /path/to/Vitis/2025.2/settings64.sh
cd boot
./make_boot.sh /home/iwave/Downloads/Baremetal_COde/Udpate_S1
```

Or drive `bootgen` directly:

```bash
bootgen -arch zynqmp -image boot.bif -o BOOT.BIN -w on
```

`-w on` overwrites an existing output. Without it bootgen fails rather than replacing.

### 4.1 What `make_boot.sh` checks before it builds

It refuses to produce an image that would fail silently on hardware:

| Check | Why |
|---|---|
| FSBL and app are **AArch64** | An FSBL built for the wrong processor is the classic "no console output at all". |
| App entry point is in **DDR**, not OCM | An OCM-linked app overlaps the FSBL and corrupts it mid-load. |
| Bitstream is a plausible size | Catches a truncated or placeholder `.bit`. |
| All four artifacts present | With specific, actionable guidance per missing item. |

Verified against a mock workspace: discovery, all validations, and the missing-artifact guidance path all behave correctly.

---

## 5. The BIF, annotated

```
the_ROM_image:
{
    [bootloader, destination_cpu=a53-0] zynqmp_fsbl.elf
    [pmufw_image]                       zynqmp_pmufw.elf
    [destination_device=pl]             H2C_pcie_control.bit
    [destination_cpu=a53-0, exception_level=el-3] app_component.elf
}
```

Two lines earn special attention:

- **`[fsbl_config] a53_x64` — REMOVED.** Older guides (and bootgen up to ~2023.x) required it. **bootgen 2025.2 rejects it:** *"[fsbl_config] a53_x64 … is no more supported. Use 'destination_cpu' attribute for bootloader partition."* `destination_cpu=a53-0` now selects the 64-bit A53 on its own. If you copied this line from an older tutorial, delete it.
- **`exception_level=el-3`** — matches the default standalone BSP. If your BSP is configured for EL1 or EL2, change this to match, or the app faults on its first privileged access.

---

## 6. Deploying

### SD card
Copy `BOOT.BIN` to the **root of a FAT32 partition**, on its own. Set the boot mode pins to SD and power-cycle.

### QSPI
```bash
program_flash -f BOOT.BIN -offset 0 -flash_type qspi-x8-dual_parallel \
              -fsbl <path>/zynqmp_fsbl.elf -cable type xilinx_tcf
```
Confirm `-flash_type` against your carrier's flash configuration — `qspi-x8-dual_parallel` is common on RFSoC carriers but is **not** universal, and a wrong value writes a valid-looking image the BootROM cannot read.

### Console
**PS UART0, 115200 8N1.** Your BSP sets `stdin`/`stdout` to `psu_uart_0`, not the PL UARTLites.

---

## 7. What a good boot looks like

Expect **20–25 seconds** before the menu appears. That is the existing design, not a fault:

| Stage | Time |
|---|---|
| FSBL + bitstream load (ZU47DR bitstream is large) | ~2–5 s |
| LMK/LMX SPI programming (~130 registers × 10 ms) | ~1.3 s |
| 8 × `usleep(200000)` tile startup | ~1.6 s |
| `sleep(15)` TSCB convergence wait | 15 s |
| MTS + NCO | < 1 s |

You should see `All enabled tiles reached state 0xF after N ms.` from the `rfdcWaitAllTilesReady()` added earlier. **If N is near 10000, the tiles are not calibrating** — and from flash, the first thing to suspect is a missing or stale PMUFW partition.

---

## 8. Troubleshooting

| Symptom | Likely cause |
|---|---|
| No UART output at all | Wrong boot mode pins; FSBL not AArch64; missing `[fsbl_config] a53_x64`; wrong UART on the carrier |
| FSBL banner, then silence | App linked into OCM; exception level mismatch; ATF wrongly included |
| App runs, RFDC init faults | Bitstream missing or ordered **after** the app |
| Works under JTAG, not from flash | Missing PMUFW; or the app relies on state JTAG left behind |
| Tile timeout only from flash | PMUFW missing/stale — see §1.1 |
| `bootgen: command not found` | `settings64.sh` not sourced |
| Boots an old image after rebuild | `-w on` omitted, or SD not re-mounted; check the BOOT.BIN timestamp |

---

## 9. Note on `psu_init.c`

`psu_init.c` was removed from `src/` during the 2025.2 port. That remains correct here: **the FSBL performs PS initialisation**. If the application also ran `psu_init()` it would re-initialise DDR and the clocks underneath itself after the FSBL had already done so. PS init belongs to the FSBL in a BOOT.BIN flow.

---

## 10. If PMU firmware is missing (only `qemu/` copies found)

This is the most common failure, and it is worth stating plainly: **a `pmufw.elf`
under any `qemu/` directory is a QEMU emulation binary.** It is shipped for
emulation, is not built against your XSA, and must not go into a hardware boot
image. The same applies to anything under `resources/` — those are platform
staging copies.

The diagnostic is simple:

```
<platform>/export/<platform>/sw/boot/
    fsbl.elf     <- present
    pmufw.elf    <- ABSENT
```

If `sw/boot/` holds `fsbl.elf` but no `pmufw.elf`, the FSBL domain was generated
and the PMU firmware domain was not.

### Fix — preferred, no platform rebuild

Build PMU firmware as its own component:

> **File → New Component → Application**
> Platform `platform_ZU47`, Processor **`psu_pmu_0`**, Template **"ZynqMP PMU Firmware"**

Build it, then point the script at the result:

```bash
PMUFW=<workspace>/zynqmp_pmufw/build/pmufw.elf ./make_boot.sh <workspace>
```

### Fix — thorough

Recreate the platform component with **"Generate boot artifacts"** ticked and
rebuild. That produces both `zynqmp_fsbl/build/fsbl.elf` and
`zynqmp_pmufw/build/pmufw.elf`, and mirrors both into `export/.../sw/boot/`.
The option cannot be added to an existing platform component.

### Escape hatch

`./make_boot.sh <ws> --allow-qemu-pmufw` will proceed. Use it only to prove out
the rest of the flow, never for a shipped image — see §1.1 for why an RFSoC in
particular depends on correct PMU firmware.

### Note on artifact selection

`make_boot.sh` v2 ranks candidates by **path priority**, not modification time:
`export/*/sw/boot/` first, then `*/build/`, then other `export/` paths. Picking
"the newest" was wrong — a stale exported artifact and a fresh staging copy are
routinely out of order.

---

## 11. Producing a PMU firmware — three routes

### Route A — build a PMUFW component (cleanest, if `psu_pmu_0` is offered)

> **File → New Component → Application**
> Platform `platform_ZU47` · Processor **`psu_pmu_0`** · Template **"ZynqMP PMU Firmware"**

Build, then:

```bash
PMUFW=<ws>/zynqmp_pmufw/build/pmufw.elf ./make_boot.sh <ws>
```

**Caveat:** under the SDT flow the processor list is limited to the domains the
platform actually exported. If `platform_ZU47` only exported
`standalone_psu_cortexa53_0`, **`psu_pmu_0` will not appear** and this route is
closed. Use Route B.

### Route B — a second, throwaway platform purely to harvest boot artifacts

This is usually the pragmatic answer, because it **does not touch the working
platform** your application already builds against.

1. **File → New Component → Platform**, name it `platform_ZU47_boot`.
2. Same XSA as `platform_ZU47`. OS `standalone`, processor `psu_cortexa53_0`.
3. **Tick "Generate boot artifacts."**
4. Build it.
5. Harvest:

```bash
ls <ws>/platform_ZU47_boot/export/platform_ZU47_boot/sw/boot/
    fsbl.elf   pmufw.elf

PMUFW=<ws>/platform_ZU47_boot/export/platform_ZU47_boot/sw/boot/pmufw.elf \
    ./make_boot.sh <ws>
```

Your application component keeps pointing at the original platform. The second
platform exists only to produce boot binaries and can be deleted afterwards, so
long as you keep the two `.elf` files.

### Route C — use the `qemu/` pmufw, knowingly

**Correcting an earlier overstatement.** The `pmufw.elf` under `sw/qemu/` is in
most cases a *genuine* PMU firmware build that Vitis produced during the
platform build and filed under `qemu/` because that is where the emulation flow
consumes it. PMU firmware is also largely **design-independent** — it manages PS
power islands, clocks and resets, and does not consume `psu_init` data the way
the FSBL does. So it will very often work on hardware.

What I can't do is *verify* which build it came from, which is why the script
refuses by default rather than quietly packing an unidentified binary into a
boot image. Inspect it first:

```bash
P=<platform>/export/<platform>/sw/qemu/pmufw.elf
readelf -h "$P" | egrep 'Class|Machine|Entry'
ls -l "$P"
```

| Field | Expected for a real PMUFW |
|---|---|
| `Class` | `ELF32` |
| `Machine` | `Xilinx MicroBlaze` |
| `Entry point` | in PMU RAM, around `0xffd…` |
| Size | roughly 100–140 KB (PMU RAM is 128 KB) |

If all four hold, it is a real PMU firmware and this is a reasonable way to get
booting today:

```bash
./make_boot.sh <ws> --allow-qemu-pmufw
```

Prefer Route A or B for anything you ship, so the artifact's provenance is
recorded rather than inferred.

---

## 12. Corrections and gotchas found on real hardware

### 12.1 `[fsbl_config] a53_x64` is deprecated in bootgen 2025.2
Earlier revisions of this guide said the line was essential. It was, through
roughly 2023.x. bootgen 2025.2 emits:

```
[WARNING]: [fsbl_config] a53_x64 | a53_x32 | r5_single | r5_dual is no more
supported. Use 'destination_cpu' attribute for bootloader partition
```

`destination_cpu=a53-0` already implies the 64-bit A53. Removed from both
`boot.bif` and the BIF that `make_boot.sh` generates.

### 12.2 bootgen resolves relative paths against the CWD, not the .bif
This produces the confusing pair:

```
$ cd <workspace>       ; bootgen -image boot.bif ...
[ERROR] : Cannot read file - boot.bif           # bif isn't here

$ cd <workspace>/boot  ; bootgen -image boot.bif ...
[ERROR] : Cannot read file - zynqmp_pmufw.elf   # bif found, contents aren't
```

The shipped `boot.bif` now uses **absolute paths**, so it works from any
directory. `make_boot.sh` has always emitted absolute paths.

### 12.3 `--allow-qemu-pmufw` was dead code (fixed)
The flag was checked *after* candidate selection had already failed, so it could
never take effect. `pick()` now receives the flag and skips the exclusion when
it is set. Verified both ways: refuses without the flag, proceeds with it.

### 12.4 Shell placeholders
`<ws>` in this document is a placeholder, not literal shell syntax. Bash reads
`<` as a redirect:

```
$ ./make_boot.sh <ws> --allow-qemu-pmufw
bash: ws: No such file or directory
```

Substitute the real path, or omit it entirely — `make_boot.sh` defaults to the
parent of its own directory, so from `boot/` you can just run:

```bash
./make_boot.sh --allow-qemu-pmufw
```

### 12.5 A verified-good `qemu/` PMU firmware
Inspection of `export/platform_ZU47/sw/qemu/pmufw.elf` on this project gave:

| Field | Value | Verdict |
|---|---|---|
| `Class` | `ELF32` | correct |
| `Machine` | `Xilinx MicroBlaze` | **genuine PMU firmware, not a stub** |
| `Entry point` | `0xffdcfab4` | inside PMU RAM (`0xffdc0000`–`0xffddffff`) |
| Size | 149,700 B (146 KB) | plausible |

All four checks pass, so this file is a real PMU firmware build and is
acceptable for bringing the board up. For a shipped image, still prefer Route A
or B from §11 so the artifact's provenance is recorded rather than inferred.

---

## 13. Silent boot + "Could not find ARM device" — diagnosis

### 13.1 The two symptoms are one fault

On ZynqMP the Debug Access Port is **disabled out of reset**. In a non-JTAG boot
mode the **BootROM** enables it, and only once it has successfully parsed the
boot header. So:

```
BootROM cannot use the image
    -> DAP never enabled      -> "Could not find ARM device on the board"
    -> no handoff to FSBL     -> no FSBL banner
    -> app never reached      -> no prints at all
```

This **rules out** the usual suspects: UART port/baud, exception level,
partition ordering, and `FSBL_DEBUG_INFO`. None of that code ever executes. The
fault is earlier — the BootROM is not accepting what is on the boot device.

### 13.2 Verified from this project's `psu_init.html`

| Peripheral | MIO | Consequence |
|---|---|---|
| **UART 0** | 6, 7 | Real MIO pins. FSBL prints do **not** depend on the bitstream. |
| **QSPI** | 0–5 only | Six pins = clk, ss, io0–3 → **single QSPI, x4**. Not dual-parallel. |
| **SD 0** | 13–23 | 11 pins, includes CD/WP |
| **SD 1** | 45–51 | 7 pins |
| NAND / eMMC | — | not present |

### 13.3 Correction to §6 — QSPI flash type

Earlier guidance in this document said:

```
program_flash ... -flash_type qspi-x8-dual_parallel      # WRONG for this design
```

With QSPI on **MIO 0–5 only**, this design is single x4:

```bash
program_flash -f BOOT.BIN -offset 0 -flash_type qspi-x4-single \
              -fsbl <platform>/export/<platform>/sw/boot/fsbl.elf \
              -cable type xilinx_tcf
```

Programming with the wrong geometry writes an image the BootROM cannot parse —
which produces exactly this symptom: no DAP, no FSBL, no prints.

### 13.4 Two SD controllers

This design exposes **both SD0 and SD1**. The boot mode pins select which one.
A card in the SD0 slot with the mode pins set for SD1 (or the reverse) gives
total silence. Confirm which physical slot the carrier wires to which
controller, then match the mode pins:

| Mode `[3:0]` | Device |
|---|---|
| `0x0` | JTAG |
| `0x1` | QSPI-24 |
| `0x2` | QSPI-32 |
| `0x3` | SD0 |
| `0x5` | SD1 |
| `0xE` | SD1 with level shifter |

### 13.5 Bisect procedure

**Step 1 — set the mode pins to JTAG (`0x0`) and power-cycle.**
In JTAG boot mode the DAP is enabled unconditionally, so the connect error
cannot occur. If JTAG *still* fails here, the fault is cable/board/power, not
your image.

**Step 2 — run `boot/jtag_boot_test.tcl`.**
It loads PMUFW → FSBL → bitstream → app over JTAG, in BOOT.BIN order, with the
boot device removed from the picture:

```bash
xsdb boot/jtag_boot_test.tcl
```

| Outcome | Meaning |
|---|---|
| Runs, UART output appears | **Artifacts are good.** Fault is the boot device or how BOOT.BIN was written (SD slot, or QSPI `-flash_type`). |
| Runs, no UART output | Artifacts fine, console path wrong. Find which `/dev/ttyUSB*` is PS UART0. |
| FSBL times out at `XFsbl_Exit` | FSBL itself is failing — rebuild with `FSBL_DEBUG_INFO`. |

**Step 3 — only once step 2 passes**, go back to the boot device with the
corrected `-flash_type` or SD slot.

### 13.6 `boot_probe.tcl`

`boot/boot_probe.tcl` reads the latched boot mode (`CRL_APB.BOOT_MODE_USER` at
`0xFF5E0200`), A53 state, and the PL DONE bit. Note it **requires a working
JTAG connection**, so it is only useful after step 1 — it cannot diagnose the
DAP failure itself.
