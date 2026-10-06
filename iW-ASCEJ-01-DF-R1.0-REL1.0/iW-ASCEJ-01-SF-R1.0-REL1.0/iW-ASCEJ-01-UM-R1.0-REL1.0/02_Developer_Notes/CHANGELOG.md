# Changelog

## [1.2.5] — 2026-09-27

Why capture and transmit broke each other, and a GUI that works around it.
**GUI only.** The firmware, the helper applications and the driver are
unchanged.

### What the bench screenshot showed

- The capture had stopped: *DMA 0.0 MB/s*.
- The generator's reader had gone: *"Reader disconnected (iwfg_h2c
  stopped?)"*, with *"H2C link: no reading yet"*.
- The readback noted *ADC QMC gain 0.0000*.

### Root cause, from the iwfg driver's own source

`SC/01_sdr_workstation_GUI/drivers/iwfg_dma.c`, `iwfg_rx_poll()`, the `usr_ovf` branch.
When the FPGA's C2H FIFO overflows, the driver:

- soft-resets the whole QDMA;
- frees and rebuilds **every** queue, the H2C queue included. If an H2C
  transfer is still waiting (for example because the card is not consuming,
  a 2 s wait), it forces the reset after about 11 ms and frees the queue out
  from under the ioctl. That is a use-after-free, and a plausible cause of a
  device that then stops delivering C2H data;
- does not re-apply the H2C datapath enable (`H2C_CTRL.VID_OUT_SEL`), which
  would explain transfers that stop completing after an overflow;
- cuts the DAC feed for at least 17 ms (`udelay(17000)`) on every overflow.
  That is the likeliest source of the "disturbances" on the 1.2.1 tone.

A 4-channel capture at 200 MSPS needs 3052 MiB/s, and the host drained
about 2400, so this recovery ran continuously. The full analysis, with
suggested driver fixes, is in
`UM/02_Developer_Notes/DRIVER_OVERFLOW_RECOVERY.md`.

### Added — transmit guard

When the generator starts in a live session and the capture needs more than
1600 MiB/s, the GUI reduces it to 2 channels (or 1) through the PL GPIO mode
(RFDC tab attached), so the C2H FIFO does not overflow while transmitting.

- **Verified.** 3 s later the measured rate is read as a channel count. If
  the stream carries a different whole count, the display follows it. If it
  did not shrink, everything is restored and you are told.
- **Restored** when the generator stops.
- **Without RFDC access**, a notice explains the overload and how to reduce
  the capture by hand.

### Added — the device is reset when the capture stalls

If the capture delivers nothing for about 5 s, the GUI:

1. closes transmit;
2. closes capture, so the driver's QDMA soft reset runs on the last close;
3. starts both again with the working settings.

The display re-attaches by itself, and there is no dialog. After two resets
in 3 minutes without success, a notice says to reload the driver or
power-cycle.

### Changed

- **Stop is ordered.** Transmit closes first, capture last, so every
  Stop/Start begins on a freshly reset device. With the old simultaneous
  SIGINT, the capture could close first and the driver then skipped its
  reset.
- **A stalled transmit helper is reopened at the same buffer size before any
  step-down.** A reopen rebuilds the H2C queue and re-enables the H2C
  datapath. 1.2.4 stepped straight down and walked the whole ladder, cycling
  the device at each rung. A helper that has streamed and then stalls for
  about 4 s is also reopened, at most 3 times a minute. `iwfg_h2c` by itself
  waits about 30 s.
- **The capture FIFO gets the largest pipe buffer allowed** (1 MiB by
  default, up from 64 KiB). `c2h_stream`'s 16 MiB writes then need 16
  wake-ups instead of 256, so the host drains faster.
- **The ADC row of the Transmit chain warns when the ADC QMC gain reads
  zero.** If QMC gain is enabled, that mutes the capture. Set it to 1.0 on
  the RFDC tab.

### Tests

- `fake_iwfg.so` can now also:
  - log opens and closes;
  - stall one H2C process (a stall a reopen cures);
  - stop delivering C2H data (a wedged card).
- `loopback_emu_test` grows from 56 to 73 checks. The new ones cover:
  - a same-size reopen cures a stall with no step-down;
  - stop closes `/dev/iwfg1` before `/dev/iwfg0`;
  - the capture FIFO pipe is 1 MiB;
  - a wedged card is brought back by `resetDevice()` with the tone received
    again;
  - the transmit-guard and channel-count arithmetic.
- `app_live_emu_test` grows from 20 to 26 checks. The whole application
  notices a card that stops delivering and resets the device by itself, with
  no dialog, and Verify loopback passes afterwards.

## [1.2.4] — 2026-09-26

Send and receive, made robust end to end, and the window no longer freezes
when a helper gets stuck. **GUI only.** The firmware and the two helper
applications (`c2h_stream`, `iwfg_h2c`) are unchanged.

### What the report showed

*"C2H capture stopped unexpectedly (exit 0) … Stopping stream… Cleanup
complete."*, no tone, and a window that hung whenever something got stuck.

