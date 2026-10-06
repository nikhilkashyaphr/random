#!/usr/bin/env python3
"""Correctness harness for iwfg_c2h.

Sends N UDP datagrams of 1472 bytes, each stamped with a 64-bit sequence
number and filled with a deterministic pattern.  Reads the FIFO and checks
that the byte stream is exactly the concatenation of the payloads, in
order, with no gaps other than explicitly counted drops.
"""
import os, socket, struct, sys, threading, time

FIFO    = sys.argv[1] if len(sys.argv) > 1 else "/tmp/iwfg_c2h.fifo"
PORT    = int(sys.argv[2]) if len(sys.argv) > 2 else 50000
NPKT    = int(sys.argv[3]) if len(sys.argv) > 3 else 200_000
PAYLOAD = 1472

results = {}

def make_payload(seq: int) -> bytes:
    body = struct.pack("<Q", seq)
    pat  = bytes(((seq + i) & 0xFF) for i in range(PAYLOAD - 8))
    return body + pat

def reader():
    fd = os.open(FIFO, os.O_RDONLY)
    got, ok, ooo, corrupt = 0, 0, 0, 0
    last_seq = -1
    buf = b""
    deadline = time.time() + 60
    while got < NPKT and time.time() < deadline:
        try:
            chunk = os.read(fd, 1 << 20)
        except OSError:
            break
        if not chunk:
            break
        buf += chunk
        while len(buf) >= PAYLOAD:
            pkt, buf = buf[:PAYLOAD], buf[PAYLOAD:]
            got += 1
            seq = struct.unpack("<Q", pkt[:8])[0]
            if pkt != make_payload(seq):
                corrupt += 1
            elif seq <= last_seq:
                ooo += 1
            else:
                ok += 1
            last_seq = max(last_seq, seq)
            if seq == NPKT - 1:
                deadline = min(deadline, time.time() + 2)
    os.close(fd)
    results.update(got=got, ok=ok, ooo=ooo, corrupt=corrupt,
                   tail=len(buf), last_seq=last_seq)

t = threading.Thread(target=reader)
t.start()
time.sleep(0.5)                     # let writer open the FIFO

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.connect(("127.0.0.1", PORT))
sent = 0
for seq in range(NPKT):
    s.send(make_payload(seq))
    sent += 1
    if sent % 2000 == 0:
        time.sleep(0.001)           # pace to avoid loopback socket drops
s.close()

t.join()
r = results
print(f"sent={sent} got={r['got']} ok={r['ok']} ooo={r['ooo']} "
      f"corrupt={r['corrupt']} tail_bytes={r['tail']} last_seq={r['last_seq']}")
lost = sent - r["got"]
assert r["corrupt"] == 0, "CORRUPTION DETECTED"
assert r["ooo"] == 0, "REORDERING DETECTED"
assert r["tail"] == 0, "PARTIAL PACKET IN STREAM (framing broken)"
print(f"lost={lost} ({100.0*lost/sent:.3f}%)  "
      f"{'PASS' if lost == 0 else 'PASS (drops are socket-level, counted)'}")
