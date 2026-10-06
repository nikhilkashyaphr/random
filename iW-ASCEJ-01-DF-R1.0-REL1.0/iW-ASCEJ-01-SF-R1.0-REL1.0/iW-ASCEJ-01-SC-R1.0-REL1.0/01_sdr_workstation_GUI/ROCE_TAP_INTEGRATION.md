# RoCEv2 extractor tap — GUI integration

## What was already there, and why it did not work

The GUI already had a RoCEv2 source: `RoceShmSource`. But it reads a
**different ABI** from the one `roce-extractor` publishes.

| Producer | Magic | Default path | Layout |
|---|---|---|---|
| `rdma_rx` / `rdma_rx_gpu` | `IQRING01` / `IQRINGG1` | `/dev/shm/iqring` | 4096 B ctrl, 32 slots × (64 B hdr + 1 MiB payload), always int16 I/Q |
| **`roce-extractor` tap** | **`RTAP`** | **`/dev/shm/roce_tap`** | 4096 B `tap_hdr`, nslots × 64 B `tap_meta`, nslots × `slot_bytes`, per-slot seqlock |

Pointing the existing source at the extractor's ring fails at the magic check.
The two are not compatible and never were.

## Approach

**One source, two ABIs — detected from the magic word**, rather than a second
`SourceMode` the operator has to select correctly.

Rejected alternative: `SourceMode::RoceTap` + a parallel class. That touches
the enum and six switch sites, duplicates the polling logic, and makes "which
RoCE option do I pick?" a question the operator can get wrong. The magic word
already answers it unambiguously.

Consequence: **no enum change, no new UI mode, no duplicated pipeline.** Point
the GUI at whichever ring exists and it works out which producer it is talking
to.

## What the tap gives that IQRING cannot

`tap_hdr_t` carries `dtype`, `channels`, `fs_hz`, `center_hz` and
`full_scale`. Those are now **read from the producer** instead of assumed:

| Header field | Used for |
|---|---|
| `dtype` | `SampleFormat` — `TAP_DT_CINT16` → `Cs16`, `INT16` → `Rs16`, `INT8` → `Rs8`, `INT32` → `Rs32`, `FLOAT32` → `Rf32` |
| `channels` | `rawChannels` for the codec |
| `fs_hz` | sample rate, so the frequency axis matches the capture |
| `center_hz` | RF centre, for absolute frequency display |
| `full_scale` | `rawFullScale`, so dBFS is correct rather than assumed |

This removes the class of bug where the display scales every frequency by the
ratio of two disagreeing sample rates. An unrecognised `dtype` is **refused**,
not defaulted — plotting the wrong decode looks like a signal problem and costs
far more time than an error message.

## The seqlock — the part that had to be right

`tap.h` publishes each slot under a seqlock: `meta.seq` is stamped **odd**
before the payload copy and **even** afterwards. The tap is an overwriting ring
by design, because at 12.25 GB/s a plotter must never be able to slow the
capture down.

So the reader:

1. takes the **newest** published slot (`produced - 1`, masked) and counts what
   it skipped as dropped — a display must not back-pressure a 100 Gbps capture;
2. reads `meta.seq` **before** the copy and rejects an odd value (mid-write);
3. reads it **again after** the copy and discards the frame if it changed.

Without step 3 a slot overwritten mid-read is plotted as a torn frame — which
looks exactly like a real signal artefact and would be chased as one.

## Storage and plotting are shared, not reimplemented

The tap path emits the same `SampleBlock` with **raw wire bytes**:

```cpp
b.raw          = payload;              // undecoded
b.rawFormat    = m_cfg.src.format;     // from the tap header
b.rawChannels  = channels;
b.rawFullScale = full_scale;
publish(b);
```

Decoding happens downstream in the existing `SampleCodec`, exactly as the PCIe
path does. So spectrum, constellation, time domain, measurements **and
recording** all work through one pipeline. There is no second decoder to keep
in step, and offline replay of a RoCE capture behaves identically to a PCIe
one.

## Ring auto-discovery

The constructor prefers whichever ring exists:

```
/dev/shm/iqring     rdma_rx        (checked first, historical default)
/dev/shm/roce_tap   roce-extractor
```

If neither is present it keeps `/dev/shm/iqring`, so the error message names
the path people expect.

## Verification

A real RTAP ring was built in `/dev/shm` using the same layout and publication
order as `tap.c` (magic written last, seqlock odd→even), then the exact offsets
and seqlock logic the GUI uses were exercised against it.

| Check | Result |
|---|---|
| magic / version | PASS |
| nslots is a power of two | PASS |
| geometry read (16 slots × 4096 B) | PASS |
| fs and centre adopted from header | PASS — 200 MHz / 3.10 GHz |
| dtype `CINT16` → `Cs16` | PASS |
| `full_scale` read | PASS |
| running flag, source string | PASS |
| geometry fits the mapping | PASS |
| `produced` counter advanced | PASS |
| seqlock even, unchanged after read | PASS |
| **payload decodes to the expected tone** | **PASS — 10.000 MHz** |
| amplitude sane vs `full_scale` | PASS — peak 8000 |

Plus: full CMake build, Qt 5.15, 1.8 MB binary, and `-Wall -Wextra -Wshadow`
clean on the changed file.

The tone check is the one that matters: it decodes the payload back through the
same arithmetic the display uses and recovers the frequency that was generated.

## Files changed — two

```
src/core/RoceShmSource.h     ABI enum, tap constants, new members/methods
src/core/RoceShmSource.cpp   tap offsets, mapRoceTap(), pollRoceTap(),
                             bytesPerSample(), dual-ABI peekRingInfo(),
                             ring auto-discovery
```

No change to `Types.h`, `MainWindow.cpp`, `Panels.cpp`, `ConfigDialog.cpp`, the
DSP chain or the recorder.

## Using it

```sh
# start the extractor with the tap enabled
./roce-extractor --interface ens5f0 --output /data/roce/out.bin \
                 --tap roce_tap --tap-dtype cint16 --tap-fs 200e6

# then in the GUI: source = RoCEv2
```

The GUI adopts rate, centre, format and full-scale from the tap header, so no
retyping — and the axes cannot silently disagree with the capture.

### If it does not connect

| Message | Meaning |
|---|---|
| `has magic 0x… — expected IQRING01 or RTAP` | producer still initialising; both write magic last |
| `RTAP ring of version N` | extractor newer than this GUI build |
| `nslots is not a power of two` | producer masks with `nslots-1`; a non-power-of-two ring would alias |
| `dtype N, which this build does not decode` | unsupported sample format — refused rather than guessed |
| `roce-extractor has stopped publishing` | `running` flag cleared; extractor exited |
