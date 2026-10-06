# RoCEv2 / CPU / GPU / Holoscan — inspection and integration

## Phase 1 — Inspection findings

The supplied `roce-iq-holoscan` is a **complete, working, well-documented
pipeline**, not fragments. It should be integrated, not rebuilt.

| component | role |
|---|---|
| `src/rdma_tx.c` | IQ generator + RoCEv2 sender, live-tunable via `/dev/shm/iqctl` |
| `src/rdma_rx.c` | receiver → **host** shm ring `/dev/shm/iqring` |
| `src/rdma_rx_gpu.c` | receiver → **GPU VRAM** ring (GPUDirect + CUDA IPC) |
| `src/rdma_common.h` | **the ABI** — wire format, ring layout, control block |
| `viz/holoscan_iq_viz.py` | cuFFT DSP + Holoviz; auto-detects which receiver runs |
| `viz/control_panel.py` | live pace / fs / fc control |
| `scripts/01..07` | install, network, RDMA verify, GPUDirect setup, selftest |

### The wire/ring contract (from `rdma_common.h`, guarded by `_Static_assert`)

```
/dev/shm/iqring
  [0 .. 4095]   struct ring_ctrl   magic "IQRING01" (host) / "IQRINGG1" (GPU)
                  num_slots@12  slot_stride@16  frame_samples@20
                  sample_rate@24  center_freq@32
                  write_count@40  bytes_total@48  seq_gaps@56
  [4096 ..]     32 slots x 1,052,672 B
                  each: 64 B frame_hdr + 1 MiB payload
                  frame_hdr: magic@0 seq@8 t_ns@16 n_samples@24
                             sample_rate@32 center_freq@40
payload = 262,144 interleaved int16 I, int16 Q
```

**Key finding:** the payload is interleaved `int16 I / int16 Q` — byte-for-byte
identical to this application's existing `SampleFormat::Cs16`. No new decoder
is needed; the existing `SampleCodec` handles it unchanged.

**Second key finding:** the producer's overwrite policy is *drop-oldest* and
the reader is expected to take the newest published slot, so a slow visualiser
never back-pressures the link. That is exactly how the existing `DmaSource`
treats the FPGA stream, so the semantics already match.

**Third:** the topology is a **single-host loopback** — ConnectX-5 Port A
cabled to Port B, TX in network namespace `ns_tx`. This explains the earlier
`flags=4099` (no `RUNNING`) observation: **the two ports must be cabled to
each other**, and `scripts/02_setup_network.sh` must have been run. That is
the current blocker, and it is a cabling/setup step, not a software defect.

---

## Phase 4/5 — Integration point chosen

`ISignalSource` again. One new class, no changes above it:

```
ISignalSource
 ├── SimulatedSource
 ├── FileSource
 ├── DmaSource          (PCIe / iwfg)
 └── RoceShmSource      (NEW — reads /dev/shm/iqring)
```

`src/core/RoceShmSource.{h,cpp}` mmaps the ring read-only, polls
`write_count`, takes the newest slot, and republishes the payload as a
`SampleBlock` carrying raw `Cs16` bytes. It **re-implements no networking**:
`rdma_rx` continues to do all RoCEv2 work, unmodified.

It also adopts `sample_rate_hz` and `center_freq_hz` from the ring's control
block, so the axes match the transmitter without the operator retyping them.

---

## Phase 6 — Evidence that it works

Because the ABI is fully specified, the reader can be validated **without the
NIC**. `tools/fake_iqring.c` is a byte-compatible producer that emits a known
CW tone into a real `/dev/shm/iqring`.

End-to-end through the **real** `RoceShmSource` → **real** `DspEngine`:

```
STATUS: RoCEv2 ring mapped: 32 slots x 1052672 B, 262144 samples/frame
blocks received from ring : 463
measured tone             : -9.9999 MHz   (expected -10.0000)
error                     : 0.063 kHz
verdict                   : PASS
```

Slot stride 1,052,672 B and 262,144 samples/frame match `rdma_common.h`
exactly, confirming the geometry is parsed correctly rather than assumed.

