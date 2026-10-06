# DAC input source, host transmit and simulation (1.2.1, updated 1.2.5)

## 1. What feeds the DAC

A 2-to-1 AXI4-Stream switch (`DDS_H2C_switch`) chooses what the DAC plays:

| Value | Name | Source | Switch port |
|---|---|---|---|
| **0** | DDS compiler | internal tone, `dds_compiler_2` | S01_AXIS |
| **1** | Host / GNU Radio stream | samples sent over PCIe (H2C) | S00_AXIS |

The port wiring comes from the bitstream itself: `design_1.hwh` inside
`H2C_pcie_control.xsa`, instance `DDS_H2C_switch`.

Firmware **before 1.2.1** passed the value straight through as a port number,
so 0 and 1 were swapped everywhere. The board also booted on the idle host
stream, which meant no tone at power-on. 1.2.1 fixes this and boots on the
DDS.

### Selecting it

| From | How |
|---|---|
| GUI | H2C CONTROL → **RFDC** → *DAC input source* |
| Serial | menu **16** → `0` DDS, `1` host (shows the current routing first) |
| CLI | `sudo ./rfdc_ctl -b <BDF> dac-source dds` or `... dac-source host` |

Every change is verified at the switch register. The GUI logs *"DAC input
confirmed by the device: …"*, and the serial menu prints *"(verified at the
switch)"*.

### How to tell which firmware you have

Serial console at boot:

```
[init] 8/9 DDS/H2C sw    (PL 0x...)...
[init]     DAC input = DDS compiler                  <- 1.2.1
[pcie] pcie_cfg ABI 4, ... caps 0x7F: basic ... DAC-SOURCE-MAPPED
```

Older firmware has neither line. The 1.2.1 GUI detects it and corrects for
it, logging *"this firmware predates 1.2.1 … The GUI is compensating"*. On
that firmware, serial menu 16 and `rfdc_ctl dac-source` stay swapped until
you reflash.

## 2. Transmitting from the host (IQ generator)

H2C CONTROL → **Transmit** → *IQ Signal Generator*.

1. Start the session with **C2H + H2C** (or *H2C DAC Only*). The GUI launches
   `iwfg_h2c`, which reads `/tmp/iwfg_h2c.fifo`. In an *ADC Only* session,
   it launches `iwfg_h2c` when you start the generator (1.2.4).
2. Attach the **RFDC** tab. The generator's *Sample rate* is then filled in
   from the DAC stream clock (200 MSPS at boot).
3. Pick the waveform, tone, IQ phase and amplitude, then press **Start
   generator**. If the DAC is on the DDS, the GUI switches it to the host
   stream and the device confirms.
4. The state goes from *Waiting for iwfg_h2c…* to *Transmitting …*. If it
   stays on *Waiting*, look at the **H2C playback** row of the chain, which
   says what the helper is doing and why.

You can change parameters while it runs; they take effect within about 20 ms.

### Proving the samples reach the DAC (1.2.3)

The Transmit tab opens with the **H2C TRANSMIT CHAIN**. Each row is read from
the device where possible:

```
DAC input       Host / GNU Radio stream — confirmed by the device
DAC             9600.00 MSPS, interpolation ×24 → stream 200.000 MSPS
NCO / RF        DAC NCO 3100.000 MHz → tone at RF 3114.990 MHz · ADC NCO 3100.000 MHz
ADC (loopback)  4800.00 MSPS, decimation ×24 → 200.000 MSPS per channel
H2C to card     iwfg_h2c is moving 762.9 MiB/s to the card (the DAC reads 762.9)
Received        -14.990 MHz at -40.2 dBFS — the generator's frequency (spectrum inverted)
```

Neither a busy DMA nor a tone on screen proves that the generator is what the
DAC plays. The card accepts H2C data even when the switch drops it, and the
tone could be the DDS. **Verify loopback** settles it. It moves the tone by
7.3 MHz and back, and checks that the received tone moves with it. On
failure it names the cause:

| Result | Meaning |
|---|---|
| ✓ verified | the samples reach the DAC and come back through the ADC |
| ✗ did not move — DAC input is the DDS | select Host / GNU Radio stream |
| ✗ did not move — RFDC not attached | attach it (needs `cap_sys_rawio`), or serial menu 16 → 1; 1.2.1+ firmware boots on the DDS |
| ✗ did not move — iwfg_h2c reports no transfers | the H2C backend is not running |
| ✗ did not move, data moving, switch on host | the card-side path QDMA H2C → `H2C_data_in` → switch S00 is not delivering: probe with the ILA |
| moved by the wrong amount | the display's sample rate does not match the stream |

