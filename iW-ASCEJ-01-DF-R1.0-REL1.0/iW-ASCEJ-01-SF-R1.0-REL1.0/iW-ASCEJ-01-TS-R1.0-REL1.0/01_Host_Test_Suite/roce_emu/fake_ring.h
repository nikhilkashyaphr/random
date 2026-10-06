#pragma once
// ---------------------------------------------------------------------------
// Synthetic RoCEv2 rings for the host test suite.
//
// Both producers in the system publish into a POSIX shared-memory object with
// a fixed ABI. This header builds those objects byte-for-byte from the ABI
// definitions, so RoceShmSource can be driven with NO NIC, NO RDMA stack and
// NO GPU — which is the only way the ingest path can be a regression gate
// rather than a bench procedure.
//
//   FakeIqRing   "IQRING01"  <- rdma_rx            (reference/.../rdma_common.h)
//   FakeGpuRing  "IQRINGG1"  <- rdma_rx_gpu        (control block only; the
//                                                   payload would be in VRAM)
//   FakeTapRing  "RTAP"      <- roce-extractor     (roce/tap.h)
//
// WHY THE OFFSETS ARE REPEATED HERE
// ---------------------------------
// They are written out as named constants rather than by including the
// producers' headers, deliberately. rdma_common.h guards its layout with
// _Static_assert; this file is the INDEPENDENT second opinion. If the two ever
// disagree, a test fails and names the field -- which is the whole point. An
// include would make both sides wrong together and silently.
//
// Every ring writes its magic LAST, as the real producers do, so a reader that
// maps the object mid-setup sees an invalid ring instead of a half-built one.
// ---------------------------------------------------------------------------

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace t {

// --- IQRING ABI (rdma_common.h) -------------------------------------------
constexpr std::uint64_t kRingMagicHost = 0x495152494e473031ULL; // "IQRING01"
constexpr std::uint64_t kRingMagicGpu  = 0x495152494e474731ULL; // "IQRINGG1"
constexpr std::uint64_t kFrameMagic    = 0x49515246524d4131ULL; // "IQRFRMA1"
constexpr std::uint32_t kCtrlBytes     = 4096u;
constexpr std::uint32_t kHdrBytes      = 64u;

// ring_ctrl field offsets
constexpr int kOffCtrlMagic = 0, kOffCtrlVersion = 8, kOffCtrlNumSlots = 12,
              kOffCtrlSlotStride = 16, kOffCtrlFrameSamples = 20,
              kOffCtrlSampleRate = 24, kOffCtrlCenterFreq = 32,
              kOffCtrlWriteCount = 40, kOffCtrlBytesTotal = 48,
              kOffCtrlSeqGaps = 56;

// frame_hdr field offsets
constexpr int kOffHdrMagic = 0, kOffHdrSeq = 8, kOffHdrTns = 16,
              kOffHdrNSamples = 24, kOffHdrSampleRate = 32,
              kOffHdrCenterFreq = 40;

// --- RTAP ABI (roce/tap.h) ------------------------------------------------
constexpr std::uint32_t kTapMagic     = 0x52544150u;  // "RTAP"
constexpr std::uint32_t kTapHdrBytes  = 4096u;
constexpr std::uint32_t kTapMetaBytes = 64u;

constexpr int kOffTapMagic = 0, kOffTapVersion = 4, kOffTapSlotBytes = 8,
              kOffTapNSlots = 12, kOffTapDecim = 16, kOffTapChannels = 20,
              kOffTapDType = 24, kOffTapFsHz = 32, kOffTapCenterHz = 40,
              kOffTapFullScale = 48, kOffTapProduced = 56, kOffTapPktSeen = 64,
              kOffTapMissing = 72, kOffTapRunning = 80;

constexpr int kOffMetaSeq = 0, kOffMetaPsn = 8, kOffMetaTsNs = 16,
              kOffMetaLen = 24, kOffMetaFlags = 28;

enum TapDType { TapInt16 = 0, TapInt8, TapInt32, TapFloat32, TapCInt16 };

// NOTE: slot counts are named `nslots`, never `slots`. Qt defines `slots`
// as a preprocessor macro that expands to nothing (the signals/slots
// keywords), so a parameter called `slots` silently vanishes and the
// resulting errors point at the assignment rather than the name.

// ---------------------------------------------------------------------------
// A mapped shm object the tests own and clean up.
// ---------------------------------------------------------------------------
class ShmFile {
public:
    ShmFile() = default;
    ShmFile(const ShmFile&) = delete;
    ShmFile& operator=(const ShmFile&) = delete;
    ~ShmFile() { destroy(); }

    bool create(const std::string& path, std::size_t bytes)
    {
        destroy();
        m_path = path;
        m_fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (m_fd < 0) { std::perror("open"); return false; }
        if (::ftruncate(m_fd, static_cast<off_t>(bytes)) != 0) {
            std::perror("ftruncate"); return false;
        }
        void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
        if (p == MAP_FAILED) { std::perror("mmap"); m_base = nullptr; return false; }
        m_base  = static_cast<char*>(p);
        m_bytes = bytes;
        std::memset(m_base, 0, bytes);
        return true;
    }

    /// Truncate to fewer bytes than the geometry claims, to exercise the
    /// reader's "geometry exceeds the mapping" refusal.
    bool shrinkTo(std::size_t bytes)
    {
        return m_fd >= 0 && ::ftruncate(m_fd, static_cast<off_t>(bytes)) == 0;
    }

    void destroy()
    {
        if (m_base) { ::munmap(m_base, m_bytes); m_base = nullptr; }
        if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }
        if (!m_path.empty()) { ::unlink(m_path.c_str()); m_path.clear(); }
        m_bytes = 0;
    }

    char* base() const { return m_base; }
    const std::string& path() const { return m_path; }

    template <typename T> void put(int off, T v)
    { std::memcpy(m_base + off, &v, sizeof(T)); }
    template <typename T> T get(int off) const
    { T v{}; std::memcpy(&v, m_base + off, sizeof(T)); return v; }

    static void barrier() { __sync_synchronize(); }

private:
    std::string m_path;
    int    m_fd    = -1;
    char*  m_base  = nullptr;
    std::size_t m_bytes = 0;
};

