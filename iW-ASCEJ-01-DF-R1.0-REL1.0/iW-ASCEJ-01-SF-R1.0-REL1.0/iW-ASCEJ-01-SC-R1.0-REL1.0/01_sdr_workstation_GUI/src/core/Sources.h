#pragma once
#include "Types.h"
#include <QElapsedTimer>
#include <QObject>
#include <random>

class QTimer;
class QFile;

namespace sdr {

/// Everything the DSP chain needs from an acquisition device. Live and offline
/// sources sit behind the same interface, which is what lets Live Mode and
/// Offline Mode share one visualisation pipeline instead of two.
///
/// All slots run on the source thread. `blockReady` is emitted from there and
/// crosses a queued connection into the DSP thread.
class ISignalSource : public QObject
{
    Q_OBJECT
public:
    explicit ISignalSource(QObject* parent = nullptr) : QObject(parent) {}
    ~ISignalSource() override = default;

    virtual QString name() const = 0;
    /// True when the source can be repositioned (file replay).
    virtual bool    seekable() const { return false; }

    void setBackPressure(const BackPressurePtr& bp) { m_bp = bp; }

public slots:
    virtual void start() = 0;
    virtual void stop()  = 0;
    virtual void setPaused(bool paused) { m_paused = paused; emitState(); }
    virtual void applyConfig(const sdr::Config& cfg) { m_cfg = cfg; }
    virtual void seekFraction(double /*frac*/) {}
    /// What the transmitter is doing. Only the simulator acts on it (as an RF
    /// loopback); a real receiver shows what really arrives.
    virtual void setTxState(const sdr::TxState& /*tx*/) {}

signals:
    void blockReady(const sdr::SampleBlock& block);
    void statusMessage(const QString& text);
    void runStateChanged(sdr::RunState state);
    void sourceError(const QString& text);
    /// Emitted roughly twice a second with throughput and progress.
    void statsUpdated(const sdr::StreamStats& stats);

protected:
    void emitState() { emit runStateChanged(state()); }

    RunState state() const
    {
        if (m_error)   return RunState::Error;
        if (!m_active) return RunState::Stopped;
        return m_paused ? RunState::Paused : RunState::Running;
    }

    /// Enforces the pipeline depth limit. Returns false when the DSP stage has
    /// fallen far enough behind that this block must be discarded — which is
    /// exactly the overrun a real DMA ring reports.
    bool admit(std::size_t samplesPerChannel);
    void publish(SampleBlock& block);
    void accountBytes(quint64 bytes);
    void refreshStats(qint64 filePos = -1, qint64 fileTotal = -1);

    Config          m_cfg;
    BackPressurePtr m_bp;
    StreamStats     m_stats;
    bool            m_active = false;
    bool            m_paused = false;
    bool            m_error  = false;
    quint64         m_seq    = 0;
    quint64         m_sampleIndex = 0;

    QElapsedTimer   m_rateClock;
    QElapsedTimer   m_runClock;
    quint64         m_bytesWindow = 0;
    quint64         m_samplesWindow = 0;
};

// ------------------------------------------------------------- simulated

/// Generates a pulse-shaped, noisy, frequency-offset burst per channel so
/// every plot has something realistic to draw with no hardware attached.
///
/// Once the transmit side is configured in the H2C / RFDC window (TxState with
/// loopback set) it models the RF loopback instead: the DDS tone when the DAC
/// input is the DDS, the IQ generator's exact waveform when it is the host
/// stream and the generator runs, and only noise when the host stream is
/// selected with nothing feeding it — the same three outcomes as the board.
class SimulatedSource : public ISignalSource
{
    Q_OBJECT
public:
    explicit SimulatedSource(QObject* parent = nullptr);

    QString name() const override { return QStringLiteral("Simulated QDMA RX"); }

public slots:
    void start() override;
    void stop()  override;
    void applyConfig(const sdr::Config& cfg) override;
    void setTxState(const sdr::TxState& tx) override;

private slots:
    void produce();

private:
    /// Fills one channel's worth of loopback samples (unit gain, no noise).
    void renderLoopback(IQBlock& dst, double fs);

    struct ChannelState {
        double phase    = 0.0;
        double burstPh  = 0.0;
        int    spsIdx   = 0;
        cf32   curSym{0.0f, 0.0f};
        cf32   prevSym{0.0f, 0.0f};
    };

    cf32 nextSymbol();

    QTimer* m_timer = nullptr;
    int     m_sps   = 4;
    std::vector<ChannelState>       m_ch;

    TxState              m_tx;
    double               m_txPos   = 0.0; ///< position in the loop, samples
    double               m_ddsPhase = 0.0;
    QString              m_loopNote;      ///< last status line, to avoid spam
    std::mt19937                    m_rng{0xC0FFEE};
    std::normal_distribution<float> m_gauss{0.0f, 1.0f};
};

// ------------------------------------------------------------ file replay

/// Offline mode. Streams a stored capture through the identical block
/// interface, optionally paced to the nominal sample rate so the display
/// behaves the way it does live.
class FileSource : public ISignalSource
{
    Q_OBJECT
public:
    explicit FileSource(QObject* parent = nullptr);
    ~FileSource() override;

    QString name() const override;
    bool    seekable() const override { return true; }

    /// Total per-channel samples in the file, or 0 if not open.
    qint64  totalSamples() const;

public slots:
    void start() override;
    void stop()  override;
    void applyConfig(const sdr::Config& cfg) override;
    void seekFraction(double frac) override;

private slots:
    void produce();

private:
    bool openFile();
    void closeFile();

    QFile*            m_file = nullptr;
    QTimer*           m_timer = nullptr;
    QString           m_openedPath;
    std::vector<char> m_raw;
    qint64            m_dataBytes = 0;
    bool              m_wrapped   = false;
};

// -------------------------------------------------------------- live DMA

/// Live mode. Reads a PCIe DMA character device (`/dev/xdma0_c2h_0`,
/// `/dev/iwfg0`, …) or a named FIFO and republishes fixed-size blocks.
///
/// The read loop re-posts itself through the event loop rather than spinning
/// inside a while(true), so stop, pause and config changes are serviced
/// promptly without a second synchronisation primitive.
class DmaSource : public ISignalSource
{
    Q_OBJECT
public:
    explicit DmaSource(QObject* parent = nullptr);
    ~DmaSource() override;

    QString name() const override;

public slots:
    void start() override;
    void stop()  override;
    void applyConfig(const sdr::Config& cfg) override;

private slots:
    void pump();

private:
    bool openDevice();
    void closeDevice();
    void fail(const QString& why);
    /// The FIFO's writer has gone (read() returned 0). Waits without spinning
    /// and re-attaches when the capture helper comes back with a new FIFO.
    /// Returns the delay before the next pump.
    int  writerGone();

    int               m_fd = -1;
    QString           m_openedPath;
    std::vector<char> m_raw;      ///< accumulates until a full block is present
    std::size_t       m_filled = 0;
    std::size_t       m_blockBytes = 0;
    bool              m_isFifo = false;
    bool              m_pollable = true;
    int               m_emptyPolls = 0;
    QElapsedTimer     m_eofClock;       ///< since the writer went away
    bool              m_eofWarned = false;
    int               m_reattaches = 0;
};

} // namespace sdr
