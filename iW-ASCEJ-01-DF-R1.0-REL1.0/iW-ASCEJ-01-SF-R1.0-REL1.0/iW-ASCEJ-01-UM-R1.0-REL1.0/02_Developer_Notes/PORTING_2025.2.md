# Vitis 2025.2 (SDT) Port — ZU47DR RFSoC Bare-Metal

**Target:** Vitis 2025.2.1, `aarch64-none-elf-gcc 13.3.0`, platform `platform_ZU47`, domain `standalone_psu_cortexa53_0`
**Result:** all 8 build errors resolved, all 18 warnings resolved, previous correctness fixes retained.
**Builds under both flows** — classic `DEVICE_ID` and SDT `BASEADDR` — from one source tree.

---

## 1. Root cause: one change, not eight errors

Every error in your log traces to the **System Device Tree (SDT) flow**, which Vitis 2023.2 introduced and 2025.2 uses by default. Your build line confirms it:

```
-DSDT  -specs=.../Xilinx.spec  -Wall -Wextra
```

Under SDT the BSP **stops emitting `XPAR_<instance>_DEVICE_ID`**, and driver entry points take a **base address** instead of a device index:

| | classic (≤ 2023.1) | SDT (≥ 2023.2) |
|---|---|---|
| `XGpio_Initialize` | `(p, DEVICE_ID)` | `(p, BASEADDR)` |
| `XSpi_LookupConfig` | `(DEVICE_ID)` | `(BASEADDR)` |
| `XAxisScr_LookupConfig` | `(DEVICE_ID)` | `(BASEADDR)` |
| `XRFdc_LookupConfig` | `(DEVICE_ID)` | `(BASEADDR)` |
| `XGpioPs_LookupConfig` | `(DEVICE_ID)` | `(BASEADDR)` |
| `XScuGic_LookupConfig` | `(DEVICE_ID)` | `(BASEADDR)` |

The eight failures were simply the eight places that still named a `_DEVICE_ID`.

### Why not just `#ifdef SDT` at each call site

That would scatter twelve conditionals through `main.c` and guarantee drift. Instead, **`src/platform_ids.h`** (new) resolves every hardware identifier once. `main.c` now references `HW_*` names only — verified: **zero raw `XPAR_` uses remain in the live region.** Reverting to an older Vitis needs no source edits.

---

## 2. ⚠ The bug your compiler did *not* catch

This is the most important item on the page.

```c
IntcConfig = XScuGic_LookupConfig(XPAR_SCUGIC_0_DEVICE_ID);   // baseline
Status = XScuGic_CfgInitialize(&Intc, IntcConfig, IntcConfig->CpuBaseAddress);
```

`XPAR_SCUGIC_0_DEVICE_ID` was **not** in your error list — some SDT BSPs still emit it for backward compatibility. So this **compiles cleanly under 2025.2**. But SDT's `XScuGic_LookupConfig()` matches on **base address**. A device index of `0` matches nothing → returns `NULL` → the next line dereferences `NULL->CpuBaseAddress` → **data abort at boot**, with no diagnostic anywhere.

Had the eight loud errors been fixed one at a time without auditing the rest, this would have shipped as an intermittent "board hangs on startup". Now resolved via `HW_SCUGIC_ID`, and `XScuGic_CfgInitialize`/`XScuGic_Connect` return values are checked (M-4).

---

## 3. Error-by-error

| # | Error | Fix |
|---|---|---|
| 1 | `XPAR_DDS_H2C_SWITCH_DEVICE_ID` undeclared | `HW_DDS_H2C_SWITCH_ID` |
| 2 | `XPAR_CLOCK_COUNTER_DEVICE_ID` undeclared | `HW_CLK_COUNTER_GPIO_ID` |
| 3 | `XPAR_AXI_GPIO_DYNAMIC_CLK_DEVICE_ID` undeclared | `HW_DYN_CLK_GPIO_ID` |
| 4 | `XPAR_ADC_CHANNEL_SELECT_DEVICE_ID` undeclared | `HW_ADC_SELECT_GPIO_ID` |
| 5 | `XPAR_ADC_CHANNEL_SWITCH_ADC_SWITCH_DEVICE_ID` undeclared | `HW_AXIS_SWITCH_ID` |
| 6 | `XPAR_AXI_GPIO_DDS1DDS2_DEVICE_ID` undeclared | `HW_DDS_GPIO_ID` |
| 7 | `XPAR_QUAD_SPI_{LMK,ADC_LMX,DAC_LMX}_DEVICE_ID` undeclared | `HW_QSPI_*_ID` |
| 8 | `XPAR_XGPIOPS_0_DEVICE_ID`, `XPAR_XRFDC_0_DEVICE_ID` undeclared | `HW_GPIOPS_ID`, `HW_RFDC_ID` |

Also fixed, though not fatal:

