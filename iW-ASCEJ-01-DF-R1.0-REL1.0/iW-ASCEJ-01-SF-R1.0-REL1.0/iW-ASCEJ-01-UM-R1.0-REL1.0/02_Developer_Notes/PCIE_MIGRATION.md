# UART → PCIe Configuration Migration

ZU47DR RFSoC bare-metal application, Vitis 2025.2 (SDT) + Linux host tool.

**Verified:** bare-metal 17 modules × 2 flows = 0 errors, 0 warnings
(`-Wall -Wextra -Wmisleading-indentation`). Host tool = 0 warnings at
`-Wall -Wextra -Wpedantic -Wconversion`. All five worked examples from
`Register_mapping.docx` reproduce byte-exact.

---

## 1. Blocking hardware dependency — read first

The register document gives **offsets only**, no base. The only PCIe control
aperture in the design is `M00_PCIe_control` at `0xA00A0000`, so that is what
`PCIE_CTRL_BASEADDR` uses.

**But inspecting `design_1.hwh` inside your XSA shows there is no register IP
at that address.** `S00_PCIe_control` and `M00_PCIe_control` are **external
ports** of the block design, wired to `smartconnect_1`. The address is a
pass-through out of the BD:

```
PS  --M_AXI_HPM0_FPD--> 0xA00A0000 --M00_PCIe_control--> (outside this XSA)
QDMA ------------------> S00_PCIe_control --> smartconnect_1
```

The block that implements `ring_register` and `RFDC_configuration` lives
outside this block design — in wrapper RTL, or in a newer XSA containing the
QDMA. The XSA supplied contains **no QDMA and no DMA IP of any kind**.

**Consequences:**

1. Confirm the PS can reach the register block at `0xA00A0000`. Override with
   `-DPCIE_CTRL_BASEADDR=0x...` if not.
2. **An AXI read to an unmapped address hangs the CPU with no message** —
   exactly the failure mode you already hit during bring-up. So the module
   ships behind `ENABLE_PCIE_CONFIG`.

**Verify before enabling**, over JTAG with the bitstream loaded:

```tcl
xsdb% targets -set -filter {name =~ "*Cortex-A53 #0*"}
xsdb% mrd 0xA00A00A8
xsdb% mrd 0xA00A00AC
```

If those return values, the aperture is live — build with
`-DENABLE_PCIE_CONFIG=1`. If `xsdb` hangs or errors, the register block is not
reachable there and the base is wrong.

---

## 2. Existing UART path (as analysed)

| Question | Finding |
|---|---|
| Where input is received | `main.c`, `scanf("%15s", buf)` in the `while(1)` menu |
| How parsed | `atoi`/`atof` on the token, then `if/else if` on `choice` (16 options) |
| Parameters extracted | decimation, interpolation, NCO freq, DDS freq, sampling rate, QMC gain, DSA, Nyquist zone, AXIS/GPIO routing, DAC source |
| Where stored | Mostly applied immediately; only `current_adc_nco` / `current_dac_nco` persist as file-scope globals |
| How used | Passed straight to `Update_Datapath_Rate()`, `Set_NCO_Freq_MTS_Safe()`, `Set_Dds_Frequency()`, `Change_Sampling_Rate_All_Tiles()` |
| Blocking or polling | **Blocking** — `scanf` halts the CPU until a token arrives |
| State machine | None. Flat `if/else` dispatch, no command state |
| Init sequence | `stdin`/`stdout` = `psu_uart_0` (BSP), UART0 on MIO 6/7 |
| Error handling | Per-option `while(1)` re-prompt loops validating factors; no error codes |

**The blocking `scanf` was the one real obstacle.** PCIe polling cannot run
while the CPU sits in `scanf`. Solved without changing menu behaviour — see §4.

---

## 3. Register interface

| Offset | Name | Access (PS) | Description |
|---|---|---|---|
| `0xA8` | `ring_register` | R/W | Command + target selection |
| `0xAC` | `RFDC_configuration` | RO | Command payload |

### `ring_register`

| Bits | Field | Values |
|---|---|---|
| `[31]` | `NEW_CMD` | 1 = host wrote a command; PS clears it — **this is the ACK** |
| `[30:28]` | `TARGET` | 000 ADC · 001 DAC · 010 DDS · 100 ADC+DAC |
| `[27]` | `LAST_TILE` | 1 on the final tile of a selection |
| `[26:24]` | `TILE` | 0–3 = tile · 7 = all · 4 = back-to-back (ambiguous, rejected) |
| `[23:20]` | `CHANNEL` | 0000 Ch0 · 0001 Ch1 · 0010 both |
| `[19:0]` | `EVENT` | 1 NCO freq · 2 NCO phase · 3 interp · 4 decim · 5 DDS |