// ---------------------------------------------------------------------------
// Host ring (IQRING01) -- what rdma_rx publishes.
//
// Geometry is parameterised so a test can use small frames; the real producer
// uses 32 slots of 262,144 samples, which the geometry test checks explicitly.
// ---------------------------------------------------------------------------
class FakeIqRing {
public:
    bool create(const std::string& path, std::uint32_t nslots,
                std::uint32_t frameSamples, double fsHz, double fcHz)
    {
        m_slots = nslots;
        m_frameSamples = frameSamples;
        // Same rule as rdma_common.h: slot stride is the 4 KiB-rounded frame.
        const std::uint32_t msg = kHdrBytes + frameSamples * 4u;
        m_stride = ((msg + 4095u) / 4096u) * 4096u;
        m_fs = fsHz; m_fc = fcHz;

        const std::size_t total = kCtrlBytes + std::size_t(nslots) * m_stride;
        if (!m_shm.create(path, total)) return false;

        m_shm.put<std::uint32_t>(kOffCtrlVersion,      1u);
        m_shm.put<std::uint32_t>(kOffCtrlNumSlots,     nslots);
        m_shm.put<std::uint32_t>(kOffCtrlSlotStride,   m_stride);
        m_shm.put<std::uint32_t>(kOffCtrlFrameSamples, frameSamples);
        m_shm.put<std::uint64_t>(kOffCtrlSampleRate,   std::uint64_t(fsHz));
        m_shm.put<std::int64_t> (kOffCtrlCenterFreq,   std::int64_t(fcHz));
        ShmFile::barrier();
        m_shm.put<std::uint64_t>(kOffCtrlMagic, kRingMagicHost);   // magic LAST
        return true;
    }

    /// Publish one frame of a complex exponential at `toneHz`, 14-bit
    /// MSB-aligned into int16 exactly as the RFSoC wire format packs it
    /// (quantise to +/-8191, then <<2, so bits[1:0] are always zero).
    void publishTone(double toneHz, double amplitude = 0.8)
    {
        std::vector<std::int16_t> iq(std::size_t(m_frameSamples) * 2u);
        const double dphi = 2.0 * M_PI * toneHz / m_fs;
        for (std::uint32_t i = 0; i < m_frameSamples; ++i) {
            const double ph = m_phase + dphi * double(i);
            const auto q = [&](double v) {
                long s = std::lround(v * 8191.0 * amplitude);
                if (s >  8191) s =  8191;
                if (s < -8191) s = -8191;
                return std::int16_t(s << 2);
            };
            iq[2u * i    ] = q(std::cos(ph));
            iq[2u * i + 1] = q(std::sin(ph));
        }
        m_phase += dphi * double(m_frameSamples);
        publishRaw(iq.data(), m_frameSamples);
    }

