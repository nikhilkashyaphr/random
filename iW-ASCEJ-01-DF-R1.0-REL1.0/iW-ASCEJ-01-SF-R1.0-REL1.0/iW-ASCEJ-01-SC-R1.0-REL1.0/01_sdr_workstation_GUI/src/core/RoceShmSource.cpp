#include "RoceShmSource.h"

#ifdef SDR_ENABLE_CUDA
#include <cuda_runtime.h>
#endif

#include <QFile>

#include <QFileInfo>
#include <QTimer>

#include <cstring>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sdr {

namespace {
// Field offsets are taken from rdma_common.h's _Static_assert set, so a
// producer-side ABI change breaks their build before it can silently
// mis-parse here.
constexpr int kOffCtrlMagic        = 0;
constexpr int kOffCtrlNumSlots     = 12;
constexpr int kOffCtrlSlotStride   = 16;
constexpr int kOffCtrlFrameSamples = 20;
constexpr int kOffCtrlSampleRate   = 24;
constexpr int kOffCtrlCenterFreq   = 32;
constexpr int kOffCtrlWriteCount   = 40;
constexpr int kOffCtrlBytesTotal   = 48;
constexpr int kOffCtrlSeqGaps      = 56;

// --- roce-extractor tap ABI (roce/tap.h), byte offsets --------------------
// tap_hdr_t: magic, version, slot_bytes, nslots, decim, channels, dtype,
//            _pad0, fs_hz, center_hz, full_scale, produced, pkt_seen,
//            missing, running, _pad1, source[128]
constexpr int kOffTapMagic      = 0;
constexpr int kOffTapVersion    = 4;
constexpr int kOffTapSlotBytes  = 8;
constexpr int kOffTapNSlots     = 12;
constexpr int kOffTapDecim      = 16;
constexpr int kOffTapChannels   = 20;
constexpr int kOffTapDType      = 24;
constexpr int kOffTapFsHz       = 32;   // double, 8-aligned after _pad0
constexpr int kOffTapCenterHz   = 40;
constexpr int kOffTapFullScale  = 48;
constexpr int kOffTapProduced   = 56;
constexpr int kOffTapPktSeen    = 64;
constexpr int kOffTapMissing    = 72;
constexpr int kOffTapRunning    = 80;
constexpr int kOffTapSource     = 88;

// tap_meta_t: seq, psn, ts_ns, len, flags, pad -> 64 B
constexpr int kOffMetaSeq   = 0;
constexpr int kOffMetaPsn   = 8;
constexpr int kOffMetaTsNs  = 16;
constexpr int kOffMetaLen   = 24;
constexpr int kOffMetaFlags = 28;

constexpr int kOffHdrMagic     = 0;
constexpr int kOffHdrSeq       = 8;
constexpr int kOffHdrNSamples  = 24;
constexpr int kOffHdrSampleRate= 32;
constexpr int kOffHdrCenterFreq= 40;

template <typename T>
T peek(const void* base, int off)
{
    T v{};
    std::memcpy(&v, static_cast<const char*>(base) + off, sizeof(T));
    return v;
}
} // namespace

RoceShmSource::RoceShmSource(QObject* parent)
    : ISignalSource(parent)
{
    // Prefer whichever ring actually exists. Two producers publish RoCEv2
    // payloads and the operator should not have to know which one is running:
    //   /dev/shm/iqring    rdma_rx / rdma_rx_gpu   (IQRING01)
    //   /dev/shm/roce_tap  roce-extractor          (RTAP)
    // If neither is present, keep the historical default so the error message
    // names the path people expect.
    m_path = resolvePath();
}

