#!/usr/bin/env python3
"""Fault-injection tests for iwfg_c2h.

t1: interleave wrong-size datagrams; good stream must remain intact.
t2: reader disconnects mid-stream and a new reader attaches; new reader
    must start exactly on a 1472-byte packet boundary.
"""
import os, socket, struct, sys, time

FIFO, PORT, PAYLOAD = "/tmp/iwfg_c2h.fifo", 50000, 1472
BASE = bytes((i & 0xFF) for i in range(PAYLOAD - 8 + 256))

def pkt(seq):
    o = seq & 0xFF
    return struct.pack("<Q", seq) + BASE[o:o + PAYLOAD - 8]

def check(p):
    seq = struct.unpack_from("<Q", p)[0]
    o = seq & 0xFF
    return p[8:] == BASE[o:o + PAYLOAD - 8], seq

test = sys.argv[1]

if test == "t1":                       # ---- bad sizes interleaved ----
    N = 5000
    pid = os.fork()
    if pid == 0:
        fd = os.open(FIFO, os.O_RDONLY)
        buf, got, bad, ooo, last = b"", 0, 0, 0, -1
        end = time.time() + 15
        while got < N and time.time() < end:
            c = os.read(fd, 1 << 20)
            if not c: break
            buf += c
            while len(buf) >= PAYLOAD:
                p, buf = buf[:PAYLOAD], buf[PAYLOAD:]
                okp, seq = check(p)
                if not okp: bad += 1
                if seq <= last: ooo += 1
                last = seq; got += 1
        print(f"t1 READER got={got} bad={bad} ooo={ooo} tail={len(buf)}",
              flush=True)
        os._exit(0 if (got == N and bad == 0 and ooo == 0 and not buf)
                 else 1)
    time.sleep(0.5)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect(("127.0.0.1", PORT))
    junk_small = b"x" * 100
    junk_big   = b"y" * 3000        # > payload, will be truncated+flagged
    for seq in range(N):
        s.send(pkt(seq))
        if seq % 7 == 0:  s.send(junk_small)
        if seq % 11 == 0: s.send(junk_big)
    s.close()
    _, st = os.waitpid(pid, 0)
    sys.exit(os.waitstatus_to_exitcode(st))

if test == "t2":                       # ---- reader reconnect ----
    N1, N2 = 3000, 3000
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect(("127.0.0.1", PORT))

    fd = os.open(FIFO, os.O_RDONLY)
    for seq in range(N1):
        s.send(pkt(seq))
    got = 0
    while got < (N1 // 2) * PAYLOAD:          # read only half, then vanish
        got += len(os.read(fd, 65536))
    os.close(fd)                               # reader disconnect
    time.sleep(1.0)

    fd = os.open(FIFO, os.O_RDONLY)            # new reader
    time.sleep(0.3)
    for seq in range(N1, N1 + N2):
        s.send(pkt(seq))
    s.close()
    buf, first_ok, cnt, bad = b"", None, 0, 0
    end = time.time() + 15
    while cnt < N2 // 2 and time.time() < end:
        c = os.read(fd, 1 << 20)
        if not c: break
        buf += c
        while len(buf) >= PAYLOAD:
            p, buf = buf[:PAYLOAD], buf[PAYLOAD:]
            okp, seq = check(p)
            if first_ok is None:
                first_ok = okp
                print(f"t2 first packet after reconnect: seq={seq} "
                      f"aligned={okp}", flush=True)
            if not okp: bad += 1
            cnt += 1
    os.close(fd)
    print(f"t2 READER got={cnt} bad={bad}", flush=True)
    sys.exit(0 if (first_ok and bad == 0 and cnt > 0) else 1)
