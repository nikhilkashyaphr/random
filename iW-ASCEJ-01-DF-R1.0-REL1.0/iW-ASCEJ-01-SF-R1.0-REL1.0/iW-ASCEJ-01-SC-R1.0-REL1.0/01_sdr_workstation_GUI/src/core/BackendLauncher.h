#pragma once
// ---------------------------------------------------------------------------
// BackendLauncher — owns the lifecycle of the external driver applications
// (C2H capture, H2C playback) that feed the acquisition pipeline.
//
// Design contract: this module contains NO UI. It knows nothing about widgets,
// dialogs or windows. It exposes state and diagnostics purely through Qt
// signals, so the UI layer can render them however it likes and the backend
// can be driven headlessly or from tests.
//
// Integration model
// -----------------
// Both supplied applications are FIFO-based, which is what makes this clean:
//
//   c2h_stream <devnode> <fifo>          card -> FIFO   (the GUI reads this FIFO)
//   iwfg_h2c   <devnode> <fifo> <chunk>  FIFO -> card   (the IQ generator writes it)
//
// So the workstation does not need to re-implement DMA, buffer registration
// or streaming synchronisation: the existing, validated applications do that
// exactly as they do under GNU Radio. The workstation launches them, watches
// them, and attaches its existing DmaSource to the capture FIFO. The apps are
// used unmodified.
//
// Lifecycle guarantees (1.2.4)
//   * Nothing here blocks the GUI thread for more than a few milliseconds.
//     start() is non-blocking; readiness arrives via becameReady(). stop() is
//     ASYNCHRONOUS: SIGINT, then SIGTERM, then SIGKILL on timers, and
//     stopped() is emitted once every helper has exited. (1.2.3 waited up to
//     10 s in stop() with the window frozen.)
//   * A running QProcess is never destroyed. Qt's destructor waits for the
//     process to die, and a helper stuck in an uninterruptible driver call
//     does not die — which froze the whole window. Such a process is handed
//     to a reaper and deleted only once it has really exited.
//   * start() while a stop is still in progress is queued, not refused.
//   * Stop is ORDERED (1.2.5): transmit first, capture once transmit has
//     gone. The driver soft-resets the QDMA only when the last C2H stream
//     closes with no other stream open, so this order leaves the device in
//     a clean state for the next start; the old simultaneous SIGINT could
//     close capture first and skip the reset.
//   * The capture helper is the source: if it ends on its own after it had
//     been streaming, it is restarted (bounded: c2hRestartLimit per minute)
//     and the reader re-attaches to its new FIFO by itself. Only a helper
//     that cannot start, or keeps dying, ends the session.
//   * The transmit helper is optional: its failure never ends the session.
//     A buffer size the card or driver will not take is stepped down
//     automatically; the operator can relaunch it with restartH2c().
//   * The destructor stops everything within ~2 s, so a closed window cannot
//     leave orphaned processes holding the device node open.
// ---------------------------------------------------------------------------

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QList>
#include <QElapsedTimer>
#include <functional>

class QTimer;

