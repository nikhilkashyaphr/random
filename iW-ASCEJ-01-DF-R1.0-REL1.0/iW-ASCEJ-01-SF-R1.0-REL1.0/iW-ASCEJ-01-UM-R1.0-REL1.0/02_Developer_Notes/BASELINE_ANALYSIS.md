# ZU47DR RFSoC Bare-Metal Application — Baseline Codebase Analysis

**Project:** `zu47dr_app` on platform `zu47dr_plat`
**Hardware handoff:** `H2C_pcie_control.xsa`
**Toolchain:** Vitis 2022.2, `standalone` v8.0 BSP, `psu_cortexa53_0`, AArch64
**Status:** Analysis only — **no code has been modified.**

---

## 0. Scope of what was actually reviewed

| Source | What it gave us |
|---|---|
| `/mnt/project/*.c`, `*.h` (17 modules, ~5,900 live LOC excl. `psu_init.c`) | Application + helper layer |
| `zu47dr_plat/export/.../bspinclude/include/xparameters.h` | Authoritative IP inventory, base addresses, device IDs, RFDC tile config |
| `.../standalone_domain/system.mss` | Driver versions, stdin/stdout routing |
| `.../bspinclude/include/xrfdc.h`, `xrfdc_hw.h` | RFDC API contracts used to validate call sites |
| `platform.spr`, `export.json` | Platform/domain/system topology |
| `hw/psu_init.c` (23k lines, generated) | PS init — reviewed as generated, not audited line-by-line |

Everything marked **[VERIFIED]** below was read out of source or the BSP.
Everything marked **[ASSUMPTION]** needs confirmation against the Vivado block design.

---

## 1. What this application actually is

This is a **control-plane-only** application. It is **not** a data-mover.

- **[VERIFIED]** There is **no AXI DMA, AXI MCDMA, or XDMA IP** in the PL design. `grep` for `axi_dma|xdma|mcdma` in `xparameters.h` returns zero hits.
- **[VERIFIED]** The RFDC has **no interrupt connected to the GIC**. The only PL fabric interrupts present are the three AXI Quad SPI cores (121/122/123) and two AXI UARTLite cores (136/137) — **none of which the application uses.**
- **[VERIFIED]** The only interrupt the application arms is a **PS GPIO (`psu_gpio_0`) bank interrupt**, used as an LMK lock indicator. Its handler does nothing but increment a counter that is never read.

The high-throughput ADC→host and host→DAC paths live **entirely in PL logic** (AXIS switches + the `M00_PCIE_CONTROL` aperture at `0xA00A0000`). The A53 firmware exists to **program the clock tree, bring up the converters, align them, and expose a serial menu** for runtime reconfiguration.

Consequence for future work: any request phrased as "improve DMA throughput" or "add an interrupt-driven acquisition path" is a **new subsystem**, not a modification of existing code.

---

## 2. Hardware inventory (from the BSP — authoritative)

### 2.1 PL peripherals and apertures

| Instance | Base | Device ID | Driver | Used by app? |
|---|---|---|---|---|
| `USP_RF_DATA_CONVERTER_0` | `0xA0280000` | `XPAR_XRFDC_0_DEVICE_ID` | `xrfdc` | Yes — core |
| `AXI_GPIO_DDS1DDS2` | `0xA0000000` | 1 | `gpio` | Yes — DDS reset (ch1) + phase increment (ch2) |
| `SYSREF_CONTROLL` | `0xA0010000` | 3 | `gpio` | Yes — SYSREF gate (ch1) + pulse (ch2) |
| `AXI_GPIO_DYNAMIC_CLK` | `0xA0020000` | 2 | `gpio` | Yes — ADC/DAC stream clock counters (read-only) |
| `CLK_WIZ_ADC` | `0xA0030000` | 0 | raw `Xil_Out32` | Yes — **not** via `xclk_wiz` driver |
| `CLK_WIZ_DAC` | `0xA0040000` | 1 | raw `Xil_Out32` | Yes — **not** via `xclk_wiz` driver |
| `ADC_CHANNEL_SWITCH_ADC_SWITCH` | `0xA0050000` | 0 | `xaxis_switch` | Yes — 8→4 ADC routing |
| `ADC_CHANNEL_SELECT` | `0xA0060000` | 4 | `gpio` | Yes — PL-side channel select |
| `CLK_WIZ_2` | `0xA0070000` | 2 | — | **No** |
| `CLOCK_COUNTER` | `0xA0080000` | 5 | `gpio` | Yes — raw PL clock counter |
| `DDS_H2C_SWITCH` | `0xA0090000` | 1 | `xaxis_switch` | Yes — 2→1 DDS vs. host-stream mux |
| `M00_PCIE_CONTROL` | `0xA00A0000` | — | — | **No** — aperture exists, firmware never touches it |
| `QUAD_SPI_LMK` | `0xA0230000` | 2 | `xspi` | Yes |
| `QUAD_SPI_ADC_LMX` | `0xA02F0000` | 0 | `xspi` | Yes |
| `QUAD_SPI_DAC_LMX` | `0xA0300000` | 1 | `xspi` | Yes |
| `PL_ZL` | `0xA0320000` | 0 | `gpio` | **No** |
| `UART`, `UART1` (uartlite) | — | — | `uartlite` | **No** |