The document's example *"if your last tile is Tile 2, the 4-bit register should
be 1010"* is reproduced exactly by `RING_SET(TILE,2) | RING_LASTTILE_MASK`
→ `0xA`. That confirms `[27]` is a LAST marker over a 3-bit tile index.

### `RFDC_configuration` payload

| Event | Encoding | Worked example (verified) |
|---|---|---|
| NCO freq | kHz magnitude, `[31]` sign | 4.5 GHz → `0x0044AA20` ✅ |
| NCO phase | integer degrees 0–180, `[31]` sign | −180° → `0x800000B4` ✅ |
| Interp/decim | factor value | x24 → `0x18` |
| DDS | Hz | 10 MHz → `0x00989680` ✅ |

Accepted factors: 1 2 3 4 5 6 8 10 12 16 20 24 40.

---

## 4. Bare-metal implementation

### New files

| File | Purpose |
|---|---|
| `src/pcie_regs.h` | Register map. **Shared verbatim with the host** — one definition, no drift |
| `src/pcie_cfg.h` | Manager API + build switches |
| `src/pcie_cfg.c` | Poll / decode / validate / apply / ACK |

### Modified file — `src/main.c`, three edits only

1. `#include "pcie_cfg.h"` and `xuartps_hw.h`
2. `PcieCfg_Init(RFdcInstPtr)` at new Step 2.7, after RFDC bring-up
3. The menu wait replaced with a super-loop:

```c
while (!XUartPs_IsReceiveData(STDIN_BASEADDRESS)) {
    if (PcieCfg_Poll()) {
        printf("\r\nSelect an option: ");
    }
}
scanf("%15s", buf);          /* unchanged */
```

`scanf` is still what parses operator input — it is simply only entered once a
byte is known to be waiting. **UART menu behaviour is bit-for-bit unchanged**,
and PCIe is serviced continuously while idle.

### Processing logic is reused, not duplicated

| Event | Calls |
|---|---|
| Decimation | `Update_Datapath_Rate(rfdc, 1, tile, block, factor)` — existing |
| Interpolation | `Update_Datapath_Rate(rfdc, 0, tile, block, factor)` — existing |
| DDS frequency | `Set_Dds_Frequency(mhz)` — existing |
| NCO freq, all tiles + both ch | `Set_NCO_Freq_MTS_Safe()` — existing, verbatim |
| NCO freq, per tile/channel | `nco_set_block()` — new; the register map allows a granularity the UART menu never exposed |
| NCO phase | `nco_set_phase_block()` — new, same reason |

The two new routines use the **corrected** `XRFdc_SetCalFreeze` pattern
(`FreezeCalibration`, not `CalFrozen`) from the H-1 fix.

### Race the design closes

`PcieCfg_Poll()` reads the **payload before** clearing `NEW_CMD`. The host
cannot overwrite `RFDC_configuration` for the next command while this one is
still being read — the only race the two-register handshake exposes.

Host-side ordering is the mirror image: payload first, then `NEW_CMD`, with a
read-back after each posted write to force it out.

### Build switches

| Macro | Default | Effect |
|---|---|---|
| `ENABLE_PCIE_CONFIG` | 1 | 0 compiles the module out — **no register access at all** |
| `PCIE_CFG_VERBOSE` | 1 | One console line per command |
| `PCIE_CFG_HAVE_STATUS_REGS` | 0 | Set to 1 **only** if the optional status registers are added |
| `PCIE_CTRL_BASEADDR` | `0xA00A0000` | Override if the aperture differs |

---

## 5. Host application

```
host/
  pcie_regs.h     identical copy of the bare-metal header
  pcie_access.h   access layer API
  pcie_access.c   sysfs-mmap and chardev backends
  rfdc_ctl.c      command manager + CLI
  Makefile
```

### Two backends, because the right one depends on your stack

| Backend | Use when |
|---|---|
| `--bdf 0000:01:00.0` | BAR is mmap-able via sysfs and no driver holds it exclusively |
| `--chardev /dev/xdma0_user` | An XDMA/QDMA stack exposes the BAR as a file |

Pick with `lspci -vv -s <BDF>` and `ls /dev`. Nothing is assumed.

### Build and run

```bash
cd host && make
./rfdc_ctl list
sudo ./rfdc_ctl -b 0000:01:00.0 dump
sudo ./rfdc_ctl -b 0000:01:00.0 nco-freq adc all both 3100
sudo ./rfdc_ctl -b 0000:01:00.0 decim 2 0 24
sudo ./rfdc_ctl -b 0000:01:00.0 dds-freq 10
```

`-v` prints every transaction. `-t` sets the ACK timeout (default 2000 ms).

