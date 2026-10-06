# Session start ignored the Ethernet selection — root cause and fix

## What your screenshots showed

Device detection was **fixed and working**:

```
Device : mlx5_0  (enp60s0f0np0)
Port   : Port 1 — link up, 192.168.1.1
Status : enp60s0f0np0 <-> mlx5_0: verified, link up, RoCEv2 GID 1, IPv4 192.168.1.1
```

Your cable is in and the port is up. But **SESSION START still read
"Live — stream from PCIe DMA / FIFO"** with `/dev/iwfg0`, so pressing Launch
ran the PCIe discovery and reported `/dev/iwfg0 · MISSING`.

## Root cause

The session-start section was hard-wired to PCIe and had no concept of a
network transport:

* `m_mode` was a fixed three-item list whose live entry was always
  *PCIe DMA / FIFO*.
* `SourceMode` had only `{ Simulated, File, Dma }` — **there was no value
  that could represent a live RoCEv2 session**, so even a correct selection
  had nowhere to go.
* The launch gate keyed the PCIe pre-flight off `mode == Dma`, which is what
  the Ethernet selection collapsed into.

So the acquisition-device section had been fixed, but the session-start
section was still a second, independent PCIe-only path.

## Fix

1. **`SourceMode::Roce` added** — a live RoCEv2 session is now representable,
   and stays distinct from `Dma` so pre-flight, paths and status text remain
   correct per transport.
2. **Session-start entry follows the interface.** `repopulateDevices()`
   rewrites entry 2 to *"Live — stream from RoCEv2 (shared-memory ring)"* for
   Ethernet, *"Live — stream from PCIe DMA / FIFO"* for PCIe, and swaps the
   default path between `/dev/shm/iqring` and `/dev/iwfg0` (only when the
   field still holds the other transport's default, so a hand-typed path is
   never clobbered).
3. **Launch gate per transport.** Ethernet + RoCEv2 validates the discovered
   mapping via `selectRoceMapping()`; the PCIe/iwfg discovery no longer runs
   for a network session.
4. **Source construction** — `MainWindow::createSource()` builds
   `RoceShmSource` for the new mode; `SourcePanel` treats it as a live source
   and, importantly, **remembers which live transport was chosen**. Returning
   `Dma` unconditionally from `SourcePanel::mode()` would have silently
   downgraded a RoCEv2 session to PCIe on the first config round-trip.
5. **`--roce <ring>`** added for headless use.

## A real bug this surfaced

First run streamed correctly but reported the tone at **−6.137 MHz** instead
of −10.000 MHz. The ratio is exactly 122.88/200.25: the GUI was applying its
default sample rate while the ring carried 200.25 MSPS.

The ring's control block is authoritative — the transmitter sets
`sample_rate_hz` and `center_freq_hz`. Added
`RoceShmSource::peekRingInfo()`, which reads them before acquisition starts
so the session adopts them instead of imposing defaults.

Verified:

```
peekRingInfo ok=1  rate=200.2500 MSPS  centre=0.000000 GHz   PASS
```

and end-to-end through the GUI against a ring carrying a −10 MHz tone:

```
Detected peaks
  P1        -10.000 MHz
```

Exactly the transmitted frequency.

## Verification summary

| check | result |
|---|---|
| PCIe selected → live entry / path | `PCIe DMA / FIFO` · `/dev/iwfg0` |
| Ethernet selected → live entry / path | `RoCEv2 (shared-memory ring)` · `/dev/shm/iqring` |
| ring rate adopted | 200.2500 MSPS — PASS |
| GUI streaming from ring | smooth sine, clean spectrum, P1 = −10.000 MHz |
| simulator | OK |
| offline capture replay | OK |
| preflight | exit 2 here (no hardware) — expected |
| build | clean, zero warnings |

## On your machine

Selecting **Ethernet** now gives a RoCEv2 session against `/dev/shm/iqring`.
Start the supplied receiver first, or the source reports
*"cannot open /dev/shm/iqring — is rdma_rx running?"* rather than hanging:

```bash
chmod +x reference/roce-iq-holoscan/scripts/*.sh
cd reference/roce-iq-holoscan && make
./rdma_rx &          # host ring  -> this GUI
./rdma_tx  &         # in ns_tx
./build/sdr_workstation      # Ethernet -> Live RoCEv2
```

For the **GPU** ring (`rdma_rx_gpu`), this GUI still declines by design: the
payload is in VRAM behind a CUDA IPC handle, and copying it down would defeat
GPUDirect. Use `viz/holoscan_iq_viz.py`, which already consumes it zero-copy.

## Unchanged

GUI layout, widgets, styling, plots, controls, channel selection, frequency
calculation, FFT, the PCIe/`iwfg` path, and all supplied RoCEv2/Holoscan
sources.
