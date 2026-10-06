#include "Sources.h"
#include "Fft.h"
#include "SampleCodec.h"
#include "Waveform.h"
#include "pcie_regs.h"

#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <algorithm>
#include <cmath>

#if defined(Q_OS_UNIX)
#  include <fcntl.h>
#  include <poll.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  include <cerrno>
#endif

namespace sdr {

namespace {
constexpr int    kSimBlocksPerSecond = 60;
/// Ceiling on synthesised block length. Above a few MSPS a software simulator
/// cannot generate samples in real time, and generating more than the DSP
/// stage analyses per frame is pure waste — it only starves the GUI thread.
/// The simulator therefore tracks the configured rate up to this point and
/// runs slower than real time beyond it; the status panel reports the measured
/// rate alongside the configured one rather than pretending otherwise.
/// File and DMA sources carry the true rate and are unaffected.
constexpr double kSimMaxBlock        = 16384.0;
constexpr int    kFileTickMs         = 10;
constexpr int    kDmaBudgetMs        = 12;
constexpr double kNoiseRefRms        = 0.5;
/// Simulated RF loopback: a full-scale DAC signal arrives at the ADC 6 dB down,
/// a typical cabled loopback, so full-scale tones never clip the display.
constexpr float  kLoopbackGain       = 0.5f;
} // namespace

// ======================================================== ISignalSource

bool ISignalSource::admit(std::size_t samplesPerChannel)
{
    if (!m_bp) return true;
    if (m_bp->queued.loadAcquire() >= m_bp->capacity) {
        ++m_stats.blocksDropped;
        m_stats.samplesDropped += samplesPerChannel;
        return false;
    }
    return true;
}

void ISignalSource::publish(SampleBlock& block)
{
    block.sequence    = m_seq++;
    block.startSample = m_sampleIndex;
    m_sampleIndex    += block.samplesPerChannel();

    ++m_stats.blocksIn;
    m_samplesWindow += block.samplesPerChannel();

    if (m_bp) m_bp->queued.fetchAndAddOrdered(1);
    emit blockReady(block);
}

void ISignalSource::accountBytes(quint64 bytes)
{
    m_bytesWindow      += bytes;
    m_stats.bytesTotal += bytes;
}

void ISignalSource::refreshStats(qint64 filePos, qint64 fileTotal)
{
    if (!m_rateClock.isValid()) m_rateClock.start();
    if (!m_runClock.isValid())  m_runClock.start();

    if (m_rateClock.elapsed() < 400) return;

    const double secs = m_rateClock.elapsed() / 1000.0;
    m_stats.dmaMBps       = static_cast<double>(m_bytesWindow) / secs / (1024.0 * 1024.0);
    m_stats.sampleRateSps = static_cast<double>(m_samplesWindow) / secs;
    m_bytesWindow = m_samplesWindow = 0;
    m_rateClock.restart();

    m_stats.state      = state();
    m_stats.elapsedMs  = m_runClock.elapsed();
    m_stats.sourceName = name();
    m_stats.bufferPct  = m_bp ? m_bp->utilisation() * 100.0 : 0.0;
    if (filePos   >= 0) m_stats.filePos   = filePos;
    if (fileTotal >= 0) m_stats.fileTotal = fileTotal;

    emit statsUpdated(m_stats);
}

// ====================================================== SimulatedSource

SimulatedSource::SimulatedSource(QObject* parent) : ISignalSource(parent)
{
    m_ch.resize(2);
}

void SimulatedSource::applyConfig(const sdr::Config& cfg)
{
    m_cfg = cfg;
    const std::size_t n = static_cast<std::size_t>(std::max(1, cfg.src.streamChannels));
    if (m_ch.size() != n) {
        m_ch.resize(n);
        // Give each channel a different burst phase so an overlay view does
        // not look like one signal drawn twice.
        for (std::size_t i = 0; i < n; ++i)
            m_ch[i].burstPh = static_cast<double>(i) / static_cast<double>(n);
    }
}

// ---------------------------------------------------------------------------
// RF loopback model
//
// The board's three outcomes, reproduced:
//   DAC input = DDS                       -> the DDS tone
//   DAC input = host, generator running   -> the generator's waveform
//   DAC input = host, nothing transmitting-> noise only
// The NCOs are assumed matched (DAC NCO == ADC NCO, the boot configuration),
// so the transmitted baseband frequency is the received one.
// ---------------------------------------------------------------------------
void SimulatedSource::setTxState(const sdr::TxState& tx)
{
    const bool wasLoopback = m_tx.loopback;
    if (tx.loopPairs != m_tx.loopPairs) m_txPos = 0.0;
    m_tx = tx;
    if (wasLoopback && !tx.loopback) {
        m_loopNote.clear();
        emit statusMessage(QStringLiteral("Simulation: modem demo signal"));
    }
}

void SimulatedSource::renderLoopback(IQBlock& dst, double fs)
{
    const std::size_t len = dst.size();
    const double fsTx = m_tx.dacStreamSps > 0.0 ? m_tx.dacStreamSps : fs;
    const double txPerRx = fsTx / (fs > 0.0 ? fs : 1.0);
    std::fill(dst.begin(), dst.end(), cf32(0.0f, 0.0f));

    auto note = [this](const QString& t) {
        if (t != m_loopNote) { m_loopNote = t; emit statusMessage(t); }
    };
    auto inBand = [fs](double hz) { return std::abs(hz) < 0.5 * fs; };

    if (m_tx.dacSource == int(PCIE_DAC_SRC_DDS)) {
        // dds_compiler_2: 32-bit phase accumulator clocked by the DAC stream
        // clock, increment = floor(f * 2^32 / fs) as the firmware programs it.
        const double inc  = std::floor(m_tx.ddsHz * 4294967296.0 / fsTx);
        const double fAct = inc * fsTx / 4294967296.0;
        if (!inBand(fAct)) {
            note(QStringLiteral("Simulated loopback: DDS %1 MHz is outside the "
                                "displayed ±%2 MHz band")
                     .arg(fAct / 1e6, 0, 'f', 3).arg(fs / 2e6, 0, 'f', 3));
            return;
        }
        const double d = wave::kTwoPi * fAct / fs;
        for (std::size_t i = 0; i < len; ++i) {
            dst[i] = cf32(static_cast<float>(std::cos(m_ddsPhase)),
                          static_cast<float>(std::sin(m_ddsPhase)));
            m_ddsPhase += d;
            if (m_ddsPhase > wave::kTwoPi) m_ddsPhase -= wave::kTwoPi;
        }
        note(QStringLiteral("Simulated loopback: DAC input DDS, %1 MHz")
                 .arg(fAct / 1e6, 0, 'f', 6));
        return;
    }

    if (m_tx.dacSource != int(PCIE_DAC_SRC_HOST)) {
        note(QStringLiteral("Simulated loopback: no DAC input routed — noise only"));
        return;
    }
    if (!m_tx.hostRunning) {
        note(QStringLiteral("Simulated loopback: DAC input is the host stream "
                            "and nothing is transmitting — noise only, as on "
                            "the board. Start the IQ generator."));
        return;
    }

    const double amp = std::clamp(m_tx.amplitude, 0.0, 1.0);
    if (m_tx.waveform == wave::Noise) {
        std::uniform_real_distribution<float> u(-1.0f, 1.0f);
        for (std::size_t i = 0; i < len; ++i)
            dst[i] = cf32(u(m_rng), u(m_rng)) * static_cast<float>(amp);
        note(QStringLiteral("Simulated loopback: host stream, noise"));
        return;
    }
    if (!inBand(m_tx.hostToneHz)) {
        note(QStringLiteral("Simulated loopback: host tone %1 MHz is outside the "
                            "displayed ±%2 MHz band")
                 .arg(m_tx.hostToneHz / 1e6, 0, 'f', 6).arg(fs / 2e6, 0, 'f', 3));
        return;
    }

    if (m_tx.coherent) {
        // The DAC plays the loop over and over. Sample n of the loop is the
        // shape at phase 2*pi*(k*n mod N)/N — exactly what IqGenerator::
        // renderLoop writes — evaluated on the fly, because a 16 MiB loop is
        // 4 M samples and a table of it would cost 32 MB per change. Read at
        // the receiver's rate, interpolating between DAC samples.
        const int N = std::max(16, m_tx.loopPairs);
        const long long k = wave::loopCycles(m_tx.hostToneHz, fsTx, N);
        const int w = m_tx.waveform;
        const double iqd = m_tx.iqPhaseDiff;
        auto txSample = [&](long long n) {
            const double phi = m_tx.phaseOffset
                             + wave::loopPhase(k, static_cast<int>(n % N), N);
            return cf32(static_cast<float>(amp * wave::shape(w, phi)),
                        static_cast<float>(amp * wave::shape(w, phi + iqd)));
        };
        for (std::size_t i = 0; i < len; ++i) {
            const long long a = static_cast<long long>(m_txPos);
            const float t = static_cast<float>(m_txPos - double(a));
            dst[i] = txSample(a) * (1.0f - t) + txSample(a + 1) * t;
            m_txPos += txPerRx;
            if (m_txPos >= double(N)) m_txPos = std::fmod(m_txPos, double(N));
        }
    } else {
        // Exact-frequency streaming, as the generator does with coherence
        // off. (On the board iwfg_h2c's replay then adds loop spurs; the
        // simulator shows the ideal signal.)
        const double d = wave::kTwoPi * m_tx.hostToneHz / fsTx;
        const int w = m_tx.waveform;
        for (std::size_t i = 0; i < len; ++i) {
            const double phi = m_tx.phaseOffset + d * m_txPos;
            dst[i] = cf32(static_cast<float>(amp * wave::shape(w, phi)),
                          static_cast<float>(amp * wave::shape(w, phi + m_tx.iqPhaseDiff)));
            m_txPos += txPerRx;
        }
        const double period = fsTx / std::max(1e-9, std::abs(m_tx.hostToneHz));
        if (m_txPos > 1e9) m_txPos = std::fmod(m_txPos, period);
    }
    note(QStringLiteral("Simulated loopback: host stream, %1 MHz")
             .arg(m_tx.hostToneHz / 1e6, 0, 'f', 6));
}

void SimulatedSource::start()
{
    if (!m_timer) {
        // Created lazily so the timer belongs to the thread running the source.
        m_timer = new QTimer(this);
        m_timer->setTimerType(Qt::PreciseTimer);
        connect(m_timer, &QTimer::timeout, this, &SimulatedSource::produce);
    }
    if (!m_timer->isActive()) m_timer->start(1000 / kSimBlocksPerSecond);

    m_active = true;
    m_error  = false;
    m_paused = false;
    m_runClock.restart();
    m_rateClock.restart();
    emitState();
    emit statusMessage(QStringLiteral("Simulated acquisition running"));
}

void SimulatedSource::stop()
{
    if (m_timer) m_timer->stop();
    m_active = false;
    m_paused = false;
    emitState();
    emit statusMessage(QStringLiteral("Acquisition stopped"));
}

cf32 SimulatedSource::nextSymbol()
{
    auto uni = [this](int levels) {
        std::uniform_int_distribution<int> d(0, levels - 1);
        return d(m_rng);
    };

    switch (m_cfg.acq.modulation) {
    case Modulation::QPSK: {
        const float i = (uni(2) ? 1.0f : -1.0f);
        const float q = (uni(2) ? 1.0f : -1.0f);
        return cf32(i, q) * 0.707f;
    }
    case Modulation::QAM16: {
        const float lv[4] = {-3, -1, 1, 3};
        return cf32(lv[uni(4)], lv[uni(4)]) * 0.316f;
    }
    case Modulation::QAM64: {
        const float lv[8] = {-7, -5, -3, -1, 1, 3, 5, 7};
        return cf32(lv[uni(8)], lv[uni(8)]) * 0.154f;
    }
    case Modulation::NoiseOnly:
    default:
        return cf32(0.0f, 0.0f);
    }
}

void SimulatedSource::produce()
{
    if (m_paused || !m_active) return;

    const int nch      = std::max(1, m_cfg.src.streamChannels);
    // Block length follows the configured display rate so the simulator
    // actually produces the rate the user asked for; previously it emitted a
    // fixed 60 x 4096 samples/s no matter what the rate box said, which is why
    // changing the sample rate appeared to have no effect on anything but the
    // axis labels. Clamped so a 122.88 MSPS setting cannot allocate unbounded
    // blocks -- above the ceiling the simulator runs slower than real time and
    // the status panel reports the honest measured rate.
    const double perTick  = m_cfg.acq.displayRateHz() / kSimBlocksPerSecond;
    const int    minBlock = std::max(2048, m_cfg.disp.fftSize * 2);
    const int    blockLen = static_cast<int>(
        std::clamp(perTick, static_cast<double>(minBlock), kSimMaxBlock));
    if (static_cast<int>(m_ch.size()) != nch) m_ch.resize(static_cast<std::size_t>(nch));

    if (!admit(static_cast<std::size_t>(blockLen))) {
        refreshStats();
        return;
    }

    SampleBlock block;
    block.channels.assign(static_cast<std::size_t>(nch), IQBlock(static_cast<std::size_t>(blockLen)));

    const double fs       = m_cfg.acq.displayRateHz();
    const double offsetHz = m_cfg.acq.signalOffsetMHz * 1e6;
    const double dPhase   = 2.0 * dsp::kPi * (offsetHz / (fs > 0 ? fs : 1.0));
    const double noiseRms = kNoiseRefRms * std::pow(10.0, -m_cfg.acq.snrDb / 20.0);

    if (m_tx.loopback) {
        // One transmitted signal, seen by every receive channel; per-channel
        // gain differs slightly so the channel selector visibly does something.
        IQBlock tx(static_cast<std::size_t>(blockLen));
        renderLoopback(tx, fs);
        for (int c = 0; c < nch; ++c) {
            IQBlock& dst = block.channels[static_cast<std::size_t>(c)];
            const float g = kLoopbackGain * (1.0f - 0.18f * static_cast<float>(c));
            for (int n = 0; n < blockLen; ++n) {
                cf32 s = tx[static_cast<std::size_t>(n)] * g;
                s += cf32(m_gauss(m_rng) * static_cast<float>(noiseRms),
                          m_gauss(m_rng) * static_cast<float>(noiseRms));
                s += cf32(-0.009f, 0.004f);
                dst[static_cast<std::size_t>(n)] = s;
            }
        }
        accountBytes(static_cast<quint64>(blockLen) * nch * sizeof(cf32));
        publish(block);
        refreshStats();
        return;
    }

    for (int c = 0; c < nch; ++c) {
        ChannelState& st = m_ch[static_cast<std::size_t>(c)];
        IQBlock& dst = block.channels[static_cast<std::size_t>(c)];

        // Each channel gets a slightly different offset and level so the
        // channel selector visibly does something.
        const double chOffset = dPhase * (1.0 + 0.35 * c);
        const float  chGain   = 1.0f - 0.18f * static_cast<float>(c);

        st.burstPh += 0.012;
        if (st.burstPh > 1.0) st.burstPh -= 1.0;

        for (int n = 0; n < blockLen; ++n) {
            if (st.spsIdx == 0) {
                st.prevSym = st.curSym;
                st.curSym  = nextSymbol();
            }
            // Raised-cosine style interpolation between symbols — a cheap
            // stand-in for a real RRC FIR.
            const float t = static_cast<float>(st.spsIdx) / static_cast<float>(m_sps);
            const float w = 0.5f * (1.0f - std::cos(static_cast<float>(dsp::kPi) * t));
            cf32 s = st.prevSym * (1.0f - w) + st.curSym * w;
            st.spsIdx = (st.spsIdx + 1) % m_sps;

            // Burst gating: a raised region that drifts slowly so the time
            // plot looks alive.
            const double pos   = static_cast<double>(n) / blockLen;
            const double start = 0.42 + 0.04 * std::sin(2.0 * dsp::kPi * st.burstPh);
            const double stop  = start + 0.26;
            double env = 0.35;
            if (pos > start && pos < stop) {
                const double edge = 0.02;
                double r = 1.0;
                if (pos < start + edge)     r = (pos - start) / edge;
                else if (pos > stop - edge) r = (stop - pos) / edge;
                env = 0.35 + 0.65 * std::clamp(r, 0.0, 1.0);
            }
            s *= static_cast<float>(env) * chGain;

            // Transmit carrier offset — the DDC's NCO is what removes this.
            const float ct = static_cast<float>(std::cos(st.phase));
            const float sn = static_cast<float>(std::sin(st.phase));
            s = cf32(s.real() * ct - s.imag() * sn, s.real() * sn + s.imag() * ct);
            st.phase += chOffset;
            if (st.phase > 2.0 * dsp::kPi) st.phase -= 2.0 * dsp::kPi;

            // Impairments, so the measurement panel is not trivially perfect.
            s += cf32(m_gauss(m_rng) * static_cast<float>(noiseRms),
                      m_gauss(m_rng) * static_cast<float>(noiseRms));
            s += cf32(-0.009f, 0.004f);

            dst[static_cast<std::size_t>(n)] = s;
        }
    }

    accountBytes(static_cast<quint64>(blockLen) * nch * sizeof(cf32));
    publish(block);
    refreshStats();
}

// =========================================================== FileSource

FileSource::FileSource(QObject* parent) : ISignalSource(parent) {}

FileSource::~FileSource() { closeFile(); }

QString FileSource::name() const
{
    if (m_openedPath.isEmpty()) return QStringLiteral("File replay (no file)");
    return QStringLiteral("File replay — %1").arg(QFileInfo(m_openedPath).fileName());
}

qint64 FileSource::totalSamples() const
{
    const int bps = bytesPerSample(m_cfg.src.format) * std::max(1, m_cfg.src.streamChannels);
    return bps > 0 ? m_dataBytes / bps : 0;
}

void FileSource::applyConfig(const sdr::Config& cfg)
{
    const bool reopen = (cfg.src.filePath != m_cfg.src.filePath)
                     || (cfg.src.format != m_cfg.src.format)
                     || (cfg.src.streamChannels != m_cfg.src.streamChannels)
                     || (cfg.src.headerBytes != m_cfg.src.headerBytes);
    m_cfg = cfg;
    if (reopen && m_active) {
        closeFile();
        if (!openFile()) return;
    }
}

bool FileSource::openFile()
{
    closeFile();
    if (m_cfg.src.filePath.isEmpty()) {
        m_error = true;
        emit sourceError(QStringLiteral("No capture file selected"));
        emitState();
        return false;
    }

    m_file = new QFile(m_cfg.src.filePath, this);
    if (!m_file->open(QIODevice::ReadOnly)) {
        const QString why = m_file->errorString();
        delete m_file;
        m_file = nullptr;
        m_error = true;
        emit sourceError(QStringLiteral("Cannot open %1 — %2").arg(m_cfg.src.filePath, why));
        emitState();
        return false;
    }

    m_openedPath = m_cfg.src.filePath;
    m_dataBytes  = std::max<qint64>(0, m_file->size() - m_cfg.src.headerBytes);
    m_file->seek(m_cfg.src.headerBytes);
    m_error   = false;
    m_wrapped = false;

    const qint64 samples = totalSamples();
    const double secs = m_cfg.acq.sampleRateHz() > 0
                            ? samples / m_cfg.acq.sampleRateHz() : 0.0;
    emit statusMessage(QStringLiteral("Opened %1 — %2 samples/ch (%3 s at %4 MSPS)")
                           .arg(QFileInfo(m_openedPath).fileName())
                           .arg(samples)
                           .arg(secs, 0, 'f', 3)
                           .arg(m_cfg.acq.sampleRateMsps, 0, 'f', 2));
    return true;
}

void FileSource::closeFile()
{
    if (m_file) {
        m_file->close();
        delete m_file;
        m_file = nullptr;
    }
    m_dataBytes = 0;
}

void FileSource::start()
{
    if (!m_file && !openFile()) return;

    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setTimerType(Qt::PreciseTimer);
        connect(m_timer, &QTimer::timeout, this, &FileSource::produce);
    }
    if (!m_timer->isActive()) m_timer->start(kFileTickMs);

