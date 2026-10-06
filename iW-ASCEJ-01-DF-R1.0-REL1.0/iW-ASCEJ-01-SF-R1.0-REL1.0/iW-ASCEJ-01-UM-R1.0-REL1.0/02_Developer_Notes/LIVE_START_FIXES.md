# Regression fix + live-start diagnostics

## 1. PCIe device list was empty — my regression, now fixed

**Cause.** In an earlier edit I extracted a block out of
`ConfigDialog::repopulateDevices()` and left an `if (m_ethStatus) { … }`
unclosed. The following `else` then bound to the wrong `if`, so the branch
that fills the list from `HardwareDb` became unreachable:

```cpp
        if (m_ethStatus) {
            ...
    } else {                       // <-- bound to the WRONG if
        for (const hw::BoardSpec& b : hw::HardwareDb::boards())
            m_device->addItem(...);   // never ran
```

Braces balanced across the file, so it compiled cleanly — which is why it
reached you. The symptom was an empty **Device** box, and with no board
selected the validator correctly refused to launch:
*"No development kit selected"*.

**Fixed and verified** — all nine kits are listed again and the summary reads
`iWave RFSoC ZU47DR · PCIe · 2 ch · 122.88 MSPS`.

I regret shipping that. The lesson applied below: a brace repair that leaves
the file compiling is not evidence that it is correct, so this round the
device list is asserted by a test rather than eyeballed.

## 2. "Start Live Acquisition" popup removed

It asked for C2H vs H2C_C2H, which the launcher's **DATA FLOW** section had
already answered. The mode now comes straight from `SystemConfig::dataFlowMode`:

```cpp
m_backendMode = (m_sys.dataFlowMode == 2) ? BackendMode::H2C_C2H
                                          : BackendMode::C2H;
```

Its Cancel path was also part of the next problem: it `return`ed silently,
leaving a running-looking application that never acquired.

## 3. Live streaming sat idle with no explanation

Several paths ended in a bare `return`, so the window looked normal and simply
never plotted. Each now states what happened and what to do:

* **Popup cancelled** — removed entirely (above).
* **Helpers failed to build** — dialog with the full build log, plus a status
  line: *"Live acquisition not started — backend helpers could not be built."*
* **Device node missing** — checked *before* launching. Without this,
  `c2h_stream` cannot open `/dev/iwfg0`, exits immediately, and the only
  symptom is a window that never plots. Now:

  > `/dev/iwfg0` does not exist, so the capture application cannot open it.
  > Load the iwfg driver (Acquisition ▸ PCIe discovery offers to do this), or
  > correct the device path in the Source panel.

### Silent-idle watchdog

For everything else — driver loaded, application running, but no samples —
a watchdog reports it after 6 s of `Running` with `blocksIn == 0`, listing
causes in order of likelihood:

1. Is the capture application still running? (status bar, `/tmp/sdr_backend.log`)
2. Is the FPGA actually transmitting? An armed but idle card produces no DMA
   completions.
3. Does the wire format match? A mismatch usually plots noise, but a wrong
   channel count can stall framing.
4. For UDP: is traffic reaching the port? Check with `tcpdump`, and use Raw
   capture if the destination MAC or IP checksum is not the host's.

It fires **once** per idle period and resets as soon as data arrives, so it
cannot nag.

## 4. Build warnings silenced

`make` inside a CMake custom command cannot see the parent jobserver and
warned on every build. The sub-builds now pass an explicit `-j4`.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

is now **completely clean** — zero errors, zero warnings — and produces all
four binaries.

## 5. Verified

| check | result |
|---|---|
| PCIe device list | **9 kits** |
| launcher summary | `iWave RFSoC ZU47DR · PCIe · 2 ch · 122.88 MSPS` |
| mode popup | removed |
| build (from clean) | 0 errors, 0 warnings, 4 binaries |
| main window | OK |
| launcher | OK |

## What I still cannot verify

Whether your card actually streams. The watchdog now converts that from a
blank window into a specific report, but the underlying cause — driver, FPGA
state, or format — needs the hardware. If it still idles, the dialog will name
which of the four checks to start from, and `/tmp/sdr_backend.log` holds the
capture application's own output.
