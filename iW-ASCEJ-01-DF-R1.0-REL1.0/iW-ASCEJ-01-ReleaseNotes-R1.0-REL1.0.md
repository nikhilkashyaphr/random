# Release identification

| Item | Value |
|---|---|
| Project | iW-ASCEJ-01 — iWave SDR platform, ZU47DR RFSoC |
| Package | iW-ASCEJ-01-DF-R1.0-REL1.0 |
| Package release | REL1.0 (first formal release package) |
| Software version | 1.2.5 |
| Firmware PCIe ABI | 4 (capabilities `0x7F`) |
| Target device | xczu47dr-ffvg1517-2-i |
| Host | x86-64 Linux, Qt 5.15 / Qt 6, iwfg QDMA driver |

# Contents of the package

| Folder | Delivered |
|---|---|
| HF | `psu_init.c`, `psu_init.html` (PS configuration), Vitis `export.json`, Hardware Interface Summary, PL hand-off list |
| FF | 37 firmware sources; `boot.bif`, `make_boot.sh`, `boot_probe.tcl`, `jtag_boot_test.tcl`, linker fragment; 8 firmware documents and the register-header reference |
| BN | `sdr_workstation` (GUI), `rfdc_ctl` (CLI), with SHA-256 sums |
| SC | GUI sources (`src/core`, `src/ui`), backend helpers (`c2h_stream`, `iwfg_h2c`, UDP receiver), `iwfg` QDMA kernel driver, resources, CMake; `rfdc_ctl` + `pcie_access` |
| UM | User Manual (PDF), topic guides, developer notes and the full change log |
| AT | Acceptance Test Procedure, Acceptance Test Report, raw logs |
| TS | Host test suite (4 suites, 10 tests, 244 checks) and verification scripts |
| DF | API and engineering-calculation workbook (1,737 APIs, live calculator), these release notes, manifest |

# What is new in software 1.2.5

All 1.2.5 changes are in the GUI only. The board does not need to be
reflashed, and the driver is unchanged.

**Capture and transmit no longer break each other.** The root cause is in the
`iwfg` driver. On every C2H FIFO overflow it soft-resets the whole QDMA and
rebuilds every queue, the H2C queue included, even while an H2C transfer is
waiting on it. A 4-channel capture at 200 MSPS needs 3052 MiB/s, and the
bench host drains about 2400 MiB/s, so this happens continuously. The result
was tone disturbances, transmit stalls and captures that died or sat at
0.0 MB/s.

| Change | Effect |
|---|---|
| Transmit guard | While the generator runs, a capture above 1600 MiB/s is reduced to 2 channels via PL GPIO mode. The GUI checks the result against the measured rate and restores the setting on stop. |
| Stalled-capture device reset | Silent capture for about 5 s → close transmit, then capture (the driver resets on the last close), then restart both. No dialog appears. |
| Ordered stop | Every stop or start leaves the device freshly reset |
| Same-size reopen | A stalled transmit helper is reopened at the same size before any step-down |
| Capture FIFO pipe | 1 MiB (was 64 KiB) |
| QMC check | The ADC row warns when the ADC QMC gain reads 0 |

# Earlier changes carried in this release (1.2.1 – 1.2.4)

| Version | Change |
|---|---|
| 1.2.4 | H2C default buffer is 1 MiB (16 MiB never completed). Automatic step-down 1 MiB → 16 KiB on a stalled transfer. Capture helper restarts automatically. Stop/Start/close never block. Helper output goes to `sdr_backend.log`. Helper rows in the transmit chain. |
| 1.2.3 | H2C TRANSMIT CHAIN view read from the device. **Verify loopback** check. |
| 1.2.2 | Clean host transmit: coherent replay loop, write-once generator |
| 1.2.1 | DAC input source fixed: DDS and host were swapped end to end. The board now boots on the DDS. |

The complete history (1.0.0 onward) is in `UM/02_Developer_Notes/CHANGELOG.md`.

# Compatibility

| Item | Note |
|---|---|
| Firmware | ABI 4 is required. A board on 1.2.0 firmware is still controlled correctly (tested), but should be reflashed. |
| `pcie_regs.h` | Must be identical in FF, SC/GUI and SC/host-tools (checked by `audit_sync.sh`) |
| Prebuilt binaries | glibc ≥ 2.38 (Ubuntu 24.04). Older systems: build from SC. |
| Backend helpers | Always built on the target host by the GUI build. Never shipped prebuilt. |
| Driver | Unchanged in this release. See the known limitations. |

# Verification

| Check | Result |
|---|---|
| Package integrity, `MANIFEST.sha256` | PASS |
| Firmware ↔ GUI ↔ CLI contract (`audit_sync.sh`, 13 checks) | PASS |
| GUI build from SC (Qt 5.15) | PASS |
| TS-01 firmware DAC routing | 17/17 PASS |
| TS-02/03 PCIe end-to-end, 1.2.1 and 1.2.0 firmware | 17/17 + 17/17 PASS |
| TS-04 transmit path | 20/20 PASS |
| TS-05 simulated loopback | 13/13 PASS |
| TS-06 control window | 39/39 PASS |
| TS-07 whole application, simulation | 4/4 PASS |
| TS-08 real `iwfg_h2c`, emulated card | 18/18 PASS |
| TS-09 send-and-receive chain with fault injection | 73/73 PASS |
| TS-10 whole application, live session, emulated card | 26/26 PASS |

# Known limitations

1. The driver's C2H overflow recovery resets H2C as well. The GUI works
   around it; the recommended driver fixes are documented in
   `UM/02_Developer_Notes/DRIVER_OVERFLOW_RECOVERY.md`.
2. A 4-channel capture at 200 MSPS exceeds the measured host drain. It is not
   usable while transmitting; the transmit guard handles this.
3. H2C buffers of 16 MiB do not complete on the bench.
4. The PL design files (XSA, bitstream, hwh) are not included. HF lists them.
5. Host-side verification uses emulated hardware. Bench acceptance follows the ATP.
