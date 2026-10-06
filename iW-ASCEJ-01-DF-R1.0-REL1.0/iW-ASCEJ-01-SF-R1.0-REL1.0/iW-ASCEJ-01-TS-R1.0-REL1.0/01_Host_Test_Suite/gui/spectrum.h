#pragma once
// Tiny spectrum analyser for the tests: Blackman-Harris window, FFT from the
// GUI's own Fft.h, then peak frequency and spur-free dynamic range.
#include "Fft.h"
#include <cmath>
#include <cstdio>
#include <vector>

namespace t {

struct Spectrum {
    double peakHz   = 0.0;
    double peakDb   = -999.0;
    double sfdrDb   = 0.0;     ///< peak vs largest bin outside +/-guard of it
    double medianDb = 0.0;
};

inline Spectrum analyse(const std::vector<sdr::cf32>& x, double fs, int guard = 8)
{
    using namespace sdr;
    const std::size_t n = x.size();
    std::vector<float> w = dsp::makeWindow(WindowType::BlackmanHarris, n);
    std::vector<cf32> a(n);
    for (std::size_t i = 0; i < n; ++i) a[i] = x[i] * w[i];
    dsp::fftInPlace(a);
    std::vector<double> db(n);
    for (std::size_t i = 0; i < n; ++i)
        db[i] = 10.0 * std::log10(std::norm(a[i]) + 1e-30);
    std::size_t pk = 0;
    for (std::size_t i = 1; i < n; ++i) if (db[i] > db[pk]) pk = i;
    Spectrum s;
    s.peakDb = db[pk];
    const double binHz = fs / double(n);
    s.peakHz = (pk < n / 2 ? double(pk) : double(pk) - double(n)) * binHz;
    double spur = -999.0;
    for (std::size_t i = 0; i < n; ++i) {
        const long d = std::labs(long(i) - long(pk));
        const long dw = std::min<long>(d, long(n) - d);        // circular
        const long dc = std::min<long>(long(i), long(n) - long(i));
        if (dw <= guard || dc <= guard) continue;
        spur = std::max(spur, db[i]);
    }
    s.sfdrDb = s.peakDb - spur;
    std::vector<double> sorted = db;
    std::nth_element(sorted.begin(), sorted.begin() + n / 2, sorted.end());
    s.medianDb = sorted[n / 2];
    return s;
}

} // namespace t