// The configured path is authoritative when it is set. Ignoring it meant
// `--roce /dev/shm/other` read its rate and centre from the named ring (main.cpp
// calls peekRingInfo on it) and then streamed samples from /dev/shm/iqring --
// one ring's axes over another ring's data when both existed, and a confusing
// "cannot open /dev/shm/iqring" when only the named one did.
QString RoceShmSource::resolvePath() const
{
    const QString configured = m_cfg.src.devicePath.trimmed();
    if (!configured.isEmpty()) return configured;

    const QString iq  = QStringLiteral("/dev/shm/iqring");
    const QString tap = QStringLiteral("/dev/shm/roce_tap");
    if (QFile::exists(iq))       return iq;
    if (QFile::exists(tap))      return tap;
    return iq;   // keep the historical default so the error names the expected path
}

RoceShmSource::~RoceShmSource() { unmapRing(); }

QString RoceShmSource::name() const
{
    if (m_abi == RingAbi::RoceTap)
        return QStringLiteral("RoCEv2 — extractor tap (%1)").arg(m_path);
    return m_gpuRing ? QStringLiteral("RoCEv2 — GPU ring (%1)").arg(m_path)
                     : QStringLiteral("RoCEv2 — host ring (%1)").arg(m_path);
}

bool RoceShmSource::peekRingInfo(const QString& path,
                                 double* rateMsps, double* centerGHz)
{
#ifndef Q_OS_UNIX
    Q_UNUSED(path); Q_UNUSED(rateMsps); Q_UNUSED(centerGHz);
    return false;
#else
    const QByteArray p = path.toLocal8Bit();
    const int fd = ::open(p.constData(), O_RDONLY);
    if (fd < 0) return false;
    char ctrl[kCtrlBytes];
    const ssize_t got = ::read(fd, ctrl, sizeof ctrl);
    ::close(fd);
    if (got < static_cast<ssize_t>(sizeof ctrl)) return false;

    // RTAP first: its magic is 32-bit, so a 64-bit read at offset 0 would
    // match neither constant and the tap would look like a corrupt IQRING.
    if (peek<quint32>(ctrl, kOffTapMagic) == kTapMagic) {
        const double tfs = peek<double>(ctrl, kOffTapFsHz);
        const double tfc = peek<double>(ctrl, kOffTapCenterHz);
        if (rateMsps  && tfs > 0.0) *rateMsps  = tfs / 1e6;
        if (centerGHz)              *centerGHz = tfc / 1e9;
        return tfs > 0.0;
    }

    const quint64 magic = peek<quint64>(ctrl, kOffCtrlMagic);
    if (magic != kRingMagicHost && magic != kRingMagicGpu) return false;

    const quint64 fs = peek<quint64>(ctrl, kOffCtrlSampleRate);
    const qint64  fc = peek<qint64>(ctrl, kOffCtrlCenterFreq);
    if (rateMsps  && fs > 0) *rateMsps  = static_cast<double>(fs) / 1e6;
    if (centerGHz)           *centerGHz = static_cast<double>(fc) / 1e9;
    return fs > 0;
#endif
}

