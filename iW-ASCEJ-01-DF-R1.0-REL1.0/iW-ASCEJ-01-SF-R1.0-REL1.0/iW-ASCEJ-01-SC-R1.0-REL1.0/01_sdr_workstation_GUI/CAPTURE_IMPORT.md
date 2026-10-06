# Capture import — .pcap, UDP and RoCEv2

## What changed

The Browse dialog only advertised `.bin / .dat / .iq / .cf32`, and the
placeholder said the same, so the file field looked like it accepted nothing
else. It now accepts `.pcap` and extracts it.

```
capture.bin / .iq / .cf32  —  or a .pcap, extracted automatically
```

## Why a .pcap cannot be plotted directly

Every payload in a capture is wrapped:

```
RoCEv2   Ethernet │ IPv4 │ UDP:4791 │ BTH │ RETH │ payload │ ICRC
UDP      Ethernet │ IPv4 │ UDP      │ payload
```

Feeding that to the plotter shows header bytes as samples. The headers have to
come off first, and the two encapsulations need different treatment.

## Two encapsulations, two owners

On selecting a `.pcap` the dialog asks which it is, defaulting from the
Transport selector:

| Encapsulation | Handled by | Why |
|---|---|---|
| **RoCEv2** | `roce-extractor` (external) | It already does PSN ordering, deduplication, loss detection and ICRC validation. Re-implementing that in the GUI would create a second parser to keep in step with the first — which is how two parsers quietly diverge. |
| **UDP** | in-process (`PcapExtract`) | Nothing else does it offline. `iwfg_c2h` strips UDP headers on the **live** path only; no tool turns a stored `.pcap` into raw UDP payloads. |

The question is asked rather than inferred because a capture's encapsulation is
a property of the **file**, not of how this session happens to be configured.

## The UDP extractor

Deliberately the simple half. If it ever grows RoCEv2 parsing, that is the
signal it has become the wrong place for the work.

Handled, and each for a reason:

| | Why it matters |
|---|---|
| pcap magic in **both byte orders**, µs and ns variants | a capture written on a different-endian host is otherwise read as garbage |
| `LINKTYPE_EN10MB` and `LINKTYPE_RAW` | |
| **802.1Q / QinQ VLAN tags** | present on most switched captures; four bytes between correct payloads and nonsense |
| IPv4 header length from **IHL** | not assumed to be 20 bytes |
| **IP fragments skipped** | a fragment has no UDP header; concatenating it would splice IP payload bytes into the sample stream |
| UDP length clamped to captured bytes | a truncated capture, or a NIC that left the FCS on, would otherwise read past the buffer |
| optional source/destination port filter | |
| **pcapng detected by name** | reports `editcap -F pcap` rather than failing with "unknown format" |

It does **not** reorder or deduplicate. UDP carries no sequence number to do it
with, so pretending otherwise would misrepresent the data. RoCEv2 does carry a
PSN, which is exactly why that path belongs to `roce-extractor`.

## Verified against synthetic captures

64 tone frames of 1024 B, **half 802.1Q-tagged**, plus three decoys: a UDP
datagram on the wrong port, an ARP frame, and an IP fragment.

```
67 packets read · 65 UDP · 64 payloads written · 65536 bytes
1 IP fragments skipped (no UDP header)
1 UDP datagrams on other ports
1 non-IPv4 frames ignored
```

| Check | Result |
|---|---|
| payload bytes = 64 × 1024 | **65536 — exact** |
| VLAN-tagged frames recovered | PASS |
| fragment / ARP / wrong port rejected | PASS |
| little-endian vs big-endian pcap | **byte-identical output** |
| extracted payload re-analysed | recovers **Complex Int 16**, 14-bit MSB-aligned |

That last row is the one that matters: the bytes go in as a pcap and come out
as the tone that was put in.

## Flow

```
Browse… ─ .bin ──────────────────────────────► plot
        └ .pcap ─ ask encapsulation
                   ├ RoCEv2 ─ roce-extractor ─► .bin ─► detect format ─► plot
                   └ UDP ─── PcapExtract ─────► _udp.bin ─► detect format ─► plot
```

Format detection runs on the result either way, so the wire format is measured
rather than guessed.

## Files

```
src/core/PcapExtract.h     new
src/core/PcapExtract.cpp   new
src/ui/ConfigDialog.cpp    encapsulation prompt, UDP path, placeholder/filter
src/ui/ConfigDialog.h
CMakeLists.txt             the two new sources
```