namespace sdr {

/// Which backend pipeline to run. Deliberately minimal — these are the only
/// modes the hardware workflow calls for.
enum class BackendMode {
    C2H,        ///< capture only: card -> FIFO -> workstation
    H2C_C2H,    ///< loopback: workstation-fed FIFO -> card -> FIFO -> workstation
    UdpStream   ///< 100G UDP -> iwfg_c2h -> FIFO -> workstation
};

QString backendModeName(BackendMode m);

// ---------------------------------------------------------------------------
// H2C playback buffer (iwfg_h2c's chunk_bytes).
//
// iwfg_h2c moves the FIFO to the card one blocking DMA_DATA call per chunk,
// and re-posts the last chunk until a new one arrives. Every call costs a
// fixed round trip: with 16 KiB chunks that capped delivery at ~640 MiB/s,
// below the 763 MiB/s the DAC reads (one 32-bit I/Q word per 200 MHz clock),
// and the shortfall played as broadband interference.
//
// 1.2.2 therefore went to 16 MiB. On the bench nothing reached the DAC at
// that size: a 16 MiB user buffer is 4096 pages, i.e. thousands of H2C
// descriptors per transfer, which the driver's H2C ring evidently does not
// complete. 1 MiB (256 pages) already makes the per-transfer cost negligible
// (~0.3 %), so it is the default; the launcher steps down this ladder by
// itself if the card still does not consume. IWFG_H2C_CHUNK_BYTES in the
// environment overrides the starting size.
//
// The chunk is also the generator's replay loop, so it sets the host tone's
// frequency resolution: fs / (bytes / 4) — 763 Hz at 1 MiB and 200 MSPS.
// ---------------------------------------------------------------------------
constexpr int kH2cChunkLadder[] = { 16 << 20, 4 << 20, 1 << 20, 256 << 10, 64 << 10, 16 << 10 };
constexpr int kH2cChunkDefault = 1 << 20;
constexpr int kH2cChunkMin = 1024;
constexpr int kH2cChunkMax = 64 << 20;          ///< iwfg_h2c's own limit

/// Starting chunk size: IWFG_H2C_CHUNK_BYTES if valid, else 1 MiB.
int defaultH2cChunkBytes();
/// Next smaller rung of kH2cChunkLadder below `bytes`, or 0 at the bottom.
int nextSmallerH2cChunk(int bytes);
/// "16 MiB", "256 KiB", "1000 B".
QString byteSizeText(qint64 bytes);
/// Parse iwfg_h2c's once-a-second line "[H2C] 640.58 MB/s  total=... GR_updates=48/s".
/// Its "MB" is MiB (it divides by 1048576). Returns false for any other line.
bool parseH2cRateLine(const QString& line, double* mibPerSec, int* updatesPerSec);
/// Parse c2h_stream's "Streaming: 2416.93 MB/s (total: ... MB)" (also MiB/s).
bool parseC2hRateLine(const QString& line, double* mibPerSec);
/// A line in which iwfg_h2c reports that the card is not consuming its data
/// (stall kick, driver timeout, EBUSY exhaustion, device cycling).
bool isH2cStallLine(const QString& line);

/// What the transmit helper is doing, for display.
enum class H2cState {
    NotRunning,      ///< no transmit helper (C2H-only mode, or stopped)
    WaitingForData,  ///< running, waiting for the IQ generator to connect
    Starting,        ///< generator connected, first transfers in flight
    Streaming,       ///< the card is consuming: rate lines arrive
    Stalled,         ///< the card is not consuming (the helper self-heals)
    Restarting,      ///< being relaunched (smaller buffer / operator retry)
    Failed           ///< exited, and will not be restarted automatically
};
QString h2cStateName(H2cState s);

/// Why c2h_stream ended. It exits 0 for every one of these, so the exit
/// code says nothing — the reason is in what it printed.
enum class C2hExit { Unknown, ReaderClosed, DriverError, Interrupted, StartupError };
/// Classify from the helper's output. `detail` receives the decisive line.
C2hExit classifyC2hExit(const QStringList& lines, QString* detail);
QString c2hExitText(C2hExit e);

/// Everything the launcher needs to bring a mode up. Defaults match the
/// applications' own documented defaults so an unconfigured start behaves
/// exactly like running them by hand.
struct BackendConfig {
    BackendMode mode          = BackendMode::C2H;
    QString     c2hDevice     = QStringLiteral("/dev/iwfg0");
    QString     c2hFifo       = QStringLiteral("/tmp/iwfg_fifo");
    QString     h2cDevice     = QStringLiteral("/dev/iwfg1");
    QString     h2cFifo       = QStringLiteral("/tmp/iwfg_h2c.fifo");
    int         h2cChunkBytes = defaultH2cChunkBytes();
    /// Automatic capture restarts allowed per minute before giving up.
    int         c2hRestartLimit = 3;

    // --- UDP live stream (backend/udp/iwfg_c2h) --------------------------
    int         udpPort      = 50000;
    QString     udpBindAddr;                 ///< empty = any
    QString     udpFifo      = QStringLiteral("/tmp/iwfg_c2h.fifo");
    int         udpPayload   = 1472;         ///< bytes per datagram
    QString     udpRawIface;                 ///< non-empty = AF_PACKET mode
    int         udpRxCpu     = -1;           ///< -1 = unpinned
    int         udpWrCpu     = -1;
    bool        udpHugepages = false;
};

/// One managed child process.
struct BackendProcess {
    enum Role { Capture, Playback, Udp };
    QString    label;       ///< "C2H capture", "H2C playback"
    QString    program;     ///< the helper itself (not a line-buffering wrapper)
    QStringList args;
    QProcess*  proc = nullptr;
    Role       role = Capture;
    /// Last lines the process printed (stdout and stderr merged).
    QStringList tail;
    /// The same, minus the once-a-second rate lines. A helper that dies
    /// explains itself in a line printed once; with the rate lines kept, that
    /// line had scrolled out of the report by the time it was shown.
    QStringList notable;
    /// Is the pipeline dead without this stage? C2H capture is the SOURCE;
    /// H2C playback is an independent transmit path.
    bool essential = true;
    /// C2H: the FIFO reader connected. H2C: a transfer rate was reported.
    bool streamed  = false;
    /// Rate lines seen. The first one covers only the fraction of a second
    /// since streaming began but is divided by a whole second, so it reads
    /// low (74 MiB/s for a link moving 757) — it is not reported.
    int  rateLines = 0;
    /// H2C only.
    int  h2cChunkBytes = 0;
    bool h2cFirstChunk = false;
    bool h2cStepQueued = false;
    /// Stall messages since the last completed transfer (after streaming).
    int  h2cStallLines = 0;
    /// Ordered stop: when this process was sent SIGINT (ms on the stop
    /// clock, -1 = not yet) and how far its escalation has gone.
    qint64 stopSignalAt = -1;
    int    stopPhase    = 0;
};

class BackendLauncher : public QObject
{
    Q_OBJECT
public:
    explicit BackendLauncher(QObject* parent = nullptr);
    ~BackendLauncher() override;

