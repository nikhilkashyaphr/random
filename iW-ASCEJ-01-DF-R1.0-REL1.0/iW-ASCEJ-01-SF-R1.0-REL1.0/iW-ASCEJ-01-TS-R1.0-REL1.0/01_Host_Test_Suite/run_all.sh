#!/usr/bin/env bash
# Run every host-side test. No board, no root, no GPU needed.
# Needs: gcc, cmake, Qt5 development files (the same as the GUI build).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
export QT_QPA_PLATFORM=offscreen
rc=0

echo "== firmware: DAC input routing (main.c) =="
"$HERE/firmware/run_tests.sh" || rc=1

for suite in pcie_emu gui h2c_emu; do
    echo; echo "== $suite =="
    if ! cmake -S "$HERE/$suite" -B "$WORK/$suite" -DCMAKE_BUILD_TYPE=Release > "$WORK/$suite.cfg.log" 2>&1; then
        echo "configure failed:"; tail -20 "$WORK/$suite.cfg.log"; rc=1; continue
    fi
    if ! cmake --build "$WORK/$suite" -j"$(nproc)" > "$WORK/$suite.build.log" 2>&1; then
        echo "build failed:"; grep -E 'error' "$WORK/$suite.build.log" | head -20; rc=1; continue
    fi
    ctest --test-dir "$WORK/$suite" --output-on-failure || rc=1
done

echo; [ $rc -eq 0 ] && echo "ALL TESTS: PASS" || echo "ALL TESTS: FAIL"
exit $rc
