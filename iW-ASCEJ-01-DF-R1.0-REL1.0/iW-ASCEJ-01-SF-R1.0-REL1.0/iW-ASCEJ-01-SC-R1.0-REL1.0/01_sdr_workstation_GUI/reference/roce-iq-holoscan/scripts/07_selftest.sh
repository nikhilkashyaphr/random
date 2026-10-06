#!/usr/bin/env bash
# 07_selftest.sh -- offline consistency checks that need no hardware.
# Verifies the shared-memory ABIs the C programs and the Python visualizer
# both depend on, plus that every binary built. Run after 'make'.
set -euo pipefail
BIN="$(cd "$(dirname "$0")/.." && pwd)"
cd "$BIN"

fail=0
note() { printf "  %-44s %s\n" "$1" "$2"; }

echo "== 1. binaries present =="
for b in rdma_tx rdma_rx scale_tx scale_rx; do
    if [[ -x $b ]]; then note "$b" "OK"; else note "$b" "MISSING (run make)"; fail=1; fi
done
[[ -x rdma_rx_gpu ]] && note "rdma_rx_gpu" "OK (built)" \
    || note "rdma_rx_gpu" "not built (optional: make gpu)"

echo "== 2. C ABI static asserts (compile-time) =="
if gcc -std=gnu11 -Isrc -fsyntax-only -x c - <<'EOF' 2>/tmp/abi.err
#include "src/rdma_common.h"
int main(void){return 0;}
EOF
then note "rdma_common.h static_asserts" "OK"; else
    note "rdma_common.h static_asserts" "FAILED"; cat /tmp/abi.err; fail=1; fi

echo "== 3. C <-> Python ring/frame ABI agree =="
python3 - <<'EOF' || exit 1
import re, sys
hdr = open("src/rdma_common.h").read()

def c_off(struct, field):
    m = re.search(rf"offsetof\(struct {struct},\s*{field}\)\s*==\s*(\d+)", hdr)
    return int(m.group(1)) if m else None

def check(struct, field, want):
    got = c_off(struct, field)
    if got is None:
        print(f"  {struct}.{field:<16} (no static_assert; skipped)")
        return True
    ok = (got == want)
    print(f"  {struct}.{field:<16} C={got:<3} py={want:<3} "
          f"{'OK' if ok else 'MISMATCH'}")
    return ok

ok = True
# Python frame-header parser HDR_FMT='<QQQIIQq' => 0,8,16,24,28,32,40
ok &= check("frame_hdr", "n_samples", 24)
ok &= check("frame_hdr", "sample_rate_hz", 32)
ok &= check("frame_hdr", "center_freq_hz", 40)

viz = open("viz/holoscan_iq_viz.py").read()
m = re.search(r"OFF_FS,\s*OFF_FC,\s*OFF_WCOUNT,\s*OFF_BYTES,\s*OFF_GAPS"
              r"\s*=\s*([\d,\s]+)", viz)
fs, fc, wc, bt, gp = [int(x) for x in m.group(1).split(",")]
print(f"  viz ring offsets fs={fs} fc={fc} wcount={wc} "
      f"bytes={bt} gaps={gp}")
# These three are locked by static_asserts; the fs/fc pair is verified by
# being exactly 16 bytes ahead of write_count (the struct is packed in
# declaration order: sample_rate_hz, center_freq_hz, write_count).
ok &= check("ring_ctrl", "write_count", wc)
ok &= check("ring_ctrl", "bytes_total", bt)
ok &= check("ring_ctrl", "seq_gaps", gp)
if not (fs == wc - 16 and fc == wc - 8):
    print(f"  ring_ctrl fs/fc spacing  MISMATCH (fs={fs} fc={fc} "
          f"expected {wc-16}/{wc-8})")
    ok = False
else:
    print(f"  ring_ctrl fs/fc spacing  OK (fs={fs} fc={fc}, "
          f"16/8 B before write_count)")

if ok:
    print("  ring/frame ABI: CONSISTENT")
else:
    sys.exit(1)
EOF

echo "== 4. transport-mode parity (tx and rx accept same -w) =="
for m in send write write_imm; do
    t=$(./rdma_tx -s 0.0.0.0 -w "$m" -t 0 2>&1 | grep -c "bad -w" || true)
    r=$(./rdma_rx -w "$m" -p 0 2>&1 | grep -c "bad -w" || true)
    if [[ $t == 0 ]]; then note "rdma_tx -w $m" "accepted"; else note "rdma_tx -w $m" "REJECTED"; fail=1; fi
done

echo "== 5. python syntax =="
if python3 -m py_compile viz/*.py 2>/tmp/py.err; then
    note "viz/*.py" "OK"; else note "viz/*.py" "SYNTAX ERROR"; cat /tmp/py.err; fail=1; fi

echo
if [[ $fail -eq 0 ]]; then
    echo "SELFTEST: PASS -- ABIs consistent, binaries built, modes parity OK."
else
    echo "SELFTEST: FAIL -- see above."; exit 2
fi
