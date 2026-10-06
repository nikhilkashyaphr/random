#!/usr/bin/env python3
"""Validate --raw mode against frames a normal UDP socket cannot receive:
zero destination MAC + deliberately WRONG IP header checksum (the exact
fault pattern of FPGA/hardware packet generators).  Sends sequenced
1472-byte payloads over loopback via AF_PACKET, reads the FIFO back and
checks byte-exact framing, ordering and content."""
import os, socket, struct, sys, time

FIFO    = sys.argv[1] if len(sys.argv) > 1 else "/tmp/iwfg_c2h.fifo"
PORT    = int(sys.argv[2]) if len(sys.argv) > 2 else 16384
COUNT   = int(sys.argv[3]) if len(sys.argv) > 3 else 20000
PAYLOAD = 1472
IF      = "lo"

def build_frame(seq: int) -> bytes:
    eth = b"\x00" * 6 + b"\xca\x06\x05\x04\x03\x20" + b"\x08\x00"  # dst MAC zero!
    total = 20 + 8 + PAYLOAD
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, total, 0, 0x4000, 64, 17,
                     0xDEAD,                     # deliberately WRONG checksum
                     socket.inet_aton("192.168.1.0"),   # network addr src, like FPGA
                     socket.inet_aton("127.0.0.1"))
    udp = struct.pack("!HHHH", 20480, PORT, 8 + PAYLOAD, 0)  # csum 0 = none
    base = bytes((seq + i) & 0xFF for i in range(PAYLOAD - 8))
    pay = struct.pack("!Q", seq) + base
    return eth + ip + udp + pay

def sender(n: int):
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
    s.bind((IF, 0))
    t0 = time.time()
    for seq in range(n):
        s.send(build_frame(seq))
        if seq % 512 == 0:
            time.sleep(0.001)          # let lo soft-irq keep up
    dt = time.time() - t0
    print(f"SENDER sent={n} in {dt:.2f}s ({n/dt:.0f} pps)", flush=True)

def reader(n: int) -> int:
    fd = os.open(FIFO, os.O_RDONLY)
    got = gaps = corrupt = 0
    expect = None
    buf = b""
    deadline = time.time() + 60
    while got < n and time.time() < deadline:
        chunk = os.read(fd, 1 << 20)
        if not chunk:
            break
        buf += chunk
        while len(buf) >= PAYLOAD:
            pkt, buf = buf[:PAYLOAD], buf[PAYLOAD:]
            seq = struct.unpack("!Q", pkt[:8])[0]
            if expect is not None and seq != expect:
                gaps += 1
            expect = seq + 1
            if pkt[8:12] != bytes(((seq + i) & 0xFF) for i in range(4)):
                corrupt += 1
            got += 1
    os.close(fd)
    tail = len(buf)
    print(f"READER got={got} gaps={gaps} corrupt={corrupt} tail={tail}",
          flush=True)
    return 0 if (got == n and gaps == 0 and corrupt == 0 and tail == 0) else 1

if __name__ == "__main__":
    pid = os.fork()
    if pid == 0:
        time.sleep(0.3)
        sender(COUNT)
        os._exit(0)
    rc = reader(COUNT)
    os.waitpid(pid, 0)
    sys.exit(rc)
