# detect_format — what wire format is this capture?

```sh
gcc -O2 -o detect_format detect_format.c -lm
./detect_format out_s1.bin          # single channel
./detect_format out_s1.bin 4        # four interleaved channels
```

The GUI does this automatically when you open a file; this is the same
measurement as a standalone tool, for scripting or for checking a file without
launching the GUI.

## How it decides

Decoded with the **correct** format a capture has spectral structure — energy
concentrates somewhere in the band. Decoded **wrongly**, the bytes are
re-partitioned into samples that mean nothing and the result approaches white
noise. Spectral flatness (geometric mean ÷ arithmetic mean of the power
spectrum) separates the two: more negative dB = more structure = more likely
correct.

It also reports the 14-bit fingerprint: if ≥95 % of int16 samples have
`bits[1:0] == 00`, the data is MSB-aligned 14-bit RFSoC packing and full scale
is `8191<<2 = 32764`, not 32767.

## A note on method

The first version of this tool tested "are the top 16 bits of each int32 just
sign extension". That gets `Complex Int 16` **wrong**: read as int32, the high
half is the *next sample*, not sign extension. Spectral flatness has no such
blind spot, and was validated against known-format reference files before being
used here.