**GPU-ring refusal test** — a control block carrying `IQRINGG1`:

```
ERROR : /dev/shm/iqring is a GPUDirect ring (magic IQRINGG1): sample payloads
        live in GPU VRAM and are not host-readable. Use the Holoscan
        visualiser for the GPU path, or run rdma_rx (host ring) for this
        display.
```

It declines with the reason instead of displaying zeros. No silent GPU→CPU
fallback.

---

## Memory-backend matrix

| selection | receiver | payload lives in | this GUI |
|---|---|---|---|
| RoCEv2 + **CPU** | `rdma_rx` | host shm ring | **`RoceShmSource` — working, validated** |
| RoCEv2 + **GPU** | `rdma_rx_gpu` | **GPU VRAM** (CUDA IPC) | refused with reason; use Holoscan |
| RoCEv2 + **Holoscan** | either | as above | launch `holoscan_iq_viz.py` as a managed process |
| UDP + CPU/GPU | existing path | unchanged | untouched |

### Why RoCEv2 + GPU is not wired into this GUI yet

The GPU ring's payload is in VRAM behind a `cudaIpcMemHandle`. Consuming it
from this C++ application requires linking CUDA and calling
`cudaIpcOpenMemHandle`, then running the DSP with cuFFT so the data never
descends to host memory. Copying VRAM→host just to feed the existing CPU DSP
would defeat the entire purpose of the GPUDirect path — and their
`holoscan_iq_viz.py` **already does this correctly**, zero-copy, with cuFFT.

The right integration is therefore to **launch their visualiser** for the GPU
path rather than duplicate it, which is what §12 of the brief asks for. That
is a `BackendLauncher` job — the class already manages external processes
(`c2h_stream`, `iwfg_h2c`) with graceful SIGINT shutdown, log capture and
orphan prevention.

---

## Bring-up order for the RoCEv2 path

```bash
cd reference/roce-iq-holoscan
sudo ./scripts/01_install_base.sh      # rdma-core, perftest, deps
sudo ./scripts/02_setup_network.sh     # ns_tx namespace, IPs, MTU 9000
     ./scripts/03_verify_rdma.sh       # ibv_devinfo, GID, link state
     ./scripts/07_selftest.sh          # end-to-end selftest
make                                    # builds rdma_tx / rdma_rx / rdma_rx_gpu
```

Then, host (CPU) path:

```bash
./rdma_rx &                                     # fills /dev/shm/iqring
./rdma_tx  &                                    # in ns_tx
./build/sdr_workstation                         # select RoCEv2 + CPU
```

GPU path:

```bash
sudo ./scripts/05_gpudirect_setup.sh   # nvidia_peermem
./rdma_rx_gpu &
python3 viz/holoscan_iq_viz.py         # zero-copy CUDA IPC + cuFFT
```

**Cable check first** — both ports showed `flags=4099` (no `RUNNING`). Port A
must be physically cabled to Port B for this single-host loopback.

---

## What was NOT changed

GUI layout, widgets, styling, plots, controls, channel selection, frequency
calculation, FFT, the PCIe/`iwfg` path, the existing Ethernet/Mellanox
detection, and every supplied RoCEv2/Holoscan source — all untouched. The
reference tree is vendored under `reference/` for provenance.

Regression after this work: clean build, zero warnings; simulator and CW
capture replay both verified.

---

## Remaining work, in order

1. **Cable Port A → Port B**, run `02_setup_network.sh`, confirm
   `Link detected: yes`. Everything else is blocked on this.
2. Run `07_selftest.sh` — proves their stack independently (Phase 2).
3. Point `RoceShmSource` at the real `rdma_rx` ring and compare the measured
   tone against `rdma_tx`'s configured `fc` (same method as the 0.063 kHz
   result above).
4. Transport/backend selection in the launcher (UDP | RoCEv2, CPU | GPU |
   Holoscan) — small additive change to the existing interface toggle.
5. `BackendLauncher` entries for `rdma_rx`, `rdma_rx_gpu`, `rdma_tx`,
   `holoscan_iq_viz.py`, reusing the existing process-lifecycle machinery.
