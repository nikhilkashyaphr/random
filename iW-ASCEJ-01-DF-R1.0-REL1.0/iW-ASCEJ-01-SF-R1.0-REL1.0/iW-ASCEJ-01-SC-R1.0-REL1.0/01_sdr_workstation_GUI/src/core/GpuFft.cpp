// ---------------------------------------------------------------------------
// GpuFft — host side. Only GpuKernels.cu needs nvcc.
// ---------------------------------------------------------------------------

#include "GpuFft.h"

#ifdef SDR_ENABLE_CUDA

#include <cuda_runtime.h>
#include <cufft.h>
#include <QElapsedTimer>
#include <algorithm>
#include <cstring>
#include <vector>

extern "C" {
void sdr_gpu_window_cf32(const cufftComplex* in, const float* w,
                         cufftComplex* out, int n, cudaStream_t s);
void sdr_gpu_power(const cufftComplex* in, float* p, int n, cudaStream_t s);
}

namespace sdr {

struct GpuFft::Impl {
    cudaStream_t  stream = nullptr;
    cufftHandle   plan   = 0;
    bool          planOk = false;
    int           n      = 0;

    cufftComplex* dIn    = nullptr;   // uploaded samples
    cufftComplex* dWin   = nullptr;   // windowed, FFT in place
    float*        dW     = nullptr;   // window coefficients
    float*        dPow   = nullptr;   // |X|^2

    // Pinned host staging: pageable memory makes every copy go through a
    // bounce buffer, roughly halving effective PCIe bandwidth.
    cufftComplex* hIn    = nullptr;
    float*        hPow   = nullptr;

    std::vector<float> lastWindow;    // to skip redundant uploads

