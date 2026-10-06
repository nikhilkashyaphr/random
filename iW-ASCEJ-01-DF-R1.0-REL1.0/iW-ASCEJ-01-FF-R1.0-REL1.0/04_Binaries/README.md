# 04_Binaries — firmware build outputs

This folder is empty in REL1.0. Firmware images depend on the PL hand-off
(XSA/bitstream), which is supplied with the board design, so they are built
in Vitis on the integration machine.

After the build, archive these files here:

| File | From |
|---|---|
| `zu47dr_app.elf` (or `app_component.elf`) | Vitis application build |
| `BOOT.BIN` | `../02_Boot/make_boot.sh` |
| `boot_generated.bif` | written by `make_boot.sh` (records exactly what went in) |
| `SHA256SUMS` | `sha256sum *.elf BOOT.BIN > SHA256SUMS` |
