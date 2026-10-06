# RoCEv2 IQ Streaming — ConnectX-5 → GPU → Interactive Holoscan

A complete, working SDR-style pipeline for one Ubuntu 22.04 machine with an
NVIDIA RTX A4000 and a dual-port Mellanox/NVIDIA ConnectX-5 100 GbE NIC
whose Port A is cabled directly to Port B:

```
 ┌───────────────────────────── one physical host ─────────────────────────────┐
 │  namespace ns_tx                          default namespace                  │
 │  ┌────────────────┐                       ┌─────────────────────┐            │
 │  │    rdma_tx     │   RoCEv2 (RC QPs,     │ rdma_rx   (host shm)│            │
 │  │ IQ generator,  │   UDP/4791, MTU 9000) │   — or —            │            │
 │  │ zero-copy SEND ├──► Port A ══cable══►  │ rdma_rx_gpu         │            │
 │  │ live-tunable   │            Port B ───►│  (GPUDirect: NIC    │            │
 │  └───────▲────────┘                       │   DMAs into VRAM)   │            │
 │          │ /dev/shm/iqctl                 └──────────┬──────────┘            │
 │          │ (live pace/fs/fc)                         │ shm ring / CUDA IPC   │
 │          │                                ┌──────────▼──────────┐            │
 │          └────────────────────────────────┤ holoscan_iq_viz.py  │            │
 │                                           │ cuFFT DSP + Holoviz │            │
 │                                           │ labeled axes, zoom/ │            │
 │                                           │ pan, markers, live  │            │
 │                                           │ RF readout & control│            │
 │                                           └─────────────────────┘            │
 └──────────────────────────────────────────────────────────────────────────────┘
```

Each RDMA SEND carries one **frame**: a 64-byte header plus 1 MiB of
interleaved `int16 I, int16 Q` (262,144 IQ pairs). Two receivers are
provided. `rdma_rx` lets the NIC DMA frames into host shared memory and the
visualizer makes one host→GPU copy per displayed frame. `rdma_rx_gpu` goes
further: the NIC DMAs payloads **straight into A4000 memory** (GPUDirect
RDMA) and the visualizer opens that ring zero-copy via CUDA IPC — sample
data never touches system RAM. The visualizer auto-detects which one is
running.

**Hardware expectation.** The card sits in PCIe Gen3 x8: 8 × 8 GT/s ×
128/130 ≈ 63 Gb/s raw, of which RDMA realistically delivers **50–55 Gb/s**.
The 100 GbE link is never the bottleneck; `ib_write_bw` at ~6.3–6.8 GB/s
means everything is healthy. GPUDirect on this box buys latency and CPU
offload, not extra throughput (both slots share the same ceiling).

---

## 1. What goes where

```
roce-iq-holoscan/
├── Makefile
├── README.md                  ← you are here
├── VALIDATION.md              GPU-pipeline analysis + 100-link methodology
├── src/
│   ├── rdma_common.h          wire format, shm ring ABI, live-control ABI
│   ├── rdma_tx.c              IQ generator + sender; live pace/fs/fc
│   ├── rdma_rx.c              receiver → host shm ring
│   ├── rdma_rx_gpu.c          receiver → GPU VRAM ring (GPUDirect + CUDA IPC)
│   ├── scale_common.h         payload pattern + latency histogram helpers
│   ├── scale_tx.c             1..128-link load generator
│   └── scale_rx.c             1..128-link validation sink (CSV/JSON, verdict)
├── viz/
│   ├── holoscan_iq_viz.py     interactive GPU visualizer (both ring types)
│   └── gpu_bw_probe.py        GPU memory-bandwidth headroom probe
└── scripts/
    ├── 01_install_base.sh     OS packages (RDMA stack, tools, Vulkan)
    ├── 02_setup_network.sh    namespace + IPs + MTU 9000 + cable test
    ├── 03_verify_rdma.sh      rping + ib_write_bw sanity benchmark
    ├── 04_scale_validate.sh   100-link sweep/soak harness with PASS/FAIL
    ├── 05_gpudirect_setup.sh  nvidia-peermem, rdma group, memlock
    ├── 06_verify_rdma_write.sh proves one-sided RDMA (counters + wire opcode)
    ├── 07_selftest.sh         offline ABI + build + mode-parity checks
    └── 99_teardown.sh         undo the network setup
```

