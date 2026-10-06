// ---------------------------------------------------------------------------
// gpu_selftest — prove the GPU FFT path on YOUR hardware.
//
// I could not run CUDA where this was written: no nvcc, no GPU. This is the
// test that closes that gap. It runs the SAME input through:
//
//   CPU : window multiply -> radix-2 FFT -> |X|^2       (DspEngine's path)
//   GPU : GpuFft::powerSpectrum()                        (the offload path)
//
// and compares them in dB. Because DspEngine applies identical post-processing
// to both, agreement here means the display cannot change when you toggle
// CPU/GPU.
//
// Build (from the GUI source root) — one line:
//   nvcc -O2 -std=c++17 -DSDR_ENABLE_CUDA -Isrc/core tools/gpu_selftest.cpp
//        src/core/GpuFft.cpp src/core/GpuKernels.cu
//        $(pkg-config --cflags --libs Qt5Core) -lcufft -o gpu_selftest
//   ./gpu_selftest
// (or use tools/build_gpu_selftest.sh, which does exactly this)
//
// Leave it running and watch nvtop: gpu_selftest must appear as a GPU process.
// ---------------------------------------------------------------------------

#include "GpuFft.h"

#include <QElapsedTimer>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

using cf = std::complex<float>;

static void cpuFft(std::vector<cf>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t b = n >> 1;
        for (; j & b; b >>= 1) j ^= b;
        j ^= b;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * float(M_PI) / float(len);
        const cf wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cf w(1, 0);
            for (size_t k = 0; k < len / 2; ++k) {
                const cf u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

static double toDb(float p) { return 10.0 * std::log10(p > 1e-20f ? p : 1e-20f); }

int main()
{
    sdr::GpuFft gpu;
    QString detail;
    if (!gpu.init(&detail)) {
        std::printf("GPU INIT FAILED\n  %s\n", qPrintable(detail));
        return 1;
    }
    std::printf("%s\n\n", qPrintable(detail));

    int fails = 0;
    for (int n : {1024, 2048, 4096, 8192}) {
        // Blackman-Harris, as the GUI uses by default.
        std::vector<float> w(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            const double x = 2.0 * M_PI * i / (n - 1);
            w[size_t(i)] = float(0.35875 - 0.48829 * std::cos(x)
                               + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x));
        }

        // Two tones plus low-level noise: exercises peak AND floor.
        std::vector<cf> in(static_cast<size_t>(n));
        unsigned r = 12345u;
        for (int i = 0; i < n; ++i) {
            const double t1 = 2 * M_PI * (n / 16.0) * i / n;
            const double t2 = 2 * M_PI * (n / 5.3)  * i / n;
            r = r * 1103515245u + 12345u;
            const float nz = (float((r >> 8) & 0xFFFF) / 65536.0f - 0.5f) * 1e-4f;
            in[size_t(i)] = cf(float(0.7 * std::cos(t1) + 0.01 * std::cos(t2)) + nz,
                               float(0.7 * std::sin(t1) + 0.01 * std::sin(t2)) + nz);
        }

        std::vector<cf> c(in);
        for (int i = 0; i < n; ++i) c[size_t(i)] *= w[size_t(i)];
        cpuFft(c);

        std::vector<float> gp(static_cast<size_t>(n));
        QString err;
        if (!gpu.setWindow(w.data(), size_t(n), &err)
         || !gpu.powerSpectrum(in.data(), size_t(n), gp.data(), &err)) {
            std::printf("  N=%-5d GPU FAILED: %s\n", n, qPrintable(err));
            ++fails;
            continue;
        }

        double worst = 0.0;
        int    peakCpu = 0, peakGpu = 0;
        for (int i = 0; i < n; ++i) {
            const double pc = double(c[size_t(i)].real()) * c[size_t(i)].real()
                            + double(c[size_t(i)].imag()) * c[size_t(i)].imag();
            const double dc = toDb(float(pc));
            const double dg = toDb(gp[size_t(i)]);
            // Compare only bins above -100 dB: below that, float rounding in
            // two different FFT algorithms legitimately differs and carries no
            // meaning for the display.
            if (dc > -100.0) worst = std::max(worst, std::fabs(dc - dg));
            if (pc > double(c[size_t(peakCpu)].real()) * c[size_t(peakCpu)].real()
                   + double(c[size_t(peakCpu)].imag()) * c[size_t(peakCpu)].imag())
                peakCpu = i;
            if (gp[size_t(i)] > gp[size_t(peakGpu)]) peakGpu = i;
        }

        const bool ok = worst < 0.01 && peakCpu == peakGpu;
        if (!ok) ++fails;
        std::printf("  N=%-5d  max |CPU-GPU| = %.5f dB   peak bin CPU %-5d GPU %-5d  %s\n",
                    n, worst, peakCpu, peakGpu, ok ? "PASS" : "** FAIL **");
    }

    // Throughput: gives nvtop something to show, and a real number.
    std::printf("\nTiming 5000 x 4096-point GPU FFTs (watch nvtop now)...\n");
    std::vector<float> w(4096, 1.0f);
    std::vector<cf>    in(4096, cf(0.5f, 0.0f));
    std::vector<float> out(4096);
    QString err;
    gpu.setWindow(w.data(), 4096, &err);
    QElapsedTimer t; t.start();
    for (int i = 0; i < 5000; ++i) gpu.powerSpectrum(in.data(), 4096, out.data(), &err);
    const double ms = double(t.nsecsElapsed()) / 1e6;
    std::printf("  %.2f ms total, %.1f us per FFT incl. upload + download\n",
                ms, ms * 1000.0 / 5000.0);

    std::printf("\n%s\n", fails ? "RESULT: FAIL — do not enable GPU offload"
                                : "RESULT: PASS — GPU offload matches CPU");
    return fails ? 1 : 0;
}
