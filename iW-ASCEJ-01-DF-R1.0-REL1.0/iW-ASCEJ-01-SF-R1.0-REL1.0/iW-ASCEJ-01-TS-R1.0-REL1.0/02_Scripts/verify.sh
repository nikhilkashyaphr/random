#!/usr/bin/env bash
# Package integrity + firmware/GUI synchronisation.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; ROOT="$(cd "$HERE/../../.." && pwd)"; cd "$ROOT"; rc=0
echo "iWave SDR $(cat VERSION) — verification"; echo
echo "1. File hashes"
if [ -f MANIFEST.sha256 ] && sha256sum --quiet -c MANIFEST.sha256 2>/dev/null; then
  echo "   PASS  $(grep -c . MANIFEST.sha256) files match"
else echo "   FAIL  hash mismatch or MANIFEST.sha256 missing"; rc=1; fi
echo; echo "2. Firmware source set"
n=$(ls iW-ASCEJ-01-FF-R1.0-REL1.0/01_Source/*.c iW-ASCEJ-01-FF-R1.0-REL1.0/01_Source/*.h 2>/dev/null | wc -l)
[ "$n" -ge 37 ] && echo "   PASS  $n files" || { echo "   FAIL  only $n"; rc=1; }
for f in rfdc_ctl.c pcie_access.c pcie_access.h; do
  [ -f "iW-ASCEJ-01-FF-R1.0-REL1.0/01_Source/$f" ] && { echo "   FAIL  $f must not be in the firmware tree"; rc=1; }
done
echo; echo "3. Firmware/GUI synchronisation"
"$HERE/audit_sync.sh" | sed 's/^/   /' || rc=1
echo; [ $rc -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"; exit $rc