Install order: 2 (hardware check) → 3 (base packages) → 4 (NVIDIA + CUDA)
→ 5 (RDMA stack choice) → 6 (network) → 7 (verify) → 8 (build + Python) →
9 (run) → 10 (drive it). GPUDirect and 100-link validation are sections
11–12.

---

## 2. Hardware sanity check

Seat the ConnectX-5, connect a DAC or transceivers + fiber between its two
ports, then confirm what PCIe width Linux negotiated:

```bash
lspci -d 15b3: -vvv | grep -E "ConnectX|LnkCap:|LnkSta:"
```

`LnkSta: Speed 8GT/s, Width x8` confirms the Gen3 x8 situation above. If
`LnkSta` shows less than the slot should provide, power down and re-seat.

---

## 3. Base packages

```bash
cd roce-iq-holoscan
sudo ./scripts/01_install_base.sh
```

Installs the inbox RDMA userspace (`rdma-core`, `ibverbs-*`,
`rdmacm-utils`), the dev headers this project compiles against,
diagnostics (`perftest`, `infiniband-diags`) and the Vulkan loader
Holoviz needs. The mlx5 kernel driver ships with Ubuntu's kernel.

---

## 4. NVIDIA driver + CUDA 12

Holoscan's pip wheels need the NVIDIA driver (≥ 535; R550+ is what NVIDIA
tests) and the CUDA 12 runtime; the GPUDirect receiver additionally needs
the CUDA toolchain to build. Clean path on Ubuntu 22.04:

```bash
wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.1-1_all.deb
sudo dpkg -i cuda-keyring_1.1-1_all.deb
sudo apt-get update
sudo apt-get install -y cuda-drivers cuda-toolkit-12-6
sudo reboot
```

Verify, then add CUDA to your path in `~/.bashrc`:

```bash
nvidia-smi                                   # lists "NVIDIA RTX A4000"
ls /usr/local/cuda/lib64/libcudart.so.12     # runtime present
export PATH=/usr/local/cuda/bin:$PATH
export LD_LIBRARY_PATH=/usr/local/cuda/lib64:$LD_LIBRARY_PATH
```

---

## 5. RDMA stack: inbox vs. DOCA-OFED

**Option A — inbox (recommended start).** Already installed by section 3;
fully supports RoCEv2 on ConnectX-5. Skip to section 6.

**Option B — NVIDIA DOCA-OFED.** Successor to standalone MLNX_OFED (final
standalone release was late 2024). Brings NVIDIA's tuned drivers and
firmware tools (`mlxconfig`, `mlxup`):

```bash
wget -qO - https://linux.mellanox.com/public/repo/doca/GPG-KEY-Mellanox.pub | \
  sudo gpg --dearmor -o /usr/share/keyrings/GPG-KEY-Mellanox.pub
echo "deb [signed-by=/usr/share/keyrings/GPG-KEY-Mellanox.pub] \
https://linux.mellanox.com/public/repo/doca/latest/ubuntu22.04/x86_64 /" | \
  sudo tee /etc/apt/sources.list.d/doca.list
sudo apt-get update && sudo apt-get install -y doca-ofed
sudo reboot
```

Current firmware never hurts: `sudo mlxup`. Both options expose the same
`libibverbs`/`librdmacm` API, so all code here runs identically.

---

## 6. Network configuration (namespace, IPs, MTU 9000)

**Why a namespace?** Both ports live in one kernel; traffic between two
local IPs short-circuits over loopback and never touches the cable or the
NIC's RDMA engine. Moving Port A (netdev *and* RDMA device) into namespace
`ns_tx` makes the ports behave like two machines, forcing real RoCEv2
packets across the cable.

Find your names, then run the script with them:

```bash
rdma link
ls /sys/class/infiniband/mlx5_0/device/net
ls /sys/class/infiniband/mlx5_1/device/net
sudo IF_TX=enp1s0f0np0 IF_RX=enp1s0f1np1 ./scripts/02_setup_network.sh
```

It enables `rdma system set netns exclusive` (falling back with a warning
if Docker blocks it — shared mode still works here), creates `ns_tx`,
moves Port A + `mlx5_0` in, assigns `192.168.100.1/24` (A) and
`192.168.100.2/24` (B), sets MTU 9000 on both, and proves the path with a
do-not-fragment jumbo ping. Config does not survive reboot — re-run it.