    m_active = true;
    m_paused = false;
    m_runClock.restart();
    m_rateClock.restart();
    emitState();
    emit statusMessage(QStringLiteral("Offline replay running"));
}

void FileSource::stop()
{
    if (m_timer) m_timer->stop();
    m_active = false;
    m_paused = false;
    closeFile();
    emitState();
    emit statusMessage(QStringLiteral("Replay stopped"));
}

void FileSource::seekFraction(double frac)
{
    if (!m_file) return;
    const qint64 bps = bytesPerSample(m_cfg.src.format) * std::max(1, m_cfg.src.streamChannels);
    qint64 target = static_cast<qint64>(std::clamp(frac, 0.0, 1.0) * static_cast<double>(m_dataBytes));
    if (bps > 0) target -= target % bps;                 // stay sample-aligned
    m_file->seek(m_cfg.src.headerBytes + target);
    m_sampleIndex = bps > 0 ? static_cast<quint64>(target / bps) : 0;
    refreshStats(target, m_dataBytes);
}

void FileSource::produce()
{
    if (!m_active || m_paused || !m_file) return;

    const int nch = std::max(1, m_cfg.src.streamChannels);
    const int bps = bytesPerSample(m_cfg.src.format);
    const std::size_t frameBytes = static_cast<std::size_t>(bps) * static_cast<std::size_t>(nch);
    // Scale the block so one tick's nominal payload fits in a handful of
    // blocks. At 100+ MSPS a fixed 16 k block means thousands of queued
    // cross-thread deliveries per second, which is pure signalling overhead;
    // larger blocks carry the same bytes in far fewer events.
    std::size_t want = static_cast<std::size_t>(std::max(1024, m_cfg.src.blockSamples));
    if (m_cfg.src.pacedReplay) {
        const double perTickN = m_cfg.acq.displayRateHz()
                              * std::max(0.01, m_cfg.src.replaySpeed)
                              * kFileTickMs / 1000.0;
        while (want < 262144 && static_cast<double>(want) * 8.0 < perTickN)
            want *= 2;
    }

    // How many blocks this tick should deliver to hold the nominal rate.
    int blocks = 1;
    if (m_cfg.src.pacedReplay) {
        // Captures hold post-DDC samples, so nominal-rate replay is the
        // *display* rate. Pacing at the ADC rate replays decimation-times
        // too fast.
        const double persec = m_cfg.acq.displayRateHz() * std::max(0.01, m_cfg.src.replaySpeed);
        const double perTick = persec * kFileTickMs / 1000.0;
        blocks = std::max(1, static_cast<int>(std::lround(perTick / static_cast<double>(want))));
        blocks = std::min(blocks, 64);          // ceiling so the GUI never starves
    } else {
        blocks = 8;                             // free-run; back-pressure throttles
    }

    for (int b = 0; b < blocks; ++b) {
        if (!admit(want)) break;

        m_raw.resize(want * frameBytes);
        const qint64 got = m_file->read(m_raw.data(), static_cast<qint64>(m_raw.size()));

        if (got <= 0) {
            if (m_cfg.src.loopFile) {
                m_file->seek(m_cfg.src.headerBytes);
                m_sampleIndex = 0;
                if (!m_wrapped) {
                    m_wrapped = true;
                    emit statusMessage(QStringLiteral("End of capture — looping"));
                }
                continue;
            }
            emit statusMessage(QStringLiteral("End of capture"));
            stop();
            return;
        }

        SampleBlock block;
        // Decode is deferred to the DSP stage: at high stream rates decoding
        // every block here burned a full core on samples the display throttle
        // then discarded.
        block.raw          = QByteArray(m_raw.data(), static_cast<int>(got));
        block.rawFormat    = m_cfg.src.format;
        block.rawChannels  = nch;
        block.rawFullScale = m_cfg.src.fullScale;
        if (block.samplesPerChannel() == 0) break;

        block.filePos   = m_file->pos() - m_cfg.src.headerBytes;
        block.fileTotal = m_dataBytes;
        accountBytes(static_cast<quint64>(got));
        publish(block);
    }

    refreshStats(m_file->pos() - m_cfg.src.headerBytes, m_dataBytes);
}