- **"exit 0" said nothing.** `c2h_stream` prints *Stopping stream…* and
  returns 0 whatever ended its loop: a DMA driver error, its reader closing
  the FIFO, or a signal. Its stdout is block-buffered into a pipe, so the one
  line that gave the reason arrived out of order and had scrolled out of the
  dialog by the time it was shown.
- **16 MiB was too large for the card's H2C path.** Since 1.2.2 nothing
  reached the DAC. With 16 KiB (1.2.1) a tone did get through, but it was
  impure. A 16 MiB user buffer is 4096 pages, i.e. thousands of H2C
  descriptors per transfer, and the step-down in 1.2.2 reacted only to a
  helper that *exited*. A transfer that never completed (stall kicks, device
  cycling) was never treated as a size problem.
- **The capture was over its bandwidth.** 4 channels × 200 MSPS × 4 B needs
  3052 MiB/s, and the screenshot shows about 2417 MiB/s delivered. The
  difference is the FPGA's C2H FIFO overflowing. The driver's overflow
  recovery soft-resets the QDMA, and that also rebuilds the H2C queue, so an
  overloaded capture keeps interrupting transmit.
- **Several paths blocked the GUI thread**: backend stop (up to 10 s),
  destroying a source (it waited on the acquisition thread with no timeout),
  destroying a helper stuck in the driver (Qt waits for it to die), the helper
  pre-flight (it ran each helper with no arguments; both then opened the
  device and waited for a FIFO peer until they were killed 3 s later), and
  `nvidia-smi` polled synchronously every 5 s.

### Fixed — H2C playback

- **Default buffer is 1 MiB** (256 pages). The per-transfer cost is 0.3 %:
  758 MiB/s against the 763 the DAC reads, in the emulation with 5 µs per
  transfer. Tone steps are 763 Hz: 10 MHz goes out as 9.999847 MHz, and the
  panel shows the exact figure. `IWFG_H2C_CHUNK_BYTES` still overrides the
  default.
- **Step-down now triggers on a stall, not only on an exit.** If there is no
  completed transfer within 4 s of the first buffer, or on any stall-kick,
  ETIMEDOUT, EBUSY or device-cycling message before streaming, the helper is
  relaunched one rung down: 1 MiB → 256 KiB → 64 KiB → 16 KiB.
- **At the bottom rung it stops guessing.** If the card takes nothing even at
  16 KiB, the problem is not the buffer size. `iwfg_h2c` is left running (it
  resumes by itself) and the Transmit tab says what to check: DAC input on
  Host, RF DAC running, driver datapath.
- **When the card then starts consuming, transmit goes back to 1 MiB.** This
  is the usual sequence when the DAC input was still on the DDS. Transmit is
  not left on a 16 KiB buffer that cannot keep the DAC fed.
- **A transmit helper that cannot run no longer stops the capture from
  starting** (for example, `/dev/iwfg1` missing). It is reported as a
  non-fatal notice.
- **"H2C DAC Only" now runs the transmit helper.** It used to map to
  capture-only, so the generator had nobody to send to. An ADC-only session
  starts the helper on demand when the generator is started.
- **The generator's FIFO follows the backend's.**
- **Retry transmit** on the Transmit tab relaunches `iwfg_h2c` at the default
  buffer.

### Fixed — C2H capture

- **The reason is read, not the exit code.** `c2h_stream` runs under
  `stdbuf -oL` (coreutils) when available, so its lines arrive live and in
  order. The exit is classified as driver error, reader closed, stray signal
  or start-up failure, and named in plain words.
- **Automatic restart.** A capture helper that ends on its own after it had
  been streaming is restarted, up to 3 times a minute. No dialog appears. The
  reader notices the new FIFO (a new inode) and re-attaches by itself. A
  helper that cannot start, or keeps dying, still ends the session, with the
  reason.
- **If the transmit helper was stalled when the capture failed, it is
  stopped** rather than left cycling the device under a capture being
  restarted. Retry transmit brings it back.
- **Source-panel edits no longer kill the capture.** A live reader used to be
  destroyed and recreated for any source-panel change. In Qt 5, focus merely
  leaving the device-path field counts as a change. That closed the FIFO, and
  `c2h_stream` exits the moment its reader goes. The live reader is now
  reconfigured in place.
- **The reader no longer spins when the writer is gone.** It used to re-poll
  a hung-up FIFO in a tight loop, burning a whole core. It now waits 50 ms
  between checks and says *"Nothing is writing to … — waiting for the capture
  helper"*.

### Added — capture bandwidth check

The main window compares what the capture delivers with what the configured
stream needs (channels × rate × sample size):

- **Below 90 % for 2 s:** a status-bar warning, and in the Transmit tab the
  C2H row reads *"OVERLOADED: … each overflow recovery resets the QDMA, which
  interrupts H2C transmit too"*.
- **Capture 1 channel (PL GPIO mode 1):** a button that sets PL GPIO mode 1
  and the display to one channel. The measured rate is checked 3 s later. If
  the stream did not shrink (the PL mode did not reduce it), both are put
  back and you are told.
