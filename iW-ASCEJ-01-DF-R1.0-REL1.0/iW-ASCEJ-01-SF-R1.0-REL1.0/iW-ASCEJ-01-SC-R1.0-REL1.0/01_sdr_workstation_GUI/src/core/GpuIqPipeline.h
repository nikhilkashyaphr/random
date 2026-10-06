#pragma once
// ---------------------------------------------------------------------------
// GpuIqPipeline — RoCEv2 GPUDirect ingest and on-GPU spectrum.
//
// WHAT RUNS WHERE
// ---------------
//   NIC ──DMA──► VRAM ring            (rdma_rx_gpu, GPUDirect RDMA)
//   VRAM ring ──CUDA IPC──► here      (zero copy: no device pointer is ever
//                                      dereferenced on the host)
//   int16 I/Q ──kernel──► cufftComplex
//   cufftComplex ──cuFFT──► spectrum
//   |X|² ──kernel──► dBFS
//   dBFS (a few KB) ──D2H──► Qt plots
//
// Sample data never reaches system RAM. Only the finished spectrum crosses the
// bus — for a 4096-point FFT that is 16 KB per displayed frame instead of the
// 1 MiB the raw slot would cost.
//
// WHY NOT HOLOVIZ IN THE QT WINDOW
// --------------------------------
// Holoviz owns a GLFW window and a GL context. Reparenting that into a
// QWidget hierarchy means reconciling two event loops and two context owners,
// and it is where this kind of integration usually stalls. The value of the
// Holoscan path is the GPU pipeline, not its renderer — the GUI already has
// plotting, markers, measurements and recording that Holoviz would duplicate.
//
// So this reproduces the Holoscan PIPELINE exactly (same ring, same ordering
// guard, same FFT and log-magnitude maths) and feeds the GUI's own plots. The
// operator sees one window, and every stage before the plot runs on the GPU.
//
// BUILD
// -----
// Compiled only when SDR_ENABLE_CUDA is defined. Without it every entry point
// below still exists and reports "built without CUDA", so the application
// builds and runs unchanged on a machine with no toolkit — which is most
// development machines.
// ---------------------------------------------------------------------------

#include <QString>
#include <QVector>
#include <cstdint>

namespace sdr {

/// Mirrors rdma_common.h. Kept as plain constants rather than an include, so
/// this file does not drag the RDMA headers into the GUI build.
namespace gpuring {
constexpr quint64 kMagicGpu     = 0x495152494e474731ull;  // "IQRINGG1"
constexpr quint32 kCtrlBytes    = 4096u;
constexpr quint32 kCtrlIpcOff   = 1024u;   // 64-byte cudaIpcMemHandle_t
constexpr quint32 kHdrBytes     = 64u;
constexpr quint64 kFrameMagic   = 0x4651524d30303031ull;  // frame_hdr magic
}

struct GpuStats {
    quint64 framesShown   = 0;
    quint64 framesSkipped = 0;   ///< arrived between displayed frames
    quint64 tornRejected  = 0;   ///< slot recycled mid-copy, discarded
    double  lastFftMs     = 0.0;
    QString deviceName;
    size_t  freeVram      = 0;
    size_t  totalVram     = 0;
};

/// Opens the GPUDirect ring published by rdma_rx_gpu and computes spectra on
/// the device. Not thread-safe: drive it from one thread.
class GpuIqPipeline
{
public:
    GpuIqPipeline();
    ~GpuIqPipeline();

    /// True if this binary was compiled with CUDA support at all.
    static bool builtWithCuda();

    /// Probe for a usable device without opening anything. `detail` always
    /// receives a human-readable reason, success or failure.
    static bool probeDevice(QString* detail);

    /// `hostCtrl` is the mmap of the receiver's /dev/shm object; the IPC
    /// handle is read from it at offset 1024.
    bool open(const void* hostCtrl, quint32 numSlots, quint32 slotStride,
              QString* err);
    void close();
    bool isOpen() const { return m_open; }

    /// Size of the FFT. Reallocates the plan and device buffers.
    bool setFftSize(int n, QString* err);
    int  fftSize() const { return m_fftSize; }

    /// Fetch the newest complete frame and return its spectrum in dBFS,
    /// fftshifted so bin 0 is the lowest frequency.
    ///
    /// `writeCount` is read from the host control block by the caller, which
    /// owns the mmap. Returns false when no new frame is available.
    bool nextSpectrum(quint64 writeCount, QVector<float>* dbfsOut, QString* err);

    const GpuStats& stats() const { return m_stats; }

private:
    bool    m_open       = false;
    int     m_fftSize    = 4096;
    quint32 m_numSlots   = 0;
    quint32 m_slotStride = 0;
    quint64 m_lastCount  = 0;
    GpuStats m_stats;

    // Opaque so this header stays free of CUDA types; the GUI must build
    // without the toolkit present.
    struct Impl;
    Impl* d = nullptr;
};

} // namespace sdr
