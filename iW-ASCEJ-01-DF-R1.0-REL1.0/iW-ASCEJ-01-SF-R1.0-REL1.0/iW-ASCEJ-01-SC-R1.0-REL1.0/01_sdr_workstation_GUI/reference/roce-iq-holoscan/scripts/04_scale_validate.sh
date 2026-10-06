#!/usr/bin/env bash
# 04_scale_validate.sh -- sweep N RoCEv2 links (default 1 10 50 100) and
# certify the system: throughput, integrity, loss, latency, NIC error
# counters. Writes per-step CSV + JSON into ./scale_results/.
#
#   sudo ./scripts/04_scale_validate.sh                 # default sweep, 30 s
#   sudo DUR=300 STEPS="100" ./scripts/04_scale_validate.sh   # 5-min soak
#   sudo GBPS=40 ./scripts/04_scale_validate.sh         # paced aggregate
#
# Full methodology, pass criteria and what each number means: VALIDATION.md
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "run with sudo"; exit 1; }

STEPS="${STEPS:-1 10 50 100}"
DUR="${DUR:-30}"
GBPS="${GBPS:-0}"           # 0 = unlimited (slams the PCIe ceiling)
FRAME="${FRAME:-262144}"
IP_RX="${IP_RX:-192.168.100.2}"
NS="${NS:-ns_tx}"
OUT="scale_results"
mkdir -p "$OUT"

BIN="$(cd "$(dirname "$0")/.." && pwd)"
[[ -x $BIN/scale_rx && -x $BIN/scale_tx ]] || { echo "run 'make' first"; exit 1; }

snap_counters() {  # $1 = file
    {
        echo "### rdma statistic"
        rdma statistic show 2>/dev/null || true
        echo "### hw_counters"
        for f in /sys/class/infiniband/mlx5_*/ports/1/hw_counters/*; do
            [[ -r $f ]] && echo "$f $(cat "$f")"
        done
        echo "### ethtool"
        for IF in $(ls /sys/class/net | grep -v lo); do
            ethtool -S "$IF" 2>/dev/null | \
                grep -Ei "discard|err|pause|out_of_buffer" | \
                sed "s/^/$IF /" || true
        done
    } > "$1"
}

overall_pass=1
for N in $STEPS; do
    echo
    echo "================ $N links, ${DUR}s, frame=$FRAME B, pace=${GBPS} Gb/s ================"
    snap_counters "$OUT/counters_${N}_before.txt"

    "$BIN/scale_rx" -a "$IP_RX" -n "$N" -b "$FRAME" -t "$DUR" \
        -C "$OUT/timeseries_${N}.csv" -O "$OUT/summary_${N}.json" \
        > "$OUT/rx_${N}.log" 2>&1 &
    RXPID=$!
    sleep 1

    ip netns exec "$NS" "$BIN/scale_tx" -s "$IP_RX" -n "$N" \
        -b "$FRAME" -g "$GBPS" -t "$DUR" > "$OUT/tx_${N}.log" 2>&1 || true

    wait $RXPID || true
    snap_counters "$OUT/counters_${N}_after.txt"

    echo "--- step result ($OUT/summary_${N}.json) ---"
    tail -n 14 "$OUT/rx_${N}.log" | grep -E "SUMMARY|links|frames|aggregate|gaps|payload|latency|verdict" || true

    if grep -q '"pass": true' "$OUT/summary_${N}.json" 2>/dev/null; then
        echo "STEP $N: PASS"
    else
        echo "STEP $N: FAIL"
        overall_pass=0
    fi

    echo "--- NIC error counter deltas (nonzero only) ---"
    diff "$OUT/counters_${N}_before.txt" "$OUT/counters_${N}_after.txt" \
        | grep '^>' | grep -Ev " 0$" || echo "(no counter movement -- clean)"
done

echo
if [[ $overall_pass -eq 1 ]]; then
    echo "OVERALL: PASS -- zero loss, zero corruption at every step."
else
    echo "OVERALL: FAIL -- inspect $OUT/ (see VALIDATION.md, section 'Reading failures')."
    exit 2
fi
