# H2C transmit control — implementation

## What the controls are, and why only these

The control surface was **not invented**. `rdma_tx` polls a `struct iq_ctl`
block at 1 Hz, and that struct — locked by `_Static_assert` in
`rdma_common.h` — carries exactly three settable fields:

```c
struct iq_ctl {
    uint64_t magic;            /* off  0  "IQCTL001"                  */
    uint32_t version;          /* off  8                              */
    volatile uint32_t seq;     /* off 12  bump after writing fields   */
    double   pace_gbps;        /* off 16  0 = unlimited               */
    uint64_t sample_rate_hz;   /* off 24                              */
    int64_t  center_freq_hz;   /* off 32                              */
};
```

So the window exposes **sample rate, centre frequency and pacing**, and
nothing else. A knob that does not reach the transmitter would be worse than
no knob at all.

## Write protocol

Fields first, then `seq`, with a barrier between:

```cpp
poke(base, kOffPace,   paceGbps);
poke(base, kOffRate,   sampleRateHz);
poke(base, kOffCentre, centerFreqHz);
__sync_synchronize();
poke(base, kOffSeq, peek(base, kOffSeq) + 1);
```

The transmitter treats a change of `seq` as "the other fields are now valid",
so bumping it first would let `rdma_tx` latch a half-written parameter set.
This matches the reference `viz/control_panel.py` exactly.

## ABI verified against the reference implementation

The C++ writer's output was parsed back with the reference Python layout:

```
magic    : 0x495143544c303031   OK
version  : 1
seq      : 1
pace_gbps: 25.0
fs_hz    : 200250000
fc_hz    : -10000000

Python reads exactly what C++ wrote: PASS
```

Byte-exact interoperability, so this control and `control_panel.py` can be
used interchangeably against the same transmitter.

## Design decisions

* **Creates the block if absent.** `O_CREAT` plus seeding with the reference
  defaults (10 Gb/s, 100 MSPS, 2.45 GHz) means parameters can be set before
  `rdma_tx` starts, and it adopts them when it comes up. Magic is written
  last, so a concurrent reader never sees a valid magic over uninitialised
  fields.
* **Centre frequency is signed** (−30000…+30000 MHz). The reference transmits
  at negative baseband offsets and the receiver reports them as such;
  clamping to ≥ 0 would make a valid configuration unreachable.
* **Pacing has an explicit "Unlimited" checkbox** rather than expecting the
  operator to know that 0 means unpaced.
* **Controls disable with a reason** when the block cannot be reached, instead
  of accepting input that will never arrive anywhere.
* **Revert** re-reads the block, so the displayed values can always be
  resynchronised with what the transmitter actually holds.
* **The log is bounded** (`setMaximumBlockCount(200)`) — this window stays
  open for the life of the session, so an unbounded log would grow without
  limit.

## Separation

`src/core/H2cControl.{h,cpp}` contains **no UI code**. It exposes
`attach()`, `apply()` and read-back accessors plus signals, so it is testable
headlessly — which is how the ABI check above was run — and the window is a
thin binding over it.

## Gating (unchanged from the previous round)

The menu-bar action is disabled in ADC Only (C2H) mode, since there is no
transmit path for these parameters to act on:

| mode | H2C CONTROL |
|---|---|
| ADC Only (C2H) | disabled |
| DAC Only (H2C) | enabled |
| ADC + DAC | enabled |

## Still scaffolding

The 3D viewport itself remains an extension point (`setViewport()`), now
collapsed to a single line so it does not take space from the controls that
work. When a real view is installed it takes the stretch instead.

---

# RF Data Converter control over PCIe (added)

## Two control surfaces, deliberately separate

The window now carries a second, independent path. They are complementary, not
alternatives:

| | `H2cControl` | `RfdcControl` (new) |
|---|---|---|
| Transport | `/dev/shm/iqctl` | PCIe BAR registers |
| Talks to | `rdma_tx` on the **host** | bare-metal app on the **A53** |
| Controls | pacing, sample rate, centre freq | NCO, decim/interp, DDS, DSA, QMC, VOP, routing, MTS, start/stop |

