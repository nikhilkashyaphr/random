#include "DspEngine.h"
#include <limits>
#include <QDebug>
#include <QElapsedTimer>
#include "SampleCodec.h"
#include "Fft.h"
#include "GpuFft.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace sdr {

namespace {
/// Floor for the analysis window so short blocks still measure sensibly.
constexpr std::size_t kMinAnalysisSamples = 16384;
// Points handed to the time plot. With contiguous sampling (stride 1) this
// value sets BOTH the resolution and the visible time window, so it is the
// display's timebase: window = kMaxTimePoints / Fs.
//
// 2048 points at 200 MSPS spans 10.2 us, which packs ~102 cycles of a 10 MHz
// tone into a ~600 px wide plot -- about 6 px per cycle, so the trace reads as
// a solid band however smooth the underlying curve is. 512 points spans
// 2.6 us, ~26 cycles, ~23 px per cycle: a recognisably sinusoidal trace at
// roughly one plotted point per pixel, which is the useful working point for
// a plot of this size. Zoom still works on the full-resolution data.
constexpr int   kMaxTimePoints = 512;
constexpr int   kMaxSymbols    = 3000;   // points handed to the constellation
constexpr float kFloorInit     = -160.0f;
} // namespace

DspEngine::~DspEngine() = default;

DspEngine::DspEngine(QObject* parent) : QObject(parent)
{
    m_frameClock.start();
    applyConfig(m_cfg);
}

void DspEngine::applyConfig(const sdr::Config& cfg)
{
    // The GPU toggle is finally READ somewhere. Changing it clears any earlier
    // fallback so a fresh attempt is made.
    if (cfg.src.useGpu != m_gpuWanted) {
        m_gpuWanted = cfg.src.useGpu;
        m_gpuFailed = false;
        if (!m_gpuWanted) {
            m_gpu.reset();
            emit computeStatus(false, QStringLiteral("CPU FFT (GPU offload off)"));
        }
    }
    const bool windowChanged = (m_windowSize != cfg.disp.fftSize)
                            || (m_windowType != cfg.disp.window);
    m_cfg = cfg;
    m_minFrameMs = cfg.disp.refreshFps > 0 ? 1000 / cfg.disp.refreshFps : 0;

    if (windowChanged) {
        m_windowSize = cfg.disp.fftSize;
        m_windowType = cfg.disp.window;
        m_window     = dsp::makeWindow(m_windowType, static_cast<std::size_t>(m_windowSize));
        m_windowGain = dsp::windowSum(m_window);
        m_windowEnbw = dsp::windowEnbw(m_window);
        clearAverages();
    }
    ensureSized(cfg.src.streamChannels);
}

void DspEngine::ensureSized(int channels)
{
    const std::size_t n = static_cast<std::size_t>(std::max(1, channels));
    if (m_state.size() != n) m_state.resize(n);
}

void DspEngine::clearAverages()
{
    for (ChannelState& st : m_state) {
        st.avg.assign(static_cast<std::size_t>(std::max(1, m_windowSize)), kFloorInit);
        st.linAcc.assign(static_cast<std::size_t>(std::max(1, m_windowSize)), 0.0);
        st.linCount = 0;
        st.primed   = false;
    }
}

void DspEngine::resetCounters()
{
    m_framesOut = m_framesSkipped = 0;
    m_stats = StreamStats{};
    clearAverages();
}

void DspEngine::updateStats(const sdr::StreamStats& stats)
{
    // Counters the source owns; the display counters stay ours.
    const quint64 framesOut     = m_stats.framesOut;
    const quint64 framesSkipped = m_stats.framesSkipped;
    m_stats = stats;
    m_stats.framesOut     = framesOut;
    m_stats.framesSkipped = framesSkipped;
}

