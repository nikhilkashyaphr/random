#pragma once
#include "Types.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace sdr::dsp {

constexpr double kPi = 3.14159265358979323846;

/// In-place iterative radix-2 decimation-in-time FFT.
/// Size must be a power of two. Replace with FFTW / cuFFT / MKL later — the
/// call site only depends on this signature.
inline void fftInPlace(std::vector<cf32>& a)
{
    const std::size_t n = a.size();
    if (n < 2) return;

    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }

    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const cf32 wLen(static_cast<float>(std::cos(ang)),
                        static_cast<float>(std::sin(ang)));
        for (std::size_t i = 0; i < n; i += len) {
            cf32 w(1.0f, 0.0f);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const cf32 u = a[i + k];
                const cf32 v = a[i + k + len / 2] * w;
                a[i + k]           = u + v;
                a[i + k + len / 2] = u - v;
                w *= wLen;
            }
        }
    }
}

/// Zeroth-order modified Bessel function, series form. Converges quickly for
/// the argument range Kaiser windows need.
inline double besselI0(double x)
{
    double sum = 1.0, term = 1.0;
    const double halfSq = (x * x) / 4.0;
    for (int k = 1; k < 60; ++k) {
        term *= halfSq / (static_cast<double>(k) * static_cast<double>(k));
        sum  += term;
        if (term < sum * 1e-14) break;
    }
    return sum;
}

/// Window function library. Each is normalised only by its own definition;
/// the caller applies coherent-gain correction from windowSum().
inline std::vector<float> makeWindow(WindowType type, std::size_t n)
{
    std::vector<float> w(n, 1.0f);
    if (n < 2) return w;

    const double den = static_cast<double>(n - 1);
    for (std::size_t i = 0; i < n; ++i) {
        const double r = static_cast<double>(i) / den;   // 0..1
        const double t = 2.0 * kPi * r;
        double v = 1.0;
        switch (type) {
        case WindowType::Rectangular:
            v = 1.0;
            break;
        case WindowType::Hann:
            v = 0.5 - 0.5 * std::cos(t);
            break;
        case WindowType::Hamming:
            v = 0.54 - 0.46 * std::cos(t);
            break;
        case WindowType::Blackman:
            v = 0.42 - 0.5 * std::cos(t) + 0.08 * std::cos(2 * t);
            break;
        case WindowType::BlackmanHarris:
            v = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t)
              - 0.01168 * std::cos(3 * t);
            break;
        case WindowType::FlatTop:
            // Amplitude-accurate window: use it when peak level matters more
            // than resolving two nearby tones.
            v = 0.21557895 - 0.41663158 * std::cos(t) + 0.277263158 * std::cos(2 * t)
              - 0.083578947 * std::cos(3 * t) + 0.006947368 * std::cos(4 * t);
            break;
        case WindowType::Kaiser: {
            constexpr double beta = 8.6;
            const double x = 2.0 * r - 1.0;
            v = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - x * x))) / besselI0(beta);
            break;
        }
        }
        w[i] = static_cast<float>(v);
    }
    return w;
}

/// Coherent gain: sum of the window. A full-scale tone lands at 0 dBFS when
/// the FFT magnitude is divided by this.
inline float windowSum(const std::vector<float>& w)
{
    return std::accumulate(w.begin(), w.end(), 0.0f);
}

/// Equivalent noise bandwidth in bins — needed to report a noise floor in
/// dBFS/Hz rather than dBFS/bin.
inline double windowEnbw(const std::vector<float>& w)
{
    double s1 = 0.0, s2 = 0.0;
    for (float v : w) { s1 += v; s2 += static_cast<double>(v) * v; }
    return s1 > 0.0 ? (w.size() * s2) / (s1 * s1) : 1.0;
}

/// Move DC from bin 0 to the centre of the array.
template <typename T>
inline void fftShift(std::vector<T>& v)
{
    if (v.size() < 2) return;
    std::rotate(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
}

inline float toDbfs(float magSq, float scale)
{
    const float m = magSq * scale;
    return 10.0f * std::log10(m > 1e-20f ? m : 1e-20f);
}

/// Local maxima above `floorDb + threshDb`, strongest first, each separated
/// from every stronger accepted peak by `minSepBins`. The separation rule is
/// what stops one broad carrier from filling the whole peak table with points
/// off its own skirt.
inline std::vector<Peak> findPeaks(const std::vector<float>& spec,
                                   double centreHz, double spanHz,
                                   double floorDb, double threshDb,
                                   int maxCount, int minSepBins)
{
    std::vector<Peak> out;
    const std::size_t n = spec.size();
    if (n < 3 || maxCount <= 0) return out;

    const double binHz = spanHz / static_cast<double>(n);
    const double gate  = floorDb + threshDb;

    std::vector<std::size_t> cand;
    for (std::size_t i = 1; i + 1 < n; ++i)
        if (spec[i] >= spec[i - 1] && spec[i] > spec[i + 1] && spec[i] > gate)
            cand.push_back(i);

    std::sort(cand.begin(), cand.end(),
              [&spec](std::size_t a, std::size_t b) { return spec[a] > spec[b]; });

    std::vector<std::size_t> taken;
    for (std::size_t i : cand) {
        if (static_cast<int>(taken.size()) >= maxCount) break;
        bool tooClose = false;
        for (std::size_t t : taken)
            if (std::abs(static_cast<long long>(i) - static_cast<long long>(t)) < minSepBins) {
                tooClose = true;
                break;
            }
        if (tooClose) continue;
        taken.push_back(i);

        // Parabolic interpolation across the three bins around the peak gives
        // a frequency estimate finer than the bin spacing.
        const double ym1 = spec[i - 1], y0 = spec[i], yp1 = spec[i + 1];
        const double denom = (ym1 - 2.0 * y0 + yp1);
        double delta = 0.0;
        if (std::abs(denom) > 1e-12) delta = 0.5 * (ym1 - yp1) / denom;
        delta = std::clamp(delta, -0.5, 0.5);

        Peak p;
        p.freqHz    = centreHz + ((static_cast<double>(i) + delta) - n / 2.0) * binHz;
        p.levelDbfs = y0 - 0.25 * (ym1 - yp1) * delta;
        out.push_back(p);
    }
    return out;
}

} // namespace sdr::dsp