// ============================================================ DmaSource

namespace {
/// Give the capture FIFO the largest pipe buffer an unprivileged process may
/// (fs.pipe-max-size, 1 MiB by default). The default 64 KiB makes
/// c2h_stream's 16 MiB writes cost ~256 writer/reader wake-ups each. While it
/// writes, the card fills the next buffer; if the write takes longer than
/// that, the FPGA's C2H FIFO overflows — and the driver's overflow recovery
/// resets the QDMA, transmit included. Best effort: failure changes nothing.
void enlargePipe(int fd)
{
#if defined(Q_OS_LINUX) && defined(F_SETPIPE_SZ)
    int want = 1 << 20;
    QFile f(QStringLiteral("/proc/sys/fs/pipe-max-size"));
    if (f.open(QIODevice::ReadOnly)) {
        bool ok = false;
        const int v = QString::fromLatin1(f.readAll()).trimmed().toInt(&ok);
        if (ok && v >= (64 << 10)) want = std::min(v, 16 << 20);
    }
    while (want >= (64 << 10) && ::fcntl(fd, F_SETPIPE_SZ, want) < 0)
        want /= 2;
#else
    (void)fd;
#endif
}
} // namespace

DmaSource::DmaSource(QObject* parent) : ISignalSource(parent) {}