The main window's live display rate is set from the device's ADC stream clock
automatically, so received frequencies read in true Hz.

### The helpers behind the chain, and how they recover (1.2.4)

Two rows show the helper applications themselves.

**H2C playback** is `iwfg_h2c`'s state, with the reason:

| State | Meaning | What happens / what to do |
|---|---|---|
| waiting for the generator | running, nothing has connected to its FIFO | press **Start generator** |
| starting | the first buffer is being transferred | a working transfer reports a rate within about a second |
| streaming | the card is consuming | — |
| restarting | being relaunched with a smaller buffer, or by Retry transmit | automatic |
| stalled | the card is not consuming | see below |
| failed | `iwfg_h2c` exited | the reason is shown; press **Retry transmit** |

**Buffer size is automatic.** The default is 1 MiB. If no transfer completes
within 4 s of the first buffer, or `iwfg_h2c` reports stall kicks, driver
timeouts or device cycling before it has streamed, the GUI first **reopens it
at the same size** (1.2.5). A reopen rebuilds the H2C queue and re-enables
the PL H2C datapath, which the driver's C2H-overflow recovery can leave
disabled. Only if the same size stalls again does it step down: 1 MiB →
256 KiB → 64 KiB → 16 KiB. The generator follows the new loop length by
itself. A helper that has streamed and then stalls for about 4 s is also
reopened at the same size, at most 3 times a minute.

If the card takes nothing even at 16 KiB, the problem is not the buffer. The
row then reads *stalled — … not a buffer-size problem*. Check:

- DAC input = Host (RFDC tab);
- the RF DAC is running (RFDC ▸ Start);
- the iwfg driver enables the PL H2C datapath (`dmesg | grep iwfg`).

`iwfg_h2c` stays running and resumes the moment the card consumes. When it
does, the GUI relaunches it once at 1 MiB, so you are not left on 16 KiB.

**C2H capture** shows what the capture delivers against what the configured
stream needs:

```
C2H capture   2417 MiB/s delivered for 3052 MiB/s needed (4 ch × 200.00 MSPS × 4 B)
              — OVERLOADED (79%): the C2H FIFO overflows, and each overflow
              recovery resets the QDMA, which interrupts H2C transmit too.
```

An overloaded capture disturbs transmit. The driver recovers from a C2H FIFO
overflow by soft-resetting the QDMA, and that rebuilds the H2C queue too,
even mid-transfer. The details for the driver team are in
`UM/02_Developer_Notes/DRIVER_OVERFLOW_RECOVERY.md`.

**Transmit guard (1.2.5).** When you start the generator in a live session,
the GUI checks the capture. If it needs more than 1600 MiB/s (4 channels at
200 MSPS need 3052), the GUI:

- reduces it to 2 channels, or 1, via the PL GPIO mode (the RFDC tab must be
  attached);
- checks the measured rate afterwards, and follows the channel count the
  stream really carries;
- restores the original setting when the generator stops.

The channels kept are the first stream channels (AXIS outputs 0, 1). Route
your looped-back ADC to AXIS output 0 on the RFDC tab (ADC Channel Routing)
so it stays visible. Without RFDC access the GUI cannot change the PL mode,
and says so in a notice.

To reduce the capture by hand for a loopback test, **Capture 1 channel (PL GPIO mode 1)**
does it in one click (the RFDC tab must be attached): it sets PL GPIO mode 1
and the display to one channel. The channel kept is AXIS output 0; the ADC
Channel Routing on the RFDC tab decides which ADC input that is. Three seconds
later the measured rate is checked. If the stream did not shrink, both
changes are undone and you are told.

**Stop order and device reset (1.2.5).** Stop closes transmit first and
capture last. The driver soft-resets the QDMA only when the last C2H stream
closes with no other stream open, so every Stop/Start now begins on a clean
device.

If the capture delivers nothing for about 5 s while running (the card has
stopped delivering C2H data), the GUI resets the device the same way: close
transmit, close capture, start both again. It does this at most twice in 3
minutes. If that does not bring the data back, a notice tells you to reload
the driver or power-cycle.

The capture FIFO also gets a larger pipe buffer (1 MiB instead of 64 KiB),
so the host drains faster.

**The capture helper restarts itself.** `c2h_stream` exits with code 0 for
every reason it stops, so the GUI reads what it printed instead. If it ends
on its own after streaming, the GUI:

- names the reason (DMA driver error, reader closed, stray signal);
- restarts it, up to 3 times a minute;
- re-attaches the display to the new FIFO.

There is no dialog, only a status-bar line and a log entry. If the transmit
helper was stalled at that moment, it is stopped too, because it is the
likely culprit; Retry transmit brings it back. A capture helper that cannot
start, or keeps dying, ends the session with the reason.

