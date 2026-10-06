#!/usr/bin/env python3
"""Generate a synthetic capture file for the workstation's offline mode.

Produces the same wire layout the application expects:
    ch0.I ch0.Q ch1.I ch1.Q ch0.I ch0.Q ...
and writes a matching `.meta` sidecar so the file reopens with the rate and
format it was created with.

    ./make_test_capture.py capture.bin --channels 2 --format cs16 \
        --rate 122.88 --decimation 8 --seconds 2 --modulation qam16
"""

import argparse
import datetime
import numpy as np

FORMATS = {
    "cs8":  (np.int8,    127.0),
    "cu8":  (np.uint8,   127.0),
    "cs16": (np.int16,   32767.0),
    "cs32": (np.int32,   2147483647.0),
    "cf32": (np.float32, 1.0),
    "cf64": (np.float64, 1.0),
}

# GNU Radio-style tags, matching sdr::formatTag() so the app can restore the
# wire format from the sidecar.
FORMAT_LABEL = {
    "cs8":  "sc8",
    "cu8":  "uc8",
    "cs16": "sc16",
    "cs32": "sc32",
    "cf32": "fc32",
    "cf64": "fc64",
}

CONSTELLATIONS = {
    "qpsk":  np.array([-1, 1]) / np.sqrt(2),
    "qam16": np.array([-3, -1, 1, 3]) / np.sqrt(10),
    "qam64": np.array([-7, -5, -3, -1, 1, 3, 5, 7]) / np.sqrt(42),
}


def make_channel(n, fs, offset_hz, snr_db, modulation, sps, rng):
    """One channel of pulse-shaped, burst-gated, offset, noisy symbols."""
    if modulation == "noise":
        symbols = np.zeros(n // sps + 2, dtype=complex)
    else:
        levels = CONSTELLATIONS[modulation]
        count = n // sps + 2
        symbols = (rng.choice(levels, count) + 1j * rng.choice(levels, count))

    # Raised-cosine interpolation between symbols — a cheap stand-in for a
    # real RRC filter, matching what the built-in simulator does.
    idx = np.arange(n)
    k = idx // sps
    frac = (idx % sps) / sps
    w = 0.5 * (1.0 - np.cos(np.pi * frac))
    sig = symbols[k] * (1 - w) + symbols[np.minimum(k + 1, len(symbols) - 1)] * w

    # Burst envelope, so the time-domain plot has structure to look at.
    burst_period = max(1, n // 6)
    phase = (idx % burst_period) / burst_period
    env = np.where((phase > 0.42) & (phase < 0.68), 1.0, 0.35)
    edge = 0.02
    ramp_up = np.clip((phase - 0.42) / edge, 0, 1)
    ramp_dn = np.clip((0.68 - phase) / edge, 0, 1)
    env = 0.35 + 0.65 * np.clip(np.minimum(ramp_up, ramp_dn), 0, 1)
    sig = sig * env

    # Carrier offset — the receiver's NCO is what removes this.
    sig = sig * np.exp(2j * np.pi * offset_hz / fs * idx)

    noise_rms = 0.5 * 10 ** (-snr_db / 20.0)
    sig = sig + noise_rms * (rng.standard_normal(n) + 1j * rng.standard_normal(n))
    sig = sig + complex(-0.009, 0.004)          # small DC offset
    return sig


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("output")
    ap.add_argument("--channels", type=int, default=2)
    ap.add_argument("--format", choices=sorted(FORMATS), default="cf32")
    ap.add_argument("--rate", type=float, default=122.88, help="MSPS")
    ap.add_argument("--decimation", type=int, default=8)
    ap.add_argument("--center", type=float, default=2.450, help="GHz")
    ap.add_argument("--offset", type=float, default=1.2288,
                    help="carrier offset in MHz, at the decimated rate")
    ap.add_argument("--seconds", type=float, default=2.0)
    ap.add_argument("--snr", type=float, default=25.0)
    ap.add_argument("--modulation", choices=list(CONSTELLATIONS) + ["noise"],
                    default="qam16")
    ap.add_argument("--sps", type=int, default=4)
    ap.add_argument("--seed", type=int, default=0xC0FFEE)
    args = ap.parse_args()

    dtype, scale = FORMATS[args.format]
    rng = np.random.default_rng(args.seed)

    # The file is written at the decimated rate the display works at, so the
    # capture lines up with what the DDC produces live.
    fs = args.rate * 1e6 / max(1, args.decimation)
    n = int(fs * args.seconds)

    print(f"generating {args.channels} ch x {n} samples "
          f"({args.seconds} s at {fs/1e6:.4f} MSPS) as {args.format}")

    chans = [
        make_channel(n, fs, args.offset * 1e6 * (1 + 0.35 * c),
                     args.snr, args.modulation, args.sps, rng) * (1 - 0.18 * c)
        for c in range(args.channels)
    ]

    # Interleave: ch0.I ch0.Q ch1.I ch1.Q ...
    inter = np.empty(n * args.channels * 2, dtype=np.float64)
    for c, sig in enumerate(chans):
        inter[c * 2 + 0::args.channels * 2] = sig.real
        inter[c * 2 + 1::args.channels * 2] = sig.imag

    if np.issubdtype(dtype, np.integer):
        inter = np.clip(inter, -1.0, 1.0) * scale
        if args.format == "cu8":
            inter = inter + 128.0
        inter = np.rint(inter)

    inter.astype(dtype).tofile(args.output)

    meta = args.output + ".meta"
    with open(meta, "w") as f:
        f.write("# SDR workstation capture metadata\n")
        f.write(f"created={datetime.datetime.now().isoformat(timespec='seconds')}\n")
        f.write(f"data_file={args.output.split('/')[-1]}\n")
        f.write(f"format={FORMAT_LABEL[args.format]}\n")
        f.write(f"channels={args.channels}\n")
        f.write(f"sample_rate_msps={args.rate:.6f}\n")
        f.write(f"center_freq_ghz={args.center:.9f}\n")
        f.write(f"nco_freq_mhz={-args.offset:.6f}\n")
        f.write(f"decimation={args.decimation}\n")
        f.write("interpolation=1\n")
        f.write("full_scale=1.000000\n")
        f.write("header_bytes=0\n")
        f.write("layout=interleaved_iq_then_channel\n")

    import os
    print(f"wrote {args.output} ({os.path.getsize(args.output)/1e6:.1f} MB) and {meta}")


if __name__ == "__main__":
    main()