### 2.2 RFDC configuration — this is the single most important table

**[VERIFIED]** `XPAR_XRFDC_0_IP_Type = 2` → **Gen 3** silicon. `Silicon_Revision = 1`.

| | ADC | DAC |
|---|---|---|
| Tiles enabled | 0, 1, 2, 3 (all) | 0, 1, 2, 3 (all) |
| Slices enabled per tile | **0,1,2,3 → 16 blocks total** | **0 and 2 only → 8 blocks total** |
| Sampling rate (design) | 4.8 GSPS | 9.6 GSPS |
| Decim/Interp mode | 24× | 24× |
| Data width | **1 sample/clock** | **2 samples/clock** |
| PLL master tile | **Tile 1** (`PLL_Enable=1`, RefClk 300 MHz, FBDIV 32, OutDiv 2) | **Tile 2** (`PLL_Enable=1`, RefClk 300 MHz, FBDIV 32, OutDiv 1) |
| Slave tiles | RefClk 4800 MHz, OutDiv 1 (distributed) | RefClk 9600 MHz, OutDiv 1 (distributed) |
| Fabric (stream) freq | 200.000 MHz | 200.000 MHz |
| Fs max | 5.0 GSPS | 10.0 GSPS |
| Mixer type | 2 (fine/DDC) | — |

This validates two things in the code:

1. `master_tile = is_adc ? 1 : 2` in `Change_Sampling_Rate_All_Tiles()` is **correct** and matches the hardware PLL ownership.
2. The stream-clock formula in `Update_Stream_Clock_Architectural()` is **arithmetically consistent** with the design: ADC 4800/24 = 200 MHz; DAC 9600/(24×2) = 200 MHz — both equal `XPAR_XRFDC_0_*_Fabric_Freq = 200.000`.

### 2.3 Memory and console

- **[VERIFIED]** DDR0: `0x0000_0000`–`0x7FFF_FFFF` (2 GB). DDR1: `0x8_0000_0000`–`0x9_7FFF_FFFF` (6 GB).
- **[VERIFIED]** `system.mss`: `stdin = stdout = psu_uart_0`. The interactive menu runs on **PS UART0**, not JTAG-DCC and not the PL uartlites.
- **[VERIFIED]** BSP libs linked: `libxil.a`, `libmetal.a`. No `xilffs`/`xilsecure` in the standalone domain (those are FSBL/PMUFW only).

---

## 3. Module map

### 3.1 Live path (executes at runtime)

| File | Role |
|---|---|
| `main.c` (lines **2260–3541**) | **Entire live application.** Init sequence, clock math, SPI PLL programming, MTS, NCO, menu loop |
| `rfdc_cmd.c` | Provides the four functions `main()` actually calls: `rfdcStartup`, `dacResetAll`, `adcResetAll`, `rfdcReady`. Also owns the global `ADC_Sync_Config` / `DAC_Sync_Config` |
| `ig60m_rfdc_clk.h` | Register images for LMK04828 and two LMX2594 (data tables only) |
| `main.h` | `RFdcInst` extern, `RFDC_BASE`, `NUM_TILES`/`NUM_BLOCKS` |

### 3.2 Compiled but **dead** (linked, never invoked)