**RoCEv2 in one paragraph.** RoCEv2 is InfiniBand transport inside UDP/IP
on destination port 4791 — it routes like IP, and `tcpdump udp port 4791`
on Port B shows your RDMA traffic. `rdma_cm` resolves the right RoCEv2 GID
from the IPs, which is why this demo uses it instead of raw verbs
plumbing. With netdev MTU 9000 the RDMA path runs `active_mtu = 4096`,
the top of the RDMA MTU ladder (256/512/1024/2048/4096).

---

## 7. Verify RDMA across the cable

```bash
sudo ./scripts/03_verify_rdma.sh
```

Shows `ibv_devinfo` in both namespaces (expect `PORT_ACTIVE`,
`active_mtu: 4096`), runs an `rping` handshake, then `ib_write_bw -R
--report_gbits -a`. Large messages leveling at **50–55 Gb/s** = the PCIe
ceiling = healthy NIC, cable, firmware and stack.

---

## 8. Build everything and prepare Python

```bash
make            # rdma_tx, rdma_rx, scale_tx, scale_rx
make gpu        # rdma_rx_gpu -- requires CUDA from section 4
```

Python environment (Holoscan ships Ubuntu 22.04 x86_64 wheels; they need
the system CUDA 12 runtime + driver from section 4, the Vulkan loader from
section 3, and a local display session):

```bash
python3 -m venv ~/holoscan-venv
source ~/holoscan-venv/bin/activate
pip install --upgrade pip
pip install holoscan cupy-cuda12x numpy
python -c "import holoscan, cupy; print('holoscan', holoscan.__version__)"
```

---

## 9. Run it (three terminals)

**Terminal 1 — receiver** (Port B, default namespace):

```bash
sudo ./rdma_rx -a 192.168.100.2          # default transport: write_imm
```

