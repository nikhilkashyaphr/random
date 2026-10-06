#!/usr/bin/env bash
# Cross-check the firmware against the GUI: every command the GUI can send must
# be implemented, every readback it requests must be resolved, and the unit
# conversions must be inverse on the two sides.
#
# Run after ANY change to pcie_regs.h, pcie_cfg.c or RfdcControl.cpp.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT"
python3 - "$ROOT" <<'PY'
import re, sys
R = sys.argv[1]
def rd(p):
    try: return open(R+"/"+p, errors="ignore").read()
    except OSError: return ""
fw  = rd("iW-ASCEJ-01-FF-R1.0-REL1.0/01_Source/pcie_cfg.c")
rf  = rd("iW-ASCEJ-01-FF-R1.0-REL1.0/01_Source/pcie_regs.h")
gui = rd("iW-ASCEJ-01-SF-R1.0-REL1.0/iW-ASCEJ-01-SC-R1.0-REL1.0/01_sdr_workstation_GUI/src/core/RfdcControl.cpp")
rg  = rd("iW-ASCEJ-01-SF-R1.0-REL1.0/iW-ASCEJ-01-SC-R1.0-REL1.0/01_sdr_workstation_GUI/src/core/pcie_regs.h")
rh  = rd("iW-ASCEJ-01-SF-R1.0-REL1.0/iW-ASCEJ-01-SC-R1.0-REL1.0/02_host_tools/pcie_regs.h")
ui  = rd("iW-ASCEJ-01-SF-R1.0-REL1.0/iW-ASCEJ-01-SC-R1.0-REL1.0/01_sdr_workstation_GUI/src/ui/ControlWindow3D.cpp")
fail = 0
def chk(label, ok, detail=""):
    global fail
    print("  %-48s %s %s" % (label, "PASS" if ok else "** FAIL **", detail))
    if not ok: fail += 1

norm = lambda s: s.replace("\r\n", "\n")
chk("pcie_regs.h identical: firmware == gui",       norm(rf) == norm(rg))
chk("pcie_regs.h identical: firmware == host-tools", norm(rf) == norm(rh))

gui_ev   = set(re.findall(r'PCIE_EVT_([A-Z_0-9]+)', gui))
fw_apply = set(re.findall(r'case\s+PCIE_EVT_([A-Z_0-9]+)\s*:', fw))
miss = sorted(gui_ev - fw_apply)
chk("every GUI event handled by firmware", not miss, str(miss) if miss else "(%d)" % len(gui_ev))

gui_rb = {i for i in re.findall(r'PCIE_RB_([A-Z_0-9]+)', gui)
          if i not in ("TAG", "REQUEST", "IS_UNANSWERED")}
fw_rb  = set(re.findall(r'case\s+PCIE_RB_([A-Z_0-9]+)\s*:', fw))
miss = sorted(gui_rb - fw_rb)
chk("every GUI readback id resolved by firmware", not miss, str(miss) if miss else "(%d)" % len(gui_rb))

# An event present in the validator but absent from the apply switch would ACK
# success and do nothing. That shipped once; it must never ship again.
chk("READBACK is answered, not merely accepted",
    "pcie_wr(PCIE_REG_CFG, pcie_readback" in fw)
chk("DEFAULTS is implemented, not merely accepted",
    "restoring boot defaults" in fw)
chk("firmware announces ABI at boot", "PCIE_CFG_ABI" in fw)
chk("ACK status marker used on both sides",
    "RING_ACK_BUILD" in fw and "RING_ACK_VALID" in gui)
chk("GUI defaults derive from PCIE_DEF_*", ui.count("PCIE_DEF_") >= 5,
    "(%d uses)" % ui.count("PCIE_DEF_"))

for name, gp, gs, fp, fs in [
    ("NCO  MHz<->kHz",  r'\* 1000\.0',    gui, r'PCIE_NCO_KHZ_TO_MHZ|/ 1000\.0', fw+rf),
    ("DDS  MHz<->Hz",   r'1e6|1000000\.0', gui, r'PCIE_DDS_HZ_TO_MHZ',            fw+rf),
    ("DSA  dB<->dBx10", r'\* 10\.0',      gui, r'/ 10\.0',                        fw),
    ("QMC  gain<->x1e4",r'\* 10000\.0',   gui, r'/ 10000\.0',                     fw),
]:
    chk("unit conversion inverse: %s" % name,
        bool(re.search(gp, gs)) and bool(re.search(fp, fs)))

print()
print("  RESULT:", "PASS" if fail == 0 else "%d FAILURE(S)" % fail)
sys.exit(1 if fail else 0)
PY