    /// Publish arbitrary samples. `nSamples` goes into the frame header.
    void publishRaw(const std::int16_t* iq, std::uint32_t nSamples)
    {
        char* slot = slotPtr(m_writeCount);
        std::memcpy(slot + kHdrBytes, iq, std::size_t(nSamples) * 4u);
        putAt<std::uint64_t>(slot, kOffHdrSeq,        m_seq);
        putAt<std::uint64_t>(slot, kOffHdrTns,        m_seq * 1000000ULL);
        putAt<std::uint32_t>(slot, kOffHdrNSamples,   nSamples);
        putAt<std::uint64_t>(slot, kOffHdrSampleRate, std::uint64_t(m_fs));
        putAt<std::int64_t> (slot, kOffHdrCenterFreq, std::int64_t(m_fc));
        ShmFile::barrier();
        putAt<std::uint64_t>(slot, kOffHdrMagic, kFrameMagic);     // magic LAST
        ShmFile::barrier();
        ++m_seq;
        ++m_writeCount;
        // write_count published last, with release ordering: a reader that
        // sees the new count is guaranteed to see the payload.
        m_shm.put<std::uint64_t>(kOffCtrlWriteCount, m_writeCount);
    }

    /// Corrupt the newest slot's frame magic, simulating a torn write the
    /// reader must discard rather than plot.
    void corruptNewestFrameMagic()
    {
        char* slot = slotPtr(m_writeCount - 1);
        putAt<std::uint64_t>(slot, kOffHdrMagic, 0xDEADBEEFDEADBEEFULL);
    }

    /// Advance write_count without writing payloads, simulating frames that
    /// arrived while the display was busy. The reader must count them as
    /// drops and still take only the newest.
    void skipFrames(std::uint64_t n)
    {
        m_writeCount += n;
        m_seq        += n;
        m_shm.put<std::uint64_t>(kOffCtrlWriteCount, m_writeCount);
    }

    void setMagic(std::uint64_t m) { m_shm.put<std::uint64_t>(kOffCtrlMagic, m); }
    void setNumSlots(std::uint32_t n) { m_shm.put<std::uint32_t>(kOffCtrlNumSlots, n); }
    void clearMagic() { m_shm.put<std::uint64_t>(kOffCtrlMagic, 0); }
    bool shrinkTo(std::size_t b) { return m_shm.shrinkTo(b); }

    const std::string& path() const { return m_shm.path(); }
    std::uint32_t stride() const { return m_stride; }
    std::uint64_t writeCount() const { return m_writeCount; }
    void destroy() { m_shm.destroy(); }

private:
    char* slotPtr(std::uint64_t wc) const
    { return m_shm.base() + kCtrlBytes + (wc % m_slots) * m_stride; }

    template <typename T> static void putAt(char* p, int off, T v)
    { std::memcpy(p + off, &v, sizeof(T)); }

    ShmFile m_shm;
    std::uint32_t m_slots = 0, m_frameSamples = 0, m_stride = 0;
    double m_fs = 0.0, m_fc = 0.0, m_phase = 0.0;
    std::uint64_t m_seq = 0, m_writeCount = 0;
};

// ---------------------------------------------------------------------------
// GPUDirect ring (IQRINGG1) -- control block and headers only.
//
// The real rdma_rx_gpu puts payloads in VRAM behind a cudaIpcMemHandle at
// control offset 1024, so a host-only mapping like this is exactly what a
// non-CUDA build can see. It exists so the refusal path can be tested: a
// CPU-only build must decline with a reason rather than display zeros.
// ---------------------------------------------------------------------------
class FakeGpuRing {
public:
    bool create(const std::string& path, std::uint32_t nslots = 32u,
                double fsHz = 200e6, double fcHz = 3.1e9)
    {
        const std::size_t total = kCtrlBytes + std::size_t(nslots) * kHdrBytes;
        if (!m_shm.create(path, total)) return false;
        m_shm.put<std::uint32_t>(kOffCtrlVersion,      1u);
        m_shm.put<std::uint32_t>(kOffCtrlNumSlots,     nslots);
        m_shm.put<std::uint32_t>(kOffCtrlSlotStride,   1024u * 1024u);
        m_shm.put<std::uint32_t>(kOffCtrlFrameSamples, 256u * 1024u);
        m_shm.put<std::uint64_t>(kOffCtrlSampleRate,   std::uint64_t(fsHz));
        m_shm.put<std::int64_t> (kOffCtrlCenterFreq,   std::int64_t(fcHz));
        // A plausible-looking 64-byte IPC handle at offset 1024. It cannot be
        // opened -- which is the point: no CUDA build must get this far.
        for (int i = 0; i < 64; ++i) m_shm.put<std::uint8_t>(1024 + i, std::uint8_t(i));
        ShmFile::barrier();
        m_shm.put<std::uint64_t>(kOffCtrlMagic, kRingMagicGpu);
        return true;
    }
    const std::string& path() const { return m_shm.path(); }
    void destroy() { m_shm.destroy(); }
private:
    ShmFile m_shm;
};