**Everything the helpers print** is written to `$TMPDIR/sdr_backend.log`
(normally `/tmp/sdr_backend.log`), with timestamps. The previous session is
kept as `.1`. Include this file when reporting a problem.

| You see | Likely cause | Do |
|---|---|---|
| H2C playback *waiting for the generator* while the generator runs | the generator writes a different FIFO | it follows the backend's since 1.2.4; check the FIFO field |
| *restarting* repeatedly, then *stalled* at 16 KiB | the card consumes nothing | DAC input = Host, RFDC ▸ Start, driver |
| *streaming*, but Verify says the tone did not move | data reaches the card, not the DAC | FPGA path QDMA H2C → `H2C_data_in` → switch S00 (ILA) |
| C2H capture *OVERLOADED* | more channels than PCIe carries | **Capture 1 channel** |
| status bar *Capture helper ended — … restarted* | a C2H DMA error (see the log) | nothing, if it recovers; otherwise send `sdr_backend.log` |
| status bar *Resetting the DMA device* | the capture delivered nothing for 5 s | nothing, if it recovers; after two tries the GUI asks you to reload the driver |
| *Transmit guard: capture reduced from 4 to 2 channel(s)* | the capture would overflow and disturb transmit | route the looped ADC to AXIS output 0 or 1 |
| ADC row *QMC gain reads 0.0000* | if QMC gain is enabled, the capture is muted | RFDC tab: QMC gain 1.0 for the ADC |

### Keeping the DAC fed (1.2.2)

The DAC reads one 32-bit I/Q word per stream clock, which is 762.9 MiB/s at
200 MSPS. Its input has **no flow control**: if the host delivers less, the
DAC plays stale data. On the spectrum that shows as a raised skirt around the
tone, bursts across the whole band, and a dotted line at 0 Hz.

The generator panel shows the live figure under the state line:

```
H2C link 758.0 MiB/s for a DAC that reads 762.9 MiB/s — kept full (buffer 1 MiB).
⚠ H2C link 640.6 MiB/s, but the DAC reads 762.9 MiB/s: it runs dry about 16% ...
```

What makes the difference is the H2C buffer, `iwfg_h2c`'s chunk size. The
helper sends one blocking DMA call per chunk. At 16 KiB, the fixed cost of
each call held delivery at about 640 MiB/s (the red line above).

1.2.2 went to 16 MiB, and on the bench nothing then reached the DAC. A 16 MiB
user buffer is thousands of H2C descriptors per transfer, and the card's H2C
path did not complete it. From 1.2.4 the default is **1 MiB**, which already
makes the per-call cost negligible (0.3 %). The GUI steps down by itself if
even that does not work (see above). To force a size yourself:

```bash
IWFG_H2C_CHUNK_BYTES=4194304 ./SC/01_sdr_workstation_GUI/build/sdr_workstation
```

A steady, faint line at 0 Hz that does not flicker is the ADC's own DC offset,
not the transmit path.

### Why the tone can be a few Hz off what you typed

`iwfg_h2c` replays the chunk it last received, over and over. So the generator
builds **one seamless loop exactly one buffer long**, writes it once, and the
card replays it: nothing further crosses the FIFO until you change a setting
(the status bar shows `GR_updates=0/s`). A loop is seamless only when the tone
fits it a whole number of times. With **Loop-coherent** ticked (the default),
the generator therefore sends the nearest frequency that does. The line under
*Tone* always shows what is really sent:

```
Tone  10.000000 MHz
      Transmits 9.999847 MHz — the nearest seamless frequency
      (1 MiB H2C buffer, 262144-sample loop, step 762.939 Hz).
```

With the 16 KiB buffer of 1.2.1 the step was 48.8 kHz. With 16 MiB (forced
through `IWFG_H2C_CHUNK_BYTES`) it is 47.7 Hz. Unticking
*Loop-coherent* streams the exact frequency as a continuous series of chunks,
which is what GNU Radio did. `iwfg_h2c` then has to copy every new chunk in
between transfers, so the output is less clean. This is not recommended.

**Stop** sends a silent chunk before closing, so the card is not left
replaying the last tone.

### Using your own GNU Radio flowgraph instead

Leave the built-in generator **stopped** (two writers on one FIFO corrupt each
other). Set *DAC input source* to *Host / GNU Radio stream*, then run your
flowgraph into `/tmp/iwfg_h2c.fifo` as before. `iwfg_h2c` publishes a new
chunk only once a full buffer (1 MiB by default) has arrived, and replays the
last one meanwhile. A continuous GNU Radio stream is therefore cut into loops,
with a short copy between transfers each time a new one arrives. For a clean
tone from GNU Radio, send a signal that repeats exactly every buffer: a
frequency that is a multiple of `fs / (buffer bytes / 4)`.

