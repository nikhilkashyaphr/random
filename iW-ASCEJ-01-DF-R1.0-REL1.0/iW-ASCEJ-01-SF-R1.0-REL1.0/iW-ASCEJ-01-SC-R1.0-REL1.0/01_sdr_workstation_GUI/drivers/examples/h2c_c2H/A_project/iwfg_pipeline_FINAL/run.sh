#!/bin/bash
# iwfg pipeline launcher (v23 - current driver)
#
# The driver creates TWO device nodes ("/dev/iwfg" no longer exists):
#   /dev/iwfg0  C2H (card -> host, qid 0)
#   /dev/iwfg1  H2C (host -> card, qid 1)
DEV_C2H=${1:-/dev/iwfg0}
DEV_H2C=${2:-/dev/iwfg1}
DIR="$(cd "$(dirname "$0")" && pwd)"

cleanup() {
    echo ""; echo "[run.sh] Stopping..."
    kill $H2C_PID $C2H_PID 2>/dev/null
    wait $H2C_PID $C2H_PID 2>/dev/null
    rm -f /tmp/iwfg_h2c.fifo /tmp/iwfg_c2h.fifo
    echo "[run.sh] Done."
    exit 0
}
trap cleanup SIGINT SIGTERM

echo "================================================="
echo "  IWFG Stream  H2C + C2H  (current driver)"
echo "  C2H device : $DEV_C2H"
echo "  H2C device : $DEV_H2C"
echo "  H2C FIFO   : /tmp/iwfg_h2c.fifo  (GNU Radio writes)"
echo "  C2H FIFO   : /tmp/iwfg_c2h.fifo  (GNU Radio reads)"
echo "================================================="
echo ""
echo "GNU Radio flowgraph:"
echo "  H2C: IQ Source -> Throttle(184320000) -> FIFO Sink (fifo_sink_v2.py)"
echo "  C2H: FIFO Source (live_stream_python_complex.py) -> display"
echo ""

"$DIR/iwfg_h2c" "$DEV_H2C" &
H2C_PID=$!
sleep 1
"$DIR/iwfg_c2h" "$DEV_C2H" &
C2H_PID=$!

echo "[run.sh] H2C=$H2C_PID  C2H=$C2H_PID"
echo "[run.sh] Start GNU Radio now. Ctrl+C to stop."
echo ""

wait $H2C_PID $C2H_PID
cleanup
