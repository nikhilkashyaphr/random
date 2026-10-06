#!/usr/bin/env bash
# Build and run the firmware routing test on the host (gcc only, no BSP).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW="${PKG_FW:-$HERE/../../../../iW-ASCEJ-01-FF-R1.0-REL1.0/01_Source}"
[ -f "$FW/main.c" ] || { echo "firmware sources not found at $FW (set PKG_FW)"; exit 1; }
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
tr -d '\r' < "$FW/main.c"         > "$T/main.c"
tr -d '\r' < "$FW/pcie_regs.h"    > "$T/pcie_regs.h"
grep -E '^#define HW_DAC_SW_SI_(HOST|DDS)' "$FW/platform_ids.h" | tr -d '\r' > "$T/platform_wiring.h"
# The code under test, verbatim from main.c.
"$HERE/extract_routing.sh" "$FW/main.c" > "$T/routing_under_test.inc"
[ -s "$T/routing_under_test.inc" ] || { echo "could not extract the routing code"; exit 1; }
gcc -std=c11 -Wall -Wextra -Werror -I"$T" -o "$T/t" "$HERE/dac_source_test.c"
"$T/t"
