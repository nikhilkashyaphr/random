# iW-ASCEJ-01 — iWave SDR Platform (ZU47DR RFSoC)
## Delivery package R1.0 · REL1.0 (software release 1.2.6)

This is the formal delivery package for the iWave SDR platform built on the
AMD/Xilinx Zynq UltraScale+ RFSoC **xczu47dr-ffvg1517-2-i**. It holds the
hardware configuration, the bare-metal firmware, the host software (GUI, QDMA
driver, helpers and CLI), the manuals, the acceptance-test record and the
automated test suite. It also holds a detailed API and engineering-calculation
workbook.

---

## 1. Folder legend

Every folder is named `iW-ASCEJ-01-<code>-R1.0-REL1.0`:

| Code | Folder | Contents |
|---|---|---|
| **DF** | `iW-ASCEJ-01-DF-R1.0-REL1.0` | **Deliverable Folder**: the package root (this folder) |
| **HF** | `iW-ASCEJ-01-HF-R1.0-REL1.0` | **Hardware Files**: PS configuration (`psu_init.c/.html`), Vitis platform export, PL design hand-off list, Hardware Interface Summary |
| **FF** | `iW-ASCEJ-01-FF-R1.0-REL1.0` | **Firmware Files**: 37 bare-metal sources, BOOT.BIN recipe (`boot.bif`, `make_boot.sh`, TCL), firmware documentation |
| **SF** | `iW-ASCEJ-01-SF-R1.0-REL1.0` | **Software Files**: the five folders below |
| └ **BN** | `iW-ASCEJ-01-BN-R1.0-REL1.0` | **Binaries**: prebuilt `sdr_workstation` (GUI) and `rfdc_ctl` (CLI) for x86-64 |
| └ **SC** | `iW-ASCEJ-01-SC-R1.0-REL1.0` | **Source Code**: Qt GUI, backend helpers, `iwfg` QDMA kernel driver, host tools |
| └ **UM** | `iW-ASCEJ-01-UM-R1.0-REL1.0` | **User Manual**: PDF manual, topic guides, developer notes |
| └ **AT** | `iW-ASCEJ-01-AT-R1.0-REL1.0` | **Acceptance Test**: procedure (ATP), report (ATR), raw logs |
| └ **TS** | `iW-ASCEJ-01-TS-R1.0-REL1.0` | **Test Suite**: host-side automated tests and verification scripts |

**R1.0** is the document and folder revision. **REL1.0** is the package
release. REL1.0 carries software version **1.2.6**.

Short paths are used in the documents for readability:

| Short form | Full path from this folder |
|---|---|
| `HF/…` | `iW-ASCEJ-01-HF-R1.0-REL1.0/…` |
| `FF/…` | `iW-ASCEJ-01-FF-R1.0-REL1.0/…` |
| `BN/…` | `iW-ASCEJ-01-SF-R1.0-REL1.0/iW-ASCEJ-01-BN-R1.0-REL1.0/…` |
| `SC/…` | `iW-ASCEJ-01-SF-R1.0-REL1.0/iW-ASCEJ-01-SC-R1.0-REL1.0/…` |
| `UM/…` | `iW-ASCEJ-01-SF-R1.0-REL1.0/iW-ASCEJ-01-UM-R1.0-REL1.0/…` |
| `AT/…` | `iW-ASCEJ-01-SF-R1.0-REL1.0/iW-ASCEJ-01-AT-R1.0-REL1.0/…` |
| `TS/…` | `iW-ASCEJ-01-SF-R1.0-REL1.0/iW-ASCEJ-01-TS-R1.0-REL1.0/…` |

## 2. Documents at this level

| File | What it is |
|---|---|
| `iW-ASCEJ-01-API-Reference-R1.0-REL1.0.xlsx` | API and engineering-calculation reference. It catalogues **1,737 APIs**: math/DSP, GUI, application core, QDMA driver, registers, helpers, firmware, PCIe mailbox, Xilinx BSP and CLI. Each row gives the file and line. It also has a **live-formula calculator** (rates, capture budget, H2C ladder, register encodings, DAC packing, FFT/ENBW, link budget), the system data path and the test coverage. |
| `iW-ASCEJ-01-ReleaseNotes-R1.0-REL1.0.pdf` (+ `.md`) | What this release contains and changes, known limitations, compatibility |
| `VERSION` | Package identification |
| `MANIFEST.sha256` | SHA-256 of every file in the package |

## 3. Quick start

```bash
# 0. Check the package is intact
iW-ASCEJ-01-SF-R1.0-REL1.0/iW-ASCEJ-01-TS-R1.0-REL1.0/02_Scripts/verify.sh

# 1. Firmware: copy ALL 37 files of FF/01_Source into the Vitis application,
#    build, and make BOOT.BIN (FF/02_Boot/make_boot.sh). Flash as a set.
#    The serial console must print:  [pcie] pcie_cfg ABI 4 ... caps 0x7F

# 2. Host software: build from source (≈1 minute)
cd iW-ASCEJ-01-SF-R1.0-REL1.0/iW-ASCEJ-01-SC-R1.0-REL1.0/01_sdr_workstation_GUI
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
#    or use the prebuilt binary in BN/01_Host_x86-64 (glibc >= 2.38)

# 3. Driver: build and load the iwfg QDMA driver (SC/01_sdr_workstation_GUI/drivers)

# 4. Run
./build/sdr_workstation
```

The **User Manual** (`UM/iW-ASCEJ-01-User-Manual-R1.0-REL1.0.pdf`) covers
installation, operation and troubleshooting in full.

## 4. Verification of this package

| Check | Result |
|---|---|
| Package integrity (`MANIFEST.sha256`) | PASS |
| Firmware source set (37 files) and firmware ↔ GUI ↔ CLI contract (`audit_sync.sh`, 13 checks) | PASS |
| GUI built from `SC` with Qt 5.15 | PASS |
| Automated suites from `TS`: 14 tests, **366 checks**, run against the sources in this package | **ALL PASS** |

The details are in `AT/iW-ASCEJ-01-ATR-R1.0-REL1.0.xlsx` and `AT/01_Logs`.
Bench (hardware) acceptance is recorded with the ATP on the target board.

## 5. Rules that keep the system consistent

1. **Flash all 37 firmware files together.** If you flash `main.c` alone,
   `pcie_cfg.c` stays stale, and the two look identical from the outside.
2. **`pcie_regs.h` exists three times and must stay byte-identical:**
   `FF/01_Source`, `SC/02_host_tools` and `SC/01_sdr_workstation_GUI/src/core`.
   `audit_sync.sh` checks this.
3. **Build the backend helpers on the machine that runs them.** The GUI build
   does this. They are never shipped prebuilt.
4. **Do not run the GUI with sudo** if you use the GPU path. Use
   `setcap cap_sys_rawio+ep` instead.