    /// Directory holding the backend applications (`backend/` beside the
    /// executable, or the source-tree location during development). Empty
    /// when not found.
    static QString backendDirectory();

    /// Path to a built helper binary, or empty when absent.
    static QString binaryPath(const QString& name);

    /// True when both helper binaries are present and executable.
    static bool binariesAvailable();

    /// iwfg_c2h builds in place via its own Makefile, so it is not in
    /// backend/bin alongside the others.
    static QString udpBinaryPath();
    static QString buildUdpBinary(bool* ok);

    /// True when nothing else holds the UDP port. Checked before launching so
    /// a clash is reported as a clash, not as an opaque immediate exit.
    static bool udpPortAvailable(int port, QString* who = nullptr);

    /// Build the helpers via `make` in the backend directory. Blocking and
    /// slow-ish (a couple of seconds); intended to be called once when
    /// binariesAvailable() is false. Returns the build transcript.
    static QString buildBinaries(bool* ok);

    /// FIFO the workstation should read for the given config.
    static QString captureFifo(const BackendConfig& cfg)
    { return cfg.mode == BackendMode::UdpStream ? cfg.udpFifo : cfg.c2hFifo; }

    /// Can the helper be loaded on this host? Runs it with arguments that make
    /// it exit before it touches any device (see the .cpp). Cached per binary.
    static bool helperLoads(const QString& path, QString* why);

    bool isRunning() const  { return !m_procs.isEmpty(); }
    bool isStopping() const { return m_stopping; }
    bool h2cRunning() const;
    H2cState h2cState() const { return m_h2cState; }
    QString  h2cStateDetail() const { return m_h2cDetail; }
    BackendConfig config() const { return m_cfg; }
    /// Helpers told to exit that have not yet (stuck in the driver).
    int lingeringProcesses() const { return m_reaping.size(); }
    /// A resetDevice() is in progress (stop, then start again).
    bool isResetting() const { return m_resetting; }

public slots:
    /// Launch the processes for `cfg`. Non-blocking. Emits statusMessage()
    /// as it goes, then either becameReady() or startFailed(). Called while a
    /// stop is still in progress, the start is queued until it completes.
    void start(const sdr::BackendConfig& cfg);

    /// Graceful, asynchronous shutdown: SIGINT, then SIGTERM, then SIGKILL.
    /// Returns at once; stopped() follows. Safe to call when nothing runs.
    void stop();

    /// Relaunch H2C playback — the operator's "Retry transmit". 0 = the
    /// default buffer size. Starts it if none runs. Needs a running backend.
    void restartH2c(int chunkBytes = 0);

    /// Start H2C playback if none is running — transmit requested in a
    /// session launched as capture only. Needs a running backend. Returns
    /// true if a helper runs (or was started).
    bool ensureH2c();

