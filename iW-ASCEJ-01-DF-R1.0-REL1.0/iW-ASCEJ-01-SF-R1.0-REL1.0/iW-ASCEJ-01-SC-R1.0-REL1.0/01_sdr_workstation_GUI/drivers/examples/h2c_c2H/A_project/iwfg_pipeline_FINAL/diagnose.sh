#!/bin/bash
# =============================================================================
# diagnose.sh - one-shot iwfg pipeline diagnostic (run as root)
#
# Runs the full A/B/C decision tree WITHOUT GNU Radio and prints ONE verdict:
#
#   [A] Card not CONSUMING H2C  -> PS: route AXIS switch QDMA->DAC, DAC/MTS up
#   [B] Card not PRODUCING C2H  -> PS: ADC tile running, ADC->QDMA C2H routed
#   [C] Data flows but samples are zero/noise -> digital mute or NCO/loopback
#   [OK] Both directions healthy -> problem is in the GNU Radio flowgraph
#
# Usage: sudo ./diagnose.sh [seconds_per_test]
# =============================================================================
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
SECS=${1:-6}
H2C_FIFO=/tmp/iwfg_h2c.fifo
C2H_FIFO=/tmp/iwfg_c2h.fifo
LOG_H2C=/tmp/diag_h2c.log
LOG_C2H=/tmp/diag_c2h.log
CAP=/tmp/diag_c2h_capture.bin

say()  { printf '\n=== %s ===\n' "$*"; }
fail() { printf 'VERDICT: %s\n' "$*"; cleanup; exit 1; }

cleanup() {
    pkill -f "$DIR/iwfg_h2c" 2>/dev/null
    pkill -f "$DIR/iwfg_c2h" 2>/dev/null
    pkill -f "dd if=/dev/zero of=$H2C_FIFO" 2>/dev/null
    sleep 0.5
    rm -f "$H2C_FIFO" "$C2H_FIFO"
}
trap cleanup EXIT

[ "$(id -u)" = 0 ] || { echo "Run as root: sudo ./diagnose.sh"; exit 1; }

say "0. Environment"
for d in /dev/iwfg0 /dev/iwfg1; do
    if [ -e "$d" ]; then echo "  $d : present"
    else fail "$d missing - driver not loaded or old driver installed. \
Check: lsmod | grep iwfg ; dmesg | grep -i iwfg"
    fi
done
[ -x "$DIR/iwfg_h2c" ] && [ -x "$DIR/iwfg_c2h" ] || fail "binaries missing - run: make"
dmesg | grep -i iwfg | tail -5 | sed 's/^/  dmesg: /'

# -----------------------------------------------------------------------------
say "1. H2C isolation test (${SECS}s, zeros -> card, no GNU Radio)"
cleanup
"$DIR/iwfg_h2c" /dev/iwfg1 "$H2C_FIFO" 16384 > "$LOG_H2C" 2>&1 &
H2C_PID=$!
sleep 1
# feed continuously; dies with the fifo on cleanup
( dd if=/dev/zero of="$H2C_FIFO" bs=16384 2>/dev/null ) &
FEED_PID=$!
sleep "$SECS"
kill -INT $H2C_PID 2>/dev/null; kill $FEED_PID 2>/dev/null
sleep 1; kill -9 $H2C_PID $FEED_PID 2>/dev/null
wait $H2C_PID 2>/dev/null

H2C_MB=$(grep -oE 'Total sent: [0-9.]+' "$LOG_H2C" | grep -oE '[0-9.]+' | tail -1)
H2C_MB=${H2C_MB:-0}
H2C_STALLED=$(grep -c 'card is NOT' "$LOG_H2C" || true)
H2C_RATE=$(awk -v mb="$H2C_MB" -v s="$SECS" 'BEGIN{printf "%.1f", mb/s}')
echo "  sent total : ${H2C_MB} MB in ${SECS}s  (~${H2C_RATE} MB/s)"
echo "  watchdog   : $([ "$H2C_STALLED" -gt 0 ] && echo FIRED || echo quiet)"
sed -n 's/^/  h2c: /p' "$LOG_H2C" | tail -4