bool RoceShmSource::mapRing(QString* err)
{
#ifndef Q_OS_UNIX
    if (err) *err = QStringLiteral("shared-memory rings require POSIX");
    return false;
#else
    const QByteArray p = m_path.toLocal8Bit();
    m_fd = ::open(p.constData(), O_RDONLY);
    if (m_fd < 0) {
        if (err) *err = QStringLiteral(
            "cannot open %1 — is rdma_rx (or rdma_rx_gpu) running? [%2]")
            .arg(m_path, QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    struct stat sb {};
    if (::fstat(m_fd, &sb) != 0 || sb.st_size < kCtrlBytes) {
        if (err) *err = QStringLiteral("%1 is too small to be a ring").arg(m_path);
        ::close(m_fd); m_fd = -1;
        return false;
    }
    m_bytes = static_cast<size_t>(sb.st_size);
    m_base = ::mmap(nullptr, m_bytes, PROT_READ, MAP_SHARED, m_fd, 0);
    if (m_base == MAP_FAILED) {
        m_base = nullptr;
        if (err) *err = QStringLiteral("mmap failed: %1")
                            .arg(QString::fromLocal8Bit(strerror(errno)));
        ::close(m_fd); m_fd = -1;
        return false;
    }

    // Decide which producer this is ONCE, from the magic word. The tap magic
    // is a 32-bit "RTAP" at offset 0; the IQRING magics are 64-bit. Test the
    // 32-bit one first, because its low word would otherwise be read as part
    // of a 64-bit value that matches neither.
    if (peek<quint32>(m_base, kOffTapMagic) == kTapMagic) {
        m_abi = RingAbi::RoceTap;
        return mapRoceTap(err);
    }
    m_abi = RingAbi::IqRing;

    const quint64 magic = peek<quint64>(m_base, kOffCtrlMagic);
    if (magic == kRingMagicGpu) {
        m_gpuRing = true;
#ifdef SDR_ENABLE_CUDA
        // GPUDirect: the NIC DMA'd the payload straight into VRAM (peer to
        // peer, NIC -> GPU, no system RAM). The host mapping holds only the
        // control block and headers; the payload is reached through the CUDA
        // IPC handle rdma_rx_gpu published at offset 1024.
        if (!openGpuRing(err)) { unmapRing(); return false; }
        m_slots      = peek<quint32>(m_base, kOffCtrlNumSlots);
        m_slotStride = peek<quint32>(m_base, kOffCtrlSlotStride);
        m_cfg.src.format = SampleFormat::Cs16;   // int16 I/Q, as the host ring
        m_lastWriteCount = peek<quint64>(m_base, kOffCtrlWriteCount);
        return true;
#else
        if (err) *err = QStringLiteral(
            "%1 is a GPUDirect ring (IQRINGG1): payloads live in GPU VRAM. "
            "This build has no CUDA, so it cannot open them. Rebuild with "
            "-DSDR_ENABLE_CUDA=ON, or run rdma_rx (host ring) instead.")
            .arg(m_path);
        unmapRing();
        return false;
#endif
    }
    if (magic != kRingMagicHost) {
        if (err) *err = QStringLiteral(
            "%1 has magic 0x%2 — expected IQRING01 (rdma_rx) or RTAP "
            "(roce-extractor). The producer may still be initialising; both "
            "write their magic last.")
            .arg(m_path).arg(magic, 16, 16, QLatin1Char('0'));
        unmapRing();
        return false;
    }

    m_slots        = peek<quint32>(m_base, kOffCtrlNumSlots);
    m_slotStride   = peek<quint32>(m_base, kOffCtrlSlotStride);
    m_frameSamples = peek<quint32>(m_base, kOffCtrlFrameSamples);
    if (m_slots == 0 || m_slotStride == 0 || m_frameSamples == 0) {
        if (err) *err = QStringLiteral("ring control block is not populated yet");
        unmapRing();
        return false;
    }
    if (kCtrlBytes + static_cast<size_t>(m_slots) * m_slotStride > m_bytes) {
        if (err) *err = QStringLiteral(
            "ring geometry (%1 slots x %2 B) exceeds the mapping (%3 B)")
            .arg(m_slots).arg(m_slotStride).arg(m_bytes);
        unmapRing();
        return false;
    }

    // Adopt the producer's rate/centre so the axes match the transmitter
    // without the operator retyping them.
    const quint64 fs = peek<quint64>(m_base, kOffCtrlSampleRate);
    const qint64  fc = peek<qint64>(m_base, kOffCtrlCenterFreq);
    if (fs > 0) m_cfg.acq.sampleRateMsps = static_cast<double>(fs) / 1e6;
    m_cfg.acq.centerFreqGHz = static_cast<double>(fc) / 1e9;
    m_cfg.acq.decimation = 1;
    m_cfg.acq.interpolation = 1;
    m_cfg.acq.ncoFreqMHz = 0.0;
    m_cfg.src.format = SampleFormat::Cs16;   // interleaved int16 I/Q
    m_cfg.src.streamChannels = 1;

    m_lastWriteCount = peek<quint64>(m_base, kOffCtrlWriteCount);
    return true;
#endif
}

// ---------------------------------------------------------------------------
// roce-extractor tap.
//
// Geometry and FORMAT both come from the header, so the display adopts what
// the producer actually publishes instead of assuming int16 I/Q.
// ---------------------------------------------------------------------------
#ifdef SDR_ENABLE_CUDA
// ---------------------------------------------------------------------------
// GPUDirect ring (IQRINGG1).
//
// Peer to peer: the NIC DMA'd each payload straight into VRAM. The host
// mapping carries only the control block and 64-byte headers; the payload is
// reached through the cudaIpcMemHandle_t rdma_rx_gpu published at offset 1024.
//
// Layout, exactly as holoscan_iq_viz.py reads it:
//   host header for slot : CTRL_BYTES + slot * 64
//   VRAM payload for slot: slot * slot_stride      (payload only, no header)
// ---------------------------------------------------------------------------
bool RoceShmSource::openGpuRing(QString* err)
{
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count == 0) {
        if (err) *err = QStringLiteral("GPUDirect ring found, but no CUDA device is visible.");
        return false;
    }
    cudaSetDevice(0);

    cudaIpcMemHandle_t h{};
    std::memcpy(&h, static_cast<const char*>(m_base) + 1024, sizeof(h));

    void* p = nullptr;
    // LazyEnablePeerAccess: lets the handle be opened even when the allocation
    // was made in a context with peer mappings enabled — the P2P case.
    const cudaError_t e = cudaIpcOpenMemHandle(&p, h, cudaIpcMemLazyEnablePeerAccess);
    if (e != cudaSuccess) {
        if (err) *err = QStringLiteral(
            "cudaIpcOpenMemHandle failed: %1\n\n"
            "rdma_rx_gpu and this application must run as the SAME user — a "
            "CUDA IPC handle cannot cross users. If the GUI was started with "
            "sudo, either run rdma_rx_gpu as root too, or start the GUI "
            "without sudo (grant PCIe access with setcap cap_sys_rawio+ep).")
            .arg(QLatin1String(cudaGetErrorString(e)));
        return false;
    }
    m_ipcPtr = p;
    emit statusMessage(tr("GPUDirect ring opened: NIC -> VRAM peer to peer, "
                          "zero copy via CUDA IPC."));
    return true;
}

void RoceShmSource::closeGpuRing()
{
    if (m_ipcPtr) { cudaIpcCloseMemHandle(m_ipcPtr); m_ipcPtr = nullptr; }
}

void RoceShmSource::pollGpuRing()
{
    const quint64 c1 = peek<quint64>(m_base, kOffCtrlWriteCount);
    if (c1 == 0 || c1 == m_lastWriteCount) return;

    // Overwriting ring: take the newest, count what was skipped. A display
    // must never slow a 50 Gb/s capture down.
    const quint64 skipped = (c1 - m_lastWriteCount) - 1;
    if (m_lastWriteCount && skipped) m_stats.blocksDropped += skipped;
    m_lastWriteCount = c1;

    const quint64 slot = (c1 - 1) % m_slots;
    const char*   hdr  = static_cast<const char*>(m_base) + kCtrlBytes + slot * 64u;
    quint32 nsamp = peek<quint32>(hdr, 24);
    if (nsamp == 0) return;

    // Only copy what the display can use. The whole 1 MiB frame at display
    // rate is harmless, but there is no reason to move bytes nothing reads.
    constexpr quint32 kDisplayCap = 65536u;
    if (nsamp > kDisplayCap) nsamp = kDisplayCap;

    const size_t bytes = size_t(nsamp) * 4u;   // int16 I + int16 Q
    QByteArray host(static_cast<int>(bytes), Qt::Uninitialized);

    const char* src = static_cast<const char*>(m_ipcPtr) + slot * size_t(m_slotStride);
    const cudaError_t e = cudaMemcpy(host.data(), src, bytes, cudaMemcpyDeviceToHost);
    if (e != cudaSuccess) {
        emit sourceError(tr("GPU ring copy failed: %1")
                              .arg(QLatin1String(cudaGetErrorString(e))));
        return;
    }

    // The visualiser's ordering guard, kept exactly: if fewer than
    // (slots - 4) frames arrived while we copied, our slot cannot have been
    // recycled underneath us. Otherwise the bytes may be two frames spliced
    // together — which looks like a signal artefact, not a bug.
    const quint64 c2 = peek<quint64>(m_base, kOffCtrlWriteCount);
    if (c2 - c1 > m_slots - 4) { m_stats.blocksDropped++; return; }

    if (!admit(nsamp)) { accountBytes(bytes); return; }

    SampleBlock b;
    b.raw          = host;
    b.rawFormat    = SampleFormat::Cs16;
    b.rawChannels  = 1;
    b.rawFullScale = 1.0;
    accountBytes(bytes);
    publish(b);
}
#endif

// Wire bytes per sample for the format the RTAP header declared. Used to turn
// a byte count into a sample count for back-pressure accounting.
quint32 RoceShmSource::bytesPerSample() const
{
    switch (m_cfg.src.format) {
    case SampleFormat::Cs16: return 4;   // int16 I + int16 Q
    case SampleFormat::Rs8:  return 1;
    case SampleFormat::Rs16: return 2;
    case SampleFormat::Rs32: return 4;
    case SampleFormat::Rf32: return 4;
    default:                 return 4;
    }
}

bool RoceShmSource::mapRoceTap(QString* err)
{
#ifndef Q_OS_UNIX
    if (err) *err = QStringLiteral("shared memory rings are POSIX-only");
    return false;
#else
    const quint32 ver = peek<quint32>(m_base, kOffTapVersion);
    if (ver != 1u) {
        if (err) *err = QStringLiteral(
            "%1 is an RTAP ring of version %2; this build understands version 1.")
            .arg(m_path).arg(ver);
        unmapRing();
        return false;
    }

    m_slots        = peek<quint32>(m_base, kOffTapNSlots);
    m_tapSlotBytes = peek<quint32>(m_base, kOffTapSlotBytes);
    m_tapChannels  = peek<quint32>(m_base, kOffTapChannels);
    m_tapDType     = peek<quint32>(m_base, kOffTapDType);
    m_tapFullScale = peek<double>(m_base, kOffTapFullScale);

    if (m_slots == 0 || m_tapSlotBytes == 0) {
        if (err) *err = QStringLiteral("RTAP header is not populated yet");
        unmapRing();
        return false;
    }
    if (m_slots & (m_slots - 1)) {
        if (err) *err = QStringLiteral(
            "RTAP nslots (%1) is not a power of two; the producer masks with "
            "nslots-1, so a non-power-of-two ring would alias.").arg(m_slots);
        unmapRing();
        return false;
    }

    const size_t need = kTapHdrBytes
                      + static_cast<size_t>(m_slots) * kTapMetaBytes
                      + static_cast<size_t>(m_slots) * m_tapSlotBytes;
    if (need > m_bytes) {
        if (err) *err = QStringLiteral(
            "RTAP geometry (%1 slots, %2 B each) needs %3 B but the mapping is "
            "%4 B").arg(m_slots).arg(m_tapSlotBytes).arg(need).arg(m_bytes);
        unmapRing();
        return false;
    }

    // Format comes from the producer. Anything unrecognised is refused rather
    // than defaulted: plotting the wrong decode looks like a signal problem
    // and wastes far more time than an error message.
    switch (m_tapDType) {
    case TapCInt16: m_cfg.src.format = SampleFormat::Cs16; break;
    case TapInt16:  m_cfg.src.format = SampleFormat::Rs16; break;
    case TapInt8:   m_cfg.src.format = SampleFormat::Rs8;  break;
    case TapInt32:  m_cfg.src.format = SampleFormat::Rs32; break;
    case TapFloat32:m_cfg.src.format = SampleFormat::Rf32; break;
    default:
        if (err) *err = QStringLiteral(
            "RTAP header declares dtype %1, which this build does not decode.")
            .arg(m_tapDType);
        unmapRing();
        return false;
    }
    m_cfg.src.streamChannels = m_tapChannels ? static_cast<int>(m_tapChannels) : 1;

    // Adopt the producer's rate and centre, exactly as the IQRING path does.
    const double fs = peek<double>(m_base, kOffTapFsHz);
    const double fc = peek<double>(m_base, kOffTapCenterHz);
    if (fs > 0.0) m_cfg.acq.sampleRateMsps = fs / 1e6;
    m_cfg.acq.centerFreqGHz = fc / 1e9;
    m_cfg.acq.decimation    = 1;
    m_cfg.acq.interpolation = 1;
    m_cfg.acq.ncoFreqMHz    = 0.0;

    // Start from what has already been published so the first poll does not
    // report every historical slot as a drop.
    m_lastProduced = peek<quint64>(m_base, kOffTapProduced);
    return true;
#endif
}

void RoceShmSource::unmapRing()
{
#ifdef SDR_ENABLE_CUDA
    closeGpuRing();
#endif
#ifdef Q_OS_UNIX
    if (m_base) { ::munmap(m_base, m_bytes); m_base = nullptr; }
    if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }
#endif
    m_bytes = 0;
}