void DspEngine::processBlock(const sdr::SampleBlock& block)
{
    // The backlog counter must fall on every path out of this function,
    // including the throttled one — otherwise the pipeline reports a
    // permanently full buffer after the first dropped frame.
    struct Release {
        BackPressurePtr bp;
        ~Release() { if (bp) bp->queued.fetchAndSubOrdered(1); }
    } release{m_bp};

    // A block is valid with either decoded channels (simulator) or raw wire
    // bytes (file/DMA with deferred decode).
    if (block.channels.empty() && block.raw.isEmpty()) return;

    m_stats.filePos   = block.filePos;
    if (block.fileTotal > 0) m_stats.fileTotal = block.fileTotal;

    // Display throttle. Everything skipped is counted, so back-pressure is
    // visible in the status bar rather than silent.
    if (m_frameClock.elapsed() < m_minFrameMs) {
        ++m_framesSkipped;
        return;
    }

    // Skip while the GUI still owes us a paint. Without this the engine posts
    // frames faster than the compositor drains them; the queued-connection
    // backlog then monopolises the GUI thread's event loop and starves its
    // timers, which is what made the window stop responding under load.
    if (m_renderGate && m_renderGate->loadAcquire() != 0) {
        ++m_framesSkipped;
        return;
    }

    m_frameClock.restart();

    const std::size_t fftN = static_cast<std::size_t>(m_cfg.disp.fftSize);
    if (block.samplesPerChannel() < fftN) return;
    if (m_windowSize != m_cfg.disp.fftSize) applyConfig(m_cfg);

    // Raw wire bytes are decoded here, after the throttle and render gate, so
    // only the ~30 blocks a second that will actually be analysed pay for
    // conversion. The other ~6000/s pass through untouched.
    const std::vector<IQBlock>* chans = &block.channels;
    if (block.channels.empty() && !block.raw.isEmpty()) {
        codec::decode(block.raw.constData(),
                      static_cast<std::size_t>(block.raw.size()),
                      block.rawFormat, block.rawChannels,
                      block.rawFullScale, m_decoded);
        chans = &m_decoded;
    }
    if (chans->empty()) return;

    const int nch = static_cast<int>(chans->size());
    ensureSized(nch);

    FrameResult frame;
    frame.sequence      = block.sequence;
    frame.displayRateHz = m_cfg.acq.displayRateHz();
    m_lastDisplayRate   = frame.displayRateHz;
    // ---------------------------------------------------------------------
    // FIX (frequency accuracy): compensate the frequency axis for the DDC.
    //
    // applyDdc() multiplies the samples by exp(+j*2*pi*f_nco*t), which
    // translates the entire spectrum UP by f_nco. A component whose true
    // baseband offset is f_b therefore lands in the FFT at f_b + f_nco.
    //
    // Every consumer of the frequency axis -- computeMetrics()'s peak
    // frequency, dsp::findPeaks() for the peak table, and the spectrum and
    // waterfall X axes -- derived its scale from this one value while
    // treating it as the RF centre, so all of them reported f_b + f_nco
    // instead of f_b. With the default NCO of -1.2288 MHz that is a constant
    // 1.2288 MHz error, and it scales directly with whatever NCO the operator
    // sets: exactly the 1-3 MHz discrepancy observed against a known carrier.
    //
    // Undoing the translation is a subtraction, not a fudge factor:
    //     f_b     = f_measured - f_nco
    //     f_RF    = centre + f_b = (centre - f_nco) + f_measured
    // so the axis origin moves by -f_nco and every derived reading follows.
    // Correcting it here keeps the plot, the peak table and the measurement
    // panel consistent by construction, since they all read this field.
    // ---------------------------------------------------------------------
    frame.centerFreqHz  = m_cfg.acq.centerFreqHz() - m_cfg.acq.ncoFreqMHz * 1e6;
    frame.blockSpanSec  = 0.0;   // set once the analysis window is known
    frame.channels.reserve(static_cast<std::size_t>(nch));

    // Bounded analysis window. At 122.88 MSPS with decimation 1 a block holds
    // a quarter-million samples per channel, but the display consumes one FFT
    // and a 2048-point trace from it. Down-converting and measuring the whole
    // block spent the GUI thread's budget on samples nobody could see, which
    // is what pinned the display at ~1 fps. A real analyser behaves the same
    // way: it measures a window per sweep, not every sample that arrives.
    // Throughput accounting and recording still see the full stream — only the
    // analysis is windowed.
    const std::size_t analysisN = std::min<std::size_t>(
        block.samplesPerChannel(),
        std::max<std::size_t>(fftN * 4, kMinAnalysisSamples));

    // Only the operator-selected channels are analysed. Skipping the rest
    // keeps the DSP cost proportional to what is displayed: selecting 1 of 16
    // channels does a sixteenth of the work, not all of it then a discard.
    const QVector<int> selected = selectedChannels(m_cfg.src.channelMask, nch);

    for (int c : selected) {
        ChannelState& st = m_state[static_cast<std::size_t>(c)];

        // Digital down-conversion first — everything downstream is baseband.
        const IQBlock& src = (*chans)[static_cast<std::size_t>(c)];
        m_work.assign(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(analysisN));
        applyDdc(m_work, block.startSample);

        ChannelFrame cf;
        cf.channel = c;

        // Scope-style trigger: start the displayed slice at the first rising
        // zero-crossing of I. Successive blocks begin at arbitrary carrier
        // phase, so an untriggered trace redraws at a different alignment
        // every frame and looks like jitter even when nothing is wrong.
        std::size_t trig = 0;
        {
            const std::size_t search = std::min<std::size_t>(m_work.size() / 4, 8192);
            for (std::size_t i = 1; i < search; ++i) {
                if (m_work[i - 1].real() <= 0.0f && m_work[i].real() > 0.0f) {
                    trig = i;
                    break;
                }
            }
        }

        // -----------------------------------------------------------------
        // Plot a CONTIGUOUS run of samples, not a strided subset.
        //
        // The previous code spread kMaxTimePoints picks across the whole
        // analysis window, giving stride = 16384/2048 = 8. That decimates the
        // trace by 8 with no anti-alias filter, so a 10 MHz tone at
        // 200.25 MSPS -- 20.0 samples per cycle in the raw data -- reached the
        // plot with only 2.50 points per cycle, sitting at 80 % of the
        // *display* Nyquist (25.03/2 MSPS). A polyline through 2.5 points per
        // cycle can only be a zigzag, which is the jagged waveform observed.
        //
        // Taking the first kMaxTimePoints samples after the trigger instead
        // keeps the full sample rate: 20.0 points per cycle, i.e. a smooth
        // curve. The trade is a shorter visible window (10.2 us rather than
        // 81.8 us), which is the correct trade for a time-domain view and is
        // exactly what an oscilloscope does -- it shortens the timebase rather
        // than undersampling a long one.
        //
        // No sample value is altered: this only changes WHICH samples are
        // drawn. Amplitude, frequency and phase are untouched, and the
        // spectrum/constellation paths still see the full analysis window.
        // -----------------------------------------------------------------
        const std::size_t avail = m_work.size() - trig;
        const std::size_t want  =
            std::min<std::size_t>(static_cast<std::size_t>(kMaxTimePoints), avail);
        cf.iq.reserve(want);
        for (std::size_t i = trig; i < trig + want; ++i)
            cf.iq.push_back(m_work[i]);

        computeSpectrum(m_work, st, cf);
        computeConstellation(m_work, cf);
        computeMetrics(cf, frame.displayRateHz, frame.centerFreqHz);

        frame.channels.push_back(std::move(cf));
    }

    // Time axis must describe the samples actually plotted. The trace is now
    // a contiguous run of at most kMaxTimePoints samples, so the span is that
    // count / Fs -- not the whole analysis window, which would stretch the
    // axis and misreport the period of the displayed waveform.
    {
        std::size_t plotted = 0;
        if (!frame.channels.empty()) plotted = frame.channels.front().iq.size();
        if (plotted == 0) plotted = analysisN;
        frame.blockSpanSec = frame.displayRateHz > 0.0
            ? static_cast<double>(plotted) / frame.displayRateHz : 0.0;
    }

    m_stats.framesOut     = ++m_framesOut;
    m_stats.framesSkipped = m_framesSkipped;
    m_stats.bufferPct     = m_bp ? m_bp->utilisation() * 100.0 : 0.0;
    frame.stats = m_stats;

    if (m_renderGate) m_renderGate->storeRelease(1);
    emit frameReady(frame);
}