- **Above 130 %:** the stream carries more channels (or a higher rate) than
  configured, so it is being deinterleaved wrongly. You are told.

### Fixed — the window never blocks

- **Backend stop is asynchronous.** It sends SIGINT, then SIGTERM at 2.5 s,
  then SIGKILL at 4 s, all on timers. A helper stuck in an uninterruptible
  driver call is handed to a reaper and never destroyed while running.
  Start pressed while a stop is still in progress is queued, not refused.
- **Destroying a source no longer waits on the acquisition thread.**
- The acquisition thread never switches a device fd to blocking.
- **The helper pre-flight takes milliseconds** (it was 6 s) and does not open
  the device. Arguments make each helper exit at its first check.
- `nvidia-smi` runs asynchronously.
- **Backend, acquisition, recorder and "no data" messages are non-modal** and
  de-duplicated. One box per kind is updated in place; they no longer stack.
- **Everything the helpers print is written to `$TMPDIR/sdr_backend.log`**
  with timestamps. The *No data received* dialog already pointed there, but
  nothing wrote it. The once-a-second rate lines no longer overwrite the
  status bar.

### Changed

- The Transmit chain has two new rows. **H2C playback** shows the helper's
  state and why (waiting for the generator / starting / streaming / stalled /
  restarting / failed). **C2H capture** shows delivered against needed.
- The **Received** row no longer says *"NOT the generator's"* when the
  frequency matches but the DAC input cannot be read. It says it matches,
  and to press Verify loopback to prove it.
- The first rate line of each helper is not reported. It covers a fraction of
  a second divided by a whole one, and read 74 MiB/s for a link moving 757.
- Environment overrides: `IWFG_C2H_DEVICE`, `IWFG_H2C_DEVICE`,
  `IWFG_C2H_FIFO`, `IWFG_H2C_FIFO`, `IWFG_NO_STDBUF`.
- The UDP receiver (`backend/udp/iwfg_c2h`) is no longer shipped prebuilt.
  It is built on first use, like the other helpers, so a glibc mismatch
  cannot stop it loading.

### Tests

The driver stand-in (`fake_iwfg.so`) now also fakes the C2H node. It loops
whatever the "DAC" plays back into the "ADC", and injects the bench faults.

- **New `loopback_emu_test`** (56 checks): the real launcher, both real
  helpers, the real generator and the real reader. It covers:
  - send and receive: the tone generated is the tone received, SFDR 82 dB;
  - C2H driver error: restarted, reader re-attached, transmit untouched, no
    dialog;
  - card refuses more than 256 KiB: stepped down, the generator follows;
  - card never consumes: stops at 16 KiB, capture unaffected;
  - card consumes late: back to 1 MiB;
  - helpers ignoring SIGINT/SIGTERM: `stop()` returns in 0 ms, the event loop
    never pauses more than 64 ms, SIGKILL at 4.1 s;
  - Start during Stop: queued;
  - pre-flight: 3 ms, no device opened;
  - reader with no writer: 0.03 s CPU in 1.5 s;
  - missing H2C node: capture starts anyway;
  - capture-only session: transmit helper started on demand.
- **New `app_live_emu_test`** (20 checks): the **whole application** in a
  live PCIe session with the emulated card. It covers:
  - Start generator, then streaming and the chain rows;
  - Verify loopback ✓;
  - Stop and Start in 0–9 ms;
  - C2H failure recovered with no dialog, Verify ✓ again;
  - `sdr_backend.log` written;
  - the longest GUI-thread gap was 101 ms, and closing took 58 ms.
- `h2c_emu_test`: 1 MiB is checked to keep the DAC fed (758 MiB/s), and the
  default is updated.
- `control_window_test`: helper-state row, Retry transmit, and the overload
  row with its button (39 checks).

## [1.2.3] — 2026-09-26

H2C transmit now shows the whole chain as the device reports it, and it can
prove that the generated samples come back through the ADC. **GUI only.** The
firmware is unchanged since 1.2.1.

### Why "it is just looping and nothing reaches the hardware" could not be answered

The report came from a 1.2.1 screen. That screen showed *"seamless
4096-sample loop, refreshed 48×/s"* and `GR_updates=48/s`, and 1.2.2 has
already removed that rewriting. The real problem is that nothing on screen
could settle the question:

- **A completed DMA is not proof.** The `H2C_data_in` port has no TREADY. The
  card's DMA engine accepts data whatever happens after it, including when
  the DAC input switch is on the DDS and drops every sample. `iwfg_h2c`
  reporting hundreds of MiB/s says only that the card took the bytes.
- **A tone on screen is not proof.** It can be the DDS. Firmware 1.2.1 boots
  with the DAC on the DDS. If the RFDC tab is not attached (for example,
  without `cap_sys_rawio`), the GUI cannot switch it to the host stream.