void RoceShmSource::start()
{
    if (m_active) return;

    // Re-resolve here: the Config carrying devicePath reaches this object
    // through applyConfig(), which runs after the constructor.
    m_path = resolvePath();

    QString err;
    if (!mapRing(&err)) {
        emit sourceError(err);
        emit runStateChanged(RunState::Error);
        return;
    }

    m_active = true;
    m_paused = false;
    emit statusMessage(QStringLiteral("RoCEv2 ring mapped: %1 slots x %2 B, "
                                      "%3 samples/frame")
                           .arg(m_slots).arg(m_slotStride).arg(m_frameSamples));
    emitState();

    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setTimerType(Qt::PreciseTimer);
        connect(m_timer, &QTimer::timeout, this, &RoceShmSource::poll);
    }
    // 2 ms: fast enough that the newest slot is fresh, slow enough that the
    // poll costs nothing. The producer is free-running; we sample it.
    m_timer->start(2);

}

void RoceShmSource::stop()
{
    if (m_timer) m_timer->stop();
    m_active = false;
    unmapRing();
    emitState();
    emit statusMessage(QStringLiteral("RoCEv2 ring released"));
}

void RoceShmSource::poll()
{
    if (!m_active || m_paused || !m_base) return;
    if (m_abi == RingAbi::RoceTap) { pollRoceTap(); return; }
#ifdef SDR_ENABLE_CUDA
    if (m_gpuRing) { pollGpuRing(); return; }
#endif
    pollIqRing();
}

