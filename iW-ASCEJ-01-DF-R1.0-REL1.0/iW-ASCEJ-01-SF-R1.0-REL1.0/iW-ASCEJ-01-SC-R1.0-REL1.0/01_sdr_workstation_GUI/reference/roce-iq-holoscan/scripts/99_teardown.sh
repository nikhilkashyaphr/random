#!/usr/bin/env bash
# 99_teardown.sh -- undo what 02_setup_network.sh did.
# Deleting the namespace automatically returns Port A's netdev (and its
# RDMA device, in exclusive mode) to the default namespace.
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "run with sudo"; exit 1; }

ip netns del ns_tx 2>/dev/null && echo "deleted ns_tx" || echo "ns_tx absent"
rdma system set netns shared 2>/dev/null || true
rm -f /dev/shm/iqring && echo "removed /dev/shm/iqring" || true
rm -f /dev/shm/iqctl  && echo "removed /dev/shm/iqctl"  || true
rm -f /tmp/vw_*.log /tmp/vw_roce.pcap 2>/dev/null || true
echo "done. (Re-run 02_setup_network.sh to set things up again.)"