**Terminal 2 — visualizer + built-in control panel** (normal user, in the
venv, on the machine's own display):

```bash
source ~/holoscan-venv/bin/activate
python viz/holoscan_iq_viz.py            # opens the plot window AND the GUI panel
```

This is now a single app: the Holoscan plot window and a Tk control panel
open together. The panel has sliders and boxes for every live parameter —
sample rate, centre frequency and link pacing drive the running
transmitter; FFT size, span, centre, averaging, reference level, dB range,
time-window width, amplitude, interpolation, decimation, pause and auto-fit
drive the display, with one-click Fs/Fc presets. Keyboard shortcuts and the
console still work alongside it. Tkinter ships with the system `python3`; if
the panel is missing run `sudo apt-get install -y python3-tk` (the plot
still works without it). Launch with `--no-panel` to suppress it.

**Terminal 3 — sender** (inside `ns_tx`, paced to 10 Gb/s):

```bash
sudo ip netns exec ns_tx ./rdma_tx -s 192.168.100.2 -g 10   # default: clean single tone
```

The default waveform is now a **clean single complex tone** at −0.25·fs —
one sharp spectral line and a smooth sine in the time domain. Options:
`-T 0.1` puts the tone at +0.1·fs, `-N 0.02` adds a light noise floor (off
by default). The tone is snapped to an exact integer number of cycles over
the internal table so it loops seamlessly — no spectral splatter.

**Both ends must use the same transport.** The default is now
`write_imm` (genuine one-sided RDMA WRITE — see section 11.5). To pick a
mode explicitly, pass the same `-w` to both:

```bash
sudo ./rdma_rx -a 192.168.100.2 -w send                     # two-sided
sudo ip netns exec ns_tx ./rdma_tx -s 192.168.100.2 -g 10 -w send
```

A window appears: I/Q time domain (cyan/orange) with time and amplitude
axes, the spectrum in dBFS (green) with an absolute frequency axis around
the advertised center frequency — a fixed CW tone at −0.25·fs plus a chirp
sweeping the band — and an RF panel (peak power/frequency, noise floor,
SNR, 99 % occupied bandwidth, RMS, PAPR, RBW, link Gb/s, sequence/gaps).
The time view opens zoomed to a readable ~2.5 µs so you see individual
cycles, not a solid block; press **`w`** to auto-fit it to ~10 cycles of
the strongest tone, **`+`/`-`** to zoom further, and **drag the window
edge or corner to resize** — the plots reflow to fill the new size. To
retune the **running transmitter live**, type into the console: `fs
122.88M`, `fc 3.5G`, or `pace 25` then Enter; the frequency axis and RF
panel follow immediately. Everything updates live; the same numbers print
once a second in the console. Stop anything with Ctrl-C in any order — the
receiver survives sender restarts and the visualizer survives receiver
restarts.

---

## 10. Driving it: interactive controls & live runtime configuration

All visual state is adjustable live with the window focused, and every
control also exists as a console command (type into terminal 2; `h` or
`help` reprints the reference):

| Area | Keys / mouse | Effect |
|---|---|---|
| Time domain | ← → (Shift = fine) · + − · a/z | pan · zoom · amplitude scale |
| | c · , . · click in plot | cursor on/off · move cursor · place cursor |
| Spectrum | s/S · n/m · g | span zoom in/out · pan · center on peak |
| | [ ] · { } | reference level · dB range |
| | k / K · click in plot | marker at peak / clear · marker at click |
| | scroll · drag | zoom about pointer · pan |
| DSP | f/F · v (V) | FFT 1k–64k · cycle averaging 1–32 (reset) |
| | d/D · e/E | decimation 1–8 · interpolation 1–8 |
| Misc | space/p · 0 · h · q | pause · reset views · help · quit |

Axes are real: time ticks auto-scale through s/ms/µs/ns as you zoom,
amplitude labels follow the gain, the frequency axis shows absolute Hz
(center frequency ± span) with 1-2-5 ticks, and the dB axis follows your
reference level and range. The footer shows the active FFT size,
averaging, view width, amplitude gain and span at all times; the RF panel
adds RBW (Hann-corrected), effective sample rate after L/D resampling, and
marker/cursor readouts with delta-to-peak.

**Live transmitter control** goes through a tiny shared-memory block
(`/dev/shm/iqctl`) that `rdma_tx` polls once a second — `/dev/shm` is
shared across network namespaces, so the visualizer can steer a sender
running inside `ns_tx`. Console commands, with k/M/G suffixes:

```
pace 25         # retune pacing to 25 Gb/s, no restart, no burst
pace 0          # unlimited (pins the PCIe ceiling)
fs 122.88M      # advertised sample rate  -> all axes/RF math follow live
fc 3.5G         # center frequency        -> frequency axis re-labels live
fft 32768       # finer RBW;  avg 8       # smoother floor
span 10M        # zoom spectrum to 10 MHz;  center 3.51G;  marker 3.475G
```

Interpolation/decimation (`e/E`, `d/D`) run a windowed-sinc FIR on the GPU
(via `cupyx.scipy.signal.fftconvolve`); the effective sample rate, both
axes, RBW and all RF parameters update to match. Changing fs/fc is pure
metadata (the waveform is normalized to fs), so those are instant and
glitch-free; pacing changes rebase the token bucket so the rate ramps
smoothly instead of bursting.

---

## 11. RDMA transport modes — SEND vs. one-sided WRITE

If you ran an earlier build and watched the wire, you saw ordinary
two-sided messaging, not the "reach into the peer's memory" pattern people
picture as RDMA. That was real RoCEv2 (RC queue pairs over the 100 G link),
but the *operation* posted was **SEND/RECV**, which behaves like a fast
message channel. The opcode is now selectable with `-w` on both ends:

| `-w` mode | Operation | Receiver CPU per frame | Flow control | Per-frame signal |
|---|---|---|---|---|
| `send` | two-sided SEND/RECV | consumes a posted RECV | RNR (automatic) | yes (completion) |
| `write` | one-sided RDMA WRITE | none — polls memory | sender's job | none |
| `write_imm` | RDMA WRITE_WITH_IMM | consumes a RECV for the immediate | RNR (automatic) | yes (immediate) |

**Why SEND was the original default, and why `write_imm` is the new one.**
Pure WRITE is maximally one-sided — the sender writes frames straight into
the receiver's advertised ring and the receiver's CPU posts nothing — but
it gives no arrival signal (the receiver must poll memory) and no built-in
flow control (a fast sender can lap a slot mid-read). `write_imm` keeps the
one-sided write but attaches a 32-bit immediate (the slot index) that lands
as a receive completion, so you get true one-sided RDMA **and** a per-frame
signal **and** RNR flow control. That is what most FPGA RDMA cores emit,
which is why it is the recommended production mode and the new default.

How the ring is advertised: at connect time the receiver registers its ring
with `IBV_ACCESS_REMOTE_WRITE` and passes the region's address + rkey to the
sender in the rdma_cm connection `private_data` (`struct rdma_region_info`).
The sender then targets `ring + ctrl + slot*stride` with each WRITE, laying
header+payload down in exactly the same byte layout SEND produced — so the
visualizer's parser is identical for all three modes.

### 11.5 Verifying it is genuinely one-sided RDMA (two ways)

```bash
sudo ./scripts/06_verify_rdma_write.sh                 # write_imm, 5 s
sudo MODE=send ./scripts/06_verify_rdma_write.sh       # contrast run
```

The script proves the opcode two independent ways and prints both:

**Method A — NIC hardware counters.** It diffs
`/sys/class/infiniband/<dev>/ports/1/hw_counters/` around a short burst. In
`write`/`write_imm` the `tx_write_requests` / `rx_write_requests` counters
climb; in `send` they stay flat. That delta is the hardware's own statement
that RDMA WRITE operations crossed the link. (You can also watch live with
`watch -n1 'grep -r . /sys/class/infiniband/mlx5_1/ports/1/hw_counters/ | grep write'`.)

**Method B — RoCEv2 opcode on the wire.** It captures UDP/4791 with tcpdump
and, if `tshark` is installed, decodes the InfiniBand BTH opcode: the WRITE
family (`0x06` WRITE_Only, `0x0a`/`0x0b` First/Last, `0x0c` Only_with_IMM)
versus the SEND family (`0x00`–`0x05`). Seeing `RDMA_WRITE_Only_with_IMM`
on the wire is direct, packet-level proof. Manual version:

```bash
sudo tcpdump -i <PortB-netdev> -s 96 udp port 4791 -w roce.pcap
wireshark roce.pcap          # filter: infiniband.bth.opcode
```

A clean way to internalize the difference: run the verify script once with
`MODE=send` and once with `MODE=write_imm` and compare the
`*_write_requests` deltas — zero versus millions.

---

## 12. GPUDirect mode (experimental — needs P2P-capable PCIe topology)

**The CPU-staging receiver (`rdma_rx`) is the supported default on this
machine.** GPUDirect RDMA is a PCIe *peer-to-peer* transaction, and many
desktop boards do not route P2P between devices that sit under **sibling
root ports** — which is exactly this box's layout:

```
+-01.0-[01]--  NVIDIA RTX A4000
+-01.1-[02]--  Mellanox ConnectX-5 Ex
```

The treacherous part: on such boards `nvidia-peermem` loads, `ibv_reg_mr`
on GPU memory succeeds, and the receiver even reports full line rate with
zero gaps — completions fire whether or not the chipset actually delivered
the payload writes to VRAM. The only trustworthy check is **empirical**:
run the visualizer and look for the test signal (CW tone at fc − 0.25·fs
plus the chirp). Tone visible → P2P works; empty or garbage spectrum while
`rdma_rx` shows it fine → your board doesn't route P2P, stay on `rdma_rx`.
(`iommu=pt` is worth one try; a server/workstation board or a PCIe switch
is the real fix. The CPU path costs one `cudaMemcpy`-equivalent per
*displayed* frame — at 60 fps that's ~60 MB/s of the ~25 GB/s host-to-GPU
budget, i.e. nothing.)

If your topology does support it:

```bash
sudo ./scripts/05_gpudirect_setup.sh    # peermem module, rdma group, memlock
# re-login so group + memlock apply, then:
make gpu
./rdma_rx_gpu -a 192.168.100.2          # NO sudo -- same user as the viz
python viz/holoscan_iq_viz.py           # banner shows [GPUDirect, zero-copy]
sudo ip netns exec ns_tx ./rdma_tx -s 192.168.100.2 -g 10 -w send
```

GPUDirect mode is **SEND-only** (`rdma_rx_gpu` accepts `-w send`): a
one-sided WRITE into GPU memory would need the header and payload in two
separate memory regions — host and VRAM — i.e. two WRs per frame, which is
pointless on a board that can't route NIC↔GPU P2P anyway. If you want to
exercise one-sided RDMA, use the host-staging receiver (`rdma_rx -w
write_imm`), which is also the supported path on this machine. So run the
sender with `-w send` to match.

How it works: each receive WR carries two scatter entries — the NIC drops
the 64-byte header into host shm (so the CPU loop and Python read metadata
cheaply) and the 1 MiB payload into a `cudaMalloc`'d ring. The receiver
exports that ring's `cudaIpcMemHandle_t` through the shm control block;
the visualizer opens it with CuPy and reads frames zero-copy. Running both
processes as the **same non-root user** matters because CUDA IPC is
per-UID — that's why script 05 puts you in the `rdma` group instead of
reaching for sudo.

---

## 13. Scaling: validating 100 simultaneous RoCEv2 links

`scale_tx`/`scale_rx` create 1–128 independent RC connections over the
cable and measure aggregate throughput, per-link integrity (every 8-byte
lane verifiable), loss (sequence gaps — must be zero on RC), one-way
latency histograms (valid because both clocks are this machine), with NIC
error-counter snapshots around every step:

```bash
sudo ./scripts/04_scale_validate.sh                   # 1, 10, 50, 100 links
sudo DUR=3600 STEPS="100" ./scripts/04_scale_validate.sh    # 1-hour soak
python viz/gpu_bw_probe.py --secs 60                  # GPU mem-BW headroom
```

Results land in `scale_results/` (per-second CSV, JSON summary, counter
diffs) with a PASS/FAIL verdict per step. The complete methodology — what
each metric proves, pass criteria, how to read failures, and how it
extends beyond one box — is `VALIDATION.md` Part 2.

---

## 14. How it actually works (code tour)

**Wire format** (`src/rdma_common.h`). One SEND = one frame = 64-byte
header (`magic, seq, timestamp, n_samples, sample_rate_hz,
center_freq_hz`) + payload. Struct offsets are locked with
`_Static_assert` because Python parses them by byte offset; the same file
defines the host ring, the GPUDirect ring layout, and the live-control
block.

**Sender** (`rdma_tx.c`). Synthesizes a 64 MiB waveform table once
(tone + chirp + noise, all normalized to fs), registers it once, then
ships every frame zero-copy with a 2-SGE list (header + table slice). In
`-w send` it posts `IBV_WR_SEND`; in `-w write`/`write_imm` it posts
`IBV_WR_RDMA_WRITE[_WITH_IMM]` targeting the slot address the receiver
advertised at connect, so header+payload land contiguously in remote
memory. Up to 32 WRs in flight on an RC QP, signaled every 8th to cut
completion overhead (RC still delivers in order); `rnr_retry = 7` lets the
NIC back off if the receiver runs dry. Each second it polls
`/dev/shm/iqctl` and applies pace/fs/fc changes, rebasing the pacing token
bucket so rate changes are smooth.

**Receivers.** `rdma_rx.c` registers the whole host ring as one memory
region (with `IBV_ACCESS_REMOTE_WRITE` in the WRITE modes) and advertises
its address+rkey in the rdma_cm `private_data`. In `send`/`write_imm` it is
CQ-driven (a completion per frame — the immediate carries the slot index in
`write_imm`); in pure `write` it polls the ring headers for the next
expected sequence number, since no completions arrive. Either way it
validates magic, accounts sequence gaps, and publishes by bumping
`write_count` with release semantics — drop-oldest by construction, so a
slow reader never back-pressures the link. `rdma_rx_gpu.c` is the same
skeleton with payloads in VRAM (SEND-only; see section 12).

**Visualizer** (`viz/holoscan_iq_viz.py`). Three Holoscan operators.
`RingSource` opens whichever ring exists, paces to `--fps`, grabs the
newest frame with a torn-read guard (re-checks `write_count` after the
copy; with 32 slots a frame would need to age ~28 frames mid-copy to be
corrupt) and emits a CuPy array — `cp.asarray` of host memory in shm mode,
a device-to-device slot copy in GPUDirect mode. `SpectrumDSP` does
everything on the GPU: optional L/D windowed-sinc resampling, Hann FFT
(1k–64k live), exponential PSD averaging, min/max envelope decimation for
the oscilloscope view, span/center slicing for spectrum zoom, and RF
estimation on the full-resolution averaged PSD (peak, median floor, SNR,
99 % OBW via the power CDF, RMS, PAPR, RBW). It then builds the entire
scene — traces, grid, tick marks, tick labels, cursor, marker, RF panel,
footer — as Holoviz tensors plus dynamic `InputSpec`s so all text updates
every frame. `HolovizOp` renders and feeds GLFW keyboard/mouse events back
into the shared UI state (with a console REPL as the always-works
fallback; `--no-dynamic-text` exists for Holoscan builds that reject the
dynamic-spec port).

---

## 15. Tuning and where to go next

The demo is near the practical optimum for its NIC generation: zero-copy
send, zero-copy receive (GPUDirect), NIC-level flow control, single
worker-core control plane. Worthwhile knobs on this exact box: pin the
receiver to a core on the NIC's NUMA node (`taskset`), make memlock
unlimited permanently (script 05 does), and grow `FRAME_SAMPLES` if you
want fewer completions per second. The next platform rungs — GPU-driven
networking with DOCA GPUNetIO, Rivermax, Holoscan's Advanced Network
Operator — need a ConnectX-6 Dx/BlueField and are mapped out in
`VALIDATION.md` Part 1.

---

## 16. Troubleshooting

| Symptom | Cause → fix |
|---|---|
| `rdma_resolve_addr` / `ADDR_ERROR` in tx | IPs/namespace not set → re-run `02_setup_network.sh` |
| CM event `REJECTED` on connect | receiver not running on Port B, or wrong `-s` IP |
| `rdma system set netns exclusive` fails | Docker holds a netns → script falls back to shared mode; demo still works |
| `ibv_reg_mr` fails / `ENOMEM` | memlock → `ulimit -l unlimited`, or script 05's limits.conf entry + re-login |
| Jumbo ping fails, normal ping works | MTU mismatch → both ports 9000; re-seat DAC/transceivers |
| `active_mtu: 1024` in ibv_devinfo | netdev MTU was 1500 at connect time → set 9000 first |
| Holoviz window won't open / Vulkan error | no local display / loader → run on the desktop session; `vulkaninfo --summary` must list the A4000 |
| `ibv_reg_mr on GPU memory failed` | `sudo modprobe nvidia-peermem` (script 05); persists via modules-load.d; if still failing add `iommu=pt` to GRUB |
| GPUDirect "works" (line rate, gaps 0) but spectrum is empty/garbage | board doesn't route NIC↔GPU P2P (sibling root ports) → use `rdma_rx`; see section 11 |
| `Permission denied` on `/dev/shm/iqctl` or `iqring` after mixed sudo/user runs | Ubuntu `fs.protected_regular` blocks O_CREAT on another user's shm file → fixed in this version (binaries retry plain open); rebuild with `make`, or clear stale files: `sudo rm /dev/shm/iqring /dev/shm/iqctl` |
| Bare `Killed` right after the Vulkan banner | 8 MB stack < Holoscan's 32 MB minimum → the viz now raises its own limit; manual fallback `ulimit -s 32768` |
| `NotImplementedError: Only int or ndarray ... searchsorted` | older copy of the viz → fixed (CuPy needs array thresholds); update `viz/holoscan_iq_viz.py` |
| `CUDA IPC open failed` in the viz | rdma_rx_gpu and the viz must be the SAME user → run both as you (rdma group), not one under sudo |
| Mouse/keys do nothing in the window | older Holoscan build without window callbacks → console commands still drive everything |
| `input_specs` port rejected at startup | run with `--no-dynamic-text` (plots stay; numbers on console) |
| Python can't open `/dev/shm/iqring` | receiver not started yet, or stale ring → `sudo rm /dev/shm/iqring`, restart receiver |
| Throughput pinned at 50–55 Gb/s at `-g 0` | that *is* PCIe Gen3 x8 — working as intended |
| scale run FAILs with gaps | receiver WQE starvation → larger `-b`, higher rx `-o`, taskset to NIC-NUMA core (see VALIDATION.md) |
| `receiver did not advertise an RDMA region` in tx | sender in write/write_imm but receiver in a different `-w` mode → match `-w` on both ends |
| Everything dies after reboot | namespace config is not persistent → re-run `02_setup_network.sh` |

---

## 17. Safe teardown

```bash
sudo ./scripts/99_teardown.sh   # removes ns_tx + ring/ctl files, restores shared mode
```
