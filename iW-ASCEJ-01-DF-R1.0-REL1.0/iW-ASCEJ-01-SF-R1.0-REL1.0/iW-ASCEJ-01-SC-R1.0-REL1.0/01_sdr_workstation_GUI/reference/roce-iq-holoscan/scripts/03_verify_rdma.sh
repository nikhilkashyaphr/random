#!/usr/bin/env bash
# 03_verify_rdma.sh -- prove RDMA works across the cable before running
# the demo: device info, an rping handshake, and an ib_write_bw benchmark.
#
# Expected ib_write_bw on PCIe Gen3 x8: roughly 6.3-6.8 GB/s (50-55 Gb/s).
# That is the PCIe slot's ceiling, not the 100 GbE link's -- see README.
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "run with sudo"; exit 1; }

NS="ns_tx"
IP_RX="192.168.100.2"

echo "== RDMA devices, default namespace (should show Port B) =="
ibv_devinfo | grep -E "hca_id|port:|state|active_mtu" || true
echo
echo "== RDMA devices inside $NS (should show Port A) =="
ip netns exec "$NS" ibv_devinfo 2>/dev/null | \
    grep -E "hca_id|port:|state|active_mtu" || \
    echo "(none visible -- shared netns mode; that is OK)"

echo
echo "== rping connectivity test (3 s) =="
rping -s -a "$IP_RX" -C 3 -v &
SRV=$!
sleep 1
if ip netns exec "$NS" rping -c -a "$IP_RX" -C 3 -v; then
    echo "rping OK"
else
    echo "rping FAILED -- RDMA-CM cannot connect across the cable."
    kill $SRV 2>/dev/null || true
    exit 1
fi
wait $SRV 2>/dev/null || true

echo
echo "== ib_write_bw benchmark (RDMA-CM mode, Gb/s) =="
ib_write_bw -R --report_gbits -a >/tmp/ibw_server.log 2>&1 &
SRV=$!
sleep 1
ip netns exec "$NS" ib_write_bw -R --report_gbits -a "$IP_RX" || {
    echo "benchmark client failed; server log:"; cat /tmp/ibw_server.log;
    kill $SRV 2>/dev/null || true; exit 1; }
wait $SRV 2>/dev/null || true

echo
echo "If the large-message lines show ~50+ Gb/s you are at the PCIe Gen3 x8"
echo "ceiling and everything is healthy. Next: make && run the demo (README 8)."
