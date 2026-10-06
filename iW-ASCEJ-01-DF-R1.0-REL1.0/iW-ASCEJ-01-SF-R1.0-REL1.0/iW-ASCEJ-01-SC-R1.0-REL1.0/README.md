# iW-ASCEJ-01-SC-R1.0-REL1.0 — Source Code (software 1.2.5)

## 01_sdr_workstation_GUI

| Path | Contents |
|---|---|
| `src/main.cpp` | Entry point: options, configuration dialog, PCIe pre-flight, main window |
| `src/core/` | Application core: `BackendLauncher` (helper lifecycle, ordered stop, device reset, H2C step-down), `RfdcControl` (PCIe mailbox client), `IqGenerator` (coherent loop, 14-bit DAC packing), `DspEngine` (FFT, windows, metrics, EVM, DDC), sources (`SimulatedSource`, `FileSource`, `DmaSource`, `RoceShmSource`), `Recorder`, `RecordingGuard`, `CaptureBudget.h`, `pcie_regs.h`, GPU FFT / IQ pipeline |
| `src/ui/` | Qt widgets: `MainWindow`, `ControlWindow3D` (H2C CONTROL: RFDC, Transmit, Signal chain), plots (spectrum, waterfall, time, constellation), panels, dialogs, theme |
| `backend/C2H/fifo_pipe.c` | `c2h_stream`: capture helper, `/dev/iwfg0` → FIFO, 4 × 16 MiB pre-armed buffers |
| `backend/H2C/iwfg_h2c.c` | `iwfg_h2c`: transmit helper, FIFO → `/dev/iwfg1`, ping-pong, replay last chunk, watchdog |
| `backend/udp/` | UDP receiver `iwfg_c2h` (Ethernet path) |
| `drivers/` | `iwfg` QDMA kernel driver (`make` → `iwfg.ko`), `qdma_access/`, examples |
| `resources/` | Icons, style sheet, Qt resource file |
| `reference/` | RoCEv2/Holoscan reference code |
| `tools/` | Auxiliary tools |
| `CMakeLists.txt` | Builds the GUI and, for this host, the backend helpers and UDP receiver |

```bash
cd 01_sdr_workstation_GUI
cmake -B build -DCMAKE_BUILD_TYPE=Release     # -DSDR_ENABLE_CUDA=ON for the GPU path
cmake --build build -j
cd drivers && make                            # iwfg.ko
```

Requires CMake ≥ 3.16, a C/C++ compiler, and Qt 5.15+ or Qt 6 (Widgets).
The build is warning-free with `-Wall -Wextra -Wpedantic`.

## 02_host_tools

`rfdc_ctl` (CLI client of the PCIe mailbox) and `pcie_access.c/.h`
(sysfs / chardev BAR access). Build with `make`.

## Rules

- `pcie_regs.h` is identical in `01_sdr_workstation_GUI/src/core`,
  `02_host_tools` and `FF/01_Source`.
- `backend/bin` is produced by the build on the machine that runs it. Never
  copy it between machines.
- The API workbook in DF documents every class, method, signal/slot, ioctl,
  register and formula in these sources, with file and line.
