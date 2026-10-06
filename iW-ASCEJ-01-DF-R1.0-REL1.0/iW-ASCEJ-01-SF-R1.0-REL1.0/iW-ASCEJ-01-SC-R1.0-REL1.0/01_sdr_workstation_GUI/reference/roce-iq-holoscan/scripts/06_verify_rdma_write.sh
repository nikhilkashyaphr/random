#!/usr/bin/env bash
# 06_verify_rdma_write.sh -- PROVE the link is doing one-sided RDMA WRITE,
# not SEND/RECV. Two independent methods, run back to back:
#
#   Method A  NIC hardware counters: port_xmit_data plus the RDMA-specific
#             counters move, and on the receive side NO receive-queue
#             completions are consumed in pure-write mode.
#   Method B  RoCEv2 opcode on the wire: tcpdump on UDP/4791 + the opcode
#             byte of the InfiniBand BTH shows RDMA_WRITE rather than SEND.
#
# Run the receiver and sender in -w write_imm (or write) first, OR let this
# script drive a short self-contained burst. Defaults assume the loopback
# namespace setup from 02_setup_network.sh.
#
#   sudo ./scripts/06_verify_rdma_write.sh                # write_imm, 5 s
#   sudo MODE=write   ./scripts/06_verify_rdma_write.sh   # pure write
#   sudo MODE=send    ./scripts/06_verify_rdma_write.sh   # contrast run
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "run with sudo"; exit 1; }

MODE="${MODE:-write_imm}"
DUR="${DUR:-5}"
IP_RX="${IP_RX:-192.168.100.2}"
NS="${NS:-ns_tx}"
IF_RX="${IF_RX:-}"        # Port B netdev, for tcpdump (auto-detect if empty)
BIN="$(cd "$(dirname "$0")/.." && pwd)"
[[ -x $BIN/rdma_rx && -x $BIN/rdma_tx ]] || { echo "run 'make' first"; exit 1; }

# locate the mlx5 device behind Port B (default-ns side)
RDMA_DEV="${RDMA_DEV:-mlx5_1}"
HW=/sys/class/infiniband/$RDMA_DEV/ports/1/hw_counters
CNT=/sys/class/infiniband/$RDMA_DEV/ports/1/counters

read_ctr() { [[ -r $1/$2 ]] && cat "$1/$2" || echo 0; }

echo "=============================================================="
echo " RDMA WRITE verification  --  mode: $MODE, ${DUR}s on $RDMA_DEV"
echo "=============================================================="

# ---- snapshot counters BEFORE ----
declare -A B
for c in port_xmit_data port_rcv_data; do B[$c]=$(read_ctr "$CNT" $c); done
for c in rx_write_requests tx_write_requests np_ecn_marked_roce_packets \
         rdma_tx_bytes rdma_rx_bytes out_of_sequence; do
    B[$c]=$(read_ctr "$HW" $c)
done

# ---- start receiver ----
"$BIN/rdma_rx" -a "$IP_RX" -w "$MODE" > /tmp/vw_rx.log 2>&1 &
RXPID=$!
sleep 1

# ---- start a wire capture (Method B) ----
if [[ -z "$IF_RX" ]]; then
    IF_RX=$(ls "/sys/class/infiniband/$RDMA_DEV/device/net" 2>/dev/null | head -1)
fi
PCAP=/tmp/vw_roce.pcap
if command -v tcpdump >/dev/null && [[ -n "$IF_RX" ]]; then
    timeout "$((DUR+2))" tcpdump -i "$IF_RX" -s 64 -c 200 -w "$PCAP" \
        'udp port 4791' >/tmp/vw_tcpdump.log 2>&1 &
    TDPID=$!
else
    echo "(tcpdump or Port B netdev unavailable -- skipping wire capture)"
    TDPID=""
fi
sleep 0.5