DmaSource::~DmaSource() { closeDevice(); }

QString DmaSource::name() const
{
    if (m_openedPath.isEmpty()) return QStringLiteral("PCIe DMA (closed)");
    return QStringLiteral("PCIe DMA — %1").arg(m_openedPath);
}

void DmaSource::applyConfig(const sdr::Config& cfg)
{
    const bool reopen = (cfg.src.devicePath != m_cfg.src.devicePath);
    m_cfg = cfg;
    m_blockBytes = static_cast<std::size_t>(std::max(1024, cfg.src.blockSamples))
                 * static_cast<std::size_t>(bytesPerSample(cfg.src.format))
                 * static_cast<std::size_t>(std::max(1, cfg.src.streamChannels));
    if (m_raw.size() != m_blockBytes) {
        // Keep the bytes already read: the reader is now reconfigured in
        // place while streaming, and throwing a partial block away would
        // shift the I/Q framing of everything after it.
        m_raw.resize(m_blockBytes, 0);
        if (m_filled > m_blockBytes) m_filled = 0;
    }
    if (reopen && m_active) {
        closeDevice();
        if (openDevice()) QMetaObject::invokeMethod(this, "pump", Qt::QueuedConnection);
    }
}

void DmaSource::fail(const QString& why)
{
    m_error  = true;
    m_active = false;
    closeDevice();
    emit sourceError(why);
    emitState();
}

