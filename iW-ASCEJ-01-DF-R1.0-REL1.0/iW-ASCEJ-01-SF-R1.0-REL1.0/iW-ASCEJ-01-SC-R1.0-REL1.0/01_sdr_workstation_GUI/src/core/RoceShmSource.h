#pragma once
// ---------------------------------------------------------------------------
// RoceShmSource — consumes the RoCEv2 receiver's shared-memory ring.
//
// This does NOT re-implement RDMA. The supplied `rdma_rx` (host path) and
// `rdma_rx_gpu` (GPUDirect path) already receive RoCEv2 frames and publish
// them into a POSIX shm ring with the ABI fixed in rdma_common.h. This class
// is a reader of that ring and nothing more, so the validated networking code
// stays untouched and there is exactly one implementation of it.
//
// Layout consumed (from rdma_common.h, byte-for-byte):
//
//   /dev/shm/iqring
//     [0 .. 4095]        struct ring_ctrl   magic "IQRING01" (host)
//                                           or   "IQRINGG1" (GPU/CUDA-IPC)
//     [4096 ..]          RING_SLOTS(32) x RING_SLOT_STRIDE
//                          each slot: 64 B struct frame_hdr + 1 MiB payload
//
//   payload = 262,144 interleaved int16 I, int16 Q pairs
//           = exactly SampleFormat::Cs16, single channel
//
// Overwrite policy in the producer is drop-oldest, and the reader always takes
// the newest published slot. A slow GUI therefore never back-pressures the
// link — which is the correct behaviour for a display and matches how the
// existing DmaSource treats the FPGA stream.
//
// TWO PRODUCERS, ONE READER
// -------------------------
// Two shared-memory ABIs exist in this system and they are not compatible:
//
//   rdma_rx / rdma_rx_gpu   "IQRING01" / "IQRINGG1"   default /dev/shm/iqring
//        4096 B ctrl, 32 slots x (64 B frame_hdr + 1 MiB payload)
//        payload always interleaved int16 I/Q
//
//   roce-extractor tap      "RTAP"                    default /dev/shm/roce_tap
//        4096 B tap_hdr, nslots x 64 B tap_meta, nslots x slot_bytes
//        per-slot seqlock; dtype / channels / fs_hz / center_hz in the header
//
// Rather than add a second SourceMode the operator has to choose correctly,
// this class reads the magic word and adapts. Both are overwriting rings that
// never back-pressure the producer, both deliver raw wire bytes to the same
// SampleCodec, and both therefore record and plot through exactly the same
// pipeline as the PCIe path.
//
// The RTAP header carries what IQRING cannot: sample format, channel count,
// sample rate, centre frequency and full-scale. Those are READ from the
// producer rather than assumed, which removes the class of bug where the
// display scales every frequency by the ratio of two disagreeing rates.
//
// RTAP slots are published under a seqlock — meta.seq is odd while the payload
// is being copied. A slot that was overwritten mid-read is discarded rather
// than plotted as a torn frame.
//
// GPU rings are detected and reported, not silently downgraded: when the
// control block carries RING_MAGIC_GPU the payload lives in VRAM behind a
// cudaIpcMemHandle and is not host-readable, so this source declines rather
// than pretending (see rule "never silently fall back from GPU to CPU").
// ---------------------------------------------------------------------------

#include "Sources.h"

#include <QString>

namespace sdr {

class RoceShmSource : public ISignalSource
{
    Q_OBJECT
public:
    explicit RoceShmSource(QObject* parent = nullptr);
    ~RoceShmSource() override;

    QString name() const override;

    // --- ABI constants, mirrored from rdma_common.h -----------------------
    static constexpr quint64 kRingMagicHost = 0x495152494e473031ULL; // "IQRING01"
    static constexpr quint64 kRingMagicGpu  = 0x495152494e474731ULL; // "IQRINGG1"
    static constexpr quint64 kFrameMagic    = 0x49515246524d4131ULL; // "IQRFRMA1"
    static constexpr quint32 kCtrlBytes     = 4096u;
    static constexpr quint32 kFrameSamples  = 256u * 1024u;
    static constexpr quint32 kBytesPerSample= 4u;
    static constexpr quint32 kFramePayload  = kFrameSamples * kBytesPerSample;
    static constexpr quint32 kHdrBytes      = 64u;

    // --- roce-extractor tap ABI, mirrored from roce/tap.h ------------------
    static constexpr quint32 kTapMagic      = 0x52544150u;  // "RTAP"
    static constexpr quint32 kTapHdrBytes   = 4096u;
    static constexpr quint32 kTapMetaBytes  = 64u;

    /// tap.h: enum { TAP_DT_INT16, TAP_DT_INT8, TAP_DT_INT32, TAP_DT_FLOAT32,
    ///               TAP_DT_CINT16 }
    enum TapDType { TapInt16 = 0, TapInt8, TapInt32, TapFloat32, TapCInt16 };

    /// Read sample_rate_hz / center_freq_hz from a ring's control block
    /// without starting acquisition. The ring is authoritative for these —
    /// the transmitter sets them — so the GUI must adopt them rather than
    /// apply its own defaults, or every frequency is scaled by the ratio of
    /// the two rates.
    static bool peekRingInfo(const QString& path,
                             double* rateMsps, double* centerGHz);

public slots:
    void start() override;
    void stop()  override;

private slots:
    void poll();

private:
    bool mapRing(QString* err);
    void unmapRing();

    QString  m_path;           ///< /dev/shm/iqring
    int      m_fd    = -1;
    void*    m_base  = nullptr;
    size_t   m_bytes = 0;

    quint32  m_slots       = 0;
    quint32  m_slotStride  = 0;
    quint32  m_frameSamples= 0;
    bool     m_gpuRing     = false;

    /// Which producer this ring belongs to. Decided once, at map time, from
    /// the magic word — never guessed per frame.
    enum class RingAbi { IqRing, RoceTap } m_abi = RingAbi::IqRing;

    // RTAP geometry and format hints, read from the header at map time.
    quint32  m_tapSlotBytes = 0;
    quint32  m_tapChannels  = 1;
    quint32  m_tapDType     = TapInt16;
    double   m_tapFullScale = 0.0;
    quint64  m_lastProduced = 0;

    void pollIqRing();
    void pollRoceTap();
    bool mapRoceTap(QString* err);
    // GPUDirect (IQRINGG1): payload in VRAM, opened by CUDA IPC.
    bool openGpuRing(QString* err);
    void closeGpuRing();
    void pollGpuRing();
    void*    m_ipcPtr = nullptr;   ///< base of the VRAM ring

    quint32 bytesPerSample() const;

    quint64  m_lastWriteCount = 0;
    quint64  m_lastSeq        = 0;
    quint64  m_seqGapsSeen    = 0;

    class QTimer* m_timer = nullptr;
};

} // namespace sdr