# ---- run the sender ----
echo "--- streaming ${DUR}s in -w $MODE ---"
ip netns exec "$NS" "$BIN/rdma_tx" -s "$IP_RX" -w "$MODE" -g 20 -t "$DUR" \
    2>&1 | tail -4 || true

sleep 1
kill -INT $RXPID 2>/dev/null || true
wait $RXPID 2>/dev/null || true
[[ -n "$TDPID" ]] && { wait $TDPID 2>/dev/null || true; }

# ---- snapshot counters AFTER ----
declare -A A
for c in port_xmit_data port_rcv_data; do A[$c]=$(read_ctr "$CNT" $c); done
for c in rx_write_requests tx_write_requests np_ecn_marked_roce_packets \
         rdma_tx_bytes rdma_rx_bytes out_of_sequence; do
    A[$c]=$(read_ctr "$HW" $c)
done

echo
echo "================== METHOD A: NIC counters ===================="
printf "%-32s %15s\n" "counter" "delta"
movement=0
for c in tx_write_requests rx_write_requests rdma_tx_bytes rdma_rx_bytes \
         port_xmit_data port_rcv_data out_of_sequence; do
    d=$(( ${A[$c]:-0} - ${B[$c]:-0} ))
    printf "%-32s %15d\n" "$c" "$d"
    [[ $c == *write_requests && $d -gt 0 ]] && movement=1
done
echo
echo "Interpretation:"
echo "  * tx/rx_write_requests climbing  => RDMA WRITE operations on the wire"
echo "    (these stay ~0 in -w send; that is the tell-tale difference)."
echo "  * rx receive log below: in -w write it shows frames with NO recv"
echo "    completions consumed; in write_imm one completion per frame."
if [[ $movement -eq 1 ]]; then
    echo "  RESULT: WRITE requests counted -> one-sided RDMA CONFIRMED."
elif [[ $MODE == send ]]; then
    echo "  RESULT: no WRITE requests (as expected for -w send)."
else
    echo "  NOTE: WRITE counters did not move -- this NIC/FW may name them"
    echo "        differently; rely on Method B (wire opcode) below."
fi

echo
echo "================== METHOD B: RoCEv2 opcode ==================="
if [[ -n "$TDPID" && -s "$PCAP" ]] && command -v tshark >/dev/null; then
    echo "InfiniBand BTH opcodes seen (RDMA_WRITE* vs SEND*):"
    tshark -r "$PCAP" -Y "infiniband" -T fields -e infiniband.bth.opcode \
        2>/dev/null | sort | uniq -c | while read -r n op; do
        case "$op" in
            0x0a|10) echo "  $n  x$op  RDMA_WRITE_First/Only" ;;
            0x0b|11) echo "  $n  x$op  RDMA_WRITE_Last" ;;
            0x06|6)  echo "  $n  x$op  RDMA_WRITE_Only" ;;
            0x0c|12) echo "  $n  x$op  RDMA_WRITE_Only_with_IMM" ;;
            0x04|4)  echo "  $n  x$op  SEND_Only" ;;
            *)       echo "  $n  x$op" ;;
        esac
    done
    echo "  (opcodes 0x06/0x0a/0x0b/0x0c = WRITE family; 0x00-0x05 = SEND family)"
elif [[ -n "$TDPID" && -s "$PCAP" ]]; then
    echo "Captured $(stat -c%s "$PCAP") bytes to $PCAP."
    echo "tshark not installed for opcode decode. Inspect with:"
    echo "  wireshark $PCAP    (filter: infiniband.bth.opcode)"
    echo "Or install: sudo apt-get install -y tshark"
else
    echo "No capture available (need tcpdump + Port B netdev access)."
fi

echo
echo "===================== receiver log =========================="
tail -6 /tmp/vw_rx.log
echo "=============================================================="
echo "Done. For a side-by-side contrast, run once with MODE=send and"
echo "once with MODE=write_imm and compare the *_write_requests deltas."