## 3. Simulation

In Simulation mode (no hardware) the plots can show what the transmit settings
would produce through a DAC → ADC loopback:

| DAC input | Generator | Plots show |
|---|---|---|
| DDS compiler | any | the DDS tone at the DDS frequency |
| Host / GNU Radio | running | the generator's exact waveform |
| Host / GNU Radio | stopped | noise only, as on the board |

- *DAC input source* and *DDS frequency* on the RFDC tab stay usable with no
  device. **Start generator** runs a model; nothing is written to the FIFO.
- The **Simulation** box on the RFDC tab switches the loopback on and off. It
  turns on by itself the first time you touch a transmit control. Untick it
  to go back to the modem demo signal.
- The status bar names what is shown. It also says when a tone lies outside
  the displayed band instead of letting it alias into view.
- The loopback assumes matched NCOs (DAC NCO = ADC NCO, as at boot), so the
  transmitted baseband frequency is the received one.

## 4. Checking it without the board

```bash
TS/01_Host_Test_Suite/run_all.sh                                # everything below, one command
TS/01_Host_Test_Suite/firmware/run_tests.sh                     # firmware routing, 17 checks
cmake -S TS/01_Host_Test_Suite/pcie_emu -B /tmp/emu && cmake --build /tmp/emu && ctest --test-dir /tmp/emu
cmake -S TS/01_Host_Test_Suite/gui -B /tmp/gt && cmake --build /tmp/gt && QT_QPA_PLATFORM=offscreen ctest --test-dir /tmp/gt
cmake -S TS/01_Host_Test_Suite/h2c_emu -B /tmp/h2c && cmake --build /tmp/h2c && ctest --test-dir /tmp/h2c
```

`h2c_emu` runs the real `iwfg_h2c` and `c2h_stream` with the card replaced by
`fake_iwfg.so`. The fake loops what the "DAC" plays back into the "ADC". It
drives them from the real launcher, generator and reader, and also runs the
whole application in a live session. It injects the bench faults:

- a C2H driver error;
- a card that refuses large transfers;
- a card that never consumes, or starts consuming late;
- helpers that ignore SIGINT and SIGTERM.

It checks that the generated tone comes back and that the GUI never blocks.

`pcie_emu` runs the GUI's real PCIe code against the firmware's real
`pcie_cfg.c` through an emulated BAR. It does this for both 1.2.1 and 1.2.0
firmware, and after every command it checks what the DAC would actually play.

## 5. Backend supervision internals (1.2.4, updated 1.2.5)

`BackendLauncher` (`SC/01_sdr_workstation_GUI/src/core/BackendLauncher.{h,cpp}`) owns
the helper processes and has no UI. The rules below keep the GUI thread from
ever blocking on them.

- **Never destroy a running `QProcess`.** Its destructor kills the process
  and waits up to 30 s, and a helper in an uninterruptible driver call does
  not die. `release(p, whenGone)` deletes a finished process at once. A
  running one gets SIGINT, then SIGKILL after 1.5 s, and is parked in
  `m_reaping` until `finished()`. `whenGone` runs on exit, or after 3 s if it
  never exits; relaunches wait for it so the old helper has released the node.
- **Asynchronous, ordered stop.** `stop()` sends SIGINT to the transmit
  helper and returns. The capture is signalled once transmit has exited, or
  after 1.5 s (`kPlaybackFirstMs`), so the C2H close is the last one and the
  driver's QDMA reset runs (`iwfg_release()`). `stopTick()` (100 ms)
  escalates each process from its own SIGINT time: SIGTERM at 2.5 s,
  SIGKILL at 4 s, and at 5.5 s it gives up on a stuck helper and leaves it
  to the reaper. `stopped()` is emitted when all have exited. `start()`
  during a stop is stored in `m_pendingStart`.
- **Device reset.** `resetDevice(why)` is an ordered stop plus a queued
  `start()` with the current config, which includes the working H2C buffer
  size. It emits `deviceReset(true/false, why)`. `MainWindow::checkCaptureRate()`
  calls it after 12 consecutive stats updates (~5 s) with no capture data,
  at most twice in 3 minutes.
- **Generation counter.** `start()` and `stop()` bump `m_generation`. Every
  delayed callback (C2H restart, H2C relaunch, first-rate watchdog) captures
  it and does nothing if it has changed.
