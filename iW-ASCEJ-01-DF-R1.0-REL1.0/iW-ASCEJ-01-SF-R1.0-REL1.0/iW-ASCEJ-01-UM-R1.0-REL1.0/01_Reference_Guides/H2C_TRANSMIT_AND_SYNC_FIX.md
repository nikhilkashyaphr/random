# Two fixes: the false "Synchronised", and why H2C never transmitted

## 1. The panel claimed success when it had read nothing

Your screenshot shows green **"Synchronised with device."** while every field
holds a software default: decimation ×1, QMC 0.0000, DAC VOP 0.000 mA,
sampling rate 2000 MHz, AXIS 0/1/2/3, PL GPIO Mode 1.

None of those came from the device. The serial log shows why — **thirteen
consecutive rejections**:

```
[pcie] REJECTED event 0x000F1: unknown EVENT code    (×13)
```

### The bug

`readParameter()` has two distinct failure paths:

| Path | Sets `m_readbackOk = false`? |
|---|---|
| Firmware writes the answer but the PL cannot (tag returns unchanged) | yes |
| **Firmware rejects the event entirely (old build)** | **no** |

Sync only tested `m_readbackOk`, so a build that rejects the event left the
flag true and sync reported **success having read nothing**.

### Why that was dangerous, not just cosmetic

Sync succeeding enabled the panel. The parameters are *applied on edit*, so
touching anything pushed a software default into the hardware. Your third
screenshot shows precisely that:

```
QMC gain applied.                                   <- 0.0000 = SILENCE
DAC VOP rejected by device: payload out of range    <- 0.000 mA, below the 2.25 mA minimum
Decimation applied.  MTS re-align applied.          <- x1, not the x24 the device was running
```

The device was reconfigured with values nobody chose. That is exactly the
failure your specification set out to prevent.

### Fixed

* Sync now judges on **how many values actually came back**, not on a flag that
  only one failure path sets.
* If none did, sync reports failure and the panel becomes **read-only**, with a
  dialog explaining that the fields are software defaults and must not be
  applied.
* `Complete Reset` applies the same rule before claiming success.
* Dangerous defaults corrected at the widget level: QMC now defaults to **1.0**
  (unity) rather than 0.0 (silence), and DAC VOP is clamped to the device's own
  **2.25–40.5 mA** so 0 mA can no longer be sent at all.

---

## 2. H2C never transmitted, because the FIFO had no producer

The playback chain is:

```
<producer>  ->  /tmp/iwfg_h2c.fifo  ->  iwfg_h2c  ->  card  ->  DAC
```

`iwfg_h2c` is **only the FIFO→card mover**. Grepping the whole workstation for
writers of that FIFO returns nothing: **the pipe had no producer.**

The Transmit tab looked healthy — green `/dev/shm/iqctl`, 200 MSPS, 2450 MHz,
10 Gb/s — because that control block carries **pacing and frequency metadata,
not samples**. Configuring it and expecting output is like setting a tape
deck's speed with no tape in it.

### Added: `IqGenerator`

A proper producer, on its own thread.

| Waveform | Use |
|---|---|
| Single tone | basic verification, spectrum peak |
| Two tone | intermodulation / linearity |
| Sweep | flatness across the span |
| Noise | spectral flatness |
| CW / DC | DC offset, static level |

Design points that matter:

* **Non-blocking FIFO open.** `open()` on a FIFO for writing blocks until a
  reader attaches, so starting the generator before `iwfg_h2c` would have
  wedged the thread with no way to cancel. It opens `O_NONBLOCK` and retries,
  then switches to blocking writes — the reader draining the FIFO is exactly
  the back-pressure wanted.
* **Phase carried across chunks.** Restarting the phase every buffer would put
  a discontinuity at each boundary and smear the spectrum.
* **Partial writes carried, never dropped.** Truncating mid-pair would shift
  I/Q alignment for the remainder of the stream.
* **Sample rate is taken from the Transmit panel**, because a generator running
  at the wrong rate puts the tone at the wrong frequency.
* Buffer allocated once and reused; nothing allocated per chunk.

Wire format is interleaved `int16` I,Q — the same Complex Int16 the capture
path uses.

### Verified numerically

The generation maths was extracted and measured rather than eyeballed:

```
requested 10.000 MHz -> measured 10.000 MHz   PASS
amplitude 0.70 FS    -> peak 22936/32767 = 0.700   PASS
```

### How to transmit

1. **Transmit** tab → set **Sample rate** to the DAC stream rate (200 MSPS).
2. Start the H2C backend so `iwfg_h2c` is reading the FIFO. *(The generator
   will wait up to 5 s for a reader and then tell you if none appeared.)*
3. In **IQ Signal Generator**, pick a waveform, set the tone and amplitude.
4. **Start generator.** The label shows live throughput in MSPS.
5. **RFDC** tab → set **DAC input source** to *Host / GNU Radio stream*, so the
   AXIS switch feeds the DAC from PCIe rather than the internal DDS.

Step 5 is easy to forget: with the DAC source left on **DDS compiler** the
converter plays its internal oscillator and ignores everything you send.

### Amplitude

Leave headroom. 1.0 clips on anything with crest factor above unity — Two tone
in particular sums two carriers, so it is internally scaled by half.

---

## Verification

| Check | Result |
|---|---|
| `ControlWindow3D`, `RfdcControl`, `IqGenerator`, `-Wall -Wextra -Wshadow` | 0 errors, 0 warnings |
| All three moc-generated sources compile | clean |
| Generator tone frequency and amplitude | measured, both PASS |
| Firmware, 17 modules × 2 flows × 2 PCIe settings | 0 errors, 0 warnings |

## Still required

The board is running firmware that predates events `0x0F1` and `0x023`. Until
it is flashed the RFDC panel will correctly stay **read-only** — which is the
right behaviour, but it does mean you cannot use that tab until you flash.
