# Device selection / Ethernet interface not appearing — root cause

## Layer-by-layer trace (your 7 steps)

| # | layer | result | evidence |
|---|---|---|---|
| 1 | OS detects Mellanox | **PASS** | `mlx5_0`, `mlx5_1`, fw 16.35.8008 |
| 2 | correct netdev identified | **PASS** | `enp60s0f0np0`, MAC …bc, MTU 9000, 192.168.1.1 |
| 3 | backend detects device | **PASS** | `enp60s0f0np0 <-> mlx5_0 [VERIFIED]`, GUID matches MAC |
| 4 | **detected device reaches GUI** | **FAIL ← ROOT CAUSE** | see below |
| 5 | device-selection option missing | consequence of 4 | |
| 6 | Ethernet interface missing | consequence of 4 | |
| 7 | fix the cause | done | |

## The break, in one line

`src/ui/ConfigDialog.cpp` built both combo boxes from **hardcoded string
literals**:

```cpp
m_device->addItems({"QDMA FPGA", "RFSoC ZCU111", "USB3 front-end", "Simulated device"});
m_port  ->addItems({"Port 0", "Port 1"});
```

and a grep confirmed the discovery API was never referenced by any UI file:

```
$ grep -rn "mapNetdevsToRdma|detectRdmaPorts|selectRoceMapping" src/ui/
NOTHING — the discovery results are never consumed by the GUI
```

So the back end was doing its job perfectly and the answer had nowhere to go.
No amount of further work on detection could ever have surfaced it — which is
exactly why the previous attempts kept missing.

## Fix

`ConfigDialog::repopulateDevices()` now fills both combos from
`probe::mapNetdevsToRdma()`, and runs at construction and on every
PCIe/Ethernet toggle.

* **Ethernet selected** → one entry per discovered RDMA device, labelled with
  its verified netdev, e.g. `mlx5_0  (enp60s0f0np0)`; unverified mappings are
  marked `[unverified]` rather than hidden. The Port combo shows the real port
  with its carrier state and IP: `Port 1 — NO CARRIER` / `Port 1 — link up, 192.168.1.1`.
  The RDMA device name is carried in the item's `userData`, so nothing
  downstream re-derives it.
* **PCIe selected** → the FPGA front ends, exactly as before.
* The entry `selectRoceMapping()` would choose is preselected, and the status
  line shows why.
* When nothing is found: `No RDMA device found` — an explicit statement, not
  an empty box.

## Verification

**On this (hardware-less) host** — combos switch correctly and report honestly:

```
--- PCIe selected ---      combo[0]: [QDMA FPGA] [RFSoC ZCU111] [USB3 front-end] [Simulated device]
                           combo[1]: [Port 0] [Port 1]
--- Ethernet selected ---  combo[0]: [No RDMA device found]
                           combo[1]: [—]
```

**Against a simulated sysfs tree carrying your machine's exact values**
(node_guid `043f7203:00a411bc`, MAC `04:3f:72:a4:11:bc`, MTU 9000, carrier up,
RoCE v2 at gid index 1):

```
mappings: 1
  enp60s0f0np0 <-> mlx5_0 verified=1 carrier=1 roce2=1 gidix=1
```

The mapping is discovered, the GUID⇄MAC cross-check passes, and the gid index
matches the `gid_index 1` your real preflight reports. Selection then
correctly declines only for the missing IPv4 on the fake netdev.

A test hook was added for this: `SDR_SYSFS_IB` / `SDR_SYSFS_NET` override the
sysfs roots, so the mapping logic can be exercised without hardware. They
default to the real paths and are read once.

## What you will see on the iWave machine

Selecting **Ethernet** will list:

```
mlx5_0  (enp60s0f0np0)
mlx5_1  (enp60s0f1np1)
```

with ports showing `NO CARRIER` until the cable is in. The status line will
read the same reason your preflight already prints:

```
enp60s0f0np0 <-> mlx5_0 not usable: NO CARRIER (cable/link down); port state 1: DOWN
```

## Still the one hardware blocker

```
Link detected: no (No cable)
```

Both ports are `1: DOWN / phys 3: Disabled`, and the reported `40 Gb/sec
(4X QDR)` is the driver's placeholder for a down port, not a negotiated rate.
The supplied design is a single-host loopback: **cable Port A to Port B**.

Also worth noting from your preflight, neither fatal nor urgent:

* GPU PCIe link reads **gen 1 x8** — almost certainly idle downclocking
  (ASPM). Re-check under load; if it stays gen 1, that caps GPUDirect
  bandwidth badly.
* **Persistence mode Disabled** → `sudo nvidia-smi -pm 1`.
* Desktop shares the GPU (Xorg/gnome-shell/firefox), so benchmark headless.
* CUDA toolkit is **12.9** while the driver reports 13.2 — fine (the driver is
  backward compatible), but pin the toolkit deliberately.

## Scripts note

`sudo ./scripts/02_setup_network.sh` failed with *command not found* — the
scripts lost their executable bit through the archive. Fix with:

```bash
chmod +x reference/roce-iq-holoscan/scripts/*.sh
```

## Unchanged

GUI layout, widgets, styling, plots, controls, channel selection, frequency
calculation, FFT, PCIe/`iwfg` path, and all supplied RoCEv2/Holoscan sources.
Clean build, zero warnings; simulator and CW replay regression-verified.
