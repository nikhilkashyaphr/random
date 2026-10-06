# Ethernet → mlx5_x → RoCEv2 → GPU: discovery, verification, honest states

## The core fix: the mapping is discovered and *verified*, never hard-coded

`mlx5_0` is nowhere in the code as a constant. The association is established
two independent ways and both must agree.

**1. sysfs (authoritative)**

```
/sys/class/infiniband/<rdma_dev>/device/net/<netdev>
```

**2. node GUID ⇄ MAC (independent cross-check)**

Derived from your own hardware. Mellanox builds the node GUID by inserting
`0x0300` between the OUI and the NIC-specific bytes — *not* the standard
EUI-64 `fffe`:

```
MAC 04:3f:72:a4:11:bc  ->  043f72 | 0300 | a411bc  =  043f720300a411bc   MATCH
MAC 04:3f:72:a4:11:bd  ->  043f72 | 0300 | a411bd  =  043f720300a411bd   MATCH
```

Both of your ports verify exactly. A mapping is reported `VERIFIED` only when
sysfs and the arithmetic agree; otherwise it is `UNVERIFIED` with the reason,
so a relabelled or renamed interface cannot silently mislead the app.

Unit-tested, including rejection of malformed input (empty, short, non-hex,
over-long) — it declines rather than guessing.

## Selection is ranked and explainable

`selectRoceMapping()` requires **all** of: verified mapping, carrier up,
port `ACTIVE`, RoCEv2 GID present, IPv4 configured. When nothing qualifies it
names precisely what is missing on the best candidate:

```
enp60s0f1np1 <-> mlx5_1 not usable: NO CARRIER (cable/link down); no IPv4 address
```

rather than a bare failure. This is what turns "RoCEv2 initialization failed"
into something actionable.

## Preflight now reports, per port

```
-- netdev <-> RDMA mapping (discovered) ------------------
  enp60s0f0np0  <->  mlx5_0 port 1   [VERIFIED]
      mac 04:3f:72:a4:11:bc   node_guid 043f720300a411bc   guid<->mac match
      ipv4 192.168.1.1   mtu 9000   carrier UP   port ACTIVE   rate 100 Gb/sec
      link_layer Ethernet   RoCEv2 yes   gid_index 3
  selected for RoCEv2 : mlx5_0
```

## nvidia_peermem: load *and re-verify*

`checkPeermem(attemptLoad)` does exactly what Step 5 demands:

1. `modinfo nvidia_peermem` → installed? version?
2. `/proc/modules` → already loaded?
3. If installed but not loaded and loading is permitted, `modprobe` (via
   `pkexec` when not root).
4. **Re-read `/proc/modules`.** A zero exit from `modprobe` is *never* taken
   as proof — the state is verified independently.
5. On failure, the real kernel/modprobe text is surfaced, and the GPUDirect
   path is refused rather than pretending GPU memory is in use.

## Distinct states, no fake success

The five states the brief calls out are genuinely different and reported as
such:

| state | evidence required |
|---|---|
| Device detected | `mlx5_x` in `/sys/class/infiniband` |
| Mapping verified | sysfs **and** GUID⇄MAC agree |
| Port usable | carrier UP + `ACTIVE` + RoCEv2 GID + IPv4 |
| RoCEv2 receiving | ring `write_count` advancing (`RoceShmSource`) |
| GPU memory active | ring magic `IQRINGG1` + peermem loaded + verified |

`RoceShmSource` already enforces the last two: it counts published frames and
sequence gaps, and it **refuses a GPU ring** with an explicit message instead
of displaying zeros.

## Current blocker on your machine

From your `ifconfig`: `enp60s0f0np0` has MTU 9000 and 192.168.1.1 — good —
but both ports previously showed `flags=4099` (no `RUNNING`). The supplied
`roce-iq-holoscan` README confirms the design is a **single-host loopback:
Port A cabled to Port B**. So:

```bash
# 1. cable enp60s0f0np0 <-> enp60s0f1np1
sudo ethtool enp60s0f0np0 | grep 'Link detected'      # must read: yes
cd reference/roce-iq-holoscan && sudo ./scripts/02_setup_network.sh
./scripts/03_verify_rdma.sh
./build/sdr_workstation --gpu-rdma-preflight          # expect VERIFIED + selected
```

Until `Link detected: yes`, every layer above is correctly blocked.

## Unchanged

GUI layout, widgets, styling, plots, controls, channel selection, frequency
calculation, FFT, PCIe/`iwfg` path, existing Ethernet detection, and all
supplied RoCEv2/Holoscan sources. Clean build, zero warnings; simulator and
CW replay regression-verified.