    void freeBuffers()
    {
        if (planOk) { cufftDestroy(plan); planOk = false; }
        cudaFree(dIn);  dIn  = nullptr;
        cudaFree(dWin); dWin = nullptr;
        cudaFree(dW);   dW   = nullptr;
        cudaFree(dPow); dPow = nullptr;
        cudaFreeHost(hIn);  hIn  = nullptr;
        cudaFreeHost(hPow); hPow = nullptr;
        n = 0;
        lastWindow.clear();
    }
};

namespace {
QString cerr(const char* what, cudaError_t e)
{
    return QStringLiteral("%1: %2").arg(QLatin1String(what),
                                        QLatin1String(cudaGetErrorString(e)));
}
} // namespace

GpuFft::GpuFft() : d(new Impl) {}

GpuFft::~GpuFft()
{
    if (d) {
        d->freeBuffers();
        if (d->stream) cudaStreamDestroy(d->stream);
        delete d;
    }
}

bool GpuFft::builtWithCuda() { return true; }

bool GpuFft::init(QString* detail)
{
    if (m_ready) { if (detail) *detail = QStringLiteral("GPU FFT active on %1").arg(m_device); return true; }

    int count = 0;
    cudaError_t e = cudaGetDeviceCount(&count);
    if (e != cudaSuccess || count == 0) {
        if (detail) *detail = QStringLiteral("No CUDA device: %1")
                                  .arg(QLatin1String(cudaGetErrorString(e)));
        return false;
    }
    if ((e = cudaSetDevice(0)) != cudaSuccess) {
        if (detail) *detail = cerr("cudaSetDevice(0)", e);
        return false;
    }

    cudaDeviceProp p{};
    cudaGetDeviceProperties(&p, 0);
    m_device = QLatin1String(p.name);

    // Creating the stream is what establishes the CUDA context — and it is
    // the context that makes this process appear in nvtop / nvidia-smi.
    if ((e = cudaStreamCreateWithFlags(&d->stream, cudaStreamNonBlocking)) != cudaSuccess) {
        if (detail) *detail = cerr("cudaStreamCreate", e);
        return false;
    }

    m_ready = true;
    if (detail) *detail = QStringLiteral("GPU FFT active on %1 (%2 SMs)")
                              .arg(m_device).arg(p.multiProcessorCount);
    return true;
}

bool GpuFft::setWindow(const float* w, std::size_t n, QString* err)
{
    if (!m_ready) { if (err) *err = QStringLiteral("GPU not initialised"); return false; }
    if (n < 2 || (n & (n - 1))) {
        if (err) *err = QStringLiteral("FFT size must be a power of two");
        return false;
    }

    // Reallocate only when the size changes.
    if (int(n) != d->n) {
        d->freeBuffers();
        cudaError_t e;
        if ((e = cudaMalloc(&d->dIn,  sizeof(cufftComplex) * n)) != cudaSuccess
         || (e = cudaMalloc(&d->dWin, sizeof(cufftComplex) * n)) != cudaSuccess
         || (e = cudaMalloc(&d->dW,   sizeof(float) * n))        != cudaSuccess
         || (e = cudaMalloc(&d->dPow, sizeof(float) * n))        != cudaSuccess
         || (e = cudaMallocHost(&d->hIn,  sizeof(cufftComplex) * n)) != cudaSuccess
         || (e = cudaMallocHost(&d->hPow, sizeof(float) * n))        != cudaSuccess) {
            if (err) *err = cerr("allocating GPU buffers", e);
            d->freeBuffers();
            return false;
        }
        if (cufftPlan1d(&d->plan, int(n), CUFFT_C2C, 1) != CUFFT_SUCCESS) {
            if (err) *err = QStringLiteral("cufftPlan1d(%1) failed").arg(n);
            d->freeBuffers();
            return false;
        }
        d->planOk = true;
        cufftSetStream(d->plan, d->stream);
        d->n = int(n);
    }

    // Upload the window only if it changed — DspEngine calls this every frame.
    if (d->lastWindow.size() == n &&
        std::equal(d->lastWindow.begin(), d->lastWindow.end(), w))
        return true;

    cudaError_t e = cudaMemcpyAsync(d->dW, w, sizeof(float) * n,
                                    cudaMemcpyHostToDevice, d->stream);
    if (e != cudaSuccess) { if (err) *err = cerr("uploading window", e); return false; }
    d->lastWindow.assign(w, w + n);
    return true;
}

bool GpuFft::powerSpectrum(const std::complex<float>* in, std::size_t n,
                           float* powerOut, QString* err)
{
    if (!m_ready || int(n) != d->n || !d->planOk) {
        if (err) *err = QStringLiteral("GPU FFT not configured for size %1").arg(n);
        return false;
    }

    QElapsedTimer t; t.start();

    // std::complex<float> and cufftComplex share layout (two floats, re then
    // im); the standard guarantees it for std::complex, and cufftComplex is
    // float2. Staging through pinned memory for full-speed DMA.
    std::memcpy(d->hIn, in, sizeof(cufftComplex) * n);

    cudaError_t e = cudaMemcpyAsync(d->dIn, d->hIn, sizeof(cufftComplex) * n,
                                    cudaMemcpyHostToDevice, d->stream);
    if (e != cudaSuccess) { if (err) *err = cerr("upload samples", e); return false; }

    sdr_gpu_window_cf32(d->dIn, d->dW, d->dWin, int(n), d->stream);

    if (cufftExecC2C(d->plan, d->dWin, d->dWin, CUFFT_FORWARD) != CUFFT_SUCCESS) {
        if (err) *err = QStringLiteral("cufftExecC2C failed");
        return false;
    }

    sdr_gpu_power(d->dWin, d->dPow, int(n), d->stream);

    e = cudaMemcpyAsync(d->hPow, d->dPow, sizeof(float) * n,
                        cudaMemcpyDeviceToHost, d->stream);
    if (e != cudaSuccess) { if (err) *err = cerr("download power", e); return false; }

    if ((e = cudaStreamSynchronize(d->stream)) != cudaSuccess) {
        if (err) *err = cerr("stream synchronise", e);
        return false;
    }
    // A kernel launch failure is only reported on the next sync, so check it
    // here rather than returning success over a failed launch.
    if ((e = cudaGetLastError()) != cudaSuccess) {
        if (err) *err = cerr("kernel launch", e);
        return false;
    }

    std::memcpy(powerOut, d->hPow, sizeof(float) * n);
    m_lastMs = double(t.nsecsElapsed()) / 1e6;
    ++m_calls;
    return true;
}

} // namespace sdr

#else  // ------------------------------------------------------------------

namespace sdr {

struct GpuFft::Impl {};
GpuFft::GpuFft() {}
GpuFft::~GpuFft() {}
bool GpuFft::builtWithCuda() { return false; }

bool GpuFft::init(QString* detail)
{
    if (detail) *detail = QStringLiteral(
        "GPU offload unavailable: this build has no CUDA. Rebuild with "
        "-DSDR_ENABLE_CUDA=ON (needs nvcc). Running on CPU.");
    return false;
}
bool GpuFft::setWindow(const float*, std::size_t, QString* err)
{ return init(err); }
bool GpuFft::powerSpectrum(const std::complex<float>*, std::size_t, float*, QString* err)
{ return init(err); }

} // namespace sdr

#endif
