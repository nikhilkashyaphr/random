#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"; cd "$ROOT"
find . -type f ! -name MANIFEST.sha256 ! -path './iW-ASCEJ-01-SF-R1.0-REL1.0/iW-ASCEJ-01-SC-R1.0-REL1.0/01_sdr_workstation_GUI/build/*' ! -name '*.o' \
  -print0 | sort -z | xargs -0 sha256sum > MANIFEST.sha256
echo "MANIFEST.sha256: $(grep -c . MANIFEST.sha256) files"