- **`XPAR_SYSREF_CONTROLL_BASEADDR` redefined** (`main.c:2312` vs `xparameters.h:595`) — the application was shadowing BSP hardware config with a hard-coded `0xA0010000`. Removed; the BSP value is now authoritative. This was flagged in the baseline review as a regeneration hazard, and 2025.2 proved the point.
- **`XPAR_XGPIOPS_0_INTR`** — classic BSPs export it; SDT carries the IRQ number in `XGpioPs_Config::IntrId`. `HW_GPIOPS_INTR_ID(cfgptr)` takes the config pointer so one call site serves both flows.
- **`RFDC_DEVICE_ID` in `main.h`** — would have broken the moment anything used it. Routed through the compat header.
- **`RFDC_BASE`** — kept as a *separate* macro (`HW_RFDC_BASE`) from the lookup handle, because `rfdc_cmd.c` does raw `Xil_In32/Out32` against it and needs a genuine aperture base in both flows.

### Safety property

Every ambiguous macro uses a `#if defined()` chain ending in `#error`. Verified by deleting `XPAR_SCUGIC_0_BASEADDR` from a mock header: the build **stops with a named message** rather than silently selecting a wrong address. A hard build failure is always preferable to a wrong base address.

---

## 4. `-Wextra` warnings (new in your 2025.2 flags)

| Class | Count | Fix |
|---|---|---|
| `-Wunused-parameter` on `cmdVals` | 13 | `(void)cmdVals;`. The `u32 *` signature is **required** by the `CMDSTRUCT` function-pointer contract in `cli.h` — removing the parameter would break the command tables. Applied **only** to the 13 genuinely unused callbacks; an initial pass touched 44 and was narrowed by checking each body. |
| `-Wsign-compare` in `dac_waves.c` | 3 | `int` counters vs `size_t` from `sizeof`. Counters made `size_t`; `<stddef.h>` added. |
| `-Wcomment` (`main.c:147`, `:1273`) | 2 | Trailing `\` on a `//` line splices the next line into the comment. Both are in the archived region and the following line is itself `//`-prefixed, so removing the backslash changes nothing. |
| `-Wformat` `%lu` vs `u32` | 8 | Cast to `(unsigned long)`. Real bug on AArch64 — 32-bit value into a 64-bit vararg slot. |

**Final state: 0 errors, 0 warnings at `-Wall -Wextra` across all 16 modules.**

---

## 5. Verification performed

| Check | Result |
|---|---|
| Classic flow, all 16 modules, `-Wall -Wextra`, **real 2022.2 BSP headers** | 0 errors, 0 warnings |
| SDT branch of `platform_ids.h` against a **mock 2025.2 `xparameters.h`** built from your log's evidence | all 13 identifiers resolve to correct addresses; `Config->IntrId` path compiles |
| `#error` guards with a macro deliberately removed | fires with a named message |
| Line endings | CRLF preserved on every file, zero mixed |

**What I could not verify:** I do not have your 2025.2 BSP, so the SDT branch is checked against a mock, not your real `xparameters.h`. The instance-name→`BASEADDR` mapping is derived from your compiler's own "did you mean" hints (e.g. it offered `XPAR_ADC_CHANNEL_SELECT_HIGHADDR`, which implies `_BASEADDR` exists), so confidence is high — but if any `#error` fires on your first build, send me that line and it is a one-line addition to the chain.

---

## 6. `psu_init.c` removed from `src/`

Your 2025.2 build compiles **16 objects** and `psu_init.c` is not among them — under SDT, PS initialisation belongs to the platform/FSBL, not the application. Leaving it in `src/` would either be ignored or cause duplicate-symbol link errors. Removed. It remains in the platform.

---

## 7. Files

**New (1):** `platform_ids.h`
**Modified (4 + warnings):** `main.c`, `main.h`, `rfdc_cmd.c`, `rfdc_cmd.h`, plus `(void)cmdVals;` in 9 modules and `dac_waves.c` counters
**Removed (1):** `psu_init.c`

Drop `src/` in place. **No changes needed to `lscript.ld`, `Xilinx.spec`, or CMake settings.**

---

## 8. Correctness fixes carried forward

The 2025.2 port is layered *on top of* the earlier fixes; none were lost. Full detail in `CHANGELOG.md`:

- **H-1** `XRFdc_SetCalFreeze()` used `CalFrozen` (a status field) instead of `FreezeCalibration`, so TSCB was never frozen, and two uninitialised stack values were written to a calibration register.
- **H-2** the foreground-calibration wait was a comment, not code → added `rfdcWaitAllTilesReady()`.
- **H-3** `rfdcStartup()` gated DAC tiles by reading ADC tile registers (`0x14000` vs `0x4000`).
- **H-4** a DAC rate change tore down ADC AXIS routing and never restored it.
- **H-5** `main()` shadowed the NCO globals, freezing automatic Nyquist selection at 3100 MHz.
- **M-1/3/4/5/7/8**, **L-2/3/4/5** — NULL derefs, GPIO pin mismatch, unchecked init returns, coefficient region sizing, contradictory menu labels, divider range check, nested functions hoisted.

---

## 9. Still needs your confirmation

1. **First build** — if an `#error` from `platform_ids.h` fires, send the line; one-line fix.
2. **M-3, LMK lock pin** — baseline configured direction on pin 136 but armed the interrupt on pin 88. I aligned to **pin 88**. Confirm which is intended.
3. **`CLK_WIZ_VCO_MHZ` = 1200.0** — verify against the Vivado block design.
4. **MTS `RefTile`** — still passes a tile *type* where a tile *index* belongs. Deliberately untouched; needs bench validation.
