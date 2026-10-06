#pragma once
// ---------------------------------------------------------------------------
// IqGenerator — the H2C producer. Replaces the GNU Radio
// iq_source_v9 -> Throttle -> fifo_sink_v9 chain with an in-application
// equivalent, keeping the wire contract and the writer semantics identical.
//
// WHY THIS EXISTS
// ---------------
//     <producer>  ->  /tmp/iwfg_h2c.fifo  ->  iwfg_h2c  ->  card  ->  DAC
//
// iwfg_h2c is only the FIFO->card mover. Nothing in the workstation wrote INTO
// the FIFO, so the pipe had no producer and the DAC received nothing. The
// control block at /dev/shm/iqctl carries pacing and frequency metadata, not
// samples.
//
// THE FOUR CONTRACTS THIS FILE HONOURS
// ------------------------------------
// The first three are taken from the reference implementation, not invented
// here. Each was a real bug that the GNU Radio side already hit and fixed.
//
// 1. DAC PACKING (fifo_sink_v9 header)
//        bits[15:2] = 14-bit signed sample, MSB-aligned
//        bits[1:0]  = 00, ALWAYS
//    Their v3 scaled floats straight to +/-32764 and rounded, leaving ~75% of
//    samples with nonzero bits[1:0]. The correct construction is: quantise to
//    int14 (+/-8191) FIRST, then left-shift by 2. Peak 8191<<2 = 32764.
//
// 2. NON-BLOCKING WRITER (fifo_sink_v9 v4)
//    A blocking write() wedges forever once the pipe fills and a reader
//    freezes. The fd stays O_NONBLOCK and writes are paced with select(),
//    so stop() is honoured within ~0.25 s in ANY state.
//
// 3. FIFO INODE RE-RESOLUTION (iwfg_h2c.c note D)
//    A blocking open(O_WRONLY) resolves ONE inode and waits on it; if the
//    consumer does unlink+mkfifo the writer waits on a dead inode forever.
//    Every retry re-resolves the path, and ENOENT recreates the FIFO.
//
// 4. LOOP COHERENCE (new in 1.2.1 — see Waveform.h for the full argument)
//    iwfg_h2c replays the most recent chunk back to back until a new one
//    arrives, so each chunk is played as a LOOP. A tone that does not fit the
//    chunk a whole number of times jumps phase at every replay and the DAC
//    output carries a comb of spurs. In coherent mode (the default) the
//    generator transmits the nearest frequency k*fs/N, every chunk is the same
//    seamless loop, and the output is a clean tone. The frequency actually
//    transmitted is reported through transmitting().
//    Coherent mode requires chunkBytes == the chunk_bytes iwfg_h2c runs with
//    (the GUI launches it with BackendConfig::h2cChunkBytes).
//
// THROUGHPUT
// ----------
// Because of the replay, the producer does NOT have to match the card's
// 763 MiB/s. In coherent mode the loop is written ONCE per configuration and
// iwfg_h2c replays it (see IqGenerator.cpp, kIdlePollMs, for why rewriting it
// is harmful with a large chunk). What must keep up is iwfg_h2c's own DMA,
// which is why the H2C chunk is large (BackendLauncher.h).
//
// STOP MEANS SILENCE
// ------------------
// On stop a zero chunk is written before the FIFO is closed, so the card's
// replay becomes silence instead of the last tone.
// ---------------------------------------------------------------------------

#include <QObject>
#include <QString>
#include <QAtomicInt>
#include <QMutex>
#include <QVector>
#include <cstdint>

namespace sdr {

class IqGenerator : public QObject
{
    Q_OBJECT
public:
    /// Waveform numbering matches iq_source_v9 exactly, so behaviour is
    /// identical to the GNU Radio flowgraph operators already know.
    enum Waveform { Sine = 0, Cosine = 1, Square = 2, Saw = 3, Triangle = 4, Noise = 5 };
    Q_ENUM(Waveform)