bool DmaSource::openDevice()
{
#if defined(Q_OS_UNIX)
    const QString path = m_cfg.src.devicePath;
    if (path.isEmpty()) {
        fail(QStringLiteral("No DMA device node or FIFO path given"));
        return false;
    }

    const QByteArray p = path.toLocal8Bit();
    struct stat st {};
    if (::stat(p.constData(), &st) != 0) {
        fail(QStringLiteral("%1 does not exist (%2)").arg(path, QString::fromLocal8Bit(strerror(errno))));
        return false;
    }
    m_isFifo = S_ISFIFO(st.st_mode);

    // O_NONBLOCK matters for a FIFO with no writer yet: without it, open()
    // itself blocks until a writer appears and the UI looks hung.
    int fd = ::open(p.constData(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        fail(QStringLiteral("Cannot open %1 — %2").arg(path, QString::fromLocal8Bit(strerror(errno))));
        return false;
    }

    if (m_isFifo) enlargePipe(fd);

    m_fd         = fd;
    m_openedPath = path;
    m_pollable   = true;
    m_emptyPolls = 0;
    m_filled     = 0;
    m_error      = false;
    m_eofClock.invalidate();
    m_eofWarned  = false;

    emit statusMessage(QStringLiteral("Attached to %1 (%2)")
                           .arg(path, m_isFifo ? QStringLiteral("FIFO")
                                               : QStringLiteral("device node")));
    return true;
#else
    fail(QStringLiteral("Live DMA capture requires a POSIX device node; "
                        "use Offline mode on this platform."));
    return false;
#endif
}

void DmaSource::closeDevice()
{
#if defined(Q_OS_UNIX)
    if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }
#endif
    m_openedPath.clear();
    m_filled = 0;
}