One paces the host-side stream; the other configures the silicon it lands in.

This also closes a gap that was documented in `ControlWindow3D.h`: the Signal
Chain panel's NCO / interpolation / decimation values were previously *reported
only* — "not part of the live iq_ctl block, so they are reported rather than
written". They now actually reach the converters.

## Register surface

`src/core/pcie_regs.h` is a **byte-for-byte copy** of the header used by the
bare-metal firmware and the `rfdc_ctl` CLI. One definition, three consumers, no
drift. If it changes, copy it to all three.

```
+0xA8  ring_register       command word, bit31 = NEW_CMD
+0xAC  RFDC_configuration  payload
```

Handshake: verify `NEW_CMD` clear → write payload → write command word with
`NEW_CMD` → poll until it clears. That clearing **is** the acknowledgement. The
firmware also embeds a result code in the ACK (marker `0x2A` in `[30:24]`,
error in `[23:20]`), which `RfdcControl` decodes and reports; firmware without
it still works, and the class falls back to "processed, outcome unknown".

**Payload is written before the command word** so the target cannot latch a
half-written parameter set — the same ordering discipline as the `iq_ctl`
`seq` bump.

## Panel layout

*RF Data Converter* tab:

- **PCIe control interface** — endpoint (auto-populated with Xilinx vendor
  `10ee` devices), BAR (default **2**), register base, Attach.
- **Target** — converter, tile (or all), channel (or both).
- **Parameters** — NCO frequency, DDS frequency, ADC DSA, DAC input source.
- **Actions** — Apply signal chain, Ping, Read registers, Start, Stop,
  Re-align MTS, Soft reset.
- Live `ring` / `cfg` readout.

### Apply order

`onRfdcApplySignalChain()` sends in an order that leaves the datapath
consistent:

1. decimation / interpolation — these move the stream clock
2. **MTS re-align**, if a rate changed
3. NCO frequency
4. ADC DSA, DAC input source
5. **DDS frequency last** — its phase increment is derived from the DAC stream
   clock, so it must be re-sent after any interpolation change

## Notes on defaults

- **BAR 2, not 0.** On this design BAR2 is the 4 KB control window; BAR0 is
  QDMA control space with the MSI-X table at `0x30000`. Writing register
  offsets into BAR0 could disturb the `iwfg` driver.
- **Root required.** sysfs BAR mapping needs `CAP_SYS_RAWIO`. The panel
  reports the reason rather than failing silently.
- **`Stop` really stops.** No data at all until `Start`.
- **`Soft reset` is not a power cycle** — MTS re-align, NCO restore, routing
  restore. Power-cycling the converters would drop the clock tree and require
  the full LMK/LMX sequence again.
- **Sampling rate and soft reset raise the timeout to 15 s** internally, since
  a PLL reprogram plus settle plus MTS takes seconds.

## Verification

| Check | Result |
|---|---|
| `moc` + compile, `RfdcControl.{h,cpp}`, Qt 5.15, `-Wall -Wextra` | clean |
| `moc` + compile, modified `ControlWindow3D`, `-Wall -Wextra` | clean |
| GUI word construction vs. words observed on hardware | 7/7 byte-exact |

The last row matters most: the GUI builds `0x8F2000F0` for ping and
`0xA8000005` for DDS — **identical to the CLI words the board already
acknowledged**, and it decodes the real `0x2a0000f0` ACK correctly.

## If the panel cannot attach

1. `lspci -D -nn | grep -i xilinx` — confirm the BDF.
2. Try BAR 0 with a register base if BAR 2 reads all zeros.
3. `EBUSY` means a driver holds the BAR exclusively; use the character-device
   backend (`attachCharDev`) instead.
4. Timeouts mean the firmware is not polling — most often it is sitting in a
   UART sub-menu. Press Enter on the console to return to the main prompt.
