# UI audit, layout redesign, and state review — verification report

## 1. Issues identified

| # | Issue | Severity |
|---|---|---|
| 1 | **Launch button unreachable** — dialog grew past the screen | blocking |
| 2 | Header consumed ~170 px, most of a laptop-height dialog | major |
| 3 | Hardware database held **4 boards**; the catalogue lists ~20 | major |
| 4 | Data-flow group rendered *below* the action buttons | major |
| 5 | Converter tile minimum blocked the default configuration | blocking |
| 6 | "within 9%" warning text was wrong and unclear | minor |
| 7 | "3D CONTROL" clipped to "ONTRO" | minor |
| 8 | Compact logo (navy on dark) had poor contrast | minor |

## 2. Root causes

**Issue 1 — the important one.** Every section was added to a single
`QVBoxLayout` **on the dialog itself**:

```cpp
auto* root = new QVBoxLayout(this);
...
root->addWidget(g);   // x4, one per configuration group
root->addWidget(row); // the action buttons, LAST
```

With no scroll area and no height cap, the dialog's `sizeHint` grew with every
section added, and the buttons — being last — were the first thing pushed off
screen. **Moving the button would only have deferred the failure to the next
section**, which is exactly what the brief warned against.

**Issue 3.** The database was seeded from the boards discussed so far rather
than from the catalogue, so most of the product line was missing.

**Issue 5.** The rate field is the *host* streaming rate. A value below the
converter tile minimum is normal — the fabric decimates first — so treating it
as an error made the default 122.88 MSPS configuration unlaunchable.

## 3. Layout changes — wide two-column, no scrolling

Feedback after the first pass: **drop the Port selector, and widen the window
rather than scroll.** Both were the right call and are now implemented.

### Port selector removed

It duplicated information the user cannot independently choose — the port
belongs to the RDMA device, so two combos could drift out of step. Its data is
folded into the device entry, which now reads:

```
mlx5_0 · port 1  (enp60s0f0np0)  —  link up, 192.168.1.1
```

One control, more information, no possible inconsistency.

### Two columns instead of a scroll

```
┌──────────────────────────────────────────────────────────┐
│  #dialogHeader                            (fixed)        │
├───────────────────────────┬──────────────────────────────┤
│  ACQUISITION DEVICE       │  SESSION START               │
│  COMPUTE TARGET           │                              │
│  DATA FLOW                │                              │
├───────────────────────────┴──────────────────────────────┤
│  #dialogActionBar   summary + Cancel + LAUNCH  (fixed)   │
└──────────────────────────────────────────────────────────┘
```

Left column = *what the hardware is*; right = *what this session does*.
Window is 900–1060 px wide and 660–780 px tall.

**Measured: the body needs 444 px.** At the 660 px minimum window height the
viewport exceeds that, so **no scrolling occurs on any normal display**. The
scroll area is retained purely as a graceful fallback for unusually short
screens — it degrades instead of clipping controls, which is what the original
single-column layout did.

(The offscreen test renderer clamps windows to a 640×600 virtual screen, so
screenshots taken here show the fallback engaging. Verified by measuring the
geometry directly: `resize(960, 780)` yields an actual height of 600 under
that platform, which is the harness limit, not a layout defect.)

### Original three-zone architecture (retained)

```
┌──────────────────────────────┐
│  #dialogHeader   (fixed)     │  brand + title
├──────────────────────────────┤
│  #dialogScroll               │
│    #dialogBody   (scrolls)   │  every configuration group
│      ACQUISITION DEVICE      │
│      COMPUTE TARGET          │
│      SESSION START           │
│      DATA FLOW               │
├──────────────────────────────┤
│  #dialogActionBar (fixed)    │  summary + Cancel + LAUNCH
└──────────────────────────────┘
```

Only the body stretches (`outer->addWidget(scroll, 1)`). The primary actions
live **outside** the scroll area, so they are reachable at any window size and
however many groups are added later. The dialog is also capped to 92 % of the
available screen height, so it can no longer exceed the display.

**Responsive header:** below 720 px the full brand plate is swapped for a
compact plated mark and the subtitle is hidden, returning ~110 px to the body.
Verified at 551 px height — the worst case — with the action bar still visible.

**Ordering fixed:** the DATA FLOW group was being appended after the summary;
it now sits inside the body with the other configuration groups, in workflow
order (device → compute → session → data flow → actions).

## 4. Functional fixes

