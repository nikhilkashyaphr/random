// ---------------------------------------------------------------------------
// PcapExtract — pull application payloads out of a .pcap.
//
// TWO ENCAPSULATIONS, TWO OWNERS
// ------------------------------
//   RoCEv2 : Ethernet / IPv4 / UDP:4791 / BTH [/ RETH] / payload / ICRC
//            Handled by roce-extractor, NOT here. It already does PSN
//            ordering, deduplication, loss detection and ICRC validation.
//            Re-implementing that in the GUI would create a second parser to
//            keep in step with the first, which is how the two quietly diverge.
//
//   UDP    : Ethernet / IPv4 / UDP / payload
//            Handled here, because nothing else does it offline. iwfg_c2h
//            strips UDP headers on the LIVE path only; there is no tool that
//            turns a stored .pcap into raw UDP payloads.
//
// So this file is deliberately the simple half. If it ever grows RoCEv2
// parsing, that is the signal it has become the wrong place for the work.
//
// WHAT IT HANDLES, AND WHY EACH MATTERS
//   * pcap magic in both byte orders, and both microsecond and nanosecond
//     variants — a capture written on a different-endian host is otherwise
//     silently read as garbage
//   * LINKTYPE_EN10MB and LINKTYPE_RAW
//   * 802.1Q and QinQ VLAN tags — present on most switched captures, and
//     skipping them is four bytes between working and nonsense
//   * IPv4 header length from IHL, not assumed to be 20 bytes
//   * fragmented datagrams are SKIPPED, not concatenated: a fragment has no
//     UDP header, so treating it as one would inject header bytes into the
//     sample stream
//   * optional source/destination port filter
//
// It does not reorder or deduplicate. UDP offers no sequence number to do it
// with, so pretending otherwise would be a lie about the data.
// ---------------------------------------------------------------------------

#pragma once

#include <QString>
#include <QObject>
#include <cstdint>

namespace sdr {

struct PcapExtractStats {
    quint64 packets      = 0;   ///< records read from the file
    quint64 udpPackets   = 0;   ///< IPv4/UDP datagrams seen
    quint64 accepted     = 0;   ///< payloads written
    quint64 bytesWritten = 0;
    quint64 fragments    = 0;   ///< skipped: no UDP header to trust
    quint64 nonIpv4      = 0;   ///< ARP, IPv6, LLDP and similar
    quint64 portFiltered = 0;   ///< UDP, but not the requested port
    QString error;              ///< empty on success
};

/// Extract UDP payloads from `pcapPath` into `binPath`.
/// `port` filters on destination OR source port; 0 accepts every UDP datagram.
/// Returns false with `stats.error` set.
bool extractUdpPayloads(const QString& pcapPath,
                        const QString& binPath,
                        quint16 port,
                        PcapExtractStats& stats);

/// Human-readable summary, for the dialog and the log.
QString describe(const PcapExtractStats& s);

} // namespace sdr
