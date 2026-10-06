#!/usr/bin/env python3
"""Fast correctness harness: forked reader process, cheap per-packet checks
(sequence continuity + 4-point pattern probe, full pattern check every
256th packet)."""
import os, socket, struct, sys, time

FIFO    = sys.argv[1] if len(sys.argv) > 1 else "/tmp/iwfg_c2h.fifo"
PORT    = int(sys.argv[2]) if len(sys.argv) > 2 else 50000
NPKT    = int(sys.argv[3]) if len(sys.argv) > 3 else 50_000
PAYLOAD = 1472

BASE = bytes((i & 0xFF) for i in range(PAYLOAD - 8 + 256))

def make_payload(seq: int) -> bytes:
    o = seq & 0xFF
    return struct.pack("<Q", seq) + BASE[o:o + PAYLOAD - 8]

pid = os.fork()
if pid == 0:                                    # ---- reader child ----
    fd = os.open(FIFO, os.O_RDONLY)
    got = gaps = corrupt = ooo = 0
    exp = 0
    buf = b""
    deadline = time.time() + 45
    while got < NPKT and time.time() < deadline:
        chunk = os.read(fd, 1 << 20)
        if not chunk:
            break
        buf += chunk
        n = len(buf) // PAYLOAD
        for k in range(n):
            pkt = buf[k*PAYLOAD:(k+1)*PAYLOAD]
            seq = struct.unpack_from("<Q", pkt)[0]
            if seq < exp:
                ooo += 1
            elif seq > exp:
                gaps += seq - exp
            exp = seq + 1
            # cheap 4-point probe
            if (pkt[8] != (seq & 0xFF) or pkt[100] != ((seq + 92) & 0xFF)
                or pkt[800] != ((seq + 792) & 0xFF)
                or pkt[1471] != ((seq + 1463) & 0xFF)):
                corrupt += 1
            elif got % 256 == 0 and pkt != make_payload(seq):
                corrupt += 1
            got += 1
            if seq == NPKT - 1:
                deadline = min(deadline, time.time() + 1)
        buf = buf[n*PAYLOAD:]
    os.close(fd)
    print(f"READER got={got} gaps={gaps} ooo={ooo} corrupt={corrupt} "
          f"tail={len(buf)}", flush=True)
    os._exit(0 if (corrupt == 0 and ooo == 0 and len(buf) == 0) else 1)

# ---- sender parent ----
time.sleep(0.6)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.connect(("127.0.0.1", PORT))
t0 = time.time()
send = s.send
pack = struct.pack
for seq in range(NPKT):
    o = seq & 0xFF
    send(pack("<Q", seq) + BASE[o:o + PAYLOAD - 8])
dt = time.time() - t0
s.close()
print(f"SENDER sent={NPKT} in {dt:.2f}s ({NPKT/dt:.0f} pps, "
      f"{NPKT*PAYLOAD*8/dt/1e9:.3f} Gbps)", flush=True)
_, st = os.waitpid(pid, 0)
sys.exit(os.waitstatus_to_exitcode(st))
