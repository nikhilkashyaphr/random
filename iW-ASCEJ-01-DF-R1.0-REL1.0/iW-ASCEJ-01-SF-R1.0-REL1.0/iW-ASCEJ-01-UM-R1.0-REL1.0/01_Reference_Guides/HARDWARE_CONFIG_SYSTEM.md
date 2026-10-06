# Hardware-aware configuration system

## Architecture

```
UI (ConfigDialog, MainWindow, ControlWindow3D)
        │  reads only  ▼
Configuration / State  (hw::RequestedConfig)
        │              ▼
Validation Engine      (hw::validate)        generic rules, no board branching
        │              ▼
Hardware Database      (hw::HardwareDb)      the only place specs exist
        │              ▼
Backend / Hardware Interface                 unchanged
```

There is **no** `if (board == "47DR")` anywhere. Adding a kit is a data change
in `HardwareDb.cpp` and nothing else.

## 1. Hardware database — with provenance

`src/core/HardwareDb.{h,cpp}`. Every figure carries where it came from:

| Provenance | Meaning |
|---|---|
| `Verified` | confirmed against vendor/silicon documentation |
| `Derived` | follows arithmetically from a verified figure |
| `Unverified` | **could not be confirmed — shown as such, never assumed** |

### Boards

| Board | Device | ADC | DAC | Status |
|---|---|---|---|---|
| iWave RFSoC ZU47DR | XCZU47DR (Gen3) | 8 ch, 14-bit, **5.00 GSPS** | 8 ch, 14-bit, **9.85 GSPS** | verified |
| iWave RFSoC ZU28DR | XCZU28DR (Gen1) | 8 ch, 12-bit, **2.00 GSPS** | 8 ch, 14-bit, **6.40 GSPS** | verified |
| iWave Agilex 9 Direct RF | Agilex 9 | **unverified** | **unverified** | flagged |
| Generic / unlisted | — | not enforced | not enforced | no validation |

**Two ambiguities recorded rather than hidden**, both attached to the ZU47DR:

* One secondary source quotes **2.5 GSPS** for the ZU47DR ADC where the
  majority and the Gen3 device data give **5.0 GSPS**. The higher figure is
  used and the conflict is stored in the spec's `note`.
* The DAC is quoted as **9.85 GSPS**; 10 GSPS exists but needs vendor
  sign-off, and some modules derate to 8.92 GSPS depending on clocking.

**The Agilex 9 entry is deliberately empty.** The family exists, but the
*iWave board-level* channel counts and rates could not be confirmed from a
primary source, so no numbers were invented. The engine then warns that
validation is unavailable instead of enforcing a fabricated limit — a
configuration tool that quietly makes up a converter limit is worse than one
that admits it does not know.

## 2. Validation engine

`src/core/ConfigValidator.{h,cpp}`. Rules: channel count, sample-rate max,
tile minimum, ADC/DAC presence, combined ADC+DAC channel budget, ADC/DAC rate
ratio, and board notes carried as data.

**Errors block launch; warnings inform.** An *unknown* limit never produces an
error — refusing work on the strength of an unverified number would block
valid configurations for an invented reason.

### Test results (all from the real engine)

| case | result |
|---|---|
| 47DR, 8 ch @ 2.0 GSPS | **ok**, 0 errors |
| 47DR, **12 channels** (brief's example) | **ERROR** — "Maximum ADC channels supported: 8, Selected: 12" |
| 47DR, **7.2 GSPS** (brief's example) | **ERROR** — "Requested 7.200 / Maximum 5.000 GSPS" |
| 47DR, 4.8 GSPS | **WARNING** — "96.0% of this board's 5.000 GSPS maximum" |
| **28DR** @ 5.0 GSPS (Gen3 figure on a Gen1 part) | **ERROR** — max 2.000 GSPS |
| **Agilex 9** (unverified) | **3 warnings, 0 errors** — not blocked |
| 47DR duplex, ADC 5.0 / DAC 0.5 GSPS | **WARNING** — shared clocking tree |

The 28DR case is the one that matters architecturally: the same rule catches
it with no board-specific code, purely because the database says Gen1.

## 3. Data-flow modes

| User-facing | Internal |
|---|---|
| ADC Only | `C2H` |
| DAC Only | `H2C` |
| ADC + DAC | `C2H + H2C` |

Mapping verified by test. The C2H/H2C terminology is retained internally and
surfaced in the capability line, so the backend contract is unchanged.

## 4. 3D Control Window

`src/ui/ControlWindow3D.{h,cpp}` plus a floating corner button.

The contents were not specified, so **none are invented**. Two extension
points are provided:

```cpp
window->setViewport(myGlWidget);              // installs the 3D view
QWidget* p = window->addControlPanel("Beam"); // adds a control section
```

Non-modal (main GUI stays usable), and `closeEvent` **hides rather than
destroys**, so reopening preserves state as the brief requires.

## 5. Bugs found and fixed during integration

* **Tile minimum was a blocking error.** The rate field is the *host*
  streaming rate; a value below the converter minimum is the normal case
  (the fabric decimates first). Treating it as an error made the default
  122.88 MSPS configuration unlaunchable. Downgraded to informational.
* **"within 9%" was wrong and unclear** — `kWarnFraction` is 0.90, so the band
  is 10%. Now reports actual utilisation: "96.0% of the 5.000 GSPS maximum".
* **Data-flow group rendered below the buttons** — it was appended after the
  summary; moved into the form.
* **"3D CONTROL" clipped to "ONTRO"** — fixed size replaced with font metrics,
  which also survives display scaling.

## 6. Regression

Clean build, zero warnings. Simulator and CW capture replay both verified;
plots, channel selection, frequency accuracy, PCIe and RoCEv2 paths untouched.

## 7. Adding a board

```cpp
BoardSpec b;
b.id = "iw_newkit"; b.displayName = "iWave New Kit";
b.adc.present = true; b.adc.channels = 4; b.adc.resolutionBits = 14;
b.adc.maxSampleRateGsps = verified(2.5, "datasheet p.12");
db.push_back(b);
```

No GUI or engine change. If a figure is unknown, use `unverified("why")` and
the engine will warn instead of enforcing.