- **The frequency read wrong.** The main window assumed 250 MSPS for a
  200 MSPS stream, which scaled every frequency by 1.25: the 14.99 MHz tone
  read as −18.80 MHz, which looks like something other than what was sent.
- **The Transmit tab showed the wrong parameters.** It opened with the
  `rdma_tx` control block (100 MSPS, 2450 MHz), which belongs to the RoCEv2
  transmitter and has no effect on PCIe H2C. The repeated *"Reloaded from
  control block"* lines came from its Revert button.

### Added — H2C TRANSMIT CHAIN, live from the device

At the top of the Transmit tab, each row read from the device where possible:

| Row | Shows |
|---|---|
| DAC input | from the switch register; red if it is the DDS while the generator runs, amber if unknown |
| DAC | DAC rate, interpolation, stream clock, and whether the generator runs at that clock |
| NCO / RF | DAC NCO, the tone's RF frequency, ADC NCO |
| ADC (loopback) | ADC rate, decimation, per-channel rate, and whether the display assumes it |
| H2C to card | `iwfg_h2c`'s delivery against what the DAC reads |
| Received | the received peak, and whether it is the generator's tone, the DDS or neither (inversion noted) |

The snapshot now reads both converters (rates and NCOs) whatever the RFDC
tab's target is set to. It is refreshed when the generator starts.

### Added — Verify loopback

One button. It moves the generator's tone by 7.3 MHz (a step that cannot be
confused with the DDS), then back, and checks that the tone received through
the ADC moves with it. This is the only real proof that the generated samples
reach the DAC.

- **Pass:** *"✓ Loopback verified. The generator moved 7.300 MHz and the tone
  received through the ADC moved −7.300 MHz (the loopback inverts the
  spectrum)… The generated samples ARE reaching the DAC and coming back
  through the ADC."*
- **Fail:** it names the cause, from what the device reports:
  - the DAC input is the DDS;
  - the RFDC tab is not attached;
  - `iwfg_h2c` is not transferring;
  - or, if data is moving and the switch is on the host stream, the card-side
    path (`QDMA H2C → H2C_data_in → DDS_H2C_switch S00_AXIS`) is not
    delivering. That points to the FPGA or driver, and it says what to probe
    with the ILA.
- A step that comes back scaled is reported as a sample-rate mismatch, not as
  a transmit failure.
- The original tone is restored afterwards.
- It works in Simulation too.

### Fixed — received frequencies scaled by a wrong display rate

When the device reports its ADC stream clock, the main window's live display
rate is set to it, with a status-bar note: *"Display sample rate set to
200.000 MSPS from the device's ADC stream clock (it was 250.000 MSPS)"*.

### Changed — the rdma_tx block appears only for RoCEv2

It is now labelled *"RDMA TRANSMITTER — rdma_tx control block (RoCEv2 path;
not used by PCIe H2C)"* and hidden in PCIe and Simulation sessions.

### Tests

- `control_window_test` drives the loopback check against a stand-in receiver
  in three cases:
  - the receiver follows the generator: verified, inversion reported;
  - the receiver stays on the DDS: not verified, cause named;
  - the display rate is wrong: reported as scaling.

  It also checks that the tone is restored (29 checks).
- New `app_loopback_test`: the **whole application** in Simulation. It runs
  the real MainWindow, DSP chain and simulated loopback, driven through the
  H2C window. Host + generator → verified; DAC on the DDS → not verified,
  with the cause.

## [1.2.2] — 2026-09-26

The host-transmitted tone now comes out pure. **Only the GUI changed.** The
firmware is identical to 1.2.1, so there is nothing to reflash if you already
flashed 1.2.1.

### Fixed — broadband interference on the Host / GNU Radio stream

The bench spectrum showed the tone sitting on a raised skirt, a dotted line at
0 Hz and bursts across the whole band. The status bar gave the cause:

```
[H2C playback] [H2C] 640.58 MB/s  total=17934.0 MB  GR_updates=48/s
```

`iwfg_h2c` counts in MiB. The DAC reads one 32-bit I/Q word per 200 MHz
stream clock, which is **762.9 MiB/s**. The host was delivering about 84% of
that. The bitstream's `H2C_data_in` port has **no TREADY**, so the DAC cannot
wait for data. Whenever the card FIFO ran dry, the DAC played stale words:

- the stale stretches amplitude-modulated the tone, which raised the skirt
  around it;
- a held word is a constant, which lands at 0 Hz after the NCOs — the dotted
  DC line;
- each restart of the data was a step, which spread energy across the band.

The generator was not the bottleneck; `iwfg_h2c` was. It moves the FIFO to
the card with one blocking DMA call per chunk, and at 16 KiB per chunk the
fixed cost of each call held delivery near 640 MiB/s. The C2H helper already
uses 16 MiB buffers with the same driver and reaches over 2 GB/s.

What changed, as you suggested ("buffer and send"):

- **The H2C buffer is 16 MiB**, up from 16 KiB. Each call now moves 16 MiB,
  so the fixed cost is spread over 1024× more data and delivery is limited by
  the DAC, not the host.
