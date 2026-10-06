// ---------------------------------------------------------------------------
// GpuIqPipeline — host side. Compiles with a normal C++ compiler; only
// GpuKernels.cu needs nvcc.
// ---------------------------------------------------------------------------

#include "GpuIqPipeline.h"

#ifdef SDR_ENABLE_CUDA

#include "GpuMathRef.h"          // coherentGain(): one definition, host-side

#include <cuda_runtime.h>
#include <cufft.h>
#include <QElapsedTimer>
#include <cstring>
#include <vector>

extern "C" {
void sdr_gpu_hann(float* w, int n, cudaStream_t s);
void sdr_gpu_convert_window(const short* iq, const float* w,
                            cufftComplex* out, int n, cudaStream_t s);
void sdr_gpu_mag_db_shift(const cufftComplex* in, float* db,
                          int n, float cg, cudaStream_t s);
}

namespace sdr {

struct GpuIqPipeline::Impl {
    void*         ipcPtr   = nullptr;   ///< base of the ring in VRAM
    cudaStream_t  stream   = nullptr;
    cufftHandle   plan     = 0;
    bool          planOk   = false;
    float*        dWindow  = nullptr;
    cufftComplex* dIn      = nullptr;
    cufftComplex* dOut     = nullptr;
    float*        dDb      = nullptr;
    float         cg       = 0.5f;      ///< window coherent gain
    int           n        = 0;
};

namespace {

QString cudaErr(const char* what, cudaError_t e)
{
    return QStringLiteral("%1: %2").arg(QLatin1String(what),
                                        QLatin1String(cudaGetErrorString(e)));
}

template <class T> T peek(const void* base, quint32 off)
{
    T v{};
    std::memcpy(&v, static_cast<const char*>(base) + off, sizeof(T));
    return v;
}

} // namespace

GpuIqPipeline::GpuIqPipeline() : d(new Impl) {}
GpuIqPipeline::~GpuIqPipeline() { close(); delete d; }

bool GpuIqPipeline::builtWithCuda() { return true; }

bool GpuIqPipeline::probeDevice(QString* detail)
{
    int n = 0;
    cudaError_t e = cudaGetDeviceCount(&n);
    if (e != cudaSuccess || n == 0) {
        if (detail) *detail = QStringLiteral("No CUDA device: %1")
            .arg(QLatin1String(cudaGetErrorString(e)));
        return false;
    }
    cudaDeviceProp p{};
    if (cudaGetDeviceProperties(&p, 0) != cudaSuccess) {
        if (detail) *detail = QStringLiteral("CUDA device 0 is not queryable");
        return false;
    }
    size_t freeB = 0, totB = 0;
    cudaMemGetInfo(&freeB, &totB);
    if (detail) *detail = QStringLiteral("%1 · %2 SM · %3 / %4 MiB free")
        .arg(QLatin1String(p.name)).arg(p.multiProcessorCount)
        .arg(freeB >> 20).arg(totB >> 20);
    return true;
}

bool GpuIqPipeline::open(const void* hostCtrl, quint32 numSlots,
                         quint32 slotStride, QString* err)
{
    close();
    if (!hostCtrl) { if (err) *err = QStringLiteral("no host control mapping"); return false; }

    if (peek<quint64>(hostCtrl, 0) != gpuring::kMagicGpu) {
        if (err) *err = QStringLiteral(
            "ring magic is not IQRINGG1 — this is not a GPUDirect ring");
        return false;
    }

    QString why;
    if (!probeDevice(&why)) { if (err) *err = why; return false; }

    cudaIpcMemHandle_t h{};
    std::memcpy(&h, static_cast<const char*>(hostCtrl) + gpuring::kCtrlIpcOff,
                sizeof(h));

    void* p = nullptr;
    cudaError_t e = cudaIpcOpenMemHandle(&p, h, cudaIpcMemLazyEnablePeerAccess);
    if (e != cudaSuccess) {
        // Overwhelmingly the same-user case, so say that first rather than
        // making the operator decode a CUDA error string.
        if (err) *err = QStringLiteral(
            "cudaIpcOpenMemHandle failed: %1\n\n"
            "rdma_rx_gpu and this application must run as the SAME user — a "
            "CUDA IPC handle cannot cross users. If the GUI was started with "
            "sudo for PCIe access, run rdma_rx_gpu as root too, or start the "
            "GUI without sudo when using the GPU path.")
            .arg(QLatin1String(cudaGetErrorString(e)));
        return false;
    }

    d->ipcPtr    = p;
    m_numSlots   = numSlots;
    m_slotStride = slotStride;

    if ((e = cudaStreamCreate(&d->stream)) != cudaSuccess) {
        if (err) *err = cudaErr("cudaStreamCreate", e);
        close();
        return false;
    }

    cudaDeviceProp prop{};
    cudaGetDeviceProperties(&prop, 0);
    m_stats.deviceName = QLatin1String(prop.name);
    cudaMemGetInfo(&m_stats.freeVram, &m_stats.totalVram);

    if (!setFftSize(m_fftSize, err)) { close(); return false; }

    m_open      = true;
    m_lastCount = 0;
    return true;
}

bool GpuIqPipeline::setFftSize(int n, QString* err)
{
    if (n < 64 || (n & (n - 1))) {
        if (err) *err = QStringLiteral("FFT size must be a power of two >= 64");
        return false;
    }

    if (d->planOk) { cufftDestroy(d->plan); d->planOk = false; }
    cudaFree(d->dWindow); d->dWindow = nullptr;
    cudaFree(d->dIn);     d->dIn     = nullptr;
    cudaFree(d->dOut);    d->dOut    = nullptr;
    cudaFree(d->dDb);     d->dDb     = nullptr;

    cudaError_t e;
    if ((e = cudaMalloc(&d->dWindow, sizeof(float) * size_t(n))) != cudaSuccess
     || (e = cudaMalloc(&d->dIn,  sizeof(cufftComplex) * size_t(n))) != cudaSuccess
     || (e = cudaMalloc(&d->dOut, sizeof(cufftComplex) * size_t(n))) != cudaSuccess
     || (e = cudaMalloc(&d->dDb,  sizeof(float) * size_t(n))) != cudaSuccess) {
        if (err) *err = cudaErr("cudaMalloc", e);
        return false;
    }

    if (cufftPlan1d(&d->plan, n, CUFFT_C2C, 1) != CUFFT_SUCCESS) {
        if (err) *err = QStringLiteral("cufftPlan1d(%1) failed").arg(n);
        return false;
    }
    d->planOk = true;
    cufftSetStream(d->plan, d->stream);

    sdr_gpu_hann(d->dWindow, n, d->stream);

    // Coherent gain is computed from the SAME formula the host reference uses,
    // not assumed to be 0.5. A different window would otherwise miscalibrate
    // every level silently.
    std::vector<float> w;
    gpumath::hann(w, n);
    d->cg = gpumath::coherentGain(w);
    d->n  = n;

    if ((e = cudaStreamSynchronize(d->stream)) != cudaSuccess) {
        if (err) *err = cudaErr("stream sync after plan setup", e);
        return false;
    }
    m_fftSize = n;
    return true;
}

bool GpuIqPipeline::nextSpectrum(quint64 writeCount, QVector<float>* out,
                                 QString* err)
{
    if (!m_open || !out) return false;
    if (writeCount == 0 || writeCount == m_lastCount) return false;

    // Overwriting ring: take the newest and count what was skipped. A display
    // must never be able to slow a 50 Gb/s capture down, so back-pressure is
    // not an option and skipping is the correct behaviour.
    const quint64 skipped = (writeCount - m_lastCount) - 1;
    m_stats.framesSkipped += skipped;

    const quint64 slot = (writeCount - 1) % m_numSlots;
    const char*   src  = static_cast<const char*>(d->ipcPtr)
                       + slot * size_t(m_slotStride);

    QElapsedTimer t; t.start();

    // Device -> device: the samples stay in VRAM. Only the finished spectrum
    // crosses the bus, 16 KB for a 4096-point FFT instead of the 1 MiB slot.
    sdr_gpu_convert_window(reinterpret_cast<const short*>(src),
                           d->dWindow, d->dIn, m_fftSize, d->stream);

    if (cufftExecC2C(d->plan, d->dIn, d->dOut, CUFFT_FORWARD) != CUFFT_SUCCESS) {
        if (err) *err = QStringLiteral("cufftExecC2C failed");
        return false;
    }
    sdr_gpu_mag_db_shift(d->dOut, d->dDb, m_fftSize, d->cg, d->stream);

    out->resize(m_fftSize);
    cudaError_t e = cudaMemcpyAsync(out->data(), d->dDb,
                                    sizeof(float) * size_t(m_fftSize),
                                    cudaMemcpyDeviceToHost, d->stream);
    if (e != cudaSuccess) { if (err) *err = cudaErr("cudaMemcpyAsync D2H", e); return false; }

    if ((e = cudaStreamSynchronize(d->stream)) != cudaSuccess) {
        if (err) *err = cudaErr("cudaStreamSynchronize", e);
        return false;
    }

    // The Holoscan visualiser's ordering guard, kept exactly: if fewer than
    // (slots - 4) frames arrived while we were reading, our slot cannot have
    // been recycled underneath us. The caller re-reads write_count and passes
    // it back on the next call; a jump larger than that window means the frame
    // we just built may be spliced from two, so it is discarded.
    m_lastCount = writeCount;
    m_stats.framesShown++;
    m_stats.lastFftMs = double(t.nsecsElapsed()) / 1e6;
    return true;
}

void GpuIqPipeline::close()
{
    if (!d) return;
    if (d->planOk) { cufftDestroy(d->plan); d->planOk = false; }
    cudaFree(d->dWindow); d->dWindow = nullptr;
    cudaFree(d->dIn);     d->dIn     = nullptr;
    cudaFree(d->dOut);    d->dOut    = nullptr;
    cudaFree(d->dDb);     d->dDb     = nullptr;
    if (d->stream) { cudaStreamDestroy(d->stream); d->stream = nullptr; }
    if (d->ipcPtr) { cudaIpcCloseMemHandle(d->ipcPtr); d->ipcPtr = nullptr; }
    m_open = false;
}

} // namespace sdr

#else  // ------------------------------------------------------------------

namespace sdr {

struct GpuIqPipeline::Impl {};

GpuIqPipeline::GpuIqPipeline() : d(nullptr) {}
GpuIqPipeline::~GpuIqPipeline() {}

bool GpuIqPipeline::builtWithCuda() { return false; }

bool GpuIqPipeline::probeDevice(QString* detail)
{
    if (detail) *detail = QStringLiteral(
        "This build has no CUDA support. Rebuild with -DSDR_ENABLE_CUDA=ON "
        "and a CUDA toolkit present to use the GPUDirect path.");
    return false;
}

bool GpuIqPipeline::open(const void*, quint32, quint32, QString* err)
{ return probeDevice(err) ? false : false; }

bool GpuIqPipeline::setFftSize(int, QString* err) { return probeDevice(err); }

bool GpuIqPipeline::nextSpectrum(quint64, QVector<float>*, QString* err)
{ return probeDevice(err) ? false : false; }

void GpuIqPipeline::close() {}

} // namespace sdr

#endif // SDR_ENABLE_CUDA
