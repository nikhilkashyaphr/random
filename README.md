# iW-ASCEJ-01 — iWave SDR Platform (ZU47DR RFSoC)

Development repository for the iWave SDR platform built on the AMD/Xilinx
Zynq UltraScale+ RFSoC **xczu47dr-ffvg1517-2-i**.

## Baseline

All development in this repository starts from the formal delivery package

> **`iW-ASCEJ-01-DF-R1.0-REL1.0/`** — package release REL1.0, document
> revision R1.0, software version **1.2.5**, firmware PCIe **ABI 4**
> (capabilities `0x7F`).

The package directory is committed verbatim, byte-for-byte as delivered, and
is the reference point for every later change. Its layout, short-path
conventions (`HF/…`, `FF/…`, `BN/…`, `SC/…`, `UM/…`, `AT/…`, `TS/…`) and
integrity manifest are described in
[`iW-ASCEJ-01-DF-R1.0-REL1.0/README.md`](iW-ASCEJ-01-DF-R1.0-REL1.0/README.md).

| Area | Path | Contents |
|---|---|---|
| Hardware | `…/iW-ASCEJ-01-HF-R1.0-REL1.0` | PS configuration (`psu_init.c/.html`), Vitis platform export, PL hand-off list, Hardware Interface Summary |
| Firmware | `…/iW-ASCEJ-01-FF-R1.0-REL1.0` | 37 bare-metal sources, BOOT.BIN recipe, firmware documentation |
| Binaries | `…/iW-ASCEJ-01-BN-R1.0-REL1.0` | Prebuilt `sdr_workstation` (GUI) and `rfdc_ctl` (CLI), x86-64, glibc ≥ 2.38 |
| Source | `…/iW-ASCEJ-01-SC-R1.0-REL1.0` | Qt GUI, backend helpers, `iwfg` QDMA kernel driver, host tools |
| Manual | `…/iW-ASCEJ-01-UM-R1.0-REL1.0` | User Manual, topic guides, developer notes, change log |
| Acceptance | `…/iW-ASCEJ-01-AT-R1.0-REL1.0` | ATP, ATR, raw logs |
| Tests | `…/iW-ASCEJ-01-TS-R1.0-REL1.0` | Host test suite and verification scripts |

(`…` = `iW-ASCEJ-01-DF-R1.0-REL1.0/iW-ASCEJ-01-SF-R1.0-REL1.0` for the last
five rows.)

## Baseline verification

Re-run on this commit, on a clean x86-64 Ubuntu 24.04 host with
`gcc`, `cmake` and Qt 5.15:

| Check | Command | Result |
|---|---|---|
| Package integrity (465 files) + firmware↔GUI↔CLI contract (13 checks) | `iW-ASCEJ-01-DF-R1.0-REL1.0/iW-ASCEJ-01-SF-R1.0-REL1.0/iW-ASCEJ-01-TS-R1.0-REL1.0/02_Scripts/verify.sh` | **PASS** |
| Host test suite — 4 suites, 10 tests, 244 checks | `iW-ASCEJ-01-DF-R1.0-REL1.0/iW-ASCEJ-01-SF-R1.0-REL1.0/iW-ASCEJ-01-TS-R1.0-REL1.0/01_Host_Test_Suite/run_all.sh` | **ALL PASS** |
| GUI build from `SC` (Qt 5.15.13, 0 errors) | `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j` | **PASS** |

The GUI build also produces the three backend helpers in-tree
(`backend/bin/c2h_stream`, `backend/bin/iwfg_h2c`, `backend/udp/iwfg_c2h`).
They are build outputs, never committed, and are listed in `.gitignore`.

## Quick start

```bash
# 1. Verify the baseline is intact
iW-ASCEJ-01-DF-R1.0-REL1.0/iW-ASCEJ-01-SF-R1.0-REL1.0/iW-ASCEJ-01-TS-R1.0-REL1.0/02_Scripts/verify.sh

# 2. Build the host software (≈1 minute)
cd iW-ASCEJ-01-DF-R1.0-REL1.0/iW-ASCEJ-01-SF-R1.0-REL1.0/iW-ASCEJ-01-SC-R1.0-REL1.0/01_sdr_workstation_GUI
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/sdr_workstation

# 3. Firmware: copy ALL 37 files of FF/01_Source into the Vitis application,
#    build, and make BOOT.BIN (FF/02_Boot/make_boot.sh). Flash as a set.
#    The serial console must print:  [pcie] pcie_cfg ABI 4 ... caps 0x7F
```

## Rules that keep the system consistent

These come from the delivery package and apply to every change made here:

1. **Flash all 37 firmware files together.** Flashing `main.c` alone leaves
   `pcie_cfg.c` stale, and the two look identical from the outside.
2. **`pcie_regs.h` exists three times and must stay byte-identical** —
   `FF/01_Source`, `SC/02_host_tools` and
   `SC/01_sdr_workstation_GUI/src/core`. `audit_sync.sh` enforces this.
3. **Build the backend helpers on the machine that runs them.** The GUI build
   does this. They are never shipped prebuilt.
4. **Do not run the GUI with `sudo`** if you use the GPU path. Use
   `setcap cap_sys_rawio+ep` instead.
5. **Re-run `verify.sh` and `run_all.sh` before every push.** Changing a file
   under the package directory invalidates `MANIFEST.sha256`; regenerate it
   with `TS/02_Scripts/make_manifest.sh` when a change to the baseline is
   intentional.

## Known limitations carried from the baseline

1. The `iwfg` driver's C2H overflow recovery resets H2C as well. The GUI works
   around it; recommended driver fixes are in
   `UM/02_Developer_Notes/DRIVER_OVERFLOW_RECOVERY.md`.
2. A 4-channel capture at 200 MSPS (3052 MiB/s) exceeds the measured host
   drain (~2400 MiB/s) and is not usable while transmitting; the transmit
   guard handles this.
3. H2C buffers of 16 MiB do not complete on the bench.
4. The PL design files (XSA, bitstream, hwh) are **not** included. `HF` lists
   them.
5. Host-side verification uses emulated hardware. Bench acceptance follows the
   ATP on the target board.

Full detail: `iW-ASCEJ-01-DF-R1.0-REL1.0/iW-ASCEJ-01-ReleaseNotes-R1.0-REL1.0.md`
and the User Manual under `UM/`.