    struct Config {
        QString  fifoPath      = QStringLiteral("/tmp/iwfg_h2c.fifo");
        Waveform waveform      = Sine;
        double   sampleRateSps = 200e6;   ///< must match the DAC stream rate
        double   frequencyHz   = 10e6;    ///< requested
        double   amplitude     = 0.9;     ///< 0..1 of full scale
        double   phaseOffset   = 0.0;     ///< radians
        double   iqPhaseDiff   = -1.5707963267948966;  ///< -pi/2 = standard IQ
        int      chunkBytes    = 16384;   ///< == iwfg_h2c chunk_bytes; multiple of 4
        /// Transmit the nearest loop-coherent frequency (clean spectrum). When
        /// false the requested frequency is streamed phase-continuously, as
        /// the GNU Radio chain did, and iwfg_h2c's replay adds loop spurs.
        bool     coherent      = true;
    };

    explicit IqGenerator(QObject* parent = nullptr);
    ~IqGenerator() override;

    bool running() const { return m_run.loadAcquire() != 0; }

    /// Peak int16 value actually emitted: 8191 << 2. Exposed so the UI can
    /// state the real full-scale figure instead of implying 32767.
    static constexpr int peakInt16() { return 8191 << 2; }

    /// IQ pairs per chunk for a byte count, after the same rounding start()
    /// applies — so the UI can preview the loop exactly.
    static int loopPairsFor(int chunkBytes);

    /// The frequency this configuration will actually put on the DAC.
    static double transmittedHz(const Config& cfg);

    /// Fill `out` (2*pairs int16, I/Q interleaved) with one loop of `cfg`,
    /// packed for the DAC. Public and static so it can be verified in
    /// isolation, and so there is exactly one definition of the samples.
    static void renderLoop(const Config& cfg, qint16* out, int pairs, quint32* rngState);

    /// Thread-safe from ANY thread, including while start() is running (its
    /// loop never returns to the event loop, so a queued slot call would never
    /// be delivered). Applied at the next chunk boundary, phase preserved
    /// where the mode allows. The FIFO path and chunk size are fixed for a run.
    void requestUpdate(const Config& cfg);

public slots:
    void start(const sdr::IqGenerator::Config& cfg);
    void stop();
    /// Kept for source compatibility; forwards to requestUpdate().
    void updateConfig(const sdr::IqGenerator::Config& cfg);

signals:
    void started();
    void stopped();
    void connectedChanged(bool connected);   ///< a reader is attached
    void error(const QString& reason);
    void progress(quint64 totalSamples, double megaSamplesPerSec,
                  quint64 chunks, quint64 reconnects);
    /// Emitted at start and after every applied update: what is really going
    /// to the DAC. `actualHz` differs from the request in coherent mode.
    void transmitting(double requestedHz, double actualHz, int loopPairs, bool coherent);
    /// Coherent mode: the loop has been handed to iwfg_h2c (count of loads).
    void loopLoaded(quint64 loads);
    /// Stop wrote a silent chunk, so the card's replay is now silence.
    void silenced();

private:
    enum class WriteResult { Done, Stopped, Broken };
    WriteResult writeBytes(int fd, const char* p, qint64 len, bool honourStop, int timeoutMs);
    void   writeSilence(int fd);
    void fillChunk(qint16* out, int pairs);  ///< non-coherent streaming path
    double waveAt(double phi) const;
    int    openFifo();
    bool   ensureFifo();
    WriteResult streamUntilBroken(int fd, quint64* total, quint64* chunks);
    void   applyPending();                   ///< generator thread only
    void   rebuildLoop();                    ///< generator thread only

    Config     m_cfg;                        ///< generator thread only
    QMutex     m_pendingLock;
    Config     m_pending;                    ///< guarded by m_pendingLock
    QAtomicInt m_run    { 0 };
    QAtomicInt m_reload { 0 };
    double     m_phase  = 0.0;    ///< carried across chunks: no discontinuity
    quint32    m_rng    = 0x12345678u;
    quint64    m_reconnects = 0;
    QVector<qint16> m_buf;
    bool       m_loopValid = false;          ///< m_buf holds the coherent loop
    qint64     m_partial   = 0;              ///< bytes of the current chunk already written
};

} // namespace sdr

Q_DECLARE_METATYPE(sdr::IqGenerator::Config)
