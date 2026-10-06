# iqring_sim — plot without the RDMA stack

## The error you hit

```
cannot open /dev/shm/iqring — is rdma_rx (or rdma_rx_gpu) running?
[No such file or directory]
```

That is exact: the file does not exist, so **no receiver is running**. The GUI
is fine; there is simply nothing publishing.

## Why this tool exists

Standing up the whole RoCEv2 path — namespaces, Mellanox NIC, `rdma_tx`,
`rdma_rx_gpu` — just to find out whether the GUI plots correctly mixes two
unknowns. If nothing appears you cannot tell whether the NIC, the receiver or
the GUI is at fault.

`iqring_sim` publishes a ring **byte-identical in layout** to the one `rdma_rx`
produces, so the GUI takes exactly the same code path.

- **It plots** → the GUI is fine; any remaining problem is in the RDMA stack.
- **It does not** → the problem is in the GUI, and no NIC configuration helps.

## Build and run

```bash
gcc -O2 -o iqring_sim iqring_sim.c -lm
./iqring_sim
```

```
iqring_sim: /dev/shm/iqring
  32 slots x 1052672 B stride, 262144 samples/frame
  fs 200.000 MSPS   fc 3.100 GHz   tone   amp 0.80   30 fps
  tone 10.000 MHz  -> expect a peak there in the FFT
```

Leave it running and start the GUI:

| Setting | Value |
|---|---|
| Interface | **Ethernet** |
| Transport | **RoCEv2 (RDMA, shm ring)** |
| Session | **Live** |

You should see a peak at **+10 MHz** from centre.

### Options

```bash
./iqring_sim --tone 25e6        # move the tone
./iqring_sim --fs 122.88e6      # different sample rate
./iqring_sim --noise            # flat noise, for checking the noise floor
./iqring_sim --fps 60 --amp 0.5 # faster, half scale -> about -6 dBFS
```

The GUI reads sample rate and centre frequency **from the ring header**, so
those fields fill themselves in.

## What it reproduces faithfully

| | |
|---|---|
| `ring_ctrl` magic `IQRING01`, written **last** | a reader mapping mid-setup sees an invalid ring, not a half-built one |
| 32 slots, 1 MiB payload, 4 KB-aligned stride | matches `rdma_common.h` exactly |
| `write_count` published **last**, release ordering | a reader seeing the new count is guaranteed to see the payload |
| **14-bit MSB-aligned** samples | quantise to ±8191 **then** `<<2`, so `bits[1:0]` are always `00` — the packing the RFSoC really sends |
| magic cleared on exit | the GUI reports "no receiver" rather than reading a ring whose producer has gone |

## Verified

Read back through the same byte offsets `RoceShmSource` uses:

| Check | Result |
|---|---|
| magic `IQRING01` | PASS |
| geometry: 32 slots, stride 1052672, 262144 samples | PASS |
| rate and centre from the header | PASS — 200.0 MSPS / 3.10 GHz |
| producer publishing (`write_count` advancing) | PASS |
| frame magic `IQRFRMA1` | PASS |
| 14-bit MSB-aligned | PASS — no sample with nonzero `bits[1:0]` |
| **payload decodes to the tone** | **PASS — 10.000 MHz** |

## Note

This is the **host** ring (`IQRING01`), so it exercises the CPU path. It cannot
simulate the GPUDirect ring (`IQRINGG1`): that one's payload lives in VRAM
behind a CUDA IPC handle, which only a real `rdma_rx_gpu` on a real GPU can
publish.

So use this to prove the GUI, then move to `rdma_rx_gpu` for the GPU path.
