# UDP live-stream integration (iwfg_c2h)

## Inspection first

`B_udp_live_stream/iwfg_c2h` is a complete, well-engineered 100 GbE UDP
receiver — not fragments. It should be integrated, not rewritten:

```
NIC ──► kernel UDP ──► [RX thread] ──► lock-free SPSC ring ──► [writer] ──► FIFO
                        recvmmsg()      368 MiB, hugepage-      write()
                        batch = 128     backed, mlocked         ≤ 4 MiB chunks
```

Key facts taken from its source and README rather than assumed:

| property | value |
|---|---|
| output | `/tmp/iwfg_c2h.fifo`, raw payloads back-to-back |
| payload | 1472 B (1500 MTU − 20 IP − 8 UDP) |
| default port | 50000 |
| raw mode | `--raw <ifname>` — AF_PACKET, needs root |
| `-b` in raw mode | **ignored** (filtering is by port) |
| reader reconnect | resumes on an exact 1472 B boundary |

## The check that mattered

1472 B must contain a whole number of samples, or every packet boundary would
split a sample and corrupt the stream:

```
Cs16 (int16 I/Q, 4 B)  : 1472 / 4 = 368   integral
Cf32 (float32 I/Q, 8 B): 1472 / 8 = 184   integral
Cs8  (2 B)             : 1472 / 2 = 736   integral
```

All clean, so no framing adaptation is needed.

## Verified end to end

Real UDP traffic, a real socket, the real receiver:

```
sent      : 4000 packets of 1472 B
captured  : 5,888,000 B  =  4000 × 1472   (every packet, none lost)
drops     : ring=0 sock=0 badsz=0 reopen=0
recovered : −10.0000 MHz   (transmitted −10.0000 MHz)
error     : 0.000 kHz
amplitude : 24000          (sent 24000)

VERDICT: byte-exact, no loss, tone intact
```

## Integration

No new source class. The receiver's output is a plain byte FIFO, which the
existing `DmaSource` already reads — the same reason the PCIe path needed no
special handling. What was added:

* **`BackendMode::UdpStream`** in `BackendLauncher`, which launches
  `iwfg_c2h` with the right arguments and manages its lifecycle (graceful
  SIGINT, log capture, orphan prevention) exactly as it does the other
  backends.
* **`captureFifo()`** now maps mode → FIFO in one place, instead of the
  mapping being re-derived at each call site.
* **A Transport selector** in the launcher, enabled only for Ethernet:
  *RoCEv2 (RDMA, shm ring)* or *UDP (100G, iwfg_c2h → FIFO)*. They are not
  interchangeable — one is RDMA into a shared-memory ring, the other a
  datagram stream into a byte FIFO — so the choice is explicit, never
  inferred.
* For UDP the device list shows **network interfaces**, not RDMA devices,
  because that is what the receiver binds to.
* `-b` is passed only on the socket path, since the receiver documents it as
  ignored in `--raw` mode.

Verified switching:

| selection | session entry | default path |
|---|---|---|
| PCIe | Live — stream from PCIe DMA / FIFO | `/dev/iwfg0` |
| Ethernet + RoCEv2 | Live — stream from RoCEv2 (shared-memory ring) | `/dev/shm/iqring` |
| Ethernet + UDP | Live — stream from UDP (iwfg_c2h FIFO) | `/tmp/iwfg_c2h.fifo` |

## Bugs found and fixed during integration

* **Readiness timing.** The shared 900 ms settle would have declared the UDP
  backend ready before its FIFO existed: the receiver pre-faults and mlocks a
  368 MiB ring first, measured at ~2.6 s here. UDP now uses a 4 s settle, with
  the reason recorded next to the constant.
* **UDP path skipped the session sync.** `repopulateDevices()` returned early
  for UDP, before the tail that rewrites the session entry and default path —
  so selecting UDP left both showing RoCEv2. The tail was extracted into
  `syncSessionForTransport()` and is now called from every branch.
* **Two-transport label.** The session entry still chose between exactly two
  strings after a third transport existed.

## What is NOT verified here

The **GUI reading a live UDP FIFO** was not completed in this environment: the
sandbox reset `/tmp` mid-test and took the receiver and its logs with it.

What *is* verified: the receiver's UDP → FIFO path is byte-exact with the tone
recovered to 0.000 kHz (above), and `DmaSource` reading a FIFO was proven in
earlier rounds against both the PCIe and synthetic-ring paths. The remaining
untested link is the join between two independently verified halves.

To confirm on the target:

```bash
cd backend/udp && make
sudo ./iwfg_c2h -p 16384 --raw enp1s0f0np0 &
./build/sdr_workstation          # Ethernet → UDP → Launch
```

Expect the receiver's per-second line (`pps / Gbps / ring fill / drops`) to
show traffic, and the workstation's spectrum to show the transmitted tone.

## Note on `--raw`

Your README documents this well and it is worth repeating: a hardware
generator emitting a zero/foreign destination MAC or a bad IP checksum is
discarded by `ip_rcv()` before UDP ever sees it — which is exactly the
"Wireshark sees it but the app gets nothing" case. `--raw` taps AF_PACKET
before the IP stack and needs root. The launcher passes it through unchanged.

---

## Follow-up: no manual commands, one build command

### 1. One command builds everything

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Verified from a clean slate (no `build/`, no `backend/bin`, no `iwfg_c2h`):

```
✓ build/sdr_workstation
✓ backend/udp/iwfg_c2h
✓ backend/bin/c2h_stream
✓ backend/bin/iwfg_h2c
```

The helpers are driven through **their own Makefiles** rather than being
re-expressed as CMake targets. That is deliberate: they are vendor-supplied
and their flags are part of their contract — the UDP receiver builds with
`-O3 -march=native -flto`, and reproducing that in CMake would risk building
something subtly different from what was validated.

### 2. Selecting UDP starts the receiver automatically

Pressing **Start** with Ethernet + UDP now does all of this itself:

1. Builds `iwfg_c2h` if it is not built yet.
2. Checks the port is free.
3. Launches the receiver with the configured port, FIFO and interface.
4. Waits for the FIFO, then attaches the workstation to it.
5. Stops and reaps the receiver on Stop or on exit.

Equivalent to the command you were running by hand:

```
sudo ./iwfg_c2h -p 16384 --raw enp1s0f0np0
```

but assembled from the launcher's fields. Port and raw-mode are editable in
the launcher — **UDP port** and **Raw capture (AF_PACKET)** appear beside the
Transport selector — and the interface comes from the Device list.

### 3. Root handling for `--raw`

AF_PACKET needs `CAP_NET_RAW`. Rather than telling the operator to run a
terminal command, the launcher elevates through **pkexec**, so authorisation
is a normal desktop prompt. The plain-socket path is never elevated, since it
does not need privilege. With no polkit agent present the exact `sudo`
command is shown instead of failing silently.

### 4. Port availability is checked, not guessed

A busy port makes the receiver exit the instant it starts, which surfaces as
an opaque exit code. A probe socket is bound first, so the clash is reported
as a clash:

```
free port 51999      : AVAILABLE
same port, now bound : in use (correct)
after release        : AVAILABLE
```

The message names the port and suggests either changing it or stopping the
process holding it.
