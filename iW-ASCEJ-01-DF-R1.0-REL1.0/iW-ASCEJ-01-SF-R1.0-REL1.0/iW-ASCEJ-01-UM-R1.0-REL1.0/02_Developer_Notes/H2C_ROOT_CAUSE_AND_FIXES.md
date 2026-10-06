# H2C data path, GUI synchronisation — root causes and fixes

## Summary

| # | Issue | Root cause | Status |
|---|---|---|---|
| 1 | H2C generates/sends nothing | **The FIFO had no producer** | Fixed — `IqGenerator` |
| 2 | No H2C control in the GUI | Never built | Added to the Transmit tab |
| 3 | Interp/decim not updating | Firmware rejects readback; GUI had no authority | Fixed both ends |
| 4 | Sampling rate not updating | Same, plus PLL quantisation stored wrongly | Fixed both ends |
| 5 | Architecture | Three copies of "the rate"; no single truth | Snapshot is now authoritative |

---

## 1. Why H2C transmitted nothing

The chain is:

```
<producer>  ->  /tmp/iwfg_h2c.fifo  ->  iwfg_h2c  ->  card  ->  DAC
```

`iwfg_h2c` is **only the FIFO→card mover**. Grepping the whole workstation for
writers of that FIFO returned nothing — **the pipe had no producer.** The
Transmit tab looked healthy because `/dev/shm/iqctl` carries pacing and
frequency *metadata*, not samples.

In the GNU Radio workflow the producer was
`iq_source_v9 → Throttle → fifo_sink_v9`. That is the missing piece, and it is
now implemented in-application as `IqGenerator`.

### Reading the reference caught three bugs in my first attempt

I had already written a generator. Comparing it against `fifo_sink_v9.py` and
`iwfg_h2c.c` showed it was wrong in three ways — each one a defect the GNU
Radio side had already hit and fixed. Worth stating plainly, because it is the
reason the reference implementation mattered:

**a. DAC packing.** The RFSoC DAC expects `bits[15:2]` = 14-bit signed sample,
`bits[1:0]` = `00` always. Their v3 scaled straight to ±32764 and rounded,
leaving ~75 % of samples with nonzero LSBs. My first version did exactly the
same thing. The correct construction is: quantise to int14 (±8191) **first**,
then left-shift by 2.

Verified over 200 000 samples:

```
MY packing, nonzero LSBs   : 0        PASS
their v3 bug, nonzero LSBs : 147200 (73.6%)   <- matches their "~75%"
peak magnitude             : 32764 = 8191<<2  PASS
```

**b. Blocking writes.** `fifo_sink_v9` v4 notes that a blocking `write()` wedges
forever once the pipe fills and the reader freezes, and cannot be interrupted
by `stop()`. My version switched to blocking writes after open. It now keeps
the fd `O_NONBLOCK` and paces with `select()` on a 100 ms timeout, so stop is
honoured within ~0.25 s in any state.

**c. FIFO inode swap.** `iwfg_h2c.c` note D: a blocking `open(O_WRONLY)`
resolves **one** inode and waits on it, so a consumer doing `unlink+mkfifo`
leaves the writer wedged on a dead inode. Every retry now re-resolves the path
and recreates the FIFO on `ENOENT`. The FIFO is never unlinked.

### One finding that simplified the design

`iwfg_h2c.c`: *"GNU Radio fifo_sink produces ~30 MB/s but the card consumes
~700 MB/s … replay the most recent chunk at full DMA speed until a new one
arrives."*

So the producer does **not** have to match the card. It has to be steady and
never wedge. That is why there is no drop-queue here — back-pressure through
`select()` is correct behaviour, not a fault.

### Compatibility

Waveform numbering matches `iq_source_v9` exactly (0 Sine, 1 Cosine, 2 Square,
3 Sawtooth, 4 Triangle, 5 Noise), as does the IQ phase-difference parameter
(−90° = standard IQ). Chunk default is 16384 bytes, forced to a multiple of 4,
matching `iwfg_h2c`'s expectations.

---

## 2. H2C control in the GUI

Added to the **Transmit** tab:

| Control | Purpose |
|---|---|
| FIFO path | Defaults to `/tmp/iwfg_h2c.fifo` |
| Waveform | Sine / Cosine / Square / Sawtooth / Triangle / Noise |
| Frequency | Tone frequency |
| IQ phase diff | −90° standard; other values deliberately unbalance I/Q |
| Amplitude | Fraction of full scale (8191<<2 = 32764) |
| Start / Stop | Enable and disable transmission |
| Live status | **MS sent, chunk count, MSPS, reconnect count** |