---

## 6. Configuration flow

```
HOST                          PCIe REGISTERS              BARE-METAL
────                          ──────────────              ──────────
check ring[31]==0
  │  (refuse if busy)
  ├── write 0xAC payload ───────► RFDC_configuration
  │                                                    ┌─ PcieCfg_Poll()
  ├── write 0xA8 ring|NEW_CMD ──► ring_register ───────┤  ring[31] set?
  │                                                    │
  │                              read 0xAC ◄───────────┤  latch payload
  │                                                    │  (BEFORE ack)
  │                                                    ├─ decode
  │                                                    ├─ validate
  │                                                    │    ├ bad → reject
  │                                                    │    └ ok  → apply
  │                                                    │        Update_Datapath_Rate()
  │                                                    │        Set_NCO_Freq_MTS_Safe()
  │                                                    │        Set_Dds_Frequency()
  │                                                    │
  │                              clear ring[31] ◄──────┴─ ACK
  └── poll until ring[31]==0
        │
        ├── cleared  → report OK
        └── timeout  → report failure
```

---

## 7. Test procedure

| # | Test | Command | Expected |
|---|---|---|---|
| 1 | Device detection | `./rfdc_ctl list` | endpoint listed |
| 2 | Register read | `dump` | plausible values, `NEW_CMD=0` |
| 3 | Register write | `raw-write 0xAC 0xDEADBEEF` then `raw-read 0xAC` | reads back |
| 4 | Valid config | `decim 0 0 24` | `OK`; target prints `[pcie] applied.` |
| 5 | ACK | run with `-v` | `ack: ring=...` with bit31 clear |
| 6 | Invalid factor | `raw-write 0xAC 7` + ring EVENT=4 | target prints `payload out of range` |
| 7 | Wrong target/event | decimation with TARGET=DAC | `EVENT not valid for this TARGET` |
| 8 | Repeat suppression | same command twice | two applies, never a double-apply of one write |
| 9 | Host timeout | halt the target, send a command | `timeout after 2000 ms` |
| 10 | Busy detection | set `NEW_CMD` by hand, then send | `device still has NEW_CMD set` |
| 11 | Stale command | set `NEW_CMD`, reboot target | `[pcie] cleared stale NEW_CMD` |
| 12 | Back-to-back tile | TILE=0100 | `back-to-back tile encoding unsupported` |
| 13 | Stress | 1000 × `nco-freq adc all both <sweep>` | no lost ACKs, counters match |
| 14 | UART coexistence | send PCIe commands while the menu is open | both work; prompt reprints |

Tests 6, 7 and 12 need `raw-write`, since the CLI refuses to send malformed
commands.

---

## 8. What could not be implemented, and why

The brief asked for these. The register map defines no encoding for them, and
the instruction was not to invent register definitions.

| Requested | Status |
|---|---|
| START / STOP / RESET | **No such EVENT exists.** Not implemented |
| Status register | Does not exist — ACK is the cleared `NEW_CMD` bit only |
| Error code register | Does not exist — errors print on the target console |
| Device ID / version | Does not exist — host cannot verify what it is talking to |
| Current-configuration readback | Does not exist |
| Sequence number | Does not exist; single-bit handshake used instead |

### Minimum recommended additions — 2 registers

```
0xB0  STATUS      RO   [0] BUSY  [1] DONE  [2] ERROR  [27:8] last EVENT
0xB4  ERROR_CODE  RO   PcieCfgError of the most recent command
```

The bare-metal side is **already written for these** — set
`PCIE_CFG_HAVE_STATUS_REGS=1` and they populate. No other change needed.

Two further recommendations:

- **`TILE` encoding `0100`.** It collides with a 3-bit tile index and is
  currently rejected. Either reserve `[27]` as LAST over a 2-bit index, or
  define `[27:24]` as a **tile bitmask** — one bit per tile — which removes the
  multi-write sequence entirely and is race-free.
- **`EVENT` has 20 bits and uses 5 values.** Sampling rate, Nyquist zone, DSA,
  QMC gain, AXIS routing and DAC source are UART-only today. Extending EVENT
  would bring the whole menu under host control.

---

## 14. Full control set + debugging (final version)

### 14.1 Finding the base address without rebuilding

Your host side is **proven**: BAR2 offset `0xAC` write/read-back returned
`0xDEADBEEF`, and a `dds-freq` command left `ring = 0xa8000005` stuck. What is
unproven is whether the PS sees that same block at `0xA00A0000`.

Two new UART menu options exist so you can settle it from the console:

```
17. PCIe interface diagnostics        <- dump base, both registers, counters
18. Set PCIe control base address     <- retarget at runtime, no rebuild
```

**Procedure:**

