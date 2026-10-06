// ---------------------------------------------------------------------------
// PcapExtract — see the header for why RoCEv2 is deliberately NOT handled here.
// ---------------------------------------------------------------------------

#include "PcapExtract.h"

#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <cstring>

namespace sdr {
namespace {

constexpr quint32 kPcapMagicUs    = 0xa1b2c3d4u;  // microsecond, host order
constexpr quint32 kPcapMagicUsSwp = 0xd4c3b2a1u;  // microsecond, swapped
constexpr quint32 kPcapMagicNs    = 0xa1b23c4du;  // nanosecond,  host order
constexpr quint32 kPcapMagicNsSwp = 0x4d3cb2a1u;  // nanosecond,  swapped

constexpr quint32 kLinkEthernet = 1;
constexpr quint32 kLinkRaw      = 101;

inline quint16 swap16(quint16 v) { return quint16((v >> 8) | (v << 8)); }
inline quint32 swap32(quint32 v)
{
    return ((v >> 24) & 0xFFu) | ((v >> 8) & 0xFF00u)
         | ((v << 8) & 0xFF0000u) | ((v << 24) & 0xFF000000u);
}

/// Big-endian reads for network byte order, independent of host endianness.
inline quint16 be16(const quint8* p) { return quint16((p[0] << 8) | p[1]); }

} // namespace

bool extractUdpPayloads(const QString& pcapPath,
                        const QString& binPath,
                        quint16 port,
                        PcapExtractStats& st)
{
    st = PcapExtractStats{};

    QFile in(pcapPath);
    if (!in.open(QIODevice::ReadOnly)) {
        st.error = QObject::tr("Cannot open %1").arg(pcapPath);
        return false;
    }

    QByteArray hdr = in.read(24);
    if (hdr.size() != 24) {
        st.error = QObject::tr("%1 is too short to be a pcap file")
                       .arg(QFileInfo(pcapPath).fileName());
        return false;
    }

    quint32 magic = 0;
    std::memcpy(&magic, hdr.constData(), 4);

    bool swap = false;
    if      (magic == kPcapMagicUs || magic == kPcapMagicNs) swap = false;
    else if (magic == kPcapMagicUsSwp || magic == kPcapMagicNsSwp) swap = true;
    else {
        // pcapng begins with a Section Header Block, not a pcap magic. Say so
        // rather than failing with "unknown format": the difference tells the
        // user exactly what to do.
        quint32 blockType = 0;
        std::memcpy(&blockType, hdr.constData(), 4);
        if (blockType == 0x0A0D0D0Au) {
            st.error = QObject::tr(
                "%1 is a pcapng file. Convert it first:\n\n"
                "    editcap -F pcap in.pcapng out.pcap")
                .arg(QFileInfo(pcapPath).fileName());
        } else {
            st.error = QObject::tr("%1 is not a pcap file (magic 0x%2)")
                           .arg(QFileInfo(pcapPath).fileName())
                           .arg(magic, 8, 16, QLatin1Char('0'));
        }
        return false;
    }

    quint32 linktype = 0;
    std::memcpy(&linktype, hdr.constData() + 20, 4);
    if (swap) linktype = swap32(linktype);

    if (linktype != kLinkEthernet && linktype != kLinkRaw) {
        st.error = QObject::tr("Unsupported pcap link type %1 "
                               "(expected Ethernet or raw IP)").arg(linktype);
        return false;
    }

    QFile out(binPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        st.error = QObject::tr("Cannot write %1").arg(binPath);
        return false;
    }

    QByteArray payloadBuf;
    payloadBuf.reserve(1 << 20);

    for (;;) {
        QByteArray rec = in.read(16);
        if (rec.size() != 16) break;                   // clean end of file

        quint32 caplen = 0, origlen = 0;
        std::memcpy(&caplen,  rec.constData() + 8,  4);
        std::memcpy(&origlen, rec.constData() + 12, 4);
        if (swap) { caplen = swap32(caplen); origlen = swap32(origlen); }

        if (caplen == 0 || caplen > (16u << 20)) {
            st.error = QObject::tr("Record %1 declares an implausible length "
                                   "(%2 bytes); the file is truncated or "
                                   "corrupt.").arg(st.packets + 1).arg(caplen);
            break;
        }

        QByteArray pkt = in.read(caplen);
        if (pkt.size() != int(caplen)) break;           // truncated final record
        st.packets++;

        const quint8* p   = reinterpret_cast<const quint8*>(pkt.constData());
        int           len = pkt.size();
        int           off = 0;

        if (linktype == kLinkEthernet) {
            if (len < 14) continue;
            quint16 eth = be16(p + 12);
            off = 14;
            // 802.1Q / QinQ. Skipping these is four bytes between correct
            // payloads and nonsense, and most switched captures carry them.
            while ((eth == 0x8100 || eth == 0x88A8 || eth == 0x9100)
                   && off + 4 <= len) {
                eth = be16(p + off + 2);
                off += 4;
            }
            if (eth != 0x0800) { st.nonIpv4++; continue; }   // not IPv4
        }

        if (off + 20 > len) continue;
        const quint8* ip = p + off;
        if ((ip[0] >> 4) != 4) { st.nonIpv4++; continue; }

        const int ihl = (ip[0] & 0x0F) * 4;
        if (ihl < 20 || off + ihl > len) continue;

        // A fragment after the first has no UDP header. Concatenating it would
        // splice IP payload bytes into the sample stream, so skip it and say
        // how many were skipped.
        const quint16 fragField = be16(ip + 6);
        const bool moreFrags = (fragField & 0x2000) != 0;
        const quint16 fragOff = fragField & 0x1FFF;
        if (moreFrags || fragOff != 0) { st.fragments++; continue; }

        if (ip[9] != 17) continue;                       // not UDP

        const int udpOff = off + ihl;
        if (udpOff + 8 > len) continue;
        const quint8* udp = p + udpOff;
        st.udpPackets++;

        const quint16 sport = be16(udp + 0);
        const quint16 dport = be16(udp + 2);
        if (port != 0 && dport != port && sport != port) {
            st.portFiltered++;
            continue;
        }

        quint16 ulen = be16(udp + 4);                    // header + payload
        int payloadLen = int(ulen) - 8;
        // Trust the capture over the header: a truncated capture or a NIC that
        // left the FCS on would otherwise make us read past the buffer.
        const int available = len - (udpOff + 8);
        if (payloadLen < 0 || payloadLen > available) payloadLen = available;
        if (payloadLen <= 0) continue;

        payloadBuf.append(reinterpret_cast<const char*>(udp + 8), payloadLen);
        st.accepted++;
        st.bytesWritten += quint64(payloadLen);

        if (payloadBuf.size() >= (1 << 20)) {
            out.write(payloadBuf);
            payloadBuf.clear();
        }
    }

    if (!payloadBuf.isEmpty()) out.write(payloadBuf);
    out.close();

    if (st.accepted == 0 && st.error.isEmpty()) {
        st.error = st.udpPackets > 0
            ? QObject::tr("Found %1 UDP datagrams but none matched port %2.")
                  .arg(st.udpPackets).arg(port)
            : QObject::tr("No IPv4/UDP datagrams found in %1.")
                  .arg(QFileInfo(pcapPath).fileName());
        return false;
    }
    return st.error.isEmpty();
}

QString describe(const PcapExtractStats& s)
{
    QString t = QObject::tr("%1 packets read · %2 UDP · %3 payloads written "
                            "· %4 bytes")
                    .arg(s.packets).arg(s.udpPackets)
                    .arg(s.accepted).arg(s.bytesWritten);
    if (s.fragments)    t += QObject::tr("\n%1 IP fragments skipped (no UDP header)")
                                 .arg(s.fragments);
    if (s.portFiltered) t += QObject::tr("\n%1 UDP datagrams on other ports")
                                 .arg(s.portFiltered);
    if (s.nonIpv4)      t += QObject::tr("\n%1 non-IPv4 frames ignored").arg(s.nonIpv4);
    return t;
}

} // namespace sdr