- **The generator writes the loop once.** It builds one seamless loop exactly
  one buffer long, hands it to `iwfg_h2c`, and then leaves the FIFO alone. The
  card replays it with nothing further crossing the FIFO (`GR_updates=0/s`).
  It is rewritten only when you change a setting. 1.2.1 rewrote it 50 times a
  second, and at 16 MiB each rewrite would have made `iwfg_h2c` copy the data
  between transfers.
- **Frequency steps are about 48 Hz**, down from 48.8 kHz. The loop is now
  4,194,304 samples: 10 MHz is sent as 9.999990 MHz and 5 MHz as 5.000019 MHz.
- Noise is also sent as a single loop.
- **Stop means silence.** `iwfg_h2c` keeps replaying the last chunk it
  received, so after Stop the DAC used to go on transmitting the tone. Stop
  now sends a zero chunk first.

### Added — H2C link monitor

The generator panel shows `iwfg_h2c`'s measured delivery against what the DAC
reads, for example *"H2C link 762.9 MiB/s for a DAC that reads 762.9 MiB/s —
kept full (buffer 16 MiB)"*. At 640 MiB/s you would now see *"⚠ … it runs dry
about 16% of the time and plays stale data"*. What you see on screen can be
checked against a number.

### Added — automatic step-down

If the driver will not take a 16 MiB H2C buffer, the GUI steps down on its
own: 16 MiB → 4 MiB → 1 MiB → 256 KiB → 16 KiB. It does this when
`iwfg_h2c` exits, or keeps timing out, before it has transferred anything,
and it says so in the status bar. The generator's loop follows the size
actually in use. To choose the size yourself, set `IWFG_H2C_CHUNK_BYTES`
(for example `IWFG_H2C_CHUNK_BYTES=4194304 ./sdr_workstation`).

### Fixed — the GUI could vanish when the H2C helper restarted

Writing to a FIFO whose reader has gone raises `SIGPIPE`, and its default
action terminates the process. Nothing handled it, so `iwfg_h2c` exiting while
the generator was writing could close the whole application. `SIGPIPE` is now
ignored process-wide (writers already handle `EPIPE`), and blocked in the
generator thread as well.

### Changed — Simulation

The simulated loopback evaluates the host loop on the fly instead of building
a table, since a 16 MiB loop is 4 M samples. What it shows is unchanged.

### Tests

- New `TS/01_Host_Test_Suite/h2c_emu`: the **real** `iwfg_h2c` fed by the real generator,
  with the card replaced by an `LD_PRELOAD` fake that consumes at the DAC's
  rate behind a per-transfer cost and records what the DAC would play.
  - 16 KiB chunks: **604 MiB/s, starved.** 16 MiB chunks: **768 MiB/s, kept
    fed.**
  - Across a loop seam, the DAC stream is a pure tone at **SFDR 90.8 dB**.
  - The loop is written once, live changes arrive, Stop leaves silence, and a
    refused buffer triggers the step-down (17 checks).
- The GUI tests were updated for the write-once loop, the 16 MiB loop, the
  link monitor and silence on Stop.

## [1.2.1] — 2026-09-25

The DAC input source was swapped (DDS and host), the host transmit path now
sends exactly what it reports, and Simulation shows the transmit side.

### Fixed — DAC input source: "DDS" played the host stream and vice versa

The bitstream wires the `DDS_H2C_switch` like this (from the hardware
handoff in `H2C_pcie_control.xsa`, `design_1.hwh`):

```
S00_AXIS  <-  External_Interface_H2C_data_in   host / GNU Radio (QDMA H2C)
S01_AXIS  <-  dds_compiler_2 / M_AXIS_DATA      DDS compiler
M00_AXIS  ->  axis_broadcaster_1 -> DAC tiles
```

The baseline firmware's comment said `Route SI 0 (DDS)`. Its menu text
(`0 = GNU Radio, 1 = DDS`) was right, but the 1.0 clean-up (finding M-7)
followed the comment instead, and every layer inherited the swap: the serial
menu, the PCIe event, the GUI and `rfdc_ctl`. That was our error.

Two consequences went beyond a wrong label:

- **The board booted with the DAC on the host stream**, which is idle at
  power-on, so there was no tone at all. The readback still reported "DDS",
  so the 1.1.4 advisory for "DAC on host stream" could never fire. This is
  very likely a large part of the earlier "ENOB 11 → 6–7" report: with no
  carrier, the analyser was measuring noise.
- The 1.1.4 guidance "menu 16 → 0 (DDS compiler)" actually selected the host
  stream on those builds.

What changed:

- The logical values are unchanged everywhere: **0 = DDS, 1 = host**
  (`PCIE_DAC_SRC_DDS` / `PCIE_DAC_SRC_HOST` in `pcie_regs.h`). The firmware
  translates them to switch ports in exactly one function, from two constants
  in `platform_ids.h` (`HW_DAC_SW_SI_HOST 0`, `HW_DAC_SW_SI_DDS 1`). If the
  block design is rewired, change only those two lines.
