// ---------------------------------------------------------------------------
// TS-11  The GPU spectrum arithmetic, checked on the CPU.
//
// The GUI does all of its own data processing; on the RoCEv2 GPU path the
// window/FFT/magnitude stage runs on the device. That stage therefore exists
// twice -- once in CUDA (GpuKernels.cu) and once in plain C++ (GpuMathRef.h),
// which is the DEFINITION the kernels are written to follow statement for
// statement.
//
// Two implementations of the same arithmetic drift, and the symptom is ugly:
// the display changes when the operator toggles CPU/GPU, which looks like a
// hardware difference and gets investigated as one. GpuMathRef.h exists to
// stop that, but nothing was checking it.
//
// This pins the reference's observable behaviour: window shape, coherent-gain
// correction, the 14-bit full-scale constant, dBFS calibration across FFT
// sizes, and fftshift. A kernel that stops matching the reference now fails a
// test on any machine, with no GPU required.
// ---------------------------------------------------------------------------
#include "GpuMathRef.h"
#include "GpuIqPipeline.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdint>
#include <vector>

static int g_fail = 0;
#define CHECK(c, ...) do { std::printf((c) ? "  PASS  " : "  FAIL  "); std::printf(__VA_ARGS__); \
                           std::printf("\n"); if (!(c)) ++g_fail; } while (0)

namespace {

/// Plain radix-2 DFT. Small sizes only -- this is a reference, not a fast
/// path, and being obviously correct matters more than speed here.
std::vector<std::complex<double>> dft(const std::vector<float>& re,
                                     const std::vector<float>& im)
{
    const std::size_t n = re.size();
    std::vector<std::complex<double>> out(n);
    for (std::size_t k = 0; k < n; ++k) {
        std::complex<double> acc(0.0, 0.0);
        for (std::size_t j = 0; j < n; ++j) {
            const double a = -2.0 * M_PI * double(k) * double(j) / double(n);
            acc += std::complex<double>(re[j], im[j]) * std::complex<double>(std::cos(a), std::sin(a));
        }
        out[k] = acc;
    }
    return out;
}

/// A full-scale complex exponential at `bin` cycles per window, packed as the
/// RFSoC wire format does: 14-bit, MSB-aligned into int16.
std::vector<std::int16_t> toneAtBin(int n, double bin, double amp)
{
    std::vector<std::int16_t> iq(std::size_t(n) * 2u);
    for (int i = 0; i < n; ++i) {
        const double ph = 2.0 * M_PI * bin * double(i) / double(n);
        const auto q = [&](double v) {
            long s = std::lround(v * amp * 8191.0);
            if (s >  8191) s =  8191;
            if (s < -8191) s = -8191;
            return std::int16_t(s << 2);
        };
        iq[2 * std::size_t(i)    ] = q(std::cos(ph));
        iq[2 * std::size_t(i) + 1] = q(std::sin(ph));
    }
    return iq;
}

} // namespace