H2C_OK=0
# stalled = watchdog fired, or total stuck around the fabric-FIFO depth (<1 MB)
if [ "$H2C_STALLED" -gt 0 ] || awk -v mb="$H2C_MB" 'BEGIN{exit !(mb<1)}'; then
    H2C_VERDICT="STALLED (card accepted ~${H2C_MB} MB then stopped taking data)"
else
    H2C_VERDICT="FLOWING (~${H2C_RATE} MB/s)"
    H2C_OK=1
fi
echo "  H2C: $H2C_VERDICT"

# -----------------------------------------------------------------------------
say "2. C2H isolation test (${SECS}s, card -> file, no GNU Radio)"
cleanup
"$DIR/iwfg_c2h" /dev/iwfg0 "$C2H_FIFO" 16 > "$LOG_C2H" 2>&1 &
C2H_PID=$!
sleep 1
timeout "$SECS" cat "$C2H_FIFO" > "$CAP" 2>/dev/null
kill -INT $C2H_PID 2>/dev/null
sleep 1; kill -9 $C2H_PID 2>/dev/null
wait $C2H_PID 2>/dev/null

C2H_BYTES=$(stat -c %s "$CAP" 2>/dev/null || echo 0)
C2H_STALLED=$(grep -c 'card is NOT' "$LOG_C2H" || true)
echo "  captured   : $C2H_BYTES bytes"
echo "  watchdog   : $([ "$C2H_STALLED" -gt 0 ] && echo FIRED || echo quiet)"
sed -n 's/^/  c2h: /p' "$LOG_C2H" | tail -4

C2H_OK=0; C2H_ZERO=0
if [ "$C2H_BYTES" -gt 0 ]; then
    C2H_OK=1
    NONZERO=$(head -c 1048576 "$CAP" | od -An -tx2 -v | grep -vcE '^( 0000)+ *$' || true)
    if [ "$NONZERO" -eq 0 ]; then
        C2H_ZERO=1
        echo "  C2H: FLOWING but samples are ALL ZERO"
    else
        # crude peak estimate from first 64K samples
        PEAK=$(head -c 262144 "$CAP" | od -An -td2 -v | tr -s ' ' '\n' \
               | awk 'BEGIN{m=0}{v=$1<0?-$1:$1; if(v>m)m=v}END{print m}')
        echo "  C2H: FLOWING, nonzero samples, peak int16 = ${PEAK:-?}"
        echo "       sample dump:"; od -An -tx2 -N32 "$CAP" | sed 's/^/         /'
    fi
else
    echo "  C2H: NO DATA (first buffer never filled)"
fi

# -----------------------------------------------------------------------------
say "FINAL VERDICT"
if [ "$H2C_OK" -eq 0 ]; then
    cat <<'EOT'
[A] The card is NOT CONSUMING H2C data.
    QDMA moved a small burst into the FPGA H2C FIFO and stalled (tready low).
    Host side is proven working. Fix ON THE PS (UART CLI / mailbox):
      - AXIS switch: route QDMA -> DAC (not the DDS compiler)
      - DAC tile enabled, clocks + MTS locked
    Then re-run this script.
EOT
elif [ "$C2H_OK" -eq 0 ]; then
    cat <<'EOT'
[B] H2C is flowing, but the card is NOT PRODUCING C2H data.
    Fix ON THE PS: ADC tile running, MTS done, ADC stream routed to QDMA C2H.
    Also check dmesg for repeating 'usr_overflow' (recovery thrash).
EOT
elif [ "$C2H_ZERO" -eq 1 ]; then
    cat <<'EOT'
[C-mute] Both directions flow, but every C2H sample is exactly 0x0000.
    Digital mute: RFdc/MTS incomplete, ADC gated, or C2H muxed to a zeroed
    source. PS-side configuration issue.
EOT
else
    cat <<'EOT'
[OK] Both directions flow with nonzero data.
    If peak int16 is only a few LSBs (noise): the DAC signal is not reaching
    the ADC - loopback cabling/attenuation, DAC output stage, or DAC/ADC NCO
    mismatch (mailbox NCO command). If peak is healthy (hundreds..~4096):
    the card path is GOOD - the remaining problem is in the GNU Radio
    flowgraph (check fifo paths, Throttle, and that the flowgraph stays
    running - your earlier log showed the sink disconnecting immediately).
EOT
fi
echo "Logs: $LOG_H2C  $LOG_C2H  capture: $CAP"