**[VERIFIED]** `main()` never calls `cli_init()`, `cli_addCmds()`, or `cmdParseAndRun()`. Every `cli_*_init()` registration function is therefore unreachable, and with it every command in those tables.

Dead modules: `cli.c`, `cmd_func_mem.c`, `dac_waves.c`, `rfdc_mts.c`, `rfdc_nyquistzone.c`, `rfdc_dsa_vop.c`, `rfdc_interpolation_decimation.c`, `rfdc_poweron_status.c`, `adc_FreezeCal.c`, `adc_LinkCoupling.c`, `adcSaveCalCoefficients.c`, `adcGetCalCoefficients.c`, `adcLoadCalCoefficients.c`, `adcDisableCoeffOvrd.c`, plus the unused half of `rfdc_cmd.c`.

**Important:** this is dead code, not dead *value*. `adc_FreezeCal.c` contains the **correct** `XRFdc_SetCalFreeze` usage pattern that `main.c` gets wrong (see finding H-1). Treat this layer as a reference library, not as deletable.

### 3.3 Dead source inside `main.c`

**[VERIFIED]** Lines **1–2259** of `main.c` are an entirely commented-out earlier revision of the same file (~64% of the file). It contains a near-duplicate of the live code minus `DDS_H2C_SWITCH` support. Any `grep` or IDE symbol search on this file will produce two hits for almost every symbol. This is a live navigation hazard for future work.

---

## 4. Execution trace: reset → steady state

```
main()
├─ 1. QSPI clock tree            QSpiInit()  → 3× XSpi_CfgInitialize, MASTER|MANUAL_SSELECT
│                                QSpiLMK4828Program()   ~10 ms per register, blocking
│                                QSpiLMX2594Program()   ADC + DAC LMX2594
│                                QSpiLMX2594Status()    reads lock bits, prints
│
├─ 2. PS GPIO + GIC              XGpioPs_CfgInitialize
│                                XGpioPs_SetDirectionPin(&Gpio, 0x88, 0)      ← see M-3
│                                XScuGic_CfgInitialize / Connect(XPAR_XGPIOPS_0_INTR)
│                                XGpioPs_SetIntrType(BANK3, 1<<10, level, low)
│                                Xil_ExceptionEnableMask(IRQ)
│
├─ 3. libmetal                   metal_init()            ← must precede XRFdc_CfgInitialize ✓
│
├─ 4. PL peripheral init         Init_Dds_Gpio()         DDS held in reset (writes 0) ✓
│                                Init_Sysref_Gpio()
│                                Init_Adc_Select_Gpio()
│                                Init_Axis_Switch()      all MI ports disabled
│                                Init_Dds_H2c_Switch()   MI0 ← SI0 (host!) — 1.2.1: SI1 (DDS)
│                                Init_Dyn_Clk_Gpio() / Init_Clk_Counter_Gpio()
│
├─ 5. Open datapath              Set_PL_Gpio_Routing(3)
│                                Set_Adc_Channel_Routing(7, 3, 2, 1)
│
├─ 6. RFDC bring-up              XRFdc_LookupConfig / XRFdc_CfgInitialize
│                                rfdcStartup(NULL)   ← writes RFDC_BASE+0x4 = 1, then XRFdc_StartUp
│                                dacResetAll(NULL)   ← NULL deref, see M-1
│                                adcResetAll(NULL)   ← NULL deref, see M-1
│                                rfdcReady(NULL)     ← prints status; does NOT block, see H-2
│                                read IP version from RFDC_BASE+0x0
│
├─ 7. Signal level opt.          all ADC blocks: DSA attenuation → 0.0 dB
│                                all DAC blocks: XRFdc_SetDACVOP(..., 32000)  (32 mA)
│
├─ 8. Reset_Dds()                releases DDS from reset
│
├─ 9. sleep(15)                  TSCB background-cal convergence wait
│
├─ 10. Run_MTS_Procedure()       ADC then DAC MultiConverter_Init + _Sync, Tiles = 0xF
│
├─ 11. NCO init                  Set_NCO_Freq_MTS_Safe(ADC, 3100 MHz)
│                                Set_NCO_Freq_MTS_Safe(DAC, 3100 MHz)
│
├─ 12. Print_Current_Rates()
├─ 13. Reset_Dds() + Set_Dds_Frequency(10 MHz)
│
└─ 14. while(1) { scanf menu }   16 options, no exit path
```