void DmaSource::start()
{
    m_blockBytes = static_cast<std::size_t>(std::max(1024, m_cfg.src.blockSamples))
                 * static_cast<std::size_t>(bytesPerSample(m_cfg.src.format))
                 * static_cast<std::size_t>(std::max(1, m_cfg.src.streamChannels));
    m_raw.assign(m_blockBytes, 0);
    m_filled = 0;

    if (m_fd < 0 && !openDevice()) return;

    m_active = true;
    m_paused = false;
    m_error  = false;
    m_runClock.restart();
    m_rateClock.restart();
    emitState();
    emit statusMessage(QStringLiteral("Live acquisition running"));

    QMetaObject::invokeMethod(this, "pump", Qt::QueuedConnection);
}

void DmaSource::stop()
{
    m_active = false;
    m_paused = false;
    closeDevice();
    emitState();
    emit statusMessage(QStringLiteral("Acquisition stopped"));
}

// The capture helper's FIFO lost its writer. 1.2.3 treated this as "stay
// attached": read() returned 0 at once, poll() reported POLLHUP at once, and
// the pump re-queued itself with no delay — a busy loop that burned a core.
// Worse, when the helper was restarted it created a NEW FIFO (unlink + mkfifo)
// and the reader stayed on the old, unlinked inode, so the stream never came
// back. Now: wait without spinning, and when the path names a different FIFO
// than the one held, re-attach to it.
int DmaSource::writerGone()
{
#if defined(Q_OS_UNIX)
    if (!m_eofClock.isValid()) m_eofClock.start();

    if (m_isFifo && m_fd >= 0) {
        const QByteArray p = m_openedPath.toLocal8Bit();
        struct stat ps {}, fs {};
        const bool pathIsFifo = ::stat(p.constData(), &ps) == 0 && S_ISFIFO(ps.st_mode);
        const bool fdOk = ::fstat(m_fd, &fs) == 0;
        if (pathIsFifo && fdOk && (ps.st_ino != fs.st_ino || ps.st_dev != fs.st_dev)) {
            const int fd = ::open(p.constData(), O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                ::close(m_fd);
                enlargePipe(fd);
                m_fd = fd;
                m_filled = 0;          // a new writer starts on a frame boundary
                ++m_reattaches;
                m_eofClock.invalidate();
                m_eofWarned = false;
                emit statusMessage(QStringLiteral(
                    "Re-attached to %1 — the capture helper was restarted (%2).")
                    .arg(m_openedPath).arg(m_reattaches));
                return 0;
            }
        }
    }
    if (!m_eofWarned && m_eofClock.elapsed() > 1500) {
        m_eofWarned = true;
        emit statusMessage(QStringLiteral(
            "Nothing is writing to %1 — waiting for the capture helper.").arg(m_openedPath));
    }
#endif
    return 50;
}