// ---------------------------------------------------------------------------
// roce-extractor tap.
//
// Overwriting ring: the producer publishes and never waits, so the reader
// always takes the NEWEST complete slot and counts what it skipped. A display
// must not be able to slow a 100 Gbps capture down.
//
// Each slot is published under a seqlock (tap.h): meta.seq is stamped odd
// before the payload copy and even afterwards. Reading seq before and after
// the copy and requiring it to be even and unchanged is what makes a torn
// frame detectable — without it, a slot overwritten mid-read would be plotted
// as a glitch that looks like a real signal artefact.
// ---------------------------------------------------------------------------
void RoceShmSource::pollRoceTap()
{
    const char* base = static_cast<const char*>(m_base);
    const char* meta0 = base + kTapHdrBytes;
    const char* data0 = meta0 + static_cast<size_t>(m_slots) * kTapMetaBytes;

    const quint64 produced = peek<quint64>(m_base, kOffTapProduced);
    if (produced == m_lastProduced) {
        // Nothing new. Surface producer exit once, rather than looking hung.
        if (peek<quint32>(m_base, kOffTapRunning) == 0u && m_active) {
            emit statusMessage(tr("roce-extractor has stopped publishing."));
        }
        return;
    }

    const quint64 missed = (produced - m_lastProduced) - 1;
    if (missed > 0) {
        m_stats.blocksDropped += missed;
        m_stats.samplesDropped += missed * (m_tapSlotBytes / bytesPerSample());
    }
    m_lastProduced = produced;

    const quint64 idx  = (produced - 1) & (m_slots - 1);
    const char*   meta = meta0 + idx * kTapMetaBytes;

    const quint64 seqBefore = peek<quint64>(meta, kOffMetaSeq);
    if (seqBefore & 1u) return;                 // slot is mid-write

    quint32 len = peek<quint32>(meta, kOffMetaLen);
    if (len == 0 || len > m_tapSlotBytes) return;

    QByteArray payload(data0 + idx * m_tapSlotBytes, static_cast<int>(len));

    // Re-read AFTER the copy: if it moved, the producer overwrote this slot
    // while we were reading it and the bytes are a mixture of two frames.
    if (peek<quint64>(meta, kOffMetaSeq) != seqBefore) {
        m_stats.blocksDropped++;
        return;
    }

    const quint32 bps  = bytesPerSample();
    const quint32 chan = m_cfg.src.streamChannels > 0
                       ? static_cast<quint32>(m_cfg.src.streamChannels) : 1u;
    const quint32 samplesPerCh = len / (bps * chan);
    if (samplesPerCh == 0) return;

    if (!admit(samplesPerCh)) { accountBytes(len); return; }

    SampleBlock b;
    // Raw wire bytes again — decoded downstream by the same SampleCodec the
    // PCIe path uses, so recording and plotting are identical for both
    // transports and there is no second decoder to keep in step.
    b.raw          = payload;
    b.rawFormat    = m_cfg.src.format;
    b.rawChannels  = static_cast<int>(chan);
    b.rawFullScale = (m_tapFullScale > 0.0) ? m_tapFullScale : 1.0;

    accountBytes(len);
    publish(b);
}

