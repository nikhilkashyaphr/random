#pragma once
// ---------------------------------------------------------------------------
// GpuFft — the spectrum's heavy step, offloaded to the GPU.
//
// WHERE IT SITS
// -------------
// DspEngine::computeSpectrum() does, per displayed frame:
//
//     window multiply  ->  FFT  ->  |X|^2  ->  dBFS, fftshift, averaging
//     \_______ GpuFft does this ______/    \_____ unchanged, on CPU _____/
//
// The GPU returns |X|^2 and nothing else. Everything downstream — the dBFS
// scaling, coherent-gain correction, fftshift, and all five averaging modes —
// is the SAME code that runs in CPU mode. So toggling CPU/GPU cannot move a
// reading: the only thing that changes is which processor did the FFT.
//
// It works for EVERY source (PCIe, RoCEv2 host ring, RoCEv2 GPU ring, files,
// the simulator), because it operates on the decoded samples DspEngine already
// has. That also means GPU offload can be verified with no network hardware.
//
// WHY THIS RATHER THAN A FINISHED SPECTRUM FROM THE GPU
// -----------------------------------------------------
// An earlier design computed dBFS on the GPU with a fixed Hann window. The
// GUI's DspEngine supports several windows and five averaging modes with
// their own calibration. Duplicating all of that in CUDA would give two
// implementations to keep in step — and when they drift the display changes
// on toggle, which looks like hardware and gets investigated as hardware.
// Here the window coefficients come FROM DspEngine, so there is only one.
// ---------------------------------------------------------------------------

#include <QString>
#include <complex>
#include <cstddef>

namespace sdr {

class GpuFft
{
public:
    GpuFft();
    ~GpuFft();
    GpuFft(const GpuFft&) = delete;
    GpuFft& operator=(const GpuFft&) = delete;

    /// True if this binary was compiled with CUDA support.
    static bool builtWithCuda();

    /// Bring up the device. Safe to call repeatedly. `detail` always receives
    /// a human-readable status — shown in the UI so the operator can see
    /// whether GPU offload is genuinely active.
    bool init(QString* detail);
    bool ready() const { return m_ready; }

    /// Upload the window. Only re-uploaded when it actually changes.
    bool setWindow(const float* w, std::size_t n, QString* err);

    /// in (host, n complex) -> power |FFT(in * window)|^2 (host, n floats,
    /// NOT fftshifted — DspEngine does that, as it does on the CPU path).
    bool powerSpectrum(const std::complex<float>* in, std::size_t n,
                       float* powerOut, QString* err);

    QString deviceName() const { return m_device; }
    double  lastMs()     const { return m_lastMs; }
    quint64 calls()      const { return m_calls; }

private:
    bool    m_ready = false;
    QString m_device;
    double  m_lastMs = 0.0;
    quint64 m_calls  = 0;

    struct Impl;          // keeps CUDA types out of this header
    Impl*   d = nullptr;
};

} // namespace sdr