void DmaSource::pump()
{
#if defined(Q_OS_UNIX)
    if (!m_active || m_fd < 0) return;

    if (m_paused) {
        // Keep the device drained while paused so resuming shows live data
        // rather than a backlog, but publish nothing.
        char sink[65536];
        bool eof = false;
        for (int i = 0; i < 8; ++i) {
            const ssize_t n = ::read(m_fd, sink, sizeof(sink));
            if (n == 0) { eof = true; break; }
            if (n < 0) break;
        }
        refreshStats();
        QTimer::singleShot(eof ? writerGone() : 30, this, &DmaSource::pump);
        return;
    }

    QElapsedTimer budget;
    budget.start();

    bool sawData = false;
    bool eof = false;
    while (budget.elapsed() < kDmaBudgetMs) {
        if (m_filled >= m_blockBytes) break;

        const ssize_t got = ::read(m_fd, m_raw.data() + m_filled, m_blockBytes - m_filled);

        if (got > 0) {
            m_filled += static_cast<std::size_t>(got);
            accountBytes(static_cast<quint64>(got));
            sawData = true;
            if (m_eofClock.isValid()) { m_eofClock.invalidate(); m_eofWarned = false; }
        } else if (got == 0) {
            eof = true;                 // FIFO writer closed
            break;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            break;
        } else {
            fail(QStringLiteral("Read error on %1 — %2")
                     .arg(m_openedPath, QString::fromLocal8Bit(strerror(errno))));
            return;
        }

        if (m_filled >= m_blockBytes) {
            const std::size_t samples =
                m_blockBytes / static_cast<std::size_t>(bytesPerSample(m_cfg.src.format)
                                                        * std::max(1, m_cfg.src.streamChannels));
            if (admit(samples)) {
                SampleBlock block;
                block.raw          = QByteArray(m_raw.data(),
                                                static_cast<int>(m_blockBytes));
                block.rawFormat    = m_cfg.src.format;
                block.rawChannels  = std::max(1, m_cfg.src.streamChannels);
                block.rawFullScale = m_cfg.src.fullScale;
                publish(block);
            }
            m_filled = 0;
        }
    }

    refreshStats();

    if (eof && !sawData) {
        QTimer::singleShot(writerGone(), this, &DmaSource::pump);
        return;
    }

    // Wait for readability rather than spinning when the fabric is idle.
    int delay = 0;
    if (!sawData) {
        if (!m_pollable) {
            delay = 2;
        } else {
            struct pollfd pfd {};
            pfd.fd     = m_fd;
            pfd.events = POLLIN;
            const int pr = ::poll(&pfd, 1, 20);
            if (pr < 0 && errno != EINTR) delay = 20;
            if (pfd.revents & POLLNVAL) {
                // A character device without poll support. 1.2.3 switched the
                // fd to BLOCKING here, which parked this thread inside read()
                // — and the GUI thread, waiting for stop() on it, froze with
                // it. Stay non-blocking and poll on a short timer instead.
                m_pollable = false;
                delay = 2;
            } else if (pfd.revents & POLLHUP && !(pfd.revents & POLLIN)) {
                delay = writerGone();
            }
        }
    }

    if (delay > 0) QTimer::singleShot(delay, this, &DmaSource::pump);
    else           QMetaObject::invokeMethod(this, "pump", Qt::QueuedConnection);
#endif
}

} // namespace sdr