void RoceShmSource::pollIqRing()
{
    const quint64 wc = peek<quint64>(m_base, kOffCtrlWriteCount);
    if (wc == m_lastWriteCount) return;          // nothing new published

    // Drop-oldest ring: always take the newest published slot. Frames the
    // display never saw are counted, not hidden.
    const quint64 missed = (wc - m_lastWriteCount) - 1;
    if (missed > 0) {
        m_stats.blocksDropped  += missed;
        m_stats.samplesDropped += missed * m_frameSamples;
    }
    m_lastWriteCount = wc;

    const quint64 slotIdx = (wc - 1) % m_slots;
    const char* slot = static_cast<const char*>(m_base)
                     + kCtrlBytes + slotIdx * m_slotStride;

    if (peek<quint64>(slot, kOffHdrMagic) != kFrameMagic) return;  // torn write

    const quint32 n = peek<quint32>(slot, kOffHdrNSamples);
    if (n == 0 || n > m_frameSamples) return;

    const quint64 seq = peek<quint64>(slot, kOffHdrSeq);
    if (m_lastSeq && seq > m_lastSeq + 1) m_seqGapsSeen += seq - m_lastSeq - 1;
    m_lastSeq = seq;

    const std::size_t payloadBytes = static_cast<std::size_t>(n) * kBytesPerSample;
    if (!admit(n)) { accountBytes(payloadBytes); return; }

    SampleBlock b;
    // Raw wire bytes, decoded downstream by the existing SampleCodec exactly
    // as the PCIe path does — no second decoder, no format guessing.
    b.raw          = QByteArray(slot + kHdrBytes, static_cast<int>(payloadBytes));
    b.rawFormat    = SampleFormat::Cs16;
    b.rawChannels  = 1;
    b.rawFullScale = 1.0;

    accountBytes(payloadBytes);
    publish(b);
}

} // namespace sdr
