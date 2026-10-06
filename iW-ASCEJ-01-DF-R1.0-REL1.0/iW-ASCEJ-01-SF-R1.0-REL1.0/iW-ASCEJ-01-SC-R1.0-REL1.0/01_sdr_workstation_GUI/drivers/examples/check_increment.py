#!/usr/bin/env python3
import argparse
import os

import numpy as np

parser = argparse.ArgumentParser(description="Check incremental 24-bit counter in 256-bit little-endian .bin file")
parser.add_argument("input_file", help="Path to .bin file")
parser.add_argument("--fast", action="store_true", help="Use fast vectorized mode")
parser.add_argument("--window", type=int, default=5, help="Number of 256-bit words before/after error to print")
args = parser.parse_args()

DATA_WIDTH_BITS = 256
WORD_BYTES = DATA_WIDTH_BITS // 8
COUNTER_BYTE_OFFSET = 0
MAX_VAL = 0xFFFFFF
PROGRESS_STEP = 8 * 1024 * 1024  # 8M 256-bit words
WINDOW = args.window

print(f"Reading file: {args.input_file}")
print(f"Data width: {DATA_WIDTH_BITS} bit ({WORD_BYTES} bytes/word)")

file_size = os.path.getsize(args.input_file)
trailing = file_size % WORD_BYTES
word_count = file_size // WORD_BYTES

if trailing:
    print(f"Warning: ignoring {trailing} trailing bytes that do not make a full 256-bit word")

if word_count == 0:
    counter = np.array([], dtype=np.uint32)
else:
    raw = np.memmap(args.input_file, dtype=np.uint8, mode="r")
    counter_lane = np.ndarray(
        shape=(word_count,),
        dtype="<u4",
        buffer=raw,
        offset=COUNTER_BYTE_OFFSET,
        strides=(WORD_BYTES,),
    )
    counter = counter_lane & MAX_VAL

print(f"Total 256-bit words: {len(counter)}")


def print_context(idx):
    start = max(0, idx - WINDOW)
    end = min(len(counter), idx + WINDOW + 1)

    print(f"\nContext around error @ word {idx}:")
    for i in range(start, end):
        marker = " <-- ERROR" if i == idx else ""
        print(f"[{i:>10}] 0x{counter[i]:06X}{marker}")


if args.fast:
    total = len(counter)
    errors = []

    for start in range(0, total - 1, PROGRESS_STEP):
        end = min(start + PROGRESS_STEP, total - 1)

        expected = (counter[start:end] + 1) & MAX_VAL

        chunk_errors = np.where(counter[start+1:end+1] != expected)[0]

        if len(chunk_errors) > 0:
            errors.extend(chunk_errors + start)

        print(f"Checked up to word {end:,} / {total:,}")

    print(f"Total errors: {len(errors)}")

    if len(errors) > 0:
        print("First few errors with context:")
        for i in errors[:10]:
            print(f"Error @ word {i+1}: prev=0x{counter[i]:06X}, curr=0x{counter[i+1]:06X}")
            print_context(i+1)
    else:
        print("\n✅ Perfect increment with wrap")

else:
    errors = 0
    total = len(counter)

    for i in range(1, total):
        if i % PROGRESS_STEP == 0:
            print(f"Checked {i:,} / {total:,} words...")

        prev = counter[i - 1]
        curr = counter[i]
        expected = 0 if prev == MAX_VAL else prev + 1

        if curr != expected:
            print(f"Error @ word {i}: prev=0x{prev:06X}, curr=0x{curr:06X}, expected=0x{expected:06X}")
            print_context(i)
            errors += 1

    print(f"Total errors: {errors}")

    if errors == 0:
        print("\n✅ Perfect increment with wrap")

 
