# iW-ASCEJ-01-FF-R1.0-REL1.0 — Firmware Files

Bare-metal firmware for the Cortex-A53 (A53-0, EL3, standalone) of the
ZU47DR RFSoC. Firmware PCIe ABI **4**, capabilities `0x7F`.

| Path | Contents |
|---|---|
| `01_Source/` | **37 files. Flash them as one set.** `main.c` (init, clocking, UART menu, DAC source switch), `pcie_cfg.c` / `pcie_regs.h` (PCIe mailbox, ABI 4), `platform_ids.h` (SDT/classic hardware IDs), `rfdc_*.c` (NCO, decimation/interpolation, Nyquist, DSA/VOP, MTS, power-on status), `adc*.c` (calibration coefficients, freeze, coupling), `cli.c`, `cmd_func_mem.c`, `dac_waves.c`, `ig60m_rfdc_clk.h` |
| `02_Boot/` | `boot.bif` (FSBL → PMUFW → bitstream → application), `make_boot.sh` (auto-discovers the artifacts, `--list`), `boot_probe.tcl`, `jtag_boot_test.tcl`, `lscript_coeff_fragment.ld` |
| `03_Documentation/` | BOOT.BIN on Vitis 2025.2, flashing and unblocking, porting to 2025.2, Vitis 2022.2 build fix, RFDC guide, UART/PCIe starvation fix, DAC source and host transmit, sampling-rate root cause, `pcie_regs.h.reference` |
| `04_Binaries/` | Destination for the built `*.elf` and `BOOT.BIN` |

## Build

1. Build the Vitis platform from `H2C_pcie_control.xsa` (see HF).
2. Copy `01_Source/*.c *.h` into the application's `src/`. Vitis 2025.2
   passes `-DSDT`, and `platform_ids.h` selects the right IDs.
3. Build, then run `02_Boot/make_boot.sh <workspace>` to produce `BOOT.BIN`.
4. On the console, confirm `[pcie] pcie_cfg ABI 4 ... caps 0x7F`.

`pcie_regs.h` must stay byte-identical to the copies in
`SC/01_sdr_workstation_GUI/src/core` and `SC/02_host_tools`.
`TS/02_Scripts/audit_sync.sh` checks this.

The API workbook (DF) catalogues every firmware function, menu option, PCIe
event and readback, and every Xilinx BSP call (sheets *Firmware*,
*PCIe Control*, *Xilinx BSP*).