**Total blocking boot time: ≈ 20–25 s** (15 s TSCB + 8× 200 ms tile startup + per-SPI-register 10 ms delays across ~130 LMK/LMX registers).

### 4.1 Menu → function map

| # | Action | Entry point |
|---|---|---|
| 1/2 | ADC decimation, all / single | `Update_Datapath_Rate(is_adc=1)` + MTS re-sync + NCO restore |
| 3/4 | DAC interpolation, all / single | `Update_Datapath_Rate(is_adc=0)` + MTS re-sync + NCO restore |
| 5 | Print rates | `Print_Current_Rates` |
| 6 | DDS frequency | `Set_Dds_Frequency` |
| 7/8 | ADC/DAC NCO | `Set_NCO_Freq_MTS_Safe` |
| 9 | AXIS switch routing | `Set_Adc_Channel_Routing` |
| 10 | PL GPIO routing | `Set_PL_Gpio_Routing` |
| 11 | Measure clocks | `Measure_Stream_Clocks` |
| 12 | Global QMC gain | inline |
| 13 | Sampling rate | `Save_Gain_State` → `Change_Sampling_Rate_All_Tiles` → `Restore_Gain_State` → verify → re-route → MTS → NCO |
| 14 | Nyquist zone | `Set_Nyquist_Zone_Manual` (nested fn) |
| 15 | ADC DSA | inline `XRFdc_GetDSA`/`SetDSA` |
| 16 | DAC source mux | `Set_Dds_H2c_Routing` |

---

## 5. Dependency register

### 5.1 Hard dependencies that will break on XSA regeneration

| Symbol | Where | Risk |
|---|---|---|
| `XPAR_ADC_CHANNEL_SWITCH_ADC_SWITCH_DEVICE_ID` | `main.c` | Device ID renumbering |
| `XPAR_DDS_H2C_SWITCH_DEVICE_ID` | `main.c` | Device ID renumbering |
| `XPAR_ADC_CHANNEL_SELECT_DEVICE_ID`, `XPAR_AXI_GPIO_DYNAMIC_CLK_DEVICE_ID`, `XPAR_CLOCK_COUNTER_DEVICE_ID`, `XPAR_AXI_GPIO_DDS1DDS2_DEVICE_ID` | `main.c` | Device ID renumbering |
| `XPAR_QUAD_SPI_{LMK,ADC_LMX,DAC_LMX}_DEVICE_ID` | `main.c` | " |
| `XPAR_CLK_WIZ_{ADC,DAC}_BASEADDR` | `main.c` | Address remap |
| **`XPAR_SYSREF_CONTROLL_BASEADDR` / `_DEVICE_ID`** | **redefined locally in `main.c`** | Currently identical to BSP values, so benign — but this is a **shadow copy of hardware config in application source**. Flag for cleanup. |

### 5.2 Behavioural dependencies on the block design **[ASSUMPTION — verify in Vivado]**

1. **Clocking Wizard dynamic reconfiguration is enabled** on both `CLK_WIZ_ADC` and `CLK_WIZ_DAC`, with the AXI4-Lite (not DRP) interface, and **VCO statically locked at 1200 MHz** (100 MHz × 12). `Program_Clock_Wizard()` writes only the output divider at offset `0x208` and asserts LOAD|SEN at `0x25C`. If the block design uses a different VCO or has dynamic reconfig disabled, this function silently produces wrong clocks.
2. **DDS Compiler phase accumulator is exactly 32 bits** and phase increment is driven from `AXI_GPIO_DDS1DDS2` channel 2. `Set_Dds_Frequency` computes `f × 2^32 / stream_clk` with no width parameter.
3. **`AXI_GPIO_DYNAMIC_CLK` ch1/ch2 and `CLOCK_COUNTER` ch1 are free-running clock-cycle counters** in the ADC-stream, DAC-stream, and raw-PL domains respectively. `Measure_Stream_Clocks()` reads a delta over 100 ms and multiplies by 10.
4. **`SYSREF_CONTROLL` ch1 = gate (1 = gated), ch2 = one-shot pulse request.** Encoding is not documented anywhere in source.
5. **`ADC_CHANNEL_SELECT` ch1 accepts 1/2/3** meaning "1 channel / 2 channels / 4 channels" per the menu text.
6. **ADC AXIS switch has 8 slave and 4 master ports.**