void DspEngine::applyDdc(IQBlock& b, quint64 startSample) const
{
    // Complex mix by the NCO frequency. The phase comes from the absolute
    // sample index rather than a local accumulator: the display throttle drops
    // blocks, and a local accumulator would then drift out of step with the
    // incoming stream and rotate every rendered frame by a random constant.
    const double fs = m_cfg.acq.displayRateHz();
    if (fs <= 0.0) return;
    const double dPhase = 2.0 * dsp::kPi * (m_cfg.acq.ncoFreqMHz * 1e6 / fs);
    if (std::abs(dPhase) < 1e-12) return;

    double phase = std::fmod(dPhase * static_cast<double>(startSample), 2.0 * dsp::kPi);
    for (cf32& s : b) {
        const float c = static_cast<float>(std::cos(phase));
        const float d = static_cast<float>(std::sin(phase));
        s = cf32(s.real() * c - s.imag() * d, s.real() * d + s.imag() * c);
        phase += dPhase;
        if (phase >  2.0 * dsp::kPi) phase -= 2.0 * dsp::kPi;
        if (phase < -2.0 * dsp::kPi) phase += 2.0 * dsp::kPi;
    }
}

void DspEngine::computeSpectrum(const IQBlock& in, ChannelState& st, ChannelFrame& out)
{
    const std::size_t n = static_cast<std::size_t>(m_cfg.disp.fftSize);
    if (in.size() < n || m_window.size() != n) return;

    // Coherent gain correction so a full-scale tone lands near 0 dBFS
    // regardless of which window is selected.
    const float scale = 1.0f / (m_windowGain * m_windowGain * 0.25f);
    std::vector<float> mag(n);

    // ---- GPU offload: window multiply + FFT + |X|^2 --------------------
    // Only the |X|^2 step comes back from the GPU. Everything after it —
    // dBFS, fftshift, and all averaging modes below — is shared with the CPU
    // path, so switching processors cannot move a reading.
    bool doneOnGpu = false;
    if (m_gpuWanted && !m_gpuFailed) {
        if (!m_gpu) {
            m_gpu = std::make_unique<GpuFft>();
            QString why;
            if (!m_gpu->init(&why)) {
                m_gpuFailed = true;                  // do not retry every frame
                m_gpu.reset();
                emit computeStatus(false, QStringLiteral("GPU offload failed — "
                                   "running on CPU. %1").arg(why));
            } else {
                emit computeStatus(true, why);
            }
        }
        if (m_gpu) {
            QString err;
            m_power.resize(n);
            if (m_gpu->setWindow(m_window.data(), n, &err)
             && m_gpu->powerSpectrum(in.data(), n, m_power.data(), &err)) {
                for (std::size_t i = 0; i < n; ++i)
                    mag[i] = dsp::toDbfs(m_power[i], scale);
                doneOnGpu = true;
            } else {
                // A runtime failure must fall back, not blank the display.
                m_gpuFailed = true;
                m_gpu.reset();
                emit computeStatus(false, QStringLiteral(
                    "GPU FFT failed mid-stream — fell back to CPU. %1").arg(err));
            }
        }
    }

    if (!doneOnGpu) {
        m_scratch.assign(n, cf32(0.0f, 0.0f));
        for (std::size_t i = 0; i < n; ++i)
            m_scratch[i] = in[i] * m_window[i];

        dsp::fftInPlace(m_scratch);

        for (std::size_t i = 0; i < n; ++i) {
            const float re = m_scratch[i].real(), im = m_scratch[i].imag();
            mag[i] = dsp::toDbfs(re * re + im * im, scale);
        }
    }
    dsp::fftShift(mag);

    if (st.avg.size() != n) {
        st.avg.assign(n, kFloorInit);
        st.linAcc.assign(n, 0.0);
        st.linCount = 0;
        st.primed   = false;
    }

    switch (m_cfg.disp.average) {
    case AverageMode::None:
        st.avg = mag;
        break;

    case AverageMode::Exponential: {
        const float a = static_cast<float>(std::clamp(m_cfg.disp.averageAlpha, 0.01, 1.0));
        if (!st.primed) { st.avg = mag; st.primed = true; }
        else for (std::size_t i = 0; i < n; ++i)
            st.avg[i] = st.avg[i] * (1.0f - a) + mag[i] * a;
        break;
    }

    case AverageMode::LinearN: {
        // Averaged in linear power, not in dB: averaging decibels biases the
        // result low and understates the noise floor.
        const int target = std::max(1, m_cfg.disp.averageCount);
        for (std::size_t i = 0; i < n; ++i)
            st.linAcc[i] += std::pow(10.0, mag[i] / 10.0);
        ++st.linCount;
        if (st.linCount >= target) {
            for (std::size_t i = 0; i < n; ++i) {
                const double p = st.linAcc[i] / st.linCount;
                st.avg[i] = static_cast<float>(10.0 * std::log10(p > 1e-20 ? p : 1e-20));
                st.linAcc[i] = 0.0;
            }
            st.linCount = 0;
            st.primed   = true;
        }
        if (!st.primed) st.avg = mag;
        break;
    }

    case AverageMode::MaxHold:
        if (!st.primed) { st.avg = mag; st.primed = true; }
        else for (std::size_t i = 0; i < n; ++i) st.avg[i] = std::max(st.avg[i], mag[i]);
        break;

    case AverageMode::MinHold:
        if (!st.primed) { st.avg = mag; st.primed = true; }
        else for (std::size_t i = 0; i < n; ++i) st.avg[i] = std::min(st.avg[i], mag[i]);
        break;
    }

    out.spectrum = st.avg;
}

