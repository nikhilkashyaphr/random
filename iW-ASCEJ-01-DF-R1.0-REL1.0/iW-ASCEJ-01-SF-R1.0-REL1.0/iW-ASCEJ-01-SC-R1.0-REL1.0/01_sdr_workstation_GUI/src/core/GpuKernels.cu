// ---------------------------------------------------------------------------
// GpuKernels.cu — the only file that needs nvcc.
//
// Every kernel here mirrors GpuMathRef.h statement for statement. That header
// is the single definition of the arithmetic and is unit-tested on the host;
// these are its device translation. If you change one, change both, and re-run
// the reference test — otherwise the display shifts when the operator toggles
// CPU/GPU, which looks like a hardware difference and gets investigated as one.
//
// Reference results the host tests pin down, and these must reproduce:
//   full-scale complex tone ->  0.000 dBFS
//   half-scale complex tone -> -6.020 dBFS
//   level independent of FFT size (1024 / 2048 / 4096)
// ---------------------------------------------------------------------------

#include <cuda_runtime.h>
#include <cufft.h>
#include <math_constants.h>

namespace {

// Must equal sdr::gpumath::kFullScale14. The RFSoC packs 14-bit samples
// MSB-aligned into int16, so full scale is 8191<<2, NOT 32767. Using 32767
// would put every reading 0.08 dB low: small, consistent, never noticed.
__device__ __constant__ float kFullScale14 = 32764.0f;

} // namespace

/// Periodic Hann, matching numpy/CuPy hanning(N) as the Holoscan visualiser
/// uses it: w[i] = 0.5 - 0.5*cos(2*pi*i/(N-1)).
__global__ void k_hann(float* __restrict__ w, int n)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    w[i] = (n == 1) ? 1.0f
                    : 0.5f - 0.5f * __cosf(2.0f * CUDART_PI_F * float(i) / float(n - 1));
}

/// Interleaved int16 I/Q -> windowed, normalised cufftComplex.
///
/// Reads straight from the GPUDirect ring: `iq` is the VRAM the NIC DMA'd
/// into, reached through the CUDA IPC handle. Sample data never touches
/// system RAM.
__global__ void k_convert_window(const short* __restrict__ iq,
                                 const float* __restrict__ w,
                                 cufftComplex* __restrict__ out,
                                 int n)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const float ww = w[i];
    out[i].x = (float(iq[2 * i    ]) / kFullScale14) * ww;
    out[i].y = (float(iq[2 * i + 1]) / kFullScale14) * ww;
}

/// |X| -> dBFS, with the FFT length and window coherent gain divided out so a
/// full-scale tone reads 0 dBFS at any FFT size. Also applies fftshift, so the
/// output runs -fs/2 .. +fs/2 and no second pass is needed.
///
/// The 1e-10 floor keeps log10 finite on empty bins; -200 dBFS is far below
/// anything a 14-bit converter can produce, so it cannot mask real data.
__global__ void k_mag_db_shift(const cufftComplex* __restrict__ in,
                               float* __restrict__ db,
                               int n, float cg)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;

    const int half = n >> 1;
    const int src  = (i < half) ? (i + half) : (i - half);   // fftshift

    const float scale = 1.0f / (float(n) * cg);
    const float mag   = sqrtf(in[src].x * in[src].x + in[src].y * in[src].y) * scale;
    db[i] = 20.0f * __log10f(fmaxf(mag, 1e-10f));
}

// ---------------------------------------------------------------------------
// GpuFft kernels: window multiply and |X|^2.
//
// Deliberately minimal. Everything after |X|^2 (dBFS scaling, coherent-gain
// correction, fftshift, averaging) stays in DspEngine on the CPU, so the CPU
// and GPU paths share that code and cannot disagree about a reading.
// ---------------------------------------------------------------------------
__global__ void k_window_cf32(const cufftComplex* __restrict__ in,
                              const float* __restrict__ w,
                              cufftComplex* __restrict__ out, int n)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const float ww = w[i];
    out[i].x = in[i].x * ww;
    out[i].y = in[i].y * ww;
}

__global__ void k_power(const cufftComplex* __restrict__ in,
                        float* __restrict__ p, int n)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    // re*re + im*im, matching DspEngine's CPU expression exactly.
    p[i] = in[i].x * in[i].x + in[i].y * in[i].y;
}

// --- C entry points, so the host file needs no nvcc -------------------------
extern "C" {

void sdr_gpu_hann(float* w, int n, cudaStream_t s)
{
    const int t = 256;
    k_hann<<<(n + t - 1) / t, t, 0, s>>>(w, n);
}

void sdr_gpu_convert_window(const short* iq, const float* w,
                            cufftComplex* out, int n, cudaStream_t s)
{
    const int t = 256;
    k_convert_window<<<(n + t - 1) / t, t, 0, s>>>(iq, w, out, n);
}

void sdr_gpu_mag_db_shift(const cufftComplex* in, float* db,
                          int n, float cg, cudaStream_t s)
{
    const int t = 256;
    k_mag_db_shift<<<(n + t - 1) / t, t, 0, s>>>(in, db, n, cg);
}

void sdr_gpu_window_cf32(const cufftComplex* in, const float* w,
                         cufftComplex* out, int n, cudaStream_t s)
{
    const int t = 256;
    k_window_cf32<<<(n + t - 1) / t, t, 0, s>>>(in, w, out, n);
}

void sdr_gpu_power(const cufftComplex* in, float* p, int n, cudaStream_t s)
{
    const int t = 256;
    k_power<<<(n + t - 1) / t, t, 0, s>>>(in, p, n);
}

} // extern "C"