1. From the host, leave a marker: `raw-write 0xAC 0xDEADBEEF`
2. On the target, press **17**.
   - `cfg = 0xDEADBEEF` → same block, base is correct. Done.
   - `cfg = 0x00000000` → different memory. Continue.
3. Press **18**, try another base. Diagnostics print automatically after.

⚠ Option 18 will hang the CPU if you enter an unmapped address — an AXI access
that never completes. Only enter bases you have reason to believe in.

### 14.2 Complete event set

Codes `0x001`–`0x005` are from `Register_mapping.docx`, unchanged. Everything
else is an **EXTENSION**: same two registers, same handshake, no new addresses
invented — but the FPGA and host teams must agree them.

| EVENT | Command | Payload |
|---|---|---|
| `0x001` | NCO frequency | kHz, `[31]` sign |
| `0x002` | NCO phase | degrees 0–180, `[31]` sign |
| `0x003` | Interpolation | factor |
| `0x004` | Decimation | factor |
| `0x005` | DDS frequency | Hz |
| `0x010` | Sampling rate | MHz |
| `0x011` | Nyquist zone | 1 or 2 |
| `0x012` | ADC DSA | dB × 10 |
| `0x013` | QMC gain | gain × 10000 |
| `0x014` | AXIS routing | `ch0 \| ch1<<4 \| ch2<<8 \| ch3<<12` |
| `0x015` | PL GPIO routing | 1–3 |
| `0x016` | DAC input source | 0 DDS, 1 host |
| `0x017` | MTS re-align | ignored |
| `0x018` | DAC VOP | microamps |
| `0x020` | START | ignored |
| `0x021` | STOP | ignored |
| `0x022` | RESET (soft) | ignored |
| `0x0F0` | PING | ignored |

**Every UART menu function is now reachable over PCIe.**

`RESET` is deliberately a *soft* reset — MTS re-align, NCO restore, routing
restore. A converter power cycle would drop the clock tree and require the full
LMK/LMX sequence again, which is not something to trigger from a remote host.

### 14.3 Status without new hardware

The map defines no status register, so the result code rides in the
acknowledge write — which the PS performs anyway to clear `NEW_CMD`:

```
[31]    = 0        NEW_CMD cleared  (unchanged contract)
[30:24] = 0x2A     marker
[23:20] = error    0 = success
[19:0]  = event    echo
```

Verified by round-trip test, including that a host command word with bit31
cleared (`0x28000005`) is **not** mistaken for a status word.

The host now prints the actual rejection reason instead of a bare "processed".
Firmware without this still works — the host falls back to "outcome unknown".

Disable with `PCIE_CFG_STATUS_IN_RING=0` if your PL makes `[30:20]` read-only
from the PS.

### 14.4 Host commands

```bash
D="-b 0000:3c:00.0 -r 2"

sudo ./rfdc_ctl $D ping                        # handshake test, changes nothing
sudo ./rfdc_ctl $D dump

sudo ./rfdc_ctl $D nco-freq adc all both 3100
sudo ./rfdc_ctl $D nco-phase dac 2 0 90
sudo ./rfdc_ctl $D decim 2 0 24
sudo ./rfdc_ctl $D interp all both 24
sudo ./rfdc_ctl $D dds-freq 10

sudo ./rfdc_ctl $D sample-rate adc 4000
sudo ./rfdc_ctl $D nyquist adc 1
sudo ./rfdc_ctl $D dsa all both 6.5
sudo ./rfdc_ctl $D qmc-gain both all both 1.0
sudo ./rfdc_ctl $D dac-vop all both 32.0
sudo ./rfdc_ctl $D axis-route 7 3 2 1
sudo ./rfdc_ctl $D gpio-route 3
sudo ./rfdc_ctl $D dac-source 1
sudo ./rfdc_ctl $D mts
sudo ./rfdc_ctl $D start
sudo ./rfdc_ctl $D stop
sudo ./rfdc_ctl $D reset
```

**Start with `ping`.** It touches no hardware, so a success proves the whole
path — host write, target poll, decode, acknowledge — with nothing at risk.

`sample-rate` takes several seconds (PLL reprogram + 500 ms settle + MTS). Use
`-t 15000`.

### 14.5 Verification performed

| Check | Result |
|---|---|
| Bare-metal, 16 configurations (2 flows × 2 IRQ × 2 PCIE × 2 STATUS) | 0 errors, 0 warnings |
| Full tree, 17 modules × 2 flows | 0 errors, 0 warnings |
| Host, `-Wall -Wextra -Wpedantic -Wconversion` | 0 warnings |
| ACK encode/decode round-trip | 6/6 pass |
| Document's worked examples | 5/5 byte-exact |
