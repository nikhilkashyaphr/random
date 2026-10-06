#pragma once
#include <QAtomicInt>
#include <memory>
#include "Types.h"

namespace sdr { class GpuFft; }
#include <QElapsedTimer>
#include <QObject>

namespace sdr {

/// Runs on its own thread. Turns raw acquisition blocks into everything the
/// plots and the measurement panel need, then throttles delivery to the
/// configured refresh rate so a fast source cannot flood the GUI thread.
class DspEngine : public QObject
{
    Q_OBJECT
public:
    explicit DspEngine(QObject* parent = nullptr);
    // Out of line: m_gpu is a unique_ptr to a type only forward-declared
    // here, so the destructor must live where GpuFft is complete.
    ~DspEngine() override;

    void setBackPressure(const BackPressurePtr& bp) { m_bp = bp; }

    /// Shared "the GUI has not painted the last frame yet" flag. Rendering is
    /// the slowest stage, so the engine asks before it posts rather than
    /// queueing frames the GUI thread has no chance of draining.
    void setRenderGate(const std::shared_ptr<QAtomicInt>& gate) { m_renderGate = gate; }

public slots:
    void applyConfig(const sdr::Config& cfg);
    void processBlock(const sdr::SampleBlock& block);
    void updateStats(const sdr::StreamStats& stats);
    void resetCounters();
    void clearAverages();

signals:
    void frameReady(const sdr::FrameResult& frame);
    /// Emitted when GPU offload engages, fails, or falls back, so the UI
    /// can show the truth rather than the state of a toggle.
    void computeStatus(bool onGpu, const QString& detail);

private:
    /// Per-channel spectrum averaging state. Kept separate so switching the
    /// active channel does not smear one channel's history into another's.
    struct ChannelState {
        std::vector<float>  avg;        // exponential / hold accumulator, dBFS
        std::vector<double> linAcc;     // linear-power accumulator for LinearN
        int                 linCount = 0;
        bool                primed   = false;
    };

    void ensureSized(int channels);
    void applyDdc(IQBlock& block, quint64 startSample) const;
    void computeSpectrum(const IQBlock& in, ChannelState& st, ChannelFrame& out);
    void computeConstellation(const IQBlock& in, ChannelFrame& out) const;
    void computeMetrics(ChannelFrame& out, double displayRateHz, double centerHz) const;
    static std::vector<float> idealLevels(Modulation m);

    Config             m_cfg;
    BackPressurePtr    m_bp;
    std::shared_ptr<QAtomicInt> m_renderGate;
    std::vector<float> m_window;
    std::vector<cf32>  m_scratch;
    IQBlock            m_work;
    mutable double m_lastDisplayRate = 0.0;  ///< for residual-carrier reporting
    std::vector<IQBlock> m_decoded;   ///< scratch for deferred wire decode
    int                m_windowSize = 0;
    WindowType         m_windowType = WindowType::BlackmanHarris;
    float              m_windowGain = 1.0f;

    // GPU offload of window+FFT+|X|^2. Owned here, created lazily on the
    // DSP thread, because a CUDA context is bound to the thread that
    // creates it.
    std::unique_ptr<GpuFft> m_gpu;
    bool               m_gpuWanted = false;
    bool               m_gpuFailed = false;   ///< fell back; do not retry every frame
    std::vector<float> m_power;              ///< |X|^2 from the GPU

    double             m_windowEnbw = 1.0;

    std::vector<ChannelState> m_state;

    StreamStats   m_stats;
    QElapsedTimer m_frameClock;
    qint64        m_minFrameMs   = 33;
    quint64       m_framesOut    = 0;
    quint64       m_framesSkipped= 0;
};

} // namespace sdr