- **Boot now puts the DDS on the DAC**, as documented. The serial console
  shows it: `[init]     DAC input = DDS compiler`.
- Every change is **verified at the switch**: the firmware reads the MI0 mux
  register back, the PCIe ACK reports failure if the switch did not take it,
  and the readback comes from the register rather than a remembered value —
  so a change made from the serial menu is also reported correctly over PCIe.
- Serial menu 16 shows the current routing and confirms from the register.
  Menu 17 (diagnostics) shows the DAC input.
- `rfdc_ctl dac-source` accepts `dds` / `host` as well as `0` / `1`. Anything
  else is rejected; `host` used to parse as 0 and select the DDS.
- Capability bit `PCIE_CAP_DAC_SOURCE_MAPPED` (bit 6, caps now `0x7F`). ABI
  stays 4: the protocol did not change, only the firmware's handling of it.

**Mixed versions are safe.** The 1.2.1 GUI reads the capability word. On a
board still running older firmware it inverts the value in both directions,
so its DAC input selection and readback are correct before you reflash (it
logs a note saying so). Only the old firmware's own serial menu and an old
`rfdc_ctl` remain swapped until you reflash. A 1.2.0 GUI works correctly
against 1.2.1 firmware.

### Fixed — the host generator's frequency was scaled by decimation

The generator's sample rate came from the Transmit tab's `iq_ctl` rate. That
rate was filled in from the **ADC** stream clock when the RFDC panel targets
the ADC (the default). The ADC clock only equals the DAC clock at the boot
configuration, so any decimation change rescaled every generated frequency.
With no control block and no readback, the rate was **0.001 MSPS** and the
generator produced garbage.

- The generator now has its own **Sample rate** field: the **DAC stream
  clock** (one I/Q pair per clock, 200 MSPS at boot), filled in from the
  device's `PCIE_RB_DAC_STREAM_KHZ` readback. The `iq_ctl` rate is set from
  the DAC clock as well.

### Fixed — the host stream carried loop spurs (now a clean tone)

`iwfg_h2c` does not stream the FIFO sample by sample. It **replays the most
recent 16 KiB chunk (4096 I/Q pairs) back to back** until the next one arrives,
because the card consumes ~800 MB/s. Each chunk is therefore played as a loop.
A tone that does not fit the loop a whole number of times jumps phase at every
replay. 10 MHz at 200 MSPS is 204.8 cycles per chunk, for example. The DAC
output carried a comb of spurs every 48.8 kHz. The GNU Radio chain had the
same defect.

- **Loop-coherent generation (default):** the generator transmits the nearest
  frequency that fits the loop exactly, `k × fs / 4096`. Every chunk is
  identical and seamless, so replaying it is the same as true streaming. The
  field shows what is really transmitted: *"Transmits 10.009766 MHz — the
  nearest frequency that loops seamlessly in the 4096-sample H2C buffer
  (step 48.828 kHz)"*. Exact multiples, such as 12.5 MHz, are transmitted
  exactly.
- Measured through a real FIFO and a consumer that replays chunks the way
  `iwfg_h2c` does: **SFDR 93 dB** (14-bit limited) coherent, against **9 dB**
  for the old phase-continuous stream (`TS/01_Host_Test_Suite/gui/tx_path_test`).
- Untick *Loop-coherent* to stream the exact frequency, as GNU Radio did.
- The loop is refreshed about 50 times a second instead of rewritten flat
  out, which frees a CPU core. Parameter changes still take effect within
  20 ms.

### Fixed — generator control

- **Edits apply while it runs.** Waveform, tone, IQ phase, amplitude, rate and
  coherence are applied at the next chunk. Before, `updateConfig` was a queued
  slot on a thread whose loop never returned to its event loop, so edits were
  never delivered.
- **Starting the generator routes the DAC to the host stream** when the RFDC
  panel is attached, and the device confirms it. When the panel is not
  attached, the log says the routing could not be checked instead of assuming
  it.
- **The state is honest.** It shows *Waiting for iwfg_h2c to open the FIFO*
  until the H2C backend is really reading, then *Transmitting*, with the loop
  refresh count as proof that samples are flowing.
- If the FIFO could not be opened, for example because the path was not a
  FIFO, the generator stopped but stayed marked as running, and later Starts
  were ignored. It now resets.
- Spin boxes commit on Enter or focus loss. Typing "25" no longer transmits
  2 MHz on the way.

### Added — Simulation shows the transmit side

With no hardware, the simulator now models the DAC → ADC loopback from the
H2C / RFDC window, with the same three outcomes as the board:

| DAC input | Generator | Plots show |
|---|---|---|
| DDS compiler | any | the DDS tone at the DDS frequency |
| Host / GNU Radio | running | the generator's waveform — the same samples it writes to the FIFO |
| Host / GNU Radio | stopped | noise only |

- The DAC input source and DDS frequency stay usable in Simulation without a
  device. The generator is modelled and never writes to the FIFO.
