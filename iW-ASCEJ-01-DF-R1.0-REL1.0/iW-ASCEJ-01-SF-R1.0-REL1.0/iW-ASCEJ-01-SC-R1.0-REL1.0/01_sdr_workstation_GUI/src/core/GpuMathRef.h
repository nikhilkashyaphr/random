#pragma once
// ---------------------------------------------------------------------------
// GpuMathRef — the spectrum maths the CUDA kernels implement, written once in
// plain C++ so it can be TESTED without a GPU.
//
// WHY THIS FILE EXISTS
// --------------------
// Two implementations of the same arithmetic drift. The usual symptom is that
// the display changes when the operator toggles CPU/GPU, which looks like a
// hardware difference and gets investigated as one.
//
// So the arithmetic is defined ONCE, here, and:
//   * the CUDA kernels in GpuIqPipeline.cu follow it statement for statement;
//   * the unit test compares this against the values the Holoscan visualiser
//     produces for the same input, which is the behaviour being reproduced.
//
// Every constant that could differ between implementations is named and
// documented rather than inlined as a magic number.
// ---------------------------------------------------------------------------

#include <cmath>
#include <cstdint>
#include <vector>

namespace sdr { namespace gpumath {

/// Full scale for the RFSoC wire format: 14-bit samples MSB-aligned into
/// int16, so the largest magnitude is 8191<<2, NOT 32767. Using 32767 here
/// would put every reading 0.08 dB low — small, consistent, and exactly the
/// kind of error that is never noticed and never explained.
constexpr float kFullScale14 = 32764.0f;

/// Periodic Hann window, matching numpy/CuPy `hanning(N)` as used by the
/// visualiser: w[i] = 0.5 - 0.5*cos(2*pi*i/(N-1)).
inline void hann(std::vector<float>& w, int n)
{
    w.resize(size_t(n));
    if (n == 1) { w[0] = 1.0f; return; }
    for (int i = 0; i < n; ++i)
        w[size_t(i)] = 0.5f - 0.5f * std::cos(2.0f * float(M_PI) * float(i) / float(n - 1));
}

/// Coherent gain of a window: the DC response of a tone is scaled by this, so
/// it must be divided out or every level reads low. For a periodic Hann it is
/// close to 0.5, but it is computed rather than assumed so a different window
/// cannot silently break the calibration.
inline float coherentGain(const std::vector<float>& w)
{
    double s = 0.0;
    for (float v : w) s += double(v);
    return w.empty() ? 1.0f : float(s / double(w.size()));
}

/// int16 interleaved I/Q -> windowed complex float, normalised to full scale.
/// Mirrors the kernel `k_convert_window`.
inline void convertAndWindow(const int16_t* iq, int n,
                             const std::vector<float>& w,
                             std::vector<float>& reOut,
                             std::vector<float>& imOut)
{
    reOut.resize(size_t(n));
    imOut.resize(size_t(n));
    for (int i = 0; i < n; ++i) {
        const float ww = w[size_t(i)];
        reOut[size_t(i)] = (float(iq[2 * i    ]) / kFullScale14) * ww;
        imOut[size_t(i)] = (float(iq[2 * i + 1]) / kFullScale14) * ww;
    }
}

/// |X|^2 -> dBFS, with the FFT length and window gain divided out so a
/// full-scale tone reads 0 dBFS regardless of FFT size. Mirrors `k_mag_db`.
///
/// The floor keeps log10 finite for empty bins; -200 dBFS is far below
/// anything a 14-bit converter can produce, so it cannot mask real data.
inline float binToDbfs(float re, float im, int n, float cg)
{
    const float scale = 1.0f / (float(n) * cg);
    const float mag   = std::sqrt(re * re + im * im) * scale;
    return 20.0f * std::log10(mag > 1e-10f ? mag : 1e-10f);
}

/// fftshift: move bin 0 to the centre so the axis runs -fs/2 .. +fs/2.
/// Matches `cp.fft.fftshift` for even N.
inline void fftshift(std::vector<float>& v)
{
    const size_t n = v.size();
    if (n < 2) return;
    const size_t h = n / 2;
    std::vector<float> t(v.begin() + long(h), v.end());
    t.insert(t.end(), v.begin(), v.begin() + long(h));
    v.swap(t);
}

}} // namespace sdr::gpumath