    /// Bring the DMA device back from a wedged state: stop transmit, then
    /// capture — the driver soft-resets the QDMA when the last C2H stream
    /// closes with no other stream open — then start both again with the
    /// same configuration. The reader re-attaches by itself.
    void resetDevice(const QString& why);

signals:
    /// Human-readable progress, one line at a time.
    void statusMessage(const QString& line);
    /// A line of stdout/stderr from a child, prefixed with its label.
    void logLine(const QString& line);
    /// All processes are up and the capture FIFO is ready to be opened.
    void becameReady(const QString& captureFifo);
    /// Could not start — `reason` is user-presentable.
    void startFailed(const QString& reason);
    /// A running process exited and will not be restarted.
    /// `fatal` is false when an OPTIONAL stage died: the session continues
    /// and the caller must not tear the source down.
    void processFailed(const QString& label, int exitCode,
                       const QString& reason, bool fatal);
    /// Everything has stopped and resources are released.
    void stopped();
    /// The chunk size H2C playback is running with — at launch, and again if
    /// the launcher had to step down. The host generator's loop must match it.
    void h2cChunkBytesChanged(int bytes);
    /// iwfg_h2c's own measured delivery to the card, once a second.
    void h2cThroughput(double mibPerSec, int updatesPerSec);
    /// The transmit helper's state changed.
    void h2cStateChanged(sdr::H2cState state, const QString& detail);
    /// c2h_stream's own measured delivery from the card, once a second.
    void c2hThroughput(double mibPerSec);
    /// The capture helper ended on its own and is being restarted.
    void c2hRecovering(const QString& why, int attempt, int limit);
    /// resetDevice() began (true) / the device is running again (false).
    void deviceReset(bool inProgress, const QString& why);

private slots:
    void onReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError err);

private:
    /// Rebuild backend helpers for this host (make -B).
    static bool rebuildHelpers(QString* log);
    /// Pre-flight a DMA node: exists, and openable by this user.
    static bool checkDeviceNode(const QString& path, const QString& role,
                                QString* why);

    bool launchOne(const QString& label, const QString& program,
                   const QStringList& args, BackendProcess::Role role,
                   QString* error);
    bool launchH2c(int chunkBytes, QString* error);
    bool launchC2h(QString* error);
    void relaunchC2h();
    /// A playback that never streamed is retried one rung down. Returns true
    /// if a retry was started.
    bool retryH2cSmaller(QProcess* p, const QString& why);
    /// Decide what a stalled transmit helper needs: reopen at the same size
    /// first (a reopen re-enables the H2C datapath and rebuilds the queue),
    /// step down only if the same size stalls again before streaming.
    void queueH2cRecovery(QProcess* p, const QString& why);
    void reopenH2c(QProcess* p, const QString& why);
    bool h2cReopenAllowed();
    void handleH2cLine(QProcess* p, const QString& line, bool* periodic);
    void handleC2hLine(QProcess* p, const QString& line, bool* periodic);
    void setH2cState(H2cState s, const QString& detail);
    bool c2hRestartAllowed();
    void stopStalledH2c(const QString& why);

    int  indexOf(const QProcess* p) const;
    BackendProcess* record(const QProcess* p);
    /// Let go of a process without ever blocking: deleted now if it has
    /// exited, otherwise signalled and reaped when it does. `whenGone` runs
    /// once it has exited (or after a fallback delay if it never does).
    void release(QProcess* p, std::function<void()> whenGone = {});
    bool checkStopDone();
    void signalStop(BackendProcess& bp);
    void stopTick();
    void finishStop();
    void teardown();

    BackendConfig           m_cfg;
    QVector<BackendProcess> m_procs;
    QList<QProcess*>        m_reaping;
    QTimer*                 m_readyTimer = nullptr;
    QTimer*                 m_stopTimer  = nullptr;
    QElapsedTimer           m_stopClock;
    bool                    m_stopping   = false;
    bool                    m_pendingStart = false;
    BackendConfig           m_pendingCfg;
    QVector<qint64>         m_c2hRestarts;          ///< ms since epoch
    H2cState                m_h2cState = H2cState::NotRunning;
    QString                 m_h2cDetail;
    /// Bumped by start() and stop(): a delayed callback from an earlier run
    /// sees a different value and does nothing.
    quint64                 m_generation = 0;
    /// The ladder was walked to the bottom because the card consumed nothing
    /// at all (typically: the DAC input was not yet on Host). When the card
    /// then starts consuming, transmit is relaunched once at the default size
    /// instead of being left on a 16 KiB buffer that cannot keep the DAC fed.
    bool                    m_h2cBottomedByStall = false;
    bool                    m_h2cPromoted = false;
    /// The chunk size already given one same-size reopen before streaming.
    int                     m_h2cRetriedSize = 0;
    QVector<qint64>         m_h2cReopens;           ///< ms since epoch
    bool                    m_resetting = false;
    QString                 m_resetWhy;
};

} // namespace sdr

Q_DECLARE_METATYPE(sdr::BackendConfig)
Q_DECLARE_METATYPE(sdr::BackendMode)
Q_DECLARE_METATYPE(sdr::H2cState)
