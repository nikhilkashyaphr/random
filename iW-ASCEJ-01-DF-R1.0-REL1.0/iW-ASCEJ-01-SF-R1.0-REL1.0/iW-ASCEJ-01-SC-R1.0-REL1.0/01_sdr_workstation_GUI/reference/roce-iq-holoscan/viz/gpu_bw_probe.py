#!/usr/bin/env python3
"""
gpu_bw_probe.py -- measure how much GPU memory bandwidth is left while the
ingest pipeline runs. Start it alongside a scale or GPUDirect test:

    python viz/gpu_bw_probe.py --secs 30

It hammers device-to-device copies and reports achieved GB/s once a second.
Compare against the RTX A4000's theoretical ~448 GB/s: ingest at the PCIe
ceiling (~6.8 GB/s) plus FFT/render traffic should leave the probe within a
few percent of an idle run -- that is your proof that GPU memory bandwidth
is nowhere near being the bottleneck. A big drop means something else
(usually a runaway kernel) is eating the bus.
"""
import argparse
import time

import cupy as cp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--secs", type=float, default=20.0)
    ap.add_argument("--mb", type=int, default=256, help="buffer size, MiB")
    args = ap.parse_args()

    n = args.mb * 1024 * 1024
    a = cp.random.bytes(n)
    src = cp.frombuffer(a, dtype=cp.uint8).copy()
    dst = cp.empty_like(src)

    name = cp.cuda.runtime.getDeviceProperties(0)["name"].decode()
    print(f"[bw ] device: {name}, buffer {args.mb} MiB, "
          f"{args.secs:.0f} s run")

    t_end = time.monotonic() + args.secs
    while time.monotonic() < t_end:
        t0 = time.monotonic()
        moved = 0
        while time.monotonic() - t0 < 1.0:
            cp.copyto(dst, src)        # read n + write n
            moved += 2 * n
        cp.cuda.runtime.deviceSynchronize()
        dt = time.monotonic() - t0
        print(f"[bw ] {moved / dt / 1e9:7.1f} GB/s device-to-device")


if __name__ == "__main__":
    main()
