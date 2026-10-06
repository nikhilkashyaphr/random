#!/usr/bin/env bash
# 02_setup_network.sh -- put Port A in its own network namespace, assign
# IPs, set MTU 9000, and prove jumbo frames cross the physical cable.
#
# WHY A NAMESPACE? With both ports in the same host, Linux notices that the
# destination IP is local and short-circuits traffic through the loopback
# path -- nothing would ever touch the cable or the NIC's RDMA engine.
# Moving Port A (netdev + its RDMA device) into namespace "ns_tx" makes the
# two ports look like two different machines, forcing real RoCEv2 packets
# out Port A and into Port B.
#
# NOTE: this configuration does not survive a reboot -- just re-run it.
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "run with sudo"; exit 1; }

# ---- EDIT THESE FOUR LINES IF YOUR NAMES DIFFER --------------------------
IF_TX="${IF_TX:-}"            # netdev of Port A, e.g. enp1s0f0np0
IF_RX="${IF_RX:-}"            # netdev of Port B, e.g. enp1s0f1np1
RDMA_TX="${RDMA_TX:-mlx5_0}"  # RDMA device behind Port A
RDMA_RX="${RDMA_RX:-mlx5_1}"  # RDMA device behind Port B
# ---------------------------------------------------------------------------

IP_TX="192.168.100.1"
IP_RX="192.168.100.2"
NS="ns_tx"

echo "== RDMA device -> netdev mapping =="
for d in /sys/class/infiniband/*; do
    [[ -e $d ]] || continue
    echo "  $(basename "$d") -> $(ls "$d/device/net" 2>/dev/null | tr '\n' ' ')"
done

if [[ -z "$IF_TX" || -z "$IF_RX" ]]; then
    echo
    echo "Set IF_TX / IF_RX to the two netdev names shown above, e.g.:"
    echo "  sudo IF_TX=enp1s0f0np0 IF_RX=enp1s0f1np1 $0"
    exit 1
fi

# RDMA devices normally live in the init namespace regardless of where the
# netdev goes. 'exclusive' mode lets us move them with the port. This can
# fail if something (often Docker) already created namespaces.
if ! rdma system set netns exclusive 2>/dev/null; then
    echo "WARNING: could not enable rdma exclusive netns mode (Docker running?)."
    echo "         Continuing in shared mode: rdma_tx may then see BOTH ports;"
    echo "         traffic still crosses the cable because the netdevs are split."
fi

ip netns del "$NS" 2>/dev/null || true
ip netns add "$NS"

# Move Port A's netdev, and its RDMA device when exclusive mode allows it.
ip link set "$IF_TX" netns "$NS"
rdma dev set "$RDMA_TX" netns "$NS" 2>/dev/null \
    && echo "moved $RDMA_TX into $NS" \
    || echo "note: $RDMA_TX stays in default ns (shared mode)"

# Port A inside ns_tx
ip netns exec "$NS" ip addr add "$IP_TX/24" dev "$IF_TX"
ip netns exec "$NS" ip link set "$IF_TX" mtu 9000 up
ip netns exec "$NS" ip link set lo up

# Port B in the default namespace
ip addr flush dev "$IF_RX" 2>/dev/null || true
ip addr add "$IP_RX/24" dev "$IF_RX"
ip link set "$IF_RX" mtu 9000 up

sleep 2   # let carrier settle

echo
echo "== Jumbo-frame ping across the cable (8972 B + headers = 9000) =="
if ip netns exec "$NS" ping -c 3 -i 0.3 -M do -s 8972 "$IP_RX"; then
    echo
    echo "SUCCESS: Port A ($IP_TX, ns_tx) <-> Port B ($IP_RX) at MTU 9000."
    echo "Next: sudo ./scripts/03_verify_rdma.sh"
else
    echo "FAILED: check the cable, link LEDs, and that IF_TX/IF_RX are right."
    exit 1
fi