- A **Simulation** box on the RFDC tab turns the loopback on or off. It turns
  on by itself when you touch a transmit control. When it is off, the
  original modem demo runs unchanged.
- The status bar names what is shown, for example *"Simulated loopback: DAC
  input DDS, 15.000000 MHz"*. It also says when a tone is outside the
  displayed band instead of letting it alias into view.

### Added — tests (`TS/01_Host_Test_Suite/`)

- `firmware/run_tests.sh`: `main.c`'s routing code, verbatim, against a
  register model of the switch with the hwh wiring (17 checks).
- `pcie_emu/`: the GUI's real `RfdcControl` talks to the firmware's real
  `pcie_cfg.c` through an emulated BAR, against 1.2.1 **and 1.2.0** firmware.
  It shows what the DAC would play after every command (17 checks each).
- `gui/`: transmit path end to end (18), simulated loopback (12), and the
  control window driven offscreen (16).

### Package

- Removed a stray duplicate of the firmware sources at the top of
  `SC/` (identical copies, but a trap for edits) and three empty
  directories named `{SC,...` left by a packaging script.
- `FF/03_Documentation/pcie_regs.h.reference` refreshed; it had fallen behind
  the header by two releases.

## [1.2.0] — 2026-09-24

Focused on the converter metrics: ENOB and the RF parameter set.

### The measurement itself was audited first, and cleared

Fed a perfect, noiseless tone, the SINAD calculation floors at **~87 dB —
ENOB 14.2**, so it can report 11 bits comfortably. The guard width is 5 bins,
correctly derived from the Blackman-Harris ENBW (measured 2.005, textbook
2.004).

The failure mode was checked too: with a 2-bin guard the window skirts fall
into the noise sum and a *perfect* tone reads **29 dB / ENOB 4.5** — which
would have matched the symptom exactly. It is not what the code does.

So the measurement was telling the truth. But two real defects were found
alongside it.

### Fixed — "SNR" was not SNR

```c
out.metrics.snrDb = peakDbfs - noiseFloorDbfs;   // two single bins
```

That is peak-to-median-**bin** contrast, not signal power against integrated
noise power. The two differ by tens of dB, which is why the panel could show
"SNR 12.86 dB" beside an ENOB implying 40 dB and neither could be reconciled
with the other.

- **SNR** is now signal power over noise power **with the harmonics removed**
  — so it is genuinely distinct from SINAD, which includes distortion.
- The old figure is kept, correctly labelled **Peak / floor**. It is useful;
  it just is not SNR.

### Fixed — ENOB was computed from noise when there was no carrier

SINAD and ENOB are ratios against a *fundamental*. With no carrier the
"fundamental" is the largest noise bin, and both stop meaning anything — they
do not become low, they become undefined. Printing 6.5 bits in that state is
worse than printing nothing: it reads as a converter fault and gets chased as
one.

A carrier must now pass two independent tests, because either alone has false
positives:

- stand at least **10 dB** clear of the median bin;
- not sit at the extreme edge of the span, where the band edge and window
  skirt produce a peak that is not a signal.

When they fail, **SNR, SINAD and ENOB show "—"** with the reason recorded,
rather than a number derived from noise.

### Added — a guard on the parameter table

The row labels and the value list are parallel sequences kept in step only by
care. Inserting one without the other shifts every parameter below it, so ENOB
would display SFDR's number. The loop hid that by stopping early; it now
asserts and warns instead.

## [1.1.4] — 2026-09-24

### Added — advisory notes on configurations that ruin a measurement

ENOB fell from 11 bits to 6.5 and nothing on screen explained it, because
every individual setting was legal. The GUI now says so after a readback:

- **Decimation 1x when the design boots at 24x.** Decimation low-passes then
  downsamples, discarding out-of-band noise while keeping the signal, so it is
  worth `10*log10(24) = 13.8 dB` — **2.3 ENOB bits** thrown away.
- **DAC input = host stream.** With no H2C playback running there is no signal
  at all; the ADC measures its own noise floor, "signal" becomes the largest
  noise bin, and SINAD — hence ENOB — is meaningless rather than merely low.
- **QMC gain near zero**, which mutes the signal. Boot value is 1.0.

These are advisory only: nothing is changed on the device. The panel shows
*"Read OK — N advisory note(s)"* and the detail goes to the log.

### Note

A board configured by an older GUI keeps those settings. Reinstalling the GUI
does not undo them — power-cycle, use COMPLETE RESET (needs ABI 4), or restore
via the serial menu (options 1, 3, 12, 16).

## [1.1.3] — 2026-09-24

### Fixed — pressing OK stopped everything

A failing **H2C playback** helper tore down the entire session, including a
healthy C2H capture running at 1.5 GB/s. Dismissing the dialog left the
application idle with nothing running.

The cause was a line of my own reasoning in `onProcessFinished()`:

> *One dead stage means the pipeline is broken: bring the rest down.*

True for C2H — it is the SOURCE, and without it there is no data. **False for
H2C**, which is an independent transmit path. Its failure says nothing about
the capture, so killing the capture was destructive rather than defensive.

