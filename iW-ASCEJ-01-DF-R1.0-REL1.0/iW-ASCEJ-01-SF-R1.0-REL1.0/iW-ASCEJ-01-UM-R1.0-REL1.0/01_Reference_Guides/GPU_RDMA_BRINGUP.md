# GPU/RDMA Bring-Up Guide — Mellanox RoCEv2 → GPUDirect → CUDA → GUI

Target — **confirmed from your `nvidia-smi` and `ifconfig` output**:

| | |
|---|---|
| GPU | **NVIDIA RTX A4000**, 16376 MiB, BDF `0000:3B:00.0` |
| GPU class | **Professional (RTX A)** → **GPUDirect RDMA supported** |
| Driver | **595.84** |
| CUDA | **13.2** |
| NIC | **Mellanox**, dual port (MAC OUI `04:3f:72`) |
| NIC ports | `enp60s0f0np0` (`0000:3c:00.0`), `enp60s0f1np1` (`0000:3c:00.1`) |
| OS | Ubuntu 22.04 LTS |
| Kernel | 6.8.0-136-generic (HWE) |
| Existing FPGA path | PCIe QDMA (`iwfg`), unaffected |

### Three findings from that output

**1. GPUDirect RDMA is supported on this GPU — green light.**
The RTX A4000 is a *professional* (Quadro/RTX-A) part. NVIDIA enables
GPUDirect RDMA on the Quadro/RTX-A and data-centre lines only; GeForce cards
are excluded at the product level regardless of driver or peermem. Yours
qualifies. The preflight now classifies this automatically.

**2. GPU and NIC are on adjacent PCIe buses (0x3b and 0x3c).**
Almost certainly the same root complex, which is the ideal placement — a
GPUDirect transfer stays local instead of crossing an inter-socket link.
Confirm with `nvidia-smi topo -m`; you want `PIX`, `PXB` or `NODE`, not `SYS`.

**3. BLOCKER — both NIC ports have no link.**

```
flags=4099<UP,BROADCAST,MULTICAST>
```

`4099 = 0x1003` = UP | BROADCAST | MULTICAST. **`RUNNING` (0x40) is absent.**
A live port reads `flags=4163<UP,BROADCAST,RUNNING,MULTICAST>`. Both ports
also show `RX packets 0 / TX packets 0`, no IP, and `mtu 1500`.

So: the interface is administratively up, but there is **no carrier** — no
cable, no link partner, or no negotiation. Nothing downstream (RoCE, GID,
GPUDirect, RDMA test) can succeed until this is resolved. **Fix this first.**

> **Rule zero — run the inventory first.** Every command below is conditional
> on what your machine actually reports. Do not paste any of it until
> `tools/inventory.sh` output has been reviewed.
>
> ```bash
> ./tools/inventory.sh > inventory.txt 2>&1
> ./build/sdr_workstation --gpu-rdma-preflight
> ```

---

## STAGE 0a — Fix the link (the current blocker)

```bash
# Which port is cabled? Check both.
sudo ethtool enp60s0f0np0 | grep -E 'Link detected|Speed|Port|Supported link modes'
sudo ethtool enp60s0f1np1 | grep -E 'Link detected|Speed|Port'
```

`Link detected: no` on both confirms the carrier problem. Work through:

1. **Cable/transceiver seated?** ConnectX ports are usually QSFP/SFP+ —
   a DAC or optic must be inserted and the far end must be live.
2. **Is the module recognised?**
   `sudo ethtool -m enp60s0f0np0` (reads the transceiver EEPROM). No output
   means no module or an unsupported one.
3. **Link partner** — a switch port that is up, or a direct back-to-back cable
   to the traffic source.
4. **Speed mismatch** — check `Supported link modes` against the far end.
5. **dmesg** — `sudo dmesg | grep -i mlx5 | tail -30` shows negotiation faults.

**Do not proceed past this point until `Link detected: yes`.** RoCEv2 GIDs
can exist on a dead port, so the preflight can look healthier than reality;
carrier is the ground truth.

---

## STAGE 0 — Inventory (installs nothing)

```bash
./tools/inventory.sh > inventory.txt 2>&1
```

