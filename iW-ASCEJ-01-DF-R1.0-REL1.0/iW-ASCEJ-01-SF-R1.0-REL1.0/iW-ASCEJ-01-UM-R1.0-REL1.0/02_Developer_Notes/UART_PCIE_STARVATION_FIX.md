# Why PCIe only responded when you touched the UART

## The observation

> "for the first time we should be using the UART only, till then it was not
> responding to anything"

Correct, and it was a real firmware bug — not a link or GUI problem.

## Root cause

The menu waited like this:

```c
while (!XUartPs_IsReceiveData(STDIN_BASEADDRESS)) {
    PcieCfg_Poll();          // the ONLY place PCIe was serviced
}
scanf("%15s", buf);          // blocks inside libc
```

`scanf("%s")` consumes its token but **leaves the terminating CR/LF in the UART
FIFO**. So on the next pass:

1. `XUartPs_IsReceiveData()` → **true** (the stale `\r` is still queued)
2. the guard loop exits **immediately**, never calling `PcieCfg_Poll()`
3. `scanf()` is entered, skips the whitespace, and **blocks** waiting for a
   real token

The CPU is now parked inside libc. PCIe is dead until you type something —
which is exactly what you saw. Pressing a menu key "woke it up" because
`scanf` finally returned and the loop ran once on the way past.

Your serial log shows the signature plainly: `[pcie] cmd ...` lines appear only
interleaved with menu output, never on their own.

### Second, related fault

There were **24** `scanf()` call sites. Every nested prompt — *"Enter ADC Tile
(0-3):"*, *"Enter new Decimation Factor:"* — blocked with **no polling at all**.
PCIe was dead for as long as you were inside any sub-menu.

## The fix

`scanf()` is gone. `MenuReadLine()` owns the FIFO instead:

* polls PCIe between **every character** and while idle
* **consumes the line terminator itself**, so nothing stale is left behind
* handles backspace/delete, ignores stray CR of a CRLF pair
* echoes characters (it bypasses libc entirely)
* never blocks in libc

All 24 prompts now use it, so the transport stays alive inside sub-menus too.

### Verified by simulation

The state machine was extracted and driven through the awkward cases:

| Input | Result | FIFO drained |
|---|---|---|
| `11\r\n` | `"11"` | yes |
| `11\r13\r` (CR only) | `"11"`, `"13"` | yes |
| `11\n13\n` (LF only) | `"11"`, `"13"` | yes |
| `\r\n\r\n7\r\n` (blank lines) | `"7"` | yes |
| `1x\b\b24\r\n` (backspace) | `"24"` | yes |
| nothing typed | idles, polling | yes |

The FIFO is drained in every case, so the next call goes straight to idle
polling instead of falling into a blocking read.

## What this changes for you

* PCIe commands are serviced **continuously**, whether or not anyone is at the
  console.
* Commands issued while you are inside a sub-menu now work.
* The console echo is ours, so editing behaves consistently.
* If a PCIe command prints while you are mid-typing, your partial line is
  redrawn after it (`> 1`) rather than being mangled.

## Still outstanding — flash the firmware

Your serial log also shows:

```
[pcie] REJECTED event 0x000F1: unknown EVENT code     <- readback
[pcie] REJECTED event 0x00023: unknown EVENT code     <- restore defaults
```

The board is running a build that predates both. Until it is flashed:

* **Read from hardware** cannot confirm anything (13 rejections per attempt —
  visible in your log)
* **COMPLETE RESET** fails at the defaults stage, which is what the dialog in
  your screenshot reported. That dialog was **correct** — it refused to claim
  success, which is the behaviour the specification asked for.

Everything else in the log worked: `ping`, `dac-source`, `dds-freq` (5 MHz and
25 MHz both applied), `start`, `stop`.
