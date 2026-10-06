# 03_PL_Design — PL hand-off (to be supplied)

The PL design is part of the board deliverable and is not included in
REL1.0. When you archive a build, place these files here:

| File | Purpose | Required for |
|---|---|---|
| `H2C_pcie_control.xsa` | Hardware hand-off: BSP, `xparameters.h`, platform | Building the firmware platform in Vitis |
| `H2C_pcie_control.bit` | PL bitstream | BOOT.BIN, PL partition (before the application) |
| `design_1.hwh` | Block-design description | Confirming the switch wiring: `DDS_H2C_switch` S00 = host H2C, S01 = DDS (`FF/01_Source/platform_ids.h`) |
| `fsbl.elf`, `pmufw.elf` | Boot loader and PMU firmware from the platform build | BOOT.BIN |

The firmware expects these PL instances: `XRFDC_0`, `ADC_CHANNEL_SWITCH_ADC_SWITCH`,
`DDS_H2C_SWITCH`, `ADC_CHANNEL_SELECT`, `AXI_GPIO_DYNAMIC_CLK`, `CLOCK_COUNTER`,
`AXI_GPIO_DDS1DDS2`, `SYSREF_CONTROLL`, `QUAD_SPI_LMK`, `QUAD_SPI_ADC_LMX`,
`QUAD_SPI_DAC_LMX`, `CLK_WIZ_ADC`, `CLK_WIZ_DAC`, and `M00_PCIE_CONTROL`
at `0xA00A0000`. If one is missing, the build stops with a `#error` in
`platform_ids.h` instead of using a wrong address.