Captures: OS/kernel/toolchain, full PCI list, GPU detail (incl. PCIe link
gen/width), every netdev with driver/firmware/speed, the **RDMA-device →
netdev mapping from sysfs**, loaded modules, relevant dmesg, SDK presence,
perftest availability (including whether it was built with `--use_cuda`),
IOMMU cmdline and `nvidia-smi topo -m`.

Two things in that output decide everything that follows:

1. **Is there a ConnectX NIC and an NVIDIA GPU at all?**
2. **`nvidia-smi topo -m`** — if GPU and NIC sit under different CPU sockets,
   GPUDirect still works but crosses the inter-socket link and loses much of
   its benefit. Same PCIe root complex is strongly preferred.

**Gate:** if either device is absent, stop. Nothing below is installable in a
useful way.

---

## STAGE 1 — Toolchain and build prerequisites

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config git \
                    linux-headers-$(uname -r)
```

Verify:
```bash
gcc --version && cmake --version && ls /lib/modules/$(uname -r)/build
```
Expected: gcc ≥ 11, cmake ≥ 3.16, and the build symlink resolving.

Rollback: `sudo apt remove --purge <pkg>` (leave `build-essential` alone).

---

## STAGE 2 — NVIDIA driver + CUDA

**Decision: use the CUDA network repo, not the `.run` installer.** The repo
keeps DKMS rebuilding the driver across kernel updates; the `.run` installer
silently breaks on the next kernel bump, which on a machine that also carries
your out-of-tree `iwfg` module is a bad failure mode.

```bash
# CUDA network repo for Ubuntu 22.04
wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.1-1_all.deb
sudo dpkg -i cuda-keyring_1.1-1_all.deb
sudo apt update

# Driver 595.84 is ALREADY installed and working — do not reinstall it.
# Installing cuda-drivers here risks pulling a different branch and breaking
# both the display and your out-of-tree iwfg module. Skip unless nvidia-smi
# stops working.

# Toolkit. Your driver (595.84) already reports CUDA 13.2, so match it.
# Pin the version; do NOT install bare `cuda`, which floats to the newest.
sudo apt install -y cuda-toolkit-13-2
```

Add to `~/.bashrc`:
```bash
export PATH=/usr/local/cuda-13.2/bin:$PATH
export LD_LIBRARY_PATH=/usr/local/cuda-13.2/lib64:$LD_LIBRARY_PATH
```

Reboot, then verify:
```bash
nvidia-smi                 # GPU, driver version, VRAM
nvcc --version             # toolkit version
```

**Functional CUDA test — do not skip:**
```bash
cat > /tmp/cudatest.cu <<'EOF'
#include <cstdio>
__global__ void k(float* d){ d[threadIdx.x] = threadIdx.x * 2.0f; }
int main(){
    float *d, h[8];
    if (cudaMalloc(&d, sizeof h) != cudaSuccess) { printf("cudaMalloc FAILED\n"); return 1; }
    k<<<1,8>>>(d);
    cudaMemcpy(h, d, sizeof h, cudaMemcpyDeviceToHost);
    cudaFree(d);
    for (int i=0;i<8;i++) if (h[i] != i*2.0f) { printf("MISMATCH at %d\n", i); return 1; }
    printf("CUDA OK\n"); return 0;
}
EOF
nvcc -o /tmp/cudatest /tmp/cudatest.cu && /tmp/cudatest
```
Expected: `CUDA OK`.

**Secure Boot note:** if `mokutil --sb-state` says enabled, unsigned DKMS
modules (NVIDIA, and later `nvidia_peermem`) will not load. Either enrol a MOK
key or disable Secure Boot. Check the inventory output for this.

Rollback: `sudo apt remove --purge 'cuda-*' 'nvidia-*' && sudo apt autoremove`.

**Gate:** `CUDA OK` must print. Otherwise stop.

Two housekeeping items visible in your `nvidia-smi`:

* **Persistence mode is Off.** Enable it — without it the driver unloads GPU
  state between clients, adding milliseconds of jitter to the first access:
  ```bash
  sudo nvidia-smi -pm 1
  ```
* **The desktop is on this GPU** (Xorg 116 MiB, gnome-shell 105 MiB,
  firefox 232 MiB, 11 % util at idle). That is workable for bring-up, but
  the compositor will contend with a line-rate DSP pipeline for SM time and
  PCIe bandwidth. For benchmarking, run headless (`sudo systemctl isolate
  multi-user.target`) or move the display to an on-board GPU. Note it when
  interpreting any throughput number.

---

## STAGE 3 — RDMA stack: upstream rdma-core vs MLNX_OFED

**Decision required — do not install both.**

| | upstream `rdma-core` (Ubuntu) | NVIDIA **MLNX_OFED / DOCA-OFED** |
|---|---|---|
| install | `apt install rdma-core` | NVIDIA `.tgz` installer |
| kernel modules | in-tree `mlx5_core`/`mlx5_ib` | replaces them with its own |
| GPUDirect | needs `nvidia-peermem` from the CUDA driver | ships its own `nv_peer_mem` |
| perftest w/ CUDA | Ubuntu build has **no** `--use_cuda` | ships CUDA-enabled perftest |
| risk to `iwfg` | none | replaces core kernel modules |

**Recommendation: start with upstream `rdma-core`.** It is far less invasive,
and on kernel 6.8 the in-tree `mlx5` driver is current. Move to MLNX_OFED only
if you hit a specific gap (a firmware tool, or a feature the in-tree driver
lacks). The deciding factor is usually CUDA-enabled `perftest`, and that can
be built from source without replacing the whole stack — see Stage 6.

```bash
sudo apt install -y rdma-core libibverbs1 libibverbs-dev ibverbs-providers \
                    ibverbs-utils infiniband-diags librdmacm1 librdmacm-dev