- Stages are now classified **essential** or **optional**. C2H is essential;
  H2C playback is not.
- An optional stage dying reports and returns. Capture keeps running, the
  dialog is titled *"Backend — capture continues"*, and the message says so.
- Only a fatal failure stops the source, so the application can no longer be
  left stuck by a transmitter that would not start.

### Fixed — a helper that cannot load is now repaired automatically

`exists() + isExecutable()` passes for a binary compiled against a newer
glibc, which then dies at load. The GUI now **runs the helper briefly before
starting the pipeline**, detects loader failures (`GLIBC_`, missing shared
object, bad interpreter), and **rebuilds it from source for this host**,
retrying once.

Verified: a deliberately broken helper was detected, rebuilt, and the result
was byte-identical to a known-good build. The exact message from the reported
failure —

```
libc.so.6: version `GLIBC_2.38' not found (required by iwfg_h2c)
```

— is recognised by the detector.

If the rebuild also fails, the dialog gives the two commands to run by hand.

## [1.1.2] — 2026-09-24

### Fixed — `GLIBC_2.38 not found (required by iwfg_h2c)`

The package shipped **prebuilt** `backend/bin/c2h_stream` and `iwfg_h2c`. They
were compiled on a newer host, so on a machine with an older glibc the loader
refused `iwfg_h2c` outright.

The project already builds these helpers from source — so why were the stale
ones used? Because `make` compares timestamps, and the packaged binaries were
**newer than their sources**. make concluded they were up to date and skipped
the rebuild. Shipping the binaries silently disabled the mechanism meant to
prevent exactly this.

`c2h_stream` kept working only by luck: it does not reference a glibc 2.38
symbol, so the loader accepted it. That is why C2H ran at full rate while H2C
died instantly — the same root cause with two different symptoms.

Two changes, either of which would have been enough; both, because this class
of fault is expensive to diagnose:

- **`backend/bin` is no longer shipped.** Host-specific binaries do not belong
  in a source package, any more than the stale `.o` files removed earlier.
- **The helpers now rebuild unconditionally** (`make -B`). Two small C files
  cost about a second, and it makes them immune to timestamps: a restored or
  copied binary can no longer suppress its own rebuild.

Verified: a tree with `backend/bin` removed rebuilds both helpers; a binary
given a 2099 timestamp is now rebuilt anyway, where before it was skipped.

## [1.1.1] — 2026-09-24

### Fixed — "H2C playback stopped unexpectedly (exit 1)"

The dialog reported an exit code and nothing else. `iwfg_h2c` prints why it is
quitting, and that output was captured — into the log, not the dialog. So the
diagnosis existed and the operator never saw it.

- **The backend's own last output is now in the failure dialog.** Up to 12
  lines, which is enough for a usage block or an errno message.
- **DMA nodes are pre-flighted before a helper is spawned.** Launching a
  process that dies on `open()` yields an exit code; checking first yields the
  reason:

  ```
  H2C playback: the device node /dev/iwfg1 does not exist.

  Present in /dev: iwfg0

  The driver derives the direction from the node: /dev/iwfg0 is C2H
  (receive), /dev/iwfg1 is H2C (transmit). If only iwfg0 exists, the
  driver did not create an H2C queue — check that the module loaded
  with both queues enabled (dmesg | grep iwfg).
  ```

- A node that exists but is not accessible now reports owner, group and mode,
  and says explicitly **not** to solve it by running the GUI as root — that
  breaks the GPU path, because a CUDA IPC handle cannot cross users.

## [1.1.0] — 2026-09-23

### Added
- **Firmware identity readable over PCIe** — `PCIE_RB_ABI` (0x12) and
  `PCIE_RB_CAPS` (0x13). The GUI reads them at attach and reports a version
  mismatch in plain language instead of failing generically. The query is
  version-safe: older firmware rejects it, and the rejection is the answer.
- `PCIE_ABI_REQUIRED` — one constant naming what the host needs.
- RoCEv2 extractor tap (`RTAP`) support alongside the existing host ring.
- pcap import: RoCEv2 via `roce-extractor`, plain UDP in-process.
- Wire-format detection by measuring the capture, not trusting its name.
- GPU FFT offload (`GpuFft`) for every source, with a CPU/GPU status badge.
- GPUDirect ring (`IQRINGG1`) opened via CUDA IPC instead of refused.
- `iqring_sim`, `detect_format`, `gpu_selftest`, `gpu_p2p_check.sh`.

### Fixed
- Offline analysis no longer requires acquisition hardware.
- Device list no longer changes meaning with the transport; the RDMA NIC is a
  separate control.
- GPU offload was previously unreachable — the toggle set a flag nothing read.
- `READBACK`/`DEFAULTS` were accepted by the validator but had no handler, so
  the firmware ACKed them as successful and did nothing.

### Changed
- ABI 3 → **4**. A host requiring readback or defaults must find ABI ≥ 4.

## [1.0.1], [1.0.0]
See `UM/02_Developer_Notes/`.