- **H2C state machine.** `handleH2cLine()` maps `iwfg_h2c`'s output to
  `H2cState` as follows:
  - `Waiting for GNU Radio` → WaitingForData.
  - `First chunk received` → Starting, and arms the 4 s first-rate watchdog.
  - A rate line → Streaming.
  - A stall line (`isH2cStallLine()`), or the watchdog, before streaming →
    `queueH2cRecovery()`: the first time at a given size it reopens at the
    same size (`reopenH2c()`, `m_h2cRetriedSize`); the second time it steps
    down.
  - After streaming: Stalled; two stall lines in a row → same-size reopen,
    limited by `h2cReopenAllowed()` (3 per minute).
  - `retryH2cSmaller()` at the bottom rung sets `m_h2cBottomedByStall`. The
    next rate line then triggers one `restartH2c(0)` (`m_h2cPromoted`).
- **Step-down on exit.** A helper that exits before streaming is stepped down
  unless its output names a cause no size can fix: `open device`,
  `chunk_bytes must be`, `NOT a FIFO`, `mkfifo`, `stat fifo`, `sigaction`.
- **C2H exit classification.** `classifyC2hExit()` scans the helper's tail
  and reads the reason from these markers:

  | Marker | Classified as |
  |---|---|
  | `IWFG_IOCTL_DMA_DATA (C2H)` | DriverError |
  | `FIFO reader disconnected`, `write to fifo` | ReaderClosed |
  | `interrupted by signal` | Interrupted |
  | `open device`, `ADD_BUF`, … | StartupError |

  If it streamed and was not a StartupError, it is restarted after 700 ms,
  within `c2hRestartLimit` per 60 s. The capture runs under
  `stdbuf -oL` (`lineBufferWrapper()`, disabled by `IWFG_NO_STDBUF`), so its
  lines arrive live and in order.
- **Reader re-attach.** `DmaSource::writerGone()` runs on EOF or POLLHUP. It
  compares `stat(path)` with `fstat(fd)`: a FIFO at the path with a different
  inode means `c2h_stream` restarted (it unlinks and recreates the FIFO).
  The reader then reopens the path, non-blocking, and resets the partial
  block. Otherwise it waits 50 ms.
- **In-place reconfiguration.** `MainWindow::onSourceRestartRequired()` keeps
  a live `DmaSource` and pushes the config. Recreating the reader closes the
  FIFO, and `c2h_stream` exits on EPIPE.
- **Capture bandwidth.** `core/CaptureBudget.h` holds the pure helpers:
  `captureNeedMib`, `captureChannelsWithinBudget` (budget 1600 MiB/s),
  `plGpioModeForChannels`, `impliedCaptureChannels`.
  `MainWindow::checkCaptureRate()` compares `stats.dmaMBps` with the need,
  and reacts to 5 consecutive stats updates (2 s) below 0.90 or above 1.30.
  After a channel change, the first update past 3 s reads the channel count
  from the rate (within 15 % of a whole 1, 2, 4 or 8):
  - equal to the target → confirmed;
  - equal to or above the old count, or still above 1.3× need → reverted
    (display and PL mode);
  - another whole count → the display follows it.
- **Transmit guard.** `MainWindow::transmitGuard()` runs on the generator's
  `hostRunning` transitions. It needs `ControlWindow3D::rfdcAttached()`, and
  sets the mode with `applyPlGpioMode()`.
- **Pipe size.** `DmaSource` calls `F_SETPIPE_SZ` (up to `fs.pipe-max-size`)
  on the capture FIFO at open and at every re-attach.

Environment variables:

| Variable | Effect |
|---|---|
| `IWFG_H2C_CHUNK_BYTES` | starting H2C buffer (multiple of 4, 1 KiB – 64 MiB) |
| `IWFG_C2H_DEVICE`, `IWFG_H2C_DEVICE` | device nodes (default `/dev/iwfg0`, `/dev/iwfg1`) |
| `IWFG_C2H_FIFO`, `IWFG_H2C_FIFO` | FIFO paths (default `/tmp/iwfg_fifo`, `/tmp/iwfg_h2c.fifo`) |
| `IWFG_NO_STDBUF` | do not wrap `c2h_stream` in `stdbuf -oL` |

The driver issues behind all this, with suggested fixes, are in
`DRIVER_OVERFLOW_RECOVERY.md`.

Test harness: `TS/01_Host_Test_Suite/h2c_emu/fake_iwfg.c` documents its `FAKE_IWFG_*`
switches. `loopback_emu_test.cpp` and `app_live_emu_test.cpp` show how to
stand up a rig with no device: a temporary `backend/` directory, regular
files as device nodes, and `LD_PRELOAD`.