### 5.3 Toolchain / build dependencies

- **`-lm` is required** — `dac_waves.c` uses `pow()`, `sin()`. Even though that module is dead code, it still compiles and links.
- **Full (non-nano) `printf` with float support is required** — `Print_Current_Rates()` and `dac_waves.c` call `printf("%f")`. The `PRINT_DOUBLE` macro exists precisely to avoid this elsewhere, which suggests float printf was historically unreliable in this project. Inconsistently applied.
- **Custom `lscript.ld` is required** if `adcSaveCalCoefficients.c` is ever activated — see finding M-5.
- **GCC nested-function support** is required — `main()` defines three nested functions (`Save_Gain_State`, `Restore_Gain_State`, `Set_Nyquist_Zone_Manual`). This is a GNU C extension that emits **stack trampolines** and requires an executable stack. Not portable to Clang/armclang.

---

## 6. Risk register

Ordered by severity. **Nothing here has been changed.** These are for deliberate triage.

### HIGH

**H-1 — `XRFdc_SetCalFreeze` is called with the wrong struct field, and two uninitialised fields.**
`main.c`, `Set_NCO_Freq_MTS_Safe()`:
```c
XRFdc_Cal_Freeze_Settings CalFreeze;
CalFreeze.CalFrozen = 1;              // ← status field, not the setter
XRFdc_SetCalFreeze(RFdcInstPtr, t, b, &CalFreeze);
```
**[VERIFIED against `xrfdc.h:535`]** The struct is `{ CalFrozen (status), DisableFreezePin, FreezeCalibration (setter) }`. The setter is `FreezeCalibration`. `CalFrozen` is what `XRFdc_GetCalFreeze` *returns*. So:
- TSCB is **not actually frozen** during the NCO update — the whole "MTS-safe" guarantee is not in effect.
- `DisableFreezePin` and `FreezeCalibration` are **uninitialised stack values** being written to a calibration control register on every enabled ADC block, twice per NCO change.

The correct pattern already exists in this codebase at `adc_FreezeCal.c:139-140` (`DisableFreezePin = 1; FreezeCalibration = 1;`). This is the highest-value fix in the project and is low-risk.

**H-2 — The FG-calibration wait is a comment, not code.**
`main.c` prints:
> `rfdcReady(NULL) already waited for State 0xF. The ADC calibrated in silence!`

**[VERIFIED]** `rfdcReady()` in `rfdc_cmd.c:876` does a single non-blocking read of `XRFDC_ADC_DEBUG_RST_OFFSET` and prints ready/not-ready. **It never loops and never blocks.** There is no wait for power-on sequence state `0xF` anywhere in the startup path. The only thing gating ADC foreground calibration is the fixed `usleep(200000)` per tile inside `rfdcStartup()`. The ENOB-optimisation reasoning documented in the comments does not match the implemented behaviour.

