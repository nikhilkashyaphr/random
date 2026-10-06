# iW-ASCEJ-01-BN-R1.0-REL1.0 — Binaries

| File | Description |
|---|---|
| `01_Host_x86-64/sdr_workstation` | Workstation GUI 1.2.5 (Qt 5.15), stripped |
| `01_Host_x86-64/rfdc_ctl` | PCIe mailbox CLI |
| `01_Host_x86-64/SHA256SUMS` | Checksums (`sha256sum -c SHA256SUMS`) |

Built from `SC` of this package on Ubuntu 24.04 (gcc 13.3, Qt 5.15.13,
glibc 2.39). They need **glibc 2.38 or newer** (`ldd --version`). On an older
system they stop with `GLIBC_2.38 not found`. That is expected, not a fault:
build from SC instead (see `SC/README.md`, about a minute).

The GUI's backend helpers (`c2h_stream`, `iwfg_h2c`) and the `iwfg.ko`
driver are **not** shipped prebuilt. They must match the host's glibc and
kernel, so build them on the target: the GUI's CMake build makes the helpers,
and `make` in `SC/01_sdr_workstation_GUI/drivers` makes the driver.

To use the prebuilt GUI for live sessions, build the helpers once and start
the GUI from the GUI source folder. The GUI looks for `./backend`,
`<exe-dir>/backend` and `<exe-dir>/../backend`:

```bash
make -C ../iW-ASCEJ-01-SC-R1.0-REL1.0/01_sdr_workstation_GUI/backend
cd ../iW-ASCEJ-01-SC-R1.0-REL1.0/01_sdr_workstation_GUI
../../iW-ASCEJ-01-BN-R1.0-REL1.0/01_Host_x86-64/sdr_workstation
```