The chunk counter is the answer to *"confirm that data is actually being
generated and transmitted"* — a number the operator can watch increase.
Connection state is logged as `iwfg_h2c` attaches and detaches.

---

## 3 & 4. Interpolation, decimation and sampling rate not updating

Two independent causes, both fixed.

**Firmware side.** The board rejects readback (`unknown EVENT code 0x0F1`), so
the GUI had nothing authoritative to display. Worse, `readParameter()` had two
failure paths and only one set the "unsupported" flag, so a rejecting build
left the flag true and **sync reported success having read nothing** — which
enabled the panel and let software defaults be written into the hardware. Your
log caught it doing exactly that: QMC 0.0000 (silence), DAC VOP 0 mA (rejected,
out of range), decimation ×1 against a device running ×24.

Now: sync judges on how many values actually came back, and if none did the
panel is **read-only** with an explanatory dialog.

**Sampling-rate maths.** The RFDC PLL synthesises `RefClk × FBDIV / OutDiv` and
cannot reach every request — 2000 MHz lands on 2006.25. The firmware stored the
*requested* value and derived the stream clock, Clocking Wizard divider, DDS
phase increment and Nyquist zone from it. It now reads the achieved rate back
from `XRFdc_GetPLLConfig` and uses that throughout, reporting the quantisation
on the console.

---

## 5. Architecture: one source of truth

"The sample rate" previously existed in three places that could disagree: the
Transmit tab, the Signal chain tab, and the RFDC panel.

The device snapshot is now the single authority. One readback pass drives:

* the RFDC panel fields
* the **Signal chain** decimation and interpolation
* the **Transmit** sample rate — derived from the DAC *stream clock*, because
  that is what the generator must run at. A generator running at a rate the
  hardware is not using puts the tone at the wrong frequency, which is exactly
  the class of desync being eliminated.

Other structural changes made along the way:

* All device I/O is on a worker thread; verified mechanically that no
  `m_rfdc->` call exists outside a queued lambda.
* Writes are verified: write → read back → compare, with a tolerance for
  legitimate PLL quantisation. Success is never reported on an unverified
  write.
* `COMPLETE RESET` confirms first, then stop → defaults → wait-ready → restart
  → re-read → report, and reports success only if every stage succeeded.
* Dangerous widget defaults corrected: QMC defaults to 1.0 (unity) not 0.0
  (silence); DAC VOP is clamped to the device's own 2.25–40.5 mA.

---

## 6. Verification performed

| Check | Result |
|---|---|
| DAC packing, 200 000 samples | 0 nonzero LSBs; peak 32764 — **PASS** |
| Reproduced the v3 defect for comparison | 73.6 % nonzero LSBs, matching the documented ~75 % |
| Tone frequency and amplitude accuracy | 10.000 MHz requested → 10.000 measured; 0.70 FS → 0.700 |
| GUI compile, `-Wall -Wextra -Wshadow`, incl. all moc | 0 errors, 0 warnings |
| Firmware, 17 modules × 2 flows × 2 PCIe settings | 0 errors, 0 warnings |
| Grep audit: device calls off the GUI thread | none remaining |

### What could not be tested here

No card is present in this environment, so the end-to-end list (items 1–12 of
your section 6) needs a bench run. The order that will get there fastest:

1. **Flash the firmware.** Until then the RFDC tab stays read-only — correct
   behaviour, but it blocks items 7–11.
2. Start the H2C backend so `iwfg_h2c` is reading the FIFO.
3. Transmit tab → **Start generator**. Watch the chunk counter increase.
4. RFDC tab → **DAC input source = Host / GNU Radio stream**. Easy to forget:
   left on *DDS compiler* the converter plays its internal oscillator and
   ignores everything sent over PCIe.
5. Stop and restart the generator to confirm items 5 and 6.

## Remaining limitations

* **Readback needs the new firmware.** Nothing on the host side can work
  around a device that rejects the event.
* **The extended EVENT codes (0x010–0x0F1) are additions**, not from
  `Register_mapping.docx`. The FPGA team must agree them.
* **No hardware confirmation that samples reached the DAC.** The generator
  confirms bytes left the application and that `iwfg_h2c` consumed them; the
  card's own delivery is out of its sight. A loopback capture on C2H would
  close that gap and is the natural next step.