* Tile-minimum check downgraded from Error to Info — the default configuration
  is launchable again.
* Warning text now reports actual utilisation ("96.0% of the 5.000 GSPS
  maximum") instead of a wrong, unclear threshold.
* 3D button sized from font metrics; "3D CONTROL" renders fully and survives
  display scaling.
* Compact logo uses the white clear-space plate, matching the titlebar
  treatment.

## 5. Hardware database — expanded to the catalogue

| Board | ADC | DAC | Status |
|---|---|---|---|
| RFSoC ZU47DR | 8 ch, 14-bit, 5.00 GSPS | 8 ch, 14-bit, 9.85 GSPS | verified |
| RFSoC ZU28DR | 8 ch, 12-bit, 2.00 GSPS | 8 ch, 14-bit, 6.40 GSPS | verified |
| RFSoC ZU49/ZU39/ZU29DR | 16 ch, 14-bit, 2.50 GSPS | 16 ch, 14-bit, 9.85 GSPS | verified, grouped |
| RFSoC ZU27/ZU25DR | 8 ch, 12-bit, 2.00 GSPS | 8 ch, 14-bit, 6.40 GSPS | verified, grouped |
| RFSoC ZU67/ZU65/ZU64/ZU63DR | — | — | **UNVERIFIED** |
| Versal RF VR1902/1652/1602 | — | — | **UNVERIFIED** |
| Agilex 9 Direct RF | — | — | **UNVERIFIED** |
| Agilex 9 Wide-band | — | — | **UNVERIFIED** |
| Generic / unlisted | — | — | not enforced |

Where the catalogue groups several devices under one line, the **superset** is
modelled and the ambiguity recorded in the board's notes — validating against
the wrong group member would be worse than warning. Four entries are marked
UNVERIFIED rather than being given invented numbers; the engine warns instead
of enforcing a fabricated limit.

## 6. State / memory review

Findings, and what was *not* changed:

* **No duplicate state introduced.** The validation path builds a
  `RequestedConfig` on the stack per call and discards it; nothing is cached.
  Capability text is a derived value computed from the database, not stored.
* **`HardwareDb::boards()`** is a function-local `static const`, so the table
  is built once, shared by every caller, and never copied.
* **`ControlWindow3D` is created lazily** on first click and hidden (not
  destroyed) on close — one instance for the process lifetime, which is both
  the memory-minimal option and what preserves its state.
* **Timers:** the render timer and the sources' timers are `QObject` children,
  so they are destroyed with their parents. No new timers were added.
* **Rejected as premature:** caching validation results. It runs on user
  interaction only (a few times a second at most) and is pure arithmetic over
  a handful of fields; memoising it would add invalidation bugs for no
  measurable gain.

## 7. Regression testing

| test | result |
|---|---|
| clean build, `-Wall -Wextra -Wpedantic` | **0 warnings, 0 errors** |
| launcher renders, action bar reachable at 551 px | **pass** |
| main window / simulator | **pass** |
| offline CW capture replay | **pass** |
| `--gpu-rdma-preflight` | exit 2 (no hardware here) — expected |
| validation: 47DR 12 ch | **ERROR**, blocks launch |
| validation: 47DR 7.2 GSPS | **ERROR**, blocks launch |
| validation: ZU49DR 12 ch | **passes** — 16-channel board |
| validation: ZU49DR 20 ch | **ERROR** — max 16 |
| validation: Agilex 9 unverified | 3 warnings, **0 errors** |
| data-flow mapping ADC/DAC/both | `C2H` / `H2C` / `C2H + H2C` |

The ZU49DR pair is the architectural proof: **the same rule accepts 12
channels on a 16-channel board and rejects it on an 8-channel board, with no
board-specific code**.

## 8. Remaining limitations

* Four board entries are UNVERIFIED. They are usable but unvalidated; populate
  them from the iWave datasheets to enable enforcement. This is deliberate —
  inventing converter limits would be worse than admitting the gap.
* Grouped catalogue entries (ZU49/39/29DR, ZU27/25DR) model the superset. A
  user selecting the smaller device in a group could configure beyond its real
  limit. Splitting them into individual entries is a data change only.
* Responsive behaviour is verified at 551 px and typical desktop heights.
  Sub-400 px viewports are not a target for a desktop configuration dialog.
* The 3D window contains extension points only; contents arrive later by
  design.

---

## 12. Follow-up round — bugs found from user testing

### Bug: 16-channel board offered only 8 channels (reported)

**Root cause.** The Channels combo was a fixed list `{1, 2, 4, 8}`, and the
Sample rate combo a fixed list topping out at 245.76 MSPS. Neither consulted
the selected board. Two consequences:

* A 16-channel board (ZU49/ZU39/ZU29DR) could not be fully configured — the
  validation *said* "max 16" while the selector offered 8. The GUI was
  **hiding available options**, the inverse of the requirement.
* No rate reachable from the combo could exceed any board's maximum, so the
  **sample-rate validation rule was unreachable from the UI**. It only ever
  fired in unit tests.

**Fix.** `refreshChannelChoices()` rebuilds both lists from the selected board
on every change: channels as powers of two up to `maxChannelsFor(mode)`, rates
as the familiar host values plus 25/50/100 % of the board's converter maximum.

Verified per board:

| board | channels offered | rates offered (MSPS) |
|---|---|---|
| ZU47DR (8 ch, 5 GSPS) | 1 2 4 8 | … 1250 2500 **5000** |
| ZU49/39/29DR (16 ch, 2.5 GSPS) | 1 2 4 8 **16** | … 625 1250 **2500** |
| Agilex 9 (unverified) | 1 2 4 8 16 | no invented ceiling |

The ceiling is now reachable, so the rate rule is exercisable from the GUI.

### Bug: empty space in the DATA FLOW group (reported)

**Root cause, two parts.** The validation label was `hide()`-ed when the
configuration was valid, leaving a blank region and making the group's height
jump as settings changed. Separately, the column split put three groups on the
left and one on the right, so the right column had dead space beneath SESSION
START.

**Fix.** The group was split along a clearer axis:

```
LEFT  — what the hardware IS        RIGHT — what this SESSION does
  ACQUISITION DEVICE                  SESSION START
  COMPUTE TARGET                      DATA FLOW
  HARDWARE STATUS
```

`HARDWARE STATUS` carries the capability read-out and the validation result,
and is given the left column's stretch so it absorbs spare height. The status
label is now **always visible**: when the configuration is valid it states so
in green rather than vanishing, which both fills the space usefully and keeps
the layout stable.

Measured group heights at 960×760: left 425 px, right 411 px — balanced, with
no dead region in either column.

### Also removed: the Port selector

Redundant once Device lists actual boards, and for Ethernet it duplicated
information the user cannot independently choose. Folded into the device
entry: `mlx5_0 · port 1  (enp60s0f0np0)  —  link up, 192.168.1.1`.

---

## 13. H2C control relocation and data-flow highlighting

### 3D button moved to the menu bar, and renamed

It floated over the plot area, sitting on top of the measurement tables and
needing repositioning on every resize. It is now a right-aligned corner widget
in the menu bar — always in the same place, obscuring nothing.

Renamed **H2C CONTROL**, because that is what it controls: the transmit
(playback / DAC) path.

### Gated on the data-flow mode

H2C is the transmit direction, so the control is meaningless in **ADC Only
(C2H)** mode — there is no transmit path for it to act on. Rather than let the
user open a window whose controls cannot do anything, the action is disabled
with a tooltip explaining why:

> *Unavailable in ADC Only (C2H) mode — no H2C transmit path. Select DAC Only
> or ADC + DAC.*

`SystemConfig::dataFlowMode` carries the launcher's choice into the main
window (stored as an int so `core/Types.h` keeps no dependency on the
hardware-database headers).

Verified across all three modes:

| mode | H2C CONTROL |
|---|---|
| ADC Only (C2H) | **disabled** |
| DAC Only (H2C) | enabled |
| ADC + DAC | enabled |

Styling distinguishes the states clearly: grey and flat when unavailable, bold
accent-blue when active.

### Data-flow selector: bold segmented toggles

Plain radio buttons signalled the selected transfer direction with a small
dot, which is easy to miss for a setting that determines what the session can
do at all. They are now segmented toggles reusing the PCIe/Ethernet visual
language already established in this dialog, with an accent-gradient fill on
the selected segment.

Each segment names both the protocol direction and its meaning, so the
C2H/H2C mapping is visible without consulting documentation:

```
┌──────────────┬──────────────┬──────────────┐
│  C2H         │  H2C         │  C2H + H2C   │
│  ADC Only    │  DAC Only    │  ADC + DAC   │
└──────────────┴──────────────┴──────────────┘
```

Verified in both selected states; the highlight follows the selection.