std::vector<float> DspEngine::idealLevels(Modulation m)
{
    // Unit-average-power constellations, so EVM is the standard
    // sqrt(mean|err|^2 / mean|ref|^2) with a reference power of 1.
    switch (m) {
    case Modulation::QPSK:
        return {-0.70711f, 0.70711f};
    case Modulation::QAM64: {
        std::vector<float> v;
        for (int k = -7; k <= 7; k += 2) v.push_back(k / 6.48074f);   // sqrt(42)
        return v;
    }
    case Modulation::QAM16:
    default: {
        std::vector<float> v;
        for (int k = -3; k <= 3; k += 2) v.push_back(k / 3.16228f);   // sqrt(10)
        return v;
    }
    }
}

void DspEngine::computeConstellation(const IQBlock& in, ChannelFrame& out) const
{
    // Placeholder symbol timing: fixed decimation at a known SPS. Replace with
    // Gardner / Mueller-Muller recovery when the real receiver chain lands.
    constexpr int sps       = 4;
    constexpr int envWindow = 32;
    // Sample at the symbol instant, not between symbols — a half-symbol offset
    // sits at maximum ISI and smears the constellation into a cloud.
    constexpr int offset    = 0;
    if (in.size() < static_cast<std::size_t>(envWindow * 2)) return;

    // Smoothed magnitude envelope. Gating on raw symbol magnitude would throw
    // away 16-QAM's inner points, so the burst is found from the envelope.
    std::vector<float> env(in.size(), 0.0f);
    float acc = 0.0f;
    for (int i = 0; i < envWindow; ++i) acc += std::norm(in[static_cast<std::size_t>(i)]);
    for (std::size_t i = 0; i + envWindow < in.size(); ++i) {
        env[i] = std::sqrt(acc / envWindow);
        acc += std::norm(in[i + envWindow]) - std::norm(in[i]);
    }
    const float envPeak = *std::max_element(env.begin(), env.end());
    const float envGate = envPeak * 0.65f;

    std::vector<cf32> raw;
    raw.reserve(in.size() / sps);
    for (std::size_t i = static_cast<std::size_t>(offset);
         i + envWindow < in.size() && raw.size() < kMaxSymbols; i += sps)
        if (env[i] > envGate) raw.push_back(in[i]);

    if (raw.size() < 16) return;

    // Static phase alignment via the 4th-power method. The constellation is
    // symmetric under 90 degree rotation, so summing r^4 gives a stable
    // estimate of 4*phi with a harmless pi/2 ambiguity.
    //
    // Deliberately no blind *frequency* estimate: the DDC already brings the
    // signal to baseband, and an open-loop 4th-power frequency estimate is
    // unreliable for QAM — a small error spirals the block into rings.
    // Residual rotation FIRST (see the note below): the 4th-power estimator
    // assumes a stationary constellation, so running it on a rotating phasor
    // yields a meaningless static phase and the de-rotated cluster then lands
    // at an arbitrary angle. Estimate and remove the rotation, then measure
    // the static phase on what is actually stationary.
    // Modulation-stripped frequency estimate (Viterbi & Viterbi, 1983).
    //
    // A plain delay-and-multiply, sum r[i]*conj(r[i-1]), is WRONG here. It
    // gives s[i]*conj(s[i-1]) * exp(j*step): for an unmodulated carrier the
    // data term is |s|^2 and sums coherently, but for QAM it is zero-mean and
    // sums as a random walk. Measured coherence of that term: 1.0000 for CW
    // versus 0.0097 for 16-QAM, so on modulated data the estimate is noise
    // and de-rotating by it destroys the constellation (16-QAM EVM went from
    // 0.67 % to 26 % when this was tried).
    //
    // Raising to the fourth power first removes the QPSK/QAM data symmetry,
    // leaving a tone at 4*step which is then differenced and divided by four.
    // Verified on both signal types: exact for CW, and within ~0.06 MHz on
    // 16-QAM where the plain estimator was off by tens of MHz.
    cf32 rotAcc(0.0f, 0.0f);
    for (std::size_t i = 1; i < raw.size(); ++i) {
        const cf32 a = raw[i]     * raw[i];
        const cf32 b = raw[i - 1] * raw[i - 1];
        rotAcc += (a * a) * std::conj(b * b);        // r^4 * conj(prev^4)
    }
    // /4 undoes the fourth power. The estimate is unambiguous only while
    // |step| < 45 deg/point, i.e. |offset| < fs/(8*sps); beyond that the
    // fourth-power tone itself aliases.
    const float stepPerPoint = std::arg(rotAcc) / 4.0f;

    constexpr float kMinRot = 1e-3f;                  // ~0.06 deg/point
    const bool derotate = std::abs(stepPerPoint) > kMinRot;

    std::vector<cf32> flat;
    flat.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i)
        flat.push_back(derotate
            ? raw[i] * std::polar(1.0f, -stepPerPoint * static_cast<float>(i))
            : raw[i]);

    cf32 phaseAcc(0.0f, 0.0f);
    for (const cf32& s : flat) {
        const cf32 v = s * s;
        phaseAcc += v * v;
    }
    // For square QAM and QPSK alike the fourth moment is real and negative, so
    // negating first keeps the argument near zero and avoids a branch-cut jump
    // that would flip the grid by 90 degrees between frames.
    const float phi0 = std::arg(-phaseAcc) / 4.0f;

    double power = 0.0;
    for (const cf32& s : flat) power += static_cast<double>(std::norm(s));
    const double rmsLevel = std::sqrt(power / raw.size());
    const float  norm     = static_cast<float>(1.0 / (rmsLevel > 1e-9 ? rmsLevel : 1.0));

    // Emit the de-rotated, statically-aligned constellation.
    //
    // Why this matters: the constellation samples every `sps` samples, so an
    // unmodulated carrier advances
    //     step = 360 * f_residual / fs * sps   degrees per plotted point
    // and a static-phase estimator can only cancel that when `step` is a
    // multiple of 90 degrees. At 200 MSPS with sps = 4 that is true at exactly
    // 25 MHz (180 deg/point) and 50 MHz (360 deg/point) — which is why the
    // phase read 0 degrees there and non-zero everywhere else. It was never a
    // frequency-dependent error; it was a rotating phasor measured with a
    // static-phase estimator.
    out.symbols.reserve(flat.size());
    for (const cf32& p : flat)
        out.symbols.push_back(p * norm * std::polar(1.0f, -phi0));

    // Report the offset that was removed, in Hz, so the figure is physical
    // rather than an artefact of the plotting stride.
    // Report the offset that was removed — but only where it is unambiguous.
    //
    // The fourth power multiplies the phase step by four, so the estimate
    // wraps once |step| exceeds 45 deg/point, i.e. |offset| > fs/(8*sps)
    // (6.25 MHz at 200 MSPS with sps = 4). Verified: a 10 MHz tone reports
    // -2.5 MHz and a 20 MHz tone -5.0 MHz, exactly the predicted aliases.
    //
    // The CONSTELLATION is still correct beyond that range, because the
    // aliased step differs from the true one by a multiple of 90 deg — which
    // is precisely the symmetry the display and the slicer ignore. Only the
    // reported number is ambiguous, so it is suppressed rather than shown
    // wrong; `Peak frequency`, taken from the FFT, remains unambiguous across
    // the whole span and is the figure to read instead.
    out.metrics.residualHz = derotate
        ? double(stepPerPoint) / (2.0 * dsp::kPi) * m_lastDisplayRate / double(sps)
        : 0.0;
    // Validity is decided in computeMetrics(), by cross-checking against the
    // FFT peak. A range test alone is not enough: an aliased estimate can land
    // inside the unambiguous window (a 10 MHz tone reports -2.5 MHz, which is
    // well within +/-6.25 MHz and would pass a range check while being wrong).
    out.metrics.residualValid = false;

    out.metrics.rms = rmsLevel;
}