// ---------------------------------------------------------------------------
// roce-extractor tap (RTAP).
//
// Unlike IQRING this header carries the sample FORMAT, channel count, rate,
// centre and full scale, and publishes each slot under a seqlock: meta.seq is
// odd while the payload is being copied. Both properties are what the tests
// below exercise.
// ---------------------------------------------------------------------------
class FakeTapRing {
public:
    bool create(const std::string& path, std::uint32_t nslots,
                std::uint32_t slotBytes, std::uint32_t channels,
                std::uint32_t dtype, double fsHz, double fcHz,
                double fullScale, std::uint32_t version = 1u)
    {
        m_slots = nslots; m_slotBytes = slotBytes;
        const std::size_t total = kTapHdrBytes
                                + std::size_t(nslots) * kTapMetaBytes
                                + std::size_t(nslots) * slotBytes;
        if (!m_shm.create(path, total)) return false;
        m_shm.put<std::uint32_t>(kOffTapVersion,   version);
        m_shm.put<std::uint32_t>(kOffTapSlotBytes, slotBytes);
        m_shm.put<std::uint32_t>(kOffTapNSlots,    nslots);
        m_shm.put<std::uint32_t>(kOffTapDecim,     1u);
        m_shm.put<std::uint32_t>(kOffTapChannels,  channels);
        m_shm.put<std::uint32_t>(kOffTapDType,     dtype);
        m_shm.put<double>(kOffTapFsHz,      fsHz);
        m_shm.put<double>(kOffTapCenterHz,  fcHz);
        m_shm.put<double>(kOffTapFullScale, fullScale);
        m_shm.put<std::uint32_t>(kOffTapRunning, 1u);
        ShmFile::barrier();
        m_shm.put<std::uint32_t>(kOffTapMagic, kTapMagic);          // magic LAST
        return true;
    }

    /// Publish `len` bytes into the next slot, completing the seqlock so the
    /// reader sees an even, stable sequence number.
    void publish(const void* data, std::uint32_t len)
    {
        const std::uint64_t idx = m_produced & (m_slots - 1);
        char* meta = metaPtr(idx);
        char* slot = dataPtr(idx);
        putAt<std::uint64_t>(meta, kOffMetaSeq, m_seq | 1ULL);      // odd: writing
        ShmFile::barrier();
        std::memcpy(slot, data, len);
        putAt<std::uint32_t>(meta, kOffMetaLen, len);
        putAt<std::uint64_t>(meta, kOffMetaPsn, m_produced);
        putAt<std::uint64_t>(meta, kOffMetaTsNs, m_produced * 1000000ULL);
        ShmFile::barrier();
        m_seq = (m_seq | 1ULL) + 1ULL;                              // even: done
        putAt<std::uint64_t>(meta, kOffMetaSeq, m_seq);
        ShmFile::barrier();
        ++m_produced;
        m_shm.put<std::uint64_t>(kOffTapProduced, m_produced);
    }

    /// Leave the newest slot's seqlock ODD, i.e. mid-write. The reader must
    /// skip it instead of plotting a half-copied frame.
    void publishTorn(const void* data, std::uint32_t len)
    {
        const std::uint64_t idx = m_produced & (m_slots - 1);
        char* meta = metaPtr(idx);
        std::memcpy(dataPtr(idx), data, len);
        putAt<std::uint32_t>(meta, kOffMetaLen, len);
        putAt<std::uint64_t>(meta, kOffMetaSeq, (m_seq | 1ULL));    // stays odd
        ShmFile::barrier();
        ++m_produced;
        m_shm.put<std::uint64_t>(kOffTapProduced, m_produced);
    }

    /// Publish a slot whose declared length exceeds slot_bytes, which the
    /// reader must reject rather than read past the slot.
    void publishOverlongLen(std::uint32_t len)
    {
        const std::uint64_t idx = m_produced & (m_slots - 1);
        char* meta = metaPtr(idx);
        putAt<std::uint32_t>(meta, kOffMetaLen, len);
        putAt<std::uint64_t>(meta, kOffMetaSeq, m_seq);             // even
        ShmFile::barrier();
        ++m_produced;
        m_shm.put<std::uint64_t>(kOffTapProduced, m_produced);
    }

    void setRunning(std::uint32_t r) { m_shm.put<std::uint32_t>(kOffTapRunning, r); }
    const std::string& path() const { return m_shm.path(); }
    void destroy() { m_shm.destroy(); }

private:
    char* metaPtr(std::uint64_t i) const
    { return m_shm.base() + kTapHdrBytes + i * kTapMetaBytes; }
    char* dataPtr(std::uint64_t i) const
    { return m_shm.base() + kTapHdrBytes
           + std::size_t(m_slots) * kTapMetaBytes + i * m_slotBytes; }
    template <typename T> static void putAt(char* p, int off, T v)
    { std::memcpy(p + off, &v, sizeof(T)); }

    ShmFile m_shm;
    std::uint32_t m_slots = 0, m_slotBytes = 0;
    std::uint64_t m_produced = 0, m_seq = 0;   // even == stable
};

} // namespace t