```

Verify:
```bash
ibv_devices        # expect e.g. mlx5_0
ibv_devinfo        # per-port state, link_layer
rdma link show
```

Expected: at least one device, `link_layer: Ethernet` (that means RoCE, not
InfiniBand), `state: PORT_ACTIVE`.

**Establish the mapping explicitly** (`mlx5_0` is *not* an interface name):
```bash
for d in /sys/class/infiniband/*; do
  echo "$(basename $d) -> $(ls $d/device/net)"
done
```
Record the result, e.g. `mlx5_0 -> enp1s0f0`. Use that netdev everywhere below.

Rollback: `sudo apt remove --purge rdma-core ibverbs-utils infiniband-diags`.

---

## STAGE 4 — RoCEv2 verification

Confirm a **RoCEv2** GID exists on the port:
```bash
NETDEV=enp1s0f0     # from Stage 3 — substitute yours
DEV=mlx5_0
for f in /sys/class/infiniband/$DEV/ports/1/gid_attrs/types/*; do
  echo "gid $(basename $f): $(cat $f 2>/dev/null)"
done
```
Expected: at least one entry reading `RoCE v2`.

Configure the interface (the app already does this via
`configureMellanox()`; doing it by hand first isolates variables):
```bash
sudo ip addr add 192.168.1.1/24 dev $NETDEV
sudo ip link set $NETDEV mtu 9000 up
ip -br addr show $NETDEV
ethtool $NETDEV | grep -E 'Speed|Link detected'
```

Observe actual traffic — RoCEv2 is UDP/4791:
```bash
sudo tcpdump -i $NETDEV -nn -c 20 udp port 4791
```

> This confirms **network traffic characteristics only**. It does not prove
> the RDMA path works end to end. That is Stage 6.

**Gate:** a RoCEv2 GID must exist. If only `RoCE v1` appears, the transport
decision changes and must be revisited.

---

## STAGE 5 — GPUDirect RDMA capability

You have said peermem is present — confirm it is actually *loaded*, not just
installed:

```bash
sudo modprobe nvidia_peermem
lsmod | grep -E 'nvidia_peermem|nv_peer_mem|mlx5_ib|ib_core'
modinfo nvidia_peermem | head -5
dmesg | grep -i -E 'peermem|nvidia|mlx5' | tail -20
```

Persist across reboots:
```bash
echo nvidia_peermem | sudo tee /etc/modules-load.d/nvidia-peermem.conf
```

`nvidia_peermem` ships with the NVIDIA driver (Stage 2), so it appears only
after that is installed. On kernel ≥ 5.12 an alternative dma-buf path exists,
but `nvidia_peermem` remains the well-trodden route.

> **The module being loaded is capability, not proof.** The preflight
> deliberately reports GPUDirect as `UNKNOWN` at this point, never `PASS`.
> Only Stage 6 can upgrade it.

---

## STAGE 6 — Functional NIC → GPU proof

This is the gate that decides whether the architecture is real.

Ubuntu's `perftest` is built **without** CUDA, so build one that has it:

```bash
sudo apt install -y autoconf automake libtool
git clone https://github.com/linux-rdma/perftest.git
cd perftest && ./autogen.sh
./configure CUDA_H_PATH=/usr/local/cuda-13.2/include/cuda.h
make -j$(nproc) && sudo make install
ib_write_bw --help | grep use_cuda      # must now appear
```

Run the test — **two terminals, or two machines**:

```bash
# server (receiver, GPU memory)
ib_write_bw -d mlx5_0 -i 1 --use_cuda=0 -s 65536 -D 10 --report_gbits

# client (sender) — <server-ip> = 192.168.1.1
ib_write_bw -d mlx5_0 -i 1 -s 65536 -D 10 --report_gbits <server-ip>
```

`--use_cuda=0` means "use CUDA device 0 as the buffer", i.e. the NIC writes
**into GPU memory**. Record BW_average.

**Interpretation:**
- Runs and reports near line rate → **GPUDirect RDMA works.** Preflight may be
  upgraded to PASS.
- Fails with a peer-memory/registration error → `nvidia_peermem` is not
  functioning. Do **not** proceed; do not silently fall back to host memory.

Also capture the host-memory baseline (drop `--use_cuda`) — the difference is
your evidence of what GPUDirect is actually buying.

Latency baseline:
```bash
ib_write_lat -d mlx5_0 -i 1 -s 2048 -n 10000
```

Record: bandwidth (Gb/s), latency (µs), message size, QP count, CPU% during
the run (`mpstat 1`).

**Gate:** `--use_cuda` bandwidth test must pass. This is the single
non-negotiable gate before `RdmaGpuSource`.

---

## STAGE 7 — Holoscan SDK

Only after Stage 6 passes.

```bash
python3 -m venv ~/holoscan-venv
source ~/holoscan-venv/bin/activate
pip install --upgrade pip
pip install holoscan
python3 -c "import holoscan; print(holoscan.__version__)"
```

Standalone smoke test (no GUI, no RDMA):
```bash
cat > /tmp/holo_test.py <<'EOF'
from holoscan.core import Application, Operator, OperatorSpec

class Ping(Operator):
    def setup(self, spec: OperatorSpec): spec.output("out")
    def compute(self, op_input, op_output, context):
        op_output.emit({"n": 42}, "out")

class Echo(Operator):
    def setup(self, spec: OperatorSpec): spec.input("in")
    def compute(self, op_input, op_output, context):
        print("Echo received:", op_input.receive("in"))

class App(Application):
    def compose(self):
        p, e = Ping(self, name="ping"), Echo(self, name="echo")
        self.add_flow(p, e)

if __name__ == "__main__":
    App().run()
    print("HOLOSCAN OK")
EOF
python3 /tmp/holo_test.py
```
Expected: the dict prints, then `HOLOSCAN OK`, clean exit.

> Holoscan is C++/Python with its own CUDA expectations. If the pip wheel
> conflicts with your CUDA 12.4, use the NGC container instead of forcing
> versions on the host — that is what dependency isolation (§18) is for.

---

## STAGE 8 — Python DSP environment

```bash
source ~/holoscan-venv/bin/activate
pip install numpy scipy
# CuPy wheels are built per CUDA major version. CUDA 13 is recent, so check
# what exists before assuming; cupy-cuda12x will NOT match a CUDA 13 runtime.
pip index versions cupy-cuda13x 2>/dev/null || echo "no cuda13x wheel yet"
pip install cupy-cuda13x    # if unavailable, keep DSP in C++/CUDA for now
python3 -c "import numpy,scipy;print(numpy.__version__, scipy.__version__)"
python3 -c "import cupy;cupy.zeros(8);print('CUPY OK')"
```

**Placement rule:** NumPy/SciPy operate on the **reduced** result (~2 k points
per frame), never on the 200 MS/s stream. CuPy is the GPU-side equivalent and
avoids the download entirely for GPU-native steps.

---

## Bring-up checklist

```
[x] NVIDIA GPU present            -- RTX A4000, BDF 3b:00.0
[x] GPU class supports GPUDirect  -- Professional (RTX A)
[x] NVIDIA driver working         -- 595.84
[x] CUDA reported by driver       -- 13.2
[x] Mellanox NIC present          -- dual port, BDF 3c:00.0/.1
[ ] ** NIC LINK UP (Link detected: yes) **   <-- CURRENT BLOCKER
[ ] GPU and NIC on same PCIe root (nvidia-smi topo -m)
[ ] Persistence mode enabled (nvidia-smi -pm 1)
[ ] Secure Boot state known
[ ] Inventory captured (tools/inventory.sh)
[ ] Toolchain + kernel headers installed
[ ] NVIDIA driver installed, nvidia-smi works
[ ] CUDA toolkit installed, nvcc works
[ ] CUDA functional test prints "CUDA OK"
[ ] RDMA stack decision recorded (rdma-core vs MLNX_OFED)
[ ] ibv_devices lists a device
[ ] RDMA device -> netdev mapping recorded explicitly
[ ] link_layer == Ethernet, state == PORT_ACTIVE
[ ] RoCEv2 GID present
[ ] IP + MTU 9000 configured, link up
[ ] tcpdump shows UDP/4791 traffic
[ ] nvidia_peermem loaded and persisted
[ ] perftest rebuilt with CUDA support
[ ] ib_write_bw --use_cuda PASSES        <-- hard gate
[ ] Host-memory baseline recorded for comparison
[ ] Latency baseline recorded
[ ] Holoscan installed, standalone test prints "HOLOSCAN OK"
[ ] NumPy/SciPy (and CuPy if used) verified
[ ] ./build/sdr_workstation --gpu-rdma-preflight => READY
[ ] ONLY THEN: begin RdmaGpuSource
```

---

## What the preflight will report, and why it is honest

`--gpu-rdma-preflight` uses five states and never guesses:

| state | meaning |
|---|---|
| `PASS` | verified from evidence on this machine |
| `FAIL` | verified absent or broken |
| `UNKNOWN` | capability visible, functional proof missing |
| `NOT INSTALLED` | component not found |
| `NOT APPLICABLE` | not needed for the chosen architecture |

GPUDirect can only ever reach `UNKNOWN` from sysfs inspection — the module
being loaded says nothing about whether a registration actually succeeds.
Stage 6 is what upgrades it to `PASS`, and only a human running that test can
do so. This is deliberate: rule 10 forbids claiming zero-copy without
verification.

---

## Rivermax — explicitly not used

**`Rivermax: NOT used in the primary RoCEv2 path.`**

Rivermax is a kernel-bypass stack for UDP media streams (SMPTE ST 2110). It
has no queue pairs, memory regions or completion queues, and cannot receive
RoCEv2, which is RDMA. Installing it would add a licensed dependency that
contributes nothing to this path.

It becomes relevant only if a *separate* requirement appears involving genuine
UDP media traffic. Do not install it for this work.

---

## Integration design (for after the gates pass)

```
RdmaGpuSource : public ISignalSource     <-- the only new class
    start()  : ibv_open_device → PD → CQ → QP
               cudaMalloc(arena) → ibv_reg_mr(arena)   <-- GPU-resident
               post RECVs, QP → RTR → RTS
    poll()   : ibv_poll_cq → completion refers to GPU memory
    publish(): SampleBlock carrying a DEVICE pointer
```

`SampleBlock` currently holds host memory (`QByteArray raw`). It needs a
device-buffer variant so the DSP stage can run cuFFT in place. That is a
contained change to one struct plus the DSP entry point — **not** a rewrite,
and it stays behind `ISignalSource`, so the GUI, plots, channel selection and
frequency measurement are untouched.

If the device variant is ever unavailable and the code falls back to host
memory, that must be **reported in the GUI status**, never silent (rule 16).

---

## Quick reference — what to send back

After Stage 0, send:

```bash
./tools/inventory.sh > inventory.txt 2>&1
./build/sdr_workstation --gpu-rdma-preflight > preflight.txt 2>&1
```

Those two files settle: whether the hardware exists, which RDMA stack to use,
whether RoCEv2 is real, and whether GPUDirect is achievable — and the exact
versions to pin in every command above.