**H-3 — `rfdcStartup()` reads ADC registers when checking DAC tile readiness.**
`rfdc_cmd.c:1016`, DAC loops:
```c
val = XRFdc_ReadReg16(RFdcInstPtr, XRFDC_ADC_TILE_CTRL_STATS_ADDR(Tile_Id), ...);
```
**[VERIFIED against `xrfdc_hw.h:2222/2224]`**: DAC tiles live at `0x4000 + tile*0x4000`, ADC tiles at `0x14000 + tile*0x4000`. Every DAC readiness gate in `rfdcStartup()` is actually reading ADC tile *N*. `rfdcReady()` and `rfdcPoweronStatus()` use the correct macro, so the bug is isolated to `rfdcStartup()`.

**H-4 — Changing DAC sampling rate tears down ADC routing.**
`Change_Sampling_Rate_All_Tiles()` unconditionally calls `XAxisScr_MiPortDisableAll(&AxisSwitchInst)` — the **ADC** switch — regardless of `is_adc`. At the end it re-enables register update but **never re-enables the MI ports**. The menu-13 handler compensates with `Set_Adc_Channel_Routing(7,3,2,1)` — but only on the success path. On PLL failure the ADC datapath is left dead with no indication.

**H-5 — Stale NCO state feeds automatic Nyquist-zone selection.**
`main()` declares **local** `current_adc_nco` / `current_dac_nco` that **shadow** the file-scope globals of the same name. Menu options 7 and 8 update the *locals*. `Update_Nyquist_Zone()` — reached via `Change_Sampling_Rate_All_Tiles()` — reads the *globals*, which are frozen at their initialiser `3100.0` forever. After any NCO change, automatic zone selection computes from the wrong frequency.

### MEDIUM

**M-1 — NULL pointer dereference in `dacResetAll(NULL)` / `adcResetAll(NULL)`.**
Both start with `Tile_Id = cmdVals[0];` before overwriting `Tile_Id` in the `for` loop. `main()` passes `NULL`. On this platform address 0 is mapped DDR so it reads garbage rather than faulting, and the value is discarded — currently harmless, but it is a real UB read at every boot and will fault the moment an MPU/MMU config changes.

**M-2 — `xil_printf("%.3f")` produces garbage.**
`Set_NCO_Freq_MTS_Safe()` formats a `double` with `xil_printf`, which has **no floating-point support**. The rest of the file uses the `PRINT_DOUBLE` macro specifically to work around this. Cosmetic, but it undermines the operator's confidence in every subsequent log line.

**M-3 — GPIO pin number hex/decimal confusion.**
```c
XGpioPs_SetDirectionPin(&Gpio, 0x88, 0x0);          // pin 136 → EMIO 58, Bank 4
XGpioPs_SetIntrType(&Gpio, XGPIOPS_BANK3, (1<<10), ...);  // Bank3 bit 10 → pin 88, EMIO 10
XGpioPs_IntrEnable(&Gpio, XGPIOPS_BANK3, (1<<10));
```
Direction is configured on **pin 136**; the interrupt is enabled on **pin 88**. `0x88 == 136`. The two statements refer to different pins, and the strong reading is that `0x88` was meant to be decimal `88`. The LMK lock interrupt is very likely not wired up as intended.

**M-4 — Return values discarded throughout the init path.**
`XRFdc_CfgInitialize`, `XSpi_CfgInitialize`, `XGpio_Initialize`, `XScuGic_CfgInitialize`, `XScuGic_Connect`, `XAxisScr_CfgInitialize`, `XRFdc_MultiConverter_Sync` (in menu handlers) — all either assign to a `Status` that is never tested, or ignore the result entirely. `Init_Axis_Switch()` additionally dereferences `Config->BaseAddress` with no NULL check, unlike `Init_Dds_H2c_Switch()` which does check.

**M-5 — Calibration-coefficient section requires a linker script that may not match.**
`adcSaveCalCoefficients.c:82` places `storedCoeffSets[4][4][4]` in section `.coeffSection`. The header comment specifies `_COEFF_SIZE = 4 × 4 × 4 × **3** × (4 + 32) = 6912 bytes`, but the actual type `perAdcCoeffType` contains `singleCoeffType type[**4**]` → **9216 bytes**. If a linker script is written to the documented formula, the array **overruns its region by 2304 bytes**. If no `.coeff_mem` region exists at all, the section is placed as an orphan wherever the linker chooses. Currently dormant (module is dead code), but this is a latent memory-corruption bug waiting for the day someone enables coefficient save/restore.

**M-6 — 15-second unconditional boot delay.**
`sleep(15)` for TSCB convergence with no polling and no way to skip. Combined with per-SPI-register `usleep(10000)` across ~130 registers (≈1.3 s) and 8× `usleep(200000)` tile startups (1.6 s), boot is ~20–25 s with no progress indication beyond the banner.

**M-7 — Menu 16 labels contradict the code.**
Menu text: `0 = GNU Radio Stream, 1 = DDS Compiler`.
Confirmation printf: `src_port == 0 ? "DDS" : "GNU Radio"`.
`Init_Dds_H2c_Switch()` comment: `Route SI 0 (DDS)`.
Two of these three must be wrong. The operator cannot tell which source is selected from the console output.

> **Resolved in 1.2.1 — from the bitstream, not by majority vote.** The
> hardware handoff (`design_1.hwh`, instance `DDS_H2C_switch`) wires
> S00_AXIS to `H2C_data_in` (host) and S01_AXIS to `dds_compiler_2`. The menu
> text was right; the comment and the confirmation printf were wrong. The 1.0
> fix for this finding went the other way, relabelling the menu to match the
> comment, and so swapped DDS and host at every layer and booted the DAC on the
> idle host stream. The mapping now lives in `platform_ids.h`
> (`HW_DAC_SW_SI_*`) and is covered by `TS/01_Host_Test_Suite/firmware/run_tests.sh`. See
> `DAC_SOURCE_AND_HOST_TX.md`.

### LOW / maintainability

- **L-1** — 2,259 lines of commented-out duplicate code at the top of `main.c`. Every symbol search returns doubled hits.
- **L-2** — Three GCC nested functions inside `main()`. Non-standard, requires executable stack trampolines, and hides `Set_Nyquist_Zone_Manual` from any other module that might want it.
- **L-3** — `Update_Stream_Clock_Architectural()` opens with dead code: `u32 factor = 1; if (factor == 0) factor = 1;`
- **L-4** — `Program_Clock_Wizard()` masks the integer divider to 8 bits (`int_div & 0xFF`) with no check that the requested divider actually fits, and the inline comment describes the register as `[15:8] Fractional` when the clk_wiz fractional field is `[17:8]`.
- **L-5** — `intr_cnt` is `static int` (not `volatile`), written only from the ISR and read nowhere.
- **L-6** — MTS `RefTile` is passed a tile-**type** constant: `XRFdc_MultiConverter_Init(&cfg, 0, 0, XRFDC_ADC_TILE)` → RefTile = 0 for ADC, 1 for DAC. These happen to be legal tile indices, but the PLL master tiles are ADC **1** and DAC **2**. Whether RefTile should track the PLL master needs hardware validation.
- **L-7** — `main()` has no exit path; `doExit()` / `cmdExitVal` in `cli.c` are unreachable. `rfdcShutdown()` is never called — there is no orderly shutdown sequence at all.
- **L-8** — ADC DSA is forced to 0.0 dB (maximum gain) at boot for every block, with no headroom check. Combined with DAC VOP at 32 mA this maximises ENOB but also maximises clipping risk on an unknown input.

---

## 7. Baseline rules going forward

Recorded so we both work from the same contract:

1. **`main.c` lines 2260–3541 are the live application.** Lines 1–2259 are archived dead text and must not be edited, "fixed", or used as a reference for current behaviour.
2. **The `cli_*` layer is dormant but load-bearing as reference.** Do not delete it. If a future requirement needs runtime commands, reviving `cli_init()` + `cmdParseAndRun()` is a smaller change than extending the `scanf` menu — but that is a decision to make explicitly, not by drift.
3. **Hardware config comes from `xparameters.h`, not from application `#define`s.** New code uses `XPAR_*`. The existing `SYSREF_CONTROLL` shadow definitions stay until we decide to clean them deliberately.
4. **Do not renumber, reorder, or "tidy" the RFDC bring-up sequence.** Steps 1→14 in §4 encode silicon timing requirements. Changes there need a stated reason and a test on hardware.
5. **Every finding in §6 stays as-is until you pick one.** When you do, I will state the exact files and line ranges, the blast radius, the regression risk, and what to look for on the console to confirm the fix — before writing anything.
6. **Anything touching DMA, PCIe data movement, or interrupt-driven acquisition is new development**, because none of it exists today (§1).

---

## 8. Open questions for you

These change the shape of any future work and I would rather ask than assume:

1. **Which findings are already known?** H-1 (CalFreeze) and H-5 (NCO shadowing) in particular would explain real, observable symptoms — degraded ENOB after NCO changes, and wrong Nyquist zone after a sampling-rate change. If you have seen either on the bench, that is strong confirmation.
2. **Is the `M00_PCIE_CONTROL` aperture at `0xA00A0000` meant to be driven by this firmware**, or is it host-side only? This determines whether the A53 has any future role in the data path.
3. **Is there a custom `lscript.ld`** in the `zu47dr_app` project? It was not in the platform archive, and M-5 depends on it.
4. **Is the LMK lock interrupt (M-3) expected to be functional**, or is it vestigial?
5. **What is the actual VCO frequency configured in `CLK_WIZ_ADC` / `CLK_WIZ_DAC`?** The code hardcodes 1200 MHz.
