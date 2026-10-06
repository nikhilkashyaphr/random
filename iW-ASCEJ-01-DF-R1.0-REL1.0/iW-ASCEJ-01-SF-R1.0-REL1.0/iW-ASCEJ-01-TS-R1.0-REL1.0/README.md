# iW-ASCEJ-01-TS-R1.0-REL1.0 — Test Suite

## 01_Host_Test_Suite

Host-side tests. They need no board, no root access and no GPU, only gcc,
CMake and the Qt 5 development files. Every suite builds the code under test
**directly from the SC and FF folders of this package** (`pkg_paths.cmake`),
so it always tests the delivered sources.

```bash
./01_Host_Test_Suite/run_all.sh        # ≈ 4 minutes; expect "ALL TESTS: PASS"
```

| Suite | Tests | Checks |
|---|---|---|
| `firmware/` | DAC input routing, taken verbatim from `main.c`, against a model of the switch | 17 |
| `pcie_emu/` | GUI `RfdcControl` ↔ emulated BAR ↔ real `pcie_cfg.c`, firmware 1.2.1 and 1.2.0 | 17 + 17 |
| `gui/` | Transmit path, simulated loopback, control window, whole application in simulation | 20 + 13 + 39 + 4 |
| `h2c_emu/` | Real `iwfg_h2c`; the send-and-receive chain with fault injection; whole application live, with an `LD_PRELOAD` fake card | 18 + 73 + 26 |

`01_Host_Test_Suite/README.md` describes each test in detail. If you move
the folders, override the source locations with `-DPKG_GUI=... -DPKG_FW=...`,
or `PKG_FW=...` for `firmware/run_tests.sh`.

## 02_Scripts

| Script | Purpose |
|---|---|
| `verify.sh` | Package integrity (`MANIFEST.sha256`), firmware source set, firmware ↔ GUI ↔ CLI contract |
| `audit_sync.sh` | 13 contract checks: identical `pcie_regs.h`, every GUI event handled, every readback answered, inverse unit conversions |
| `make_manifest.sh` | Regenerates `MANIFEST.sha256` at the package root |
| `inventory.sh` | Read-only host inventory (OS, toolchain, PCIe, NIC, GPU), useful for support |
| `gpu_p2p_check.sh` | GPU peer-to-peer / GPUDirect readiness check |

The scripts locate the package root themselves. Run them from any directory.