void DspEngine::computeMetrics(ChannelFrame& out, double displayRateHz, double centerHz) const
{
    const auto& s = out.spectrum;
    if (s.empty()) return;

    const std::size_t n = s.size();
    const double binHz  = displayRateHz / static_cast<double>(n);

    // ---------------------------------------------------------------------
    // DC guard: exclude a window around the DC bin before locating the
    // fundamental.
    //
    // LO leakage and ADC offset put a large spur at 0 Hz. Because it is often
    // the tallest line in the spectrum, a bare argmax locks onto it and the
    // reported frequency collapses to 0.000 MHz -- measured here at a DC
    // offset of 1.0 with a -10 MHz carrier still clearly present. This
    // mirrors the reference implementation (SDR_RF_Parameter.py), which zeroes
    // psd[dc +/- dc_guard_bins] before argmax for the same reason.
    //
    // The width is derived from the window's equivalent noise bandwidth
    // rather than hardcoded: a wide window (flat-top, ENBW ~3.8 bins) smears
    // DC over more bins than a narrow one, so the guard must scale with it.
    // Same 1.5x ENBW rule the reference uses for its signal mask.
    // ---------------------------------------------------------------------
    const std::size_t dcBin = n / 2;
    const std::size_t guard = static_cast<std::size_t>(
        std::max(2.0, std::ceil(1.5 * dsp::windowEnbw(m_window)))) + 1;

    std::size_t lo = 0, hi = n;
    if (guard < dcBin) { lo = dcBin - guard; hi = dcBin + guard + 1; }

    std::size_t pk = 0;
    float best = -std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < n; ++i) {
        if (i >= lo && i < hi) continue;            // inside the DC guard
        if (s[i] > best) { best = s[i]; pk = i; }
    }
    // Degenerate case (guard covers everything): fall back to a plain argmax
    // rather than reporting nothing.
    if (!std::isfinite(best)) {
        const auto it = std::max_element(s.begin(), s.end());
        pk = static_cast<std::size_t>(std::distance(s.begin(), it));
        best = *it;
    }
    const auto peakIt = s.begin() + static_cast<long>(pk);

    // Sub-bin refinement by parabolic interpolation over the three bins around
    // the maximum -- the same estimator dsp::findPeaks() already used for the
    // peak table. Reporting the bare argmax quantised this reading to the bin
    // centre, an unnecessary error of up to +/- binHz/2 (about +/- 49 kHz at
    // 200 MSPS with a 2048-point FFT) and, worse, it disagreed with the
    // interpolated value shown for P1 in the peak table for the same carrier.
    //
    // Interpolating in dB is deliberate: the log of a windowed main lobe is
    // close to a parabola, so a 3-point fit on dB values is markedly more
    // accurate than the same fit on linear magnitude. Residual error is then
    // set by window shape and SNR rather than by the bin grid.
    double delta = 0.0;
    if (pk > 0 && pk + 1 < n) {
        const double ym1 = s[pk - 1], y0 = s[pk], yp1 = s[pk + 1];
        const double denom = ym1 - 2.0 * y0 + yp1;
        if (std::abs(denom) > 1e-12) delta = 0.5 * (ym1 - yp1) / denom;
        delta = std::clamp(delta, -0.5, 0.5);
    }

    // ---------------------------------------------------------------------
    // IEEE-1241 style converter metrics (the Xilinx RF Analyzer set).
    //
    // Computed from the same windowed spectrum the display shows, in LINEAR
    // power. Bins within +/- `guard` of a tone are treated as belonging to
    // that tone: a window spreads a pure sinusoid over several bins (Blackman-
    // Harris ENBW is ~2 bins), so summing only the peak bin would understate
    // signal power and inflate every derived figure.
    {
        auto lin = [](float db) { return std::pow(10.0, double(db) / 10.0); };

        const std::size_t gw = guard;              // same window-derived width
        auto binPower = [&](std::size_t c) {       // power in a tone's bins
            double p = 0.0;
            const std::size_t lo2 = c > gw ? c - gw : 0;
            const std::size_t hi2 = std::min(n - 1, c + gw);
            for (std::size_t i = lo2; i <= hi2; ++i) p += lin(s[i]);
            return p;
        };

        // Fundamental: the peak already located, excluding DC.
        const double pSig = binPower(pk);

        // Harmonics H2..H6. The signal sits at (pk - N/2) bins from DC; a
        // harmonic at k*f folds back into the digitised band, so the position
        // is computed modulo the span rather than assumed to be off-scale.
        const double sigBinOff = double(pk) - double(n) / 2.0;
        double pHarm = 0.0, worstSpurLin = 0.0;
        for (int h = 2; h <= 6; ++h) {
            double off = std::fmod(sigBinOff * h, double(n));
            if (off < -double(n) / 2.0) off += double(n);
            if (off >  double(n) / 2.0) off -= double(n);
            const auto hb = std::size_t(std::llround(off + double(n) / 2.0));
            if (hb >= n) continue;
            // A harmonic that folds onto the fundamental is not measurable.
            if (hb > pk - std::min(pk, gw) && hb < pk + gw) continue;
            const double ph = binPower(hb);
            pHarm += ph;
            out.metrics.harmonicsDbc[h - 2] = 10.0 * std::log10(ph / (pSig > 0 ? pSig : 1));
            worstSpurLin = std::max(worstSpurLin, ph);
        }

        // Noise-and-distortion: everything except DC and the fundamental.
        double pTotal = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            if (i >= lo && i < hi) continue;                 // DC guard
            if (i + gw >= pk && i <= pk + gw) continue;      // fundamental
            const double p = lin(s[i]);
            pTotal += p;
            worstSpurLin = std::max(worstSpurLin, p);        // includes spurs
        }

        const double pNad = pTotal;                          // noise + distortion

        // --- is there actually a carrier? ---------------------------------
        //
        // SINAD and ENOB are ratios against a FUNDAMENTAL. With no carrier the
        // "fundamental" is simply the largest noise bin, and both numbers stop
        // meaning anything — they do not become low, they become undefined.
        // Printing 6.5 bits in that state is worse than printing nothing: it
        // sends people looking for a converter fault that does not exist.
        //
        // Two independent rejections, because either alone has false positives:
        //   * the peak must stand clear of the median bin
        //   * it must not sit at the very edge of the span, where the window
        //     skirt and band edge produce a peak that is not a signal
        const double medianBin = double(*std::next(
            [&]{ static std::vector<float> t; t = s; std::nth_element(
                 t.begin(), t.begin() + long(n / 2), t.end()); return t.begin(); }(),
            long(n / 2)));
        const double peakOverFloor = double(s[pk]) - medianBin;
        const std::size_t edge = std::max<std::size_t>(gw + 1, n / 64);

        constexpr double kMinPeakOverFloorDb = 10.0;
        if (peakOverFloor < kMinPeakOverFloorDb) {
            out.metrics.toneValid  = false;
            out.metrics.toneReason = QStringLiteral(
                "no carrier: strongest bin is only %1 dB over the noise floor "
                "(need %2 dB). SINAD/ENOB would be measuring noise.")
                .arg(peakOverFloor, 0, 'f', 1).arg(kMinPeakOverFloorDb, 0, 'f', 0);
        } else if (pk < edge || pk >= n - edge) {
            out.metrics.toneValid  = false;
            out.metrics.toneReason = QStringLiteral(
                "strongest bin is at the edge of the span, which is a band-edge "
                "artefact rather than a carrier.");
        } else {
            out.metrics.toneValid = true;
        }

        if (out.metrics.toneValid && pSig > 0.0 && pNad > 0.0) {
            out.metrics.sinadDb = 10.0 * std::log10(pSig / pNad);
            // ENOB from SINAD: the standard relation, with the 1.76 dB term
            // that accounts for ideal quantisation noise being uniform.
            out.metrics.enob = (out.metrics.sinadDb - 1.76) / 6.02;

            // TRUE SNR: signal against noise with the harmonics removed.
            // SINAD includes distortion; SNR must not, or the two are the same
            // number and one of them is mislabelled.
            const double pNoiseOnly = (pNad > pHarm) ? (pNad - pHarm) : pNad;
            out.metrics.snrDb = 10.0 * std::log10(pSig / pNoiseOnly);
        } else {
            out.metrics.sinadDb = 0.0;
            out.metrics.enob    = 0.0;
            out.metrics.snrDb   = 0.0;
        }
        if (pSig > 0.0 && pHarm > 0.0)
            out.metrics.thdDbc = 10.0 * std::log10(pHarm / pSig);
        if (pSig > 0.0 && worstSpurLin > 0.0)
            out.metrics.sfdrDbc = 10.0 * std::log10(pSig / worstSpurLin);
    }

    // Cross-check the constellation's frequency estimate against the FFT
    // peak. The FFT is unambiguous across the whole span, so agreement means
    // the fourth-power estimate has not aliased; disagreement means it has,
    // and the figure is suppressed rather than printed as if measured.
    {
        const double binHzLocal = displayRateHz / double(n);
        const double peakOffset = (double(pk) - double(n) / 2.0) * binHzLocal;
        out.metrics.residualValid =
            out.metrics.residualHz != 0.0
            && std::abs(out.metrics.residualHz - peakOffset) < 4.0 * binHzLocal;
    }

    out.metrics.peakDbfs   = *peakIt;
    out.metrics.peakFreqHz =
        centerHz + ((static_cast<double>(pk) + delta) - n / 2.0) * binHz;

    // ---------------------------------------------------------------------
    // Frequency-pipeline diagnostics. Off unless SDR_DEBUG_FREQ=1, so it
    // costs nothing in normal operation and cannot flood a live session.
    // Every term that enters the bin -> frequency conversion is printed, so
    // this GUI can be compared field-by-field against a reference chain
    // (GNU Radio, TI HSDC) instead of comparing only the final number.
    // ---------------------------------------------------------------------
    static const bool dbg = qEnvironmentVariableIntValue("SDR_DEBUG_FREQ") == 1;
    if (dbg) {
        static QElapsedTimer t;
        if (!t.isValid()) t.start();
        if (t.elapsed() >= 1000) {
            t.restart();
            const double fs = displayRateHz;
            qInfo().noquote() << QStringLiteral(
                "\n--- frequency pipeline ---------------------------------\n"
                "  configured rate   : %1 MSPS\n"
                "  interpolation     : %2\n"
                "  decimation        : %3\n"
                "  display rate (Fs) : %4 MSPS   (= rate * interp / decim)\n"
                "  wire format       : %5  (complex=%6)\n"
                "  FFT size (N)      : %7\n"
                "  bin spacing       : %8 Hz     (= Fs / N)\n"
                "  axis span         : %9 MHz    (= Fs)\n"
                "  NCO / DDC         : %10 MHz\n"
                "  RF centre         : %11 MHz\n"
                "  axis origin used  : %12 MHz   (= centre - NCO)\n"
                "  peak bin (k)      : %13 of %7   (DC at N/2 = %14)\n"
                "  sub-bin delta     : %15 bins\n"
                "  peak level        : %16 dBFS\n"
                "  baseband offset   : %17 MHz   (= (k+delta-N/2) * binHz)\n"
                "  DISPLAYED FREQ    : %18 MHz\n"
                "--------------------------------------------------------")
                .arg(m_cfg.acq.sampleRateMsps, 0, 'f', 6)
                .arg(m_cfg.acq.interpolation)
                .arg(m_cfg.acq.decimation)
                .arg(fs / 1e6, 0, 'f', 6)
                .arg(formatName(m_cfg.src.format))
                .arg(isRealFormat(m_cfg.src.format) ? QStringLiteral("no")
                                                    : QStringLiteral("yes"))
                .arg(n)
                .arg(binHz, 0, 'f', 3)
                .arg(fs / 1e6, 0, 'f', 4)
                .arg(m_cfg.acq.ncoFreqMHz, 0, 'f', 6)
                .arg(m_cfg.acq.centerFreqHz() / 1e6, 0, 'f', 6)
                .arg(centerHz / 1e6, 0, 'f', 6)
                .arg(pk)
                .arg(n / 2)
                .arg(delta, 0, 'f', 4)
                .arg(out.metrics.peakDbfs, 0, 'f', 2)
                .arg(((static_cast<double>(pk) + delta) - n / 2.0) * binHz / 1e6, 0, 'f', 6)
                .arg(out.metrics.peakFreqHz / 1e6, 0, 'f', 6);
        }
    }

    // Noise floor: the median is robust against the carrier itself.
    std::vector<float> sorted(s);
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<long>(n / 2), sorted.end());
    out.metrics.noiseFloorDbfs = sorted[n / 2];
    // Peak-to-median-bin. This was previously reported AS "SNR", which it is
    // not: it compares two single bins, while SNR is signal power against
    // integrated noise power. The two differ by tens of dB, so the panel was
    // showing an SNR that could not be reconciled with its own ENOB.
    out.metrics.peakToFloorDb  = out.metrics.peakDbfs - out.metrics.noiseFloorDbfs;

    // 99 % occupied bandwidth, integrated in linear power.
    std::vector<double> lin(n);
    double total = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        lin[i] = std::pow(10.0, s[i] / 10.0);
        total += lin[i];
    }
    if (total > 0.0) {
        const double edge = total * 0.005;
        double acc = 0.0;
        std::size_t lo = 0, hi = n - 1;
        for (std::size_t i = 0; i < n; ++i) { acc += lin[i]; if (acc >= edge) { lo = i; break; } }
        acc = 0.0;
        for (std::size_t i = n; i-- > 0;)  { acc += lin[i]; if (acc >= edge) { hi = i; break; } }
        out.metrics.occupiedBwHz = (hi > lo) ? (hi - lo) * binHz : 0.0;

        double band = 0.0;
        for (std::size_t i = lo; i <= hi && i < n; ++i) band += lin[i];
        out.metrics.channelPowerDb = 10.0 * std::log10(band > 1e-20 ? band : 1e-20);
    }

    if (m_cfg.disp.peakSearch) {
        const int minSep = std::max(2, static_cast<int>(n * m_cfg.disp.peakExcursionPct / 100.0));
        out.metrics.peaks = dsp::findPeaks(s, centerHz, displayRateHz,
                                           out.metrics.noiseFloorDbfs,
                                           m_cfg.disp.peakThreshDb,
                                           m_cfg.disp.peakCount, minSep);
    }

    // Time-domain statistics from the decimated slice.
    if (!out.iq.empty()) {
        double sumSq = 0.0, peakAmp = 0.0, sumI2 = 0.0, sumQ2 = 0.0;
        for (const cf32& v : out.iq) {
            const double m2 = static_cast<double>(std::norm(v));
            sumSq  += m2;
            sumI2  += static_cast<double>(v.real()) * v.real();
            sumQ2  += static_cast<double>(v.imag()) * v.imag();
            peakAmp = std::max(peakAmp, std::sqrt(m2));
        }
        const double meanSq = sumSq / out.iq.size();
        const double rms    = std::sqrt(meanSq);
        out.metrics.peakAmplitude = peakAmp;
        out.metrics.paprDb = (meanSq > 1e-20)
            ? 10.0 * std::log10((peakAmp * peakAmp) / meanSq) : 0.0;
        if (out.metrics.rms <= 0.0) out.metrics.rms = rms;
        out.metrics.iqImbalanceDb = (sumQ2 > 1e-20 && sumI2 > 1e-20)
            ? 10.0 * std::log10(sumI2 / sumQ2) : 0.0;
    }

    // Constellation quality against the nearest ideal point.
    if (!out.symbols.empty()) {
        const std::vector<float> levels = idealLevels(m_cfg.acq.modulation);
        auto slice = [&levels](float v) {
            float best = levels.front();
            for (float l : levels)
                if (std::abs(v - l) < std::abs(v - best)) best = l;
            return best;
        };

        double errSq = 0.0, refSq = 0.0, phase = 0.0, sumI = 0.0, sumQ = 0.0;
        for (const cf32& p : out.symbols) {
            const cf32 ideal(slice(p.real()), slice(p.imag()));
            const cf32 err = p - ideal;
            errSq += static_cast<double>(std::norm(err));
            refSq += static_cast<double>(std::norm(ideal));
            if (std::abs(ideal) > 1e-6f)
                phase += std::abs(std::arg(p) - std::arg(ideal));
            sumI += p.real();
            sumQ += p.imag();
        }
        const double k = static_cast<double>(out.symbols.size());
        out.metrics.evm         = refSq > 0.0 ? std::sqrt(errSq / refSq) : 0.0;
        out.metrics.phaseErrDeg = (phase / k) * 180.0 / dsp::kPi;
        out.metrics.dcOffset    = std::hypot(sumI / k, sumQ / k);
    }
}

} // namespace sdr