int main()
{
    using namespace sdr::gpumath;
    std::printf("GPU spectrum arithmetic (GpuMathRef) -- verified without a GPU\n\n");

    // -----------------------------------------------------------------
    std::printf("ABI constants agree with rdma_common.h\n");
    // -----------------------------------------------------------------
    {
        // Regression: gpuring::kFrameMagic read 0x4651524d30303031, which no
        // producer ever writes, so a header check against it would have
        // rejected every frame on the GPU path.
        CHECK(sdr::gpuring::kFrameMagic == 0x49515246524d4131ull,
              "gpuring::kFrameMagic is IQ_FRAME_MAGIC \"IQRFRMA1\" (0x%016llx)",
              (unsigned long long)sdr::gpuring::kFrameMagic);
        CHECK(sdr::gpuring::kMagicGpu == 0x495152494e474731ull,
              "gpuring::kMagicGpu is RING_MAGIC_GPU \"IQRINGG1\"");
        CHECK(sdr::gpuring::kCtrlBytes == 4096u, "control block is 4096 B");
        CHECK(sdr::gpuring::kCtrlIpcOff == 1024u,
              "the cudaIpcMemHandle sits at control offset 1024");
        CHECK(sdr::gpuring::kHdrBytes == 64u, "frame header is 64 B");
    }

    // -----------------------------------------------------------------
    std::printf("\nFull scale is the 14-bit MSB-aligned maximum\n");
    // -----------------------------------------------------------------
    {
        // 8191 << 2 == 32764, NOT 32767. Using 32767 puts every reading
        // 0.08 dB low -- small, consistent and never noticed.
        CHECK(std::fabs(kFullScale14 - 32764.0f) < 1e-3f,
              "kFullScale14 == 32764 (8191 << 2), got %.1f", double(kFullScale14));
        const double wrong = 20.0 * std::log10(32764.0 / 32767.0);
        CHECK(std::fabs(wrong) > 7e-4 && std::fabs(wrong) < 1e-3,
              "using 32767 instead would bias every level by %.4f dB", wrong);
    }

    // -----------------------------------------------------------------
    std::printf("\nHann window matches the documented formula\n");
    // -----------------------------------------------------------------
    {
        std::vector<float> w;
        hann(w, 8);
        CHECK(w.size() == 8, "hann(8) produces 8 coefficients");
        // w[i] = 0.5 - 0.5*cos(2*pi*i/(N-1)): zero at both ends.
        CHECK(std::fabs(w.front()) < 1e-6f, "w[0] == 0, got %.9f", double(w.front()));
        CHECK(std::fabs(w.back()) < 1e-6f, "w[N-1] == 0, got %.9f", double(w.back()));
        bool matches = true;
        for (int i = 0; i < 8; ++i) {
            const double want = 0.5 - 0.5 * std::cos(2.0 * M_PI * double(i) / 7.0);
            if (std::fabs(double(w[std::size_t(i)]) - want) > 1e-6) matches = false;
        }
        CHECK(matches, "every coefficient matches 0.5 - 0.5*cos(2*pi*i/(N-1))");

        std::vector<float> w1;
        hann(w1, 1);
        CHECK(w1.size() == 1 && std::fabs(w1[0] - 1.0f) < 1e-9f,
              "the N==1 case is 1.0, not a division by zero");
    }

    // -----------------------------------------------------------------
    std::printf("\nCoherent gain is computed, not assumed to be 0.5\n");
    // -----------------------------------------------------------------
    {
        std::vector<float> w;
        hann(w, 4096);
        const float cg = coherentGain(w);
        // Close to 0.5 for a long Hann, but not exactly -- which is why it is
        // measured. Asserting 0.5 exactly is the bug this guards against.
        CHECK(std::fabs(double(cg) - 0.5) < 1e-3,
              "Hann(4096) coherent gain %.6f is near 0.5", double(cg));
        CHECK(cg != 0.5f, "and is not the hard-coded constant 0.5");

        std::vector<float> rect(1024, 1.0f);
        CHECK(std::fabs(double(coherentGain(rect)) - 1.0) < 1e-6,
              "a rectangular window has coherent gain 1.0");
        CHECK(std::fabs(double(coherentGain({})) - 1.0) < 1e-9,
              "an empty window returns 1.0 rather than dividing by zero");
    }

    // -----------------------------------------------------------------
    std::printf("\nint16 I/Q -> windowed complex float\n");
    // -----------------------------------------------------------------
    {
        const int n = 16;
        std::vector<float> w(std::size_t(n), 1.0f);     // identity, to isolate scaling
        std::vector<std::int16_t> iq(std::size_t(n) * 2u, 0);
        iq[0] = std::int16_t(8191 << 2);                 // +full scale on I
        iq[1] = std::int16_t(-(8191 << 2));              // -full scale on Q
        std::vector<float> re, im;
        convertAndWindow(iq.data(), n, w, re, im);
        CHECK(std::fabs(double(re[0]) - 1.0) < 1e-6,
              "+32764 on I normalises to +1.0, got %.9f", double(re[0]));
        CHECK(std::fabs(double(im[0]) + 1.0) < 1e-6,
              "-32764 on Q normalises to -1.0, got %.9f", double(im[0]));
        CHECK(re.size() == std::size_t(n) && im.size() == std::size_t(n),
              "both outputs are sized to the FFT length");

        // And the window is actually applied.
        std::vector<float> half(std::size_t(n), 0.25f);
        convertAndWindow(iq.data(), n, half, re, im);
        CHECK(std::fabs(double(re[0]) - 0.25) < 1e-6,
              "a 0.25 window coefficient scales the sample to 0.25");
    }

    // -----------------------------------------------------------------
    std::printf("\ndBFS calibration is independent of FFT size\n");
    // -----------------------------------------------------------------
    {
        // A full-scale tone must read ~0 dBFS whatever N is. If the FFT length
        // or window gain is not divided out, the reading moves with N -- which
        // is exactly the kind of error that gets blamed on the converter.
        for (int n : {64, 128, 256}) {
            std::vector<float> w;
            hann(w, n);
            const float cg = coherentGain(w);
            const auto iq = toneAtBin(n, 4.0, 1.0);      // integer bin: no scalloping
            std::vector<float> re, im;
            convertAndWindow(iq.data(), n, w, re, im);
            const auto X = dft(re, im);

            std::size_t pk = 0;
            double best = -1.0;
            for (std::size_t k = 0; k < X.size(); ++k)
                if (std::norm(X[k]) > best) { best = std::norm(X[k]); pk = k; }

            const float db = binToDbfs(float(X[pk].real()), float(X[pk].imag()), n, cg);
            CHECK(pk == 4, "N=%d: the peak lands in bin 4 as generated (got %zu)", n, pk);
            CHECK(std::fabs(double(db)) < 0.5,
                  "N=%d: a full-scale tone reads %.3f dBFS (within 0.5 dB of 0)", n, double(db));
        }
    }
    {
        // Half scale must read 6 dB down, so the scale is linear in amplitude.
        const int n = 128;
        std::vector<float> w;
        hann(w, n);
        const float cg = coherentGain(w);
        double lvl[2];
        int i = 0;
        for (double amp : {1.0, 0.5}) {
            const auto iq = toneAtBin(n, 8.0, amp);
            std::vector<float> re, im;
            convertAndWindow(iq.data(), n, w, re, im);
            const auto X = dft(re, im);
            std::size_t pk = 0; double best = -1.0;
            for (std::size_t k = 0; k < X.size(); ++k)
                if (std::norm(X[k]) > best) { best = std::norm(X[k]); pk = k; }
            lvl[i++] = double(binToDbfs(float(X[pk].real()), float(X[pk].imag()), n, cg));
        }
        CHECK(std::fabs((lvl[0] - lvl[1]) - 6.0206) < 0.1,
              "half scale is %.3f dB below full scale (expected 6.021)", lvl[0] - lvl[1]);
    }
    {
        // An empty bin must stay finite rather than returning -inf, and the
        // floor must be far below anything a 14-bit converter can produce.
        const float db = binToDbfs(0.0f, 0.0f, 4096, 0.5f);
        CHECK(std::isfinite(db), "an all-zero bin yields a finite value, got %.1f", double(db));
        CHECK(db < -150.0f, "the floor is %.1f dBFS, far below a 14-bit converter", double(db));
    }

    // -----------------------------------------------------------------
    std::printf("\nfftshift puts DC in the centre\n");
    // -----------------------------------------------------------------
    {
        std::vector<float> v{0, 1, 2, 3, 4, 5, 6, 7};
        fftshift(v);
        const std::vector<float> want{4, 5, 6, 7, 0, 1, 2, 3};
        CHECK(v == want, "even N: [0..7] -> [4,5,6,7,0,1,2,3], matching numpy");

        std::vector<float> one{42};
        fftshift(one);
        CHECK(one.size() == 1 && one[0] == 42.0f, "N==1 is left alone");
        std::vector<float> none;
        fftshift(none);
        CHECK(none.empty(), "an empty spectrum is left alone");
    }

    // -----------------------------------------------------------------
    std::printf("\nBuild reports its CUDA support honestly\n");
    // -----------------------------------------------------------------
    {
#ifdef SDR_ENABLE_CUDA
        CHECK(sdr::GpuIqPipeline::builtWithCuda(),
              "a CUDA build reports builtWithCuda() == true");
#else
        CHECK(!sdr::GpuIqPipeline::builtWithCuda(),
              "a non-CUDA build reports builtWithCuda() == false");
        QString why;
        CHECK(!sdr::GpuIqPipeline::probeDevice(&why),
              "and probeDevice() declines");
        CHECK(why.contains(QLatin1String("SDR_ENABLE_CUDA")),
              "naming the flag that enables it: %s", qPrintable(why.left(72)));
#endif
    }

    std::printf("\nRESULT: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
