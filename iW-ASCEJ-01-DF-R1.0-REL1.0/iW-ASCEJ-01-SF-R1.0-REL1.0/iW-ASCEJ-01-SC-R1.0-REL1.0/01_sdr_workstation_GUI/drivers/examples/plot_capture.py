#!/usr/bin/env python3
import argparse
import os
import sys

import numpy as np


def parse_int(value):
    return int(value, 0)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Plot IWFG capture data from a raw binary file."
    )
    parser.add_argument("input_file", help="Path to capture file, for example capture.bin")
    parser.add_argument(
        "--format",
        choices=("counter24", "u32", "s16", "s24le", "u8"),
        default="counter24",
        help="How to interpret samples (default: counter24)",
    )
    parser.add_argument(
        "--samples",
        type=int,
        default=20000,
        help="Maximum number of samples to plot (default: 20000)",
    )
    parser.add_argument(
        "--offset",
        type=int,
        default=0,
        help="Sample offset to start plotting from (default: 0)",
    )
    parser.add_argument(
        "--output",
        "-o",
        help="Save graph to this image file instead of opening a window",
    )
    parser.add_argument("--title", help="Plot title")
    parser.add_argument(
        "--live",
        action="store_true",
        help="Continuously refresh while the input file grows",
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=0.2,
        help="Live refresh interval in seconds (default: 0.2)",
    )
    parser.add_argument(
        "--follow",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="In live mode, follow the latest samples (default: enabled)",
    )
    parser.add_argument(
        "--mask",
        type=parse_int,
        default=0x00FFFFFF,
        help="Mask used by counter24 format, e.g. 0x00FFFFFF or 0x000FFFFF",
    )
    parser.add_argument(
        "--full-scale",
        action="store_true",
        help="For counter24, keep the Y axis fixed from 0 to the mask value",
    )
    parser.add_argument(
        "--tail-only",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="In live follow mode, read only the plotted tail window (default: enabled)",
    )
    parser.add_argument(
        "--absolute-index",
        action="store_true",
        help="Plot file/global sample indices on X axis instead of 0..N",
    )
    parser.add_argument(
        "--zero-base",
        action="store_true",
        help="For counter24, subtract the first plotted value so the window starts at 0",
    )
    return parser.parse_args()


def sample_size_bytes(sample_format):
    if sample_format in ("counter24", "u32"):
        return 4
    if sample_format == "s16":
        return 2
    if sample_format == "s24le":
        return 3
    if sample_format == "u8":
        return 1
    raise ValueError(f"Unsupported format: {sample_format}")


