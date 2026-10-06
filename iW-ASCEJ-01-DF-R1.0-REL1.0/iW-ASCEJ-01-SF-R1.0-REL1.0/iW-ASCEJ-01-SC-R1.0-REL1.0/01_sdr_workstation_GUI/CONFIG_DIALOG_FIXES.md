# Two fixes to System Configuration

## 1. Offline mode had a hardware dependency it should never have had

**Symptom.** With *Offline — analyse a stored capture* selected and a valid
`.bin` chosen, the dialog showed:

```
✗ ERROR  No development kit selected
→ Choose a kit from the Platform list.
no RDMA devices found — is rdma-core / MLNX_OFED loaded?
```

and **LAUNCH WORKSTATION stayed greyed out**.

**Cause.** `refreshState()` ran the full converter/board validator on every
change, regardless of session type. `validate()` opens with:

```cpp
const BoardSpec* b = HardwareDb::byId(req.boardId);
if (!b) { ... Error "No development kit selected" ... return r; }
```

and launch is gated on `vr.ok()`. On the Ethernet path the Device combo held
RDMA NICs rather than boards (see §2), so `boardId` was empty and the error was
guaranteed.

The result: a machine with **no RDMA NIC and no kit selected could not open a
capture it had recorded itself.** Hardware validation was blocking a task with
no hardware in it.

**Fix.** Offline returns before any hardware rule runs:

```cpp
const bool offline = (mode == 1);
if (offline) {
    m_launch->setEnabled(ready);           // ready == a file is chosen
    ...
    return;                                // hw::validate() never called
}
```

A stored capture is self-describing — wire format, channel count and sample
rate come from the file and the fields beside it. Launch is now gated on
exactly one thing: **has a file been chosen.**

The panel says so plainly rather than showing converter limits that do not
apply:

```
Offline analysis — no acquisition hardware is used.
Wire format, channel count and sample rate are taken from the fields
above and must match how the capture was recorded.
```

The RoCE pre-flight was already gated on `SourceMode::Roce`, so offline never
touched it. Verified, not assumed.

## 2. The Device list changed meaning with the transport

**Symptom.** Selecting **Ethernet** replaced the development-kit list with
`No RDMA device found`.

**Cause.** One combo was carrying two unrelated things:

| Transport | Device combo held | Item data |
|---|---|---|
| PCIe | development kits from `HardwareDb` | board id |
| Ethernet | discovered RDMA NICs | rdma device name |

That is wrong on its own terms: **the development kit is the same board
whichever transport reaches it.** A ZU47DR is a ZU47DR over PCIe or over
RoCEv2. The NIC is a property of the *host*, not of the kit.

It also broke validation, because `req.boardId` was then a NIC name that
`HardwareDb::byId()` could not resolve — which is what produced the "No
development kit selected" error in §1.

**Fix.** Two controls for two questions:

```
Device      development kits          ← identical on PCIe and Ethernet
Interface   PCIe | Ethernet
Transport   RoCEv2 | UDP              ← Ethernet only
RDMA NIC    mlx5_0 · port 1 (…)       ← Ethernet only, hidden otherwise
```

The Device list is now rebuilt from `HardwareDb` on both paths, so the board
stays selected when the transport changes and the validator always has a board
to reason about. The NIC selector carries the port, link state and IP exactly
as before, and is hidden on PCIe where it means nothing.

## Scope

```
src/ui/ConfigDialog.h     m_rdmaNic, m_rdmaNicLabel
src/ui/ConfigDialog.cpp   offline early-return; NIC combo; board list on
                          both transports; UDP row moved to make space
```

No change to the acquisition sources, the DSP chain, the RFDC/PCIe control, or
`ConfigValidator` itself — the validator's rules are correct, they were simply
being applied where they did not belong.

## Verified

| Check | Result |
|---|---|
| offline returns before `hw::validate()` | PASS |
| offline launch gated only on file present | PASS |
| board list populated on the Ethernet path | PASS |
| RDMA NIC is a separate control | PASS |
| NIC row hidden unless Ethernet | PASS |
| Device combo no longer receives NIC entries | PASS |
| RoCE pre-flight still gated on `SourceMode::Roce` | PASS |
| full CMake build, Qt 5.15 | 1.8 MB binary |