def decode_samples(raw, sample_format, mask):
    if sample_format in ("counter24", "u32"):
        usable = (raw.size // 4) * 4
        data = raw[:usable].view("<u4")
        if sample_format == "counter24":
            return data & mask
        return data

    if sample_format == "s16":
        usable = (raw.size // 2) * 2
        return raw[:usable].view("<i2")

    if sample_format == "u8":
        return raw

    if sample_format == "s24le":
        usable = (raw.size // 3) * 3
        raw = raw[:usable].reshape(-1, 3).astype(np.int32)
        values = raw[:, 0] | (raw[:, 1] << 8) | (raw[:, 2] << 16)
        sign = values & 0x800000
        return np.where(sign, values | ~0xFFFFFF, values)

    raise ValueError(f"Unsupported format: {sample_format}")


def load_samples(path, sample_format, mask):
    raw = np.fromfile(path, dtype=np.uint8)
    return decode_samples(raw, sample_format, mask)


def load_sample_window(path, sample_format, mask, start_sample, count):
    bytes_per_sample = sample_size_bytes(sample_format)
    byte_offset = start_sample * bytes_per_sample
    byte_count = count * bytes_per_sample

    with open(path, "rb") as f:
        f.seek(byte_offset)
        raw = np.frombuffer(f.read(byte_count), dtype=np.uint8)

    return decode_samples(raw, sample_format, mask)


def total_samples_for_size(file_size, sample_format):
    return file_size // sample_size_bytes(sample_format)


def get_plot_window(args, samples):
    if samples.size == 0:
        return None, None

    if args.live and args.follow:
        start = max(samples.size - args.samples, 0)
    else:
        start = max(args.offset, 0)

    end = min(start + args.samples, samples.size)
    if start >= samples.size:
        return None, None

    if args.absolute_index:
        x = np.arange(start, end)
    else:
        x = np.arange(0, end - start)

    y = samples[start:end]
    return x, normalize_samples(args, y)


def normalize_samples(args, samples):
    if args.format != "counter24" or not args.zero_base or samples.size == 0:
        return samples

    modulo = args.mask + 1
    return (samples.astype(np.uint64) - int(samples[0])) % modulo


def import_pyplot():
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print(
            "matplotlib is required for plotting. Install it with: "
            "python3 -m pip install matplotlib",
            file=sys.stderr,
        )
        return None

    return plt


def plot_once(args, plt):
    try:
        samples = load_samples(args.input_file, args.format, args.mask)
    except OSError as exc:
        print(f"Failed to read {args.input_file}: {exc}", file=sys.stderr)
        return 1

    x, y = get_plot_window(args, samples)
    if x is None:
        print(f"No plottable samples found in {args.input_file}.", file=sys.stderr)
        return 1

    plt.figure(figsize=(12, 5))
    plt.plot(x, y, linewidth=0.8)
    plt.xlabel("Sample index" if args.absolute_index else "Window sample")
    plt.ylabel(args.format)
    plt.title(args.title or f"{args.input_file} ({args.format})")
    plt.grid(True, alpha=0.3)
    if args.format == "counter24" and args.full_scale:
        plt.ylim(0, args.mask)
    plt.tight_layout()

    if args.output:
        plt.savefig(args.output, dpi=150)
        print(f"Saved plot to {args.output}")
    else:
        plt.show()

    return 0


def plot_live(args, plt):
    plt.ion()
    fig, ax = plt.subplots(figsize=(12, 5))
    (line,) = ax.plot([], [], linewidth=0.8)
    ax.set_xlabel("Sample index" if args.absolute_index else "Window sample")
    ax.set_ylabel(args.format)
    ax.set_title(args.title or f"{args.input_file} ({args.format}, live)")
    ax.grid(True, alpha=0.3)
    if args.format == "counter24" and args.full_scale:
        ax.set_ylim(0, args.mask)
    fig.tight_layout()

    last_size = -1
    waiting_printed = False

    while plt.fignum_exists(fig.number):
        try:
            file_size = os.path.getsize(args.input_file)
        except OSError:
            file_size = 0

        if file_size == 0:
            if not waiting_printed:
                print(f"Waiting for data in {args.input_file}...")
                waiting_printed = True
            plt.pause(args.interval)
            continue

        if file_size != last_size:
            try:
                total_samples = total_samples_for_size(file_size, args.format)
                if args.follow and args.tail_only:
                    start = max(total_samples - args.samples, 0)
                    samples = load_sample_window(
                        args.input_file,
                        args.format,
                        args.mask,
                        start,
                        args.samples,
                    )
                    if args.absolute_index:
                        x = np.arange(start, start + samples.size)
                    else:
                        x = np.arange(samples.size)
                    y = normalize_samples(args, samples)
                else:
                    samples = load_samples(args.input_file, args.format, args.mask)
                    total_samples = samples.size
                    x, y = get_plot_window(args, samples)
            except OSError as exc:
                print(f"Failed to read {args.input_file}: {exc}", file=sys.stderr)
                plt.pause(args.interval)
                continue

            if x is not None:
                line.set_data(x, y)
                if args.format == "counter24" and args.full_scale:
                    ax.set_xlim(x[0], x[-1] if x.size > 1 else x[0] + 1)
                    ax.set_ylim(0, args.mask)
                else:
                    ax.relim()
                    ax.autoscale_view()
                ax.set_title(
                    args.title
                    or f"{args.input_file} ({args.format}, {total_samples} samples)"
                )
                fig.canvas.draw_idle()
                last_size = file_size

        plt.pause(args.interval)

    return 0


def main():
    args = parse_args()

    if args.output and args.live:
        print("--output cannot be used with --live", file=sys.stderr)
        return 1

    plt = import_pyplot()
    if plt is None:
        return 1

    if args.live:
        return plot_live(args, plt)

    return plot_once(args, plt)


if __name__ == "__main__":
    raise SystemExit(main())

