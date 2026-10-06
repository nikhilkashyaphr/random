#include "BackendLauncher.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QHash>
#include <QPointer>
#include <QStandardPaths>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <csignal>
#include <memory>
#ifdef Q_OS_UNIX
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <cstring>
#include <unistd.h>
#endif

namespace sdr {

namespace {

/// How long a child gets to unwind after SIGINT before SIGTERM. The C2H app
/// can be blocked in an in-kernel DMA wait, which its handler interrupts; a
/// couple of seconds is generous for that path.
constexpr int kGraceMs = 2500;
/// After SIGTERM, before SIGKILL.
constexpr int kTermMs = 1500;
/// After SIGKILL, before a helper is declared stuck and left to the reaper.
constexpr int kKillWaitMs = 1500;

/// Delay before declaring the pipeline ready. The capture app has to open the
/// device, register its DMA buffers and create the FIFO before a reader can
/// usefully attach. This is a settle window, not a correctness dependency —
/// the reader retries regardless.
constexpr int kReadySettleMs = 900;

/// iwfg_c2h allocates, pre-faults and mlocks a 368 MiB ring before it opens
/// its FIFO; that took ~2.6 s when measured.
constexpr int kUdpReadySettleMs = 4000;

/// H2C: after "First chunk received", a working transfer reports a rate
/// within about a second. No rate in this long means the card is not
/// completing transfers of this size.
constexpr int kH2cFirstRateMs = 4000;

/// A capture restart waits this long so the driver finishes releasing the
/// previous helper's queue before the next one opens the node.
constexpr int kC2hRestartDelayMs = 700;
constexpr qint64 kC2hRestartWindowMs = 60000;

/// A helper being replaced gets this long to exit before the replacement is
/// launched anyway.
constexpr int kRelaunchFallbackMs = 3000;

/// Ordered stop: capture is signalled once transmit has exited, or after
/// this long at the latest.
constexpr int kPlaybackFirstMs = 1500;

/// Same-size reopens of a transmit helper that streamed and then stalled.
constexpr int kH2cReopenMax = 3;
constexpr qint64 kH2cReopenWindowMs = 60000;

/// Lines kept per helper.
constexpr int kTailLines = 40;
constexpr int kNotableLines = 12;

/// coreutils `stdbuf`, used to make c2h_stream's stdout line-buffered. When
/// stdout is a pipe, c2h_stream buffers it in 4 KiB blocks: its progress
/// reached the GUI in bursts and — worse — the one line that said why it
/// stopped arrived only at exit, after its perror() on unbuffered stderr, and
/// out of order with it. Empty when unavailable (behaviour as before).
QString lineBufferWrapper()
{
    if (qEnvironmentVariableIsSet("IWFG_NO_STDBUF")) return {};
    static const QString p = QStandardPaths::findExecutable(QStringLiteral("stdbuf"));
    return p;
}

} // namespace

// ------------------------------------------------------------------ helpers

int defaultH2cChunkBytes()
{
    bool ok = false;
    const int v = qEnvironmentVariableIntValue("IWFG_H2C_CHUNK_BYTES", &ok);
    if (ok && v >= kH2cChunkMin && v <= kH2cChunkMax && v % 4 == 0) return v;
    return kH2cChunkDefault;
}

int nextSmallerH2cChunk(int bytes)
{
    for (int rung : kH2cChunkLadder)
        if (rung < bytes) return rung;
    return 0;
}

QString byteSizeText(qint64 bytes)
{
    if (bytes >= (1 << 20) && bytes % (1 << 20) == 0)
        return QStringLiteral("%1 MiB").arg(bytes >> 20);
    if (bytes >= 1024 && bytes % 1024 == 0)
        return QStringLiteral("%1 KiB").arg(bytes >> 10);
    return QStringLiteral("%1 B").arg(bytes);
}

bool parseH2cRateLine(const QString& line, double* mibPerSec, int* updatesPerSec)
{
    static const QRegularExpression re(
        QStringLiteral("\\[H2C\\]\\s+([0-9]+(?:\\.[0-9]+)?)\\s*MB/s(?:.*GR_updates=([0-9]+)/s)?"));
    const QRegularExpressionMatch m = re.match(line);
    if (!m.hasMatch()) return false;
    if (mibPerSec) *mibPerSec = m.captured(1).toDouble();
    if (updatesPerSec) *updatesPerSec = m.captured(2).isEmpty() ? -1 : m.captured(2).toInt();
    return true;
}

bool parseC2hRateLine(const QString& line, double* mibPerSec)
{
    static const QRegularExpression re(
        QStringLiteral("^Streaming:\\s+([0-9]+(?:\\.[0-9]+)?)\\s*MB/s"));
    const QRegularExpressionMatch m = re.match(line.trimmed());
    if (!m.hasMatch()) return false;
    if (mibPerSec) *mibPerSec = m.captured(1).toDouble();
    return true;
}

bool isH2cStallLine(const QString& line)
{
    static const char* kStall[] = {
        "stall kick", "ETIMEDOUT", "no DMA completion", "cycling device",
        "Device reopened", "EBUSY"
    };
    for (const char* s : kStall)
        if (line.contains(QLatin1String(s))) return true;
    return false;
}

QString h2cStateName(H2cState s)
{
    switch (s) {
    case H2cState::NotRunning:     return QStringLiteral("not running");
    case H2cState::WaitingForData: return QStringLiteral("waiting for the generator");
    case H2cState::Starting:       return QStringLiteral("starting");
    case H2cState::Streaming:      return QStringLiteral("streaming");
    case H2cState::Stalled:        return QStringLiteral("stalled");
    case H2cState::Restarting:     return QStringLiteral("restarting");
    case H2cState::Failed:         return QStringLiteral("failed");
    }
    return QStringLiteral("unknown");
}

C2hExit classifyC2hExit(const QStringList& lines, QString* detail)
{
    auto find = [&](const char* needle) -> bool {
        for (const QString& l : lines)
            if (l.contains(QLatin1String(needle))) { if (detail) *detail = l.trimmed(); return true; }
        return false;
    };
    // c2h_stream leaves its loop for exactly one reason, then prints
    // "Stopping stream..." and returns 0 whatever that reason was.
    if (find("IWFG_IOCTL_DMA_DATA (C2H)"))      return C2hExit::DriverError;
    if (find("FIFO reader disconnected"))       return C2hExit::ReaderClosed;
    if (find("write to fifo"))                  return C2hExit::ReaderClosed;
    if (find("interrupted by signal"))          return C2hExit::Interrupted;
    if (find("Interrupted before a FIFO reader")) return C2hExit::Interrupted;
    if (find("open device") || find("IWFG_IOCTL_ADD_BUF") || find("posix_memalign")
        || find("open fifo") || find("sigaction"))
        return C2hExit::StartupError;
    if (detail) detail->clear();
    return C2hExit::Unknown;
}

QString c2hExitText(C2hExit e)
{
    switch (e) {
    case C2hExit::ReaderClosed:
        return QStringLiteral("the capture FIFO lost its reader");
    case C2hExit::DriverError:
        return QStringLiteral("the DMA driver failed a C2H transfer");
    case C2hExit::Interrupted:
        return QStringLiteral("it received a stop signal the workstation did not send");
    case C2hExit::StartupError:
        return QStringLiteral("it could not open or set up the device");
    case C2hExit::Unknown:
        break;
    }
    return QStringLiteral("it gave no reason");
}

QString backendModeName(BackendMode m)
{
    switch (m) {
    case BackendMode::C2H:     return QStringLiteral("C2H");
    case BackendMode::H2C_C2H: return QStringLiteral("H2C_C2H");
    case BackendMode::UdpStream: return QStringLiteral("UDP");
    case BackendMode::RoceRx:    return QStringLiteral("RoCEv2");
    }
    return QStringLiteral("unknown");
}

// ------------------------------------------------------------- discovery

QString BackendLauncher::backendDirectory()
{
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        exeDir + QStringLiteral("/backend"),
        exeDir + QStringLiteral("/../backend"),      // running from build/
        QDir::currentPath() + QStringLiteral("/backend"),
    };
    for (const QString& c : candidates) {
        QDir d(c);
        if (d.exists() && QFileInfo::exists(d.absoluteFilePath(QStringLiteral("Makefile"))))
            return d.absolutePath();
    }
    return {};
}

QString BackendLauncher::binaryPath(const QString& name)
{
    const QString dir = backendDirectory();
    if (dir.isEmpty()) return {};
    const QString p = dir + QStringLiteral("/bin/") + name;
    const QFileInfo fi(p);
    return (fi.exists() && fi.isExecutable()) ? p : QString();
}

/// Can this helper actually be LOADED?
///
/// exists() + isExecutable() is not enough: a binary compiled against a newer
/// glibc satisfies both and still dies at load with
///     libc.so.6: version `GLIBC_2.38' not found
/// Running it briefly turns that into a fact we can act on before anything
/// else is launched.
///
/// 1.2.3 ran the helpers with NO arguments for this — and neither helper
/// treats that as "print usage": both ran with their defaults, opened the DMA
/// device, registered buffers and then blocked waiting for a FIFO peer until
/// the probe killed them 3 s later. Six seconds of frozen window on every
/// start, and two needless open/close cycles of the device. The arguments
/// below make each helper exit at its first check, before any device is
/// touched; loader failures happen before main() and are still caught.
bool BackendLauncher::helperLoads(const QString& path, QString* why)
{
    static QHash<QString, QString> verified;          // path -> size:mtime
    const QFileInfo fi(path);
    const QString stamp = QStringLiteral("%1:%2").arg(fi.size())
                              .arg(fi.lastModified().toMSecsSinceEpoch());
    if (verified.value(path) == stamp) return true;

    QStringList args;
    const QString base = fi.fileName();
    if (base == QLatin1String("iwfg_h2c")) {
        // Argument check (chunk_bytes 0 is invalid) precedes everything.
        args = QStringList{QStringLiteral("/nonexistent/iwfg-probe"),
                           QStringLiteral("/nonexistent/iwfg-probe.fifo"), QStringLiteral("0")};
    } else if (base == QLatin1String("c2h_stream")) {
        // open(device) fails at once; the scratch FIFO it creates is removed
        // by its own cleanup.
        args = QStringList{QStringLiteral("/nonexistent/iwfg-probe"),
                           QDir::tempPath() + QStringLiteral("/iwfg_probe_%1.fifo")
                                                  .arg(QCoreApplication::applicationPid())};
    }

    auto* probe = new QProcess;
    probe->setProcessChannelMode(QProcess::MergedChannels);
    probe->start(path, args);
    bool ok = true;
    if (!probe->waitForStarted(2000)) {
        if (why) *why = QStringLiteral("could not be started: %1").arg(probe->errorString());
        ok = false;
    } else {
        probe->waitForFinished(2000);
        const QString out = QString::fromLocal8Bit(probe->readAll());
        static const char* kLoaderFaults[] = {
            "GLIBC_", "version `GLIBC", "cannot open shared object",
            "no version information", "Exec format error",
            "cannot execute binary file"
        };
        for (const char* needle : kLoaderFaults) {
            if (out.contains(QLatin1String(needle))) {
                if (why) *why = out.trimmed();
                ok = false;
                break;
            }
        }
    }
    if (probe->state() != QProcess::NotRunning) {
        // Never destroy a running QProcess: its destructor waits for it.
        probe->kill();
        QObject::connect(probe, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                         probe, &QObject::deleteLater);
    } else {
        delete probe;
    }
    if (ok) verified.insert(path, stamp);
    return ok;
}

/// Rebuild the helpers for THIS host. Returns false with the compiler output.
bool BackendLauncher::rebuildHelpers(QString* log)
{
    const QString dir = backendDirectory();
    if (dir.isEmpty() || !QFileInfo::exists(dir + QStringLiteral("/Makefile"))) {
        if (log) *log = QStringLiteral("backend/Makefile not found at %1").arg(dir);
        return false;
    }
    QProcess make;
    make.setProcessChannelMode(QProcess::MergedChannels);
    make.setWorkingDirectory(dir);
    // -B so a stale binary with a newer timestamp than its source cannot
    // suppress its own rebuild — the exact trap that caused this.
    make.start(QStringLiteral("make"), {QStringLiteral("-B")});
    if (!make.waitForStarted(5000)) {
        if (log) *log = QStringLiteral("could not run make: %1").arg(make.errorString());
        return false;
    }
    make.waitForFinished(120000);
    if (log) *log = QString::fromLocal8Bit(make.readAll()).trimmed();
    return make.exitStatus() == QProcess::NormalExit && make.exitCode() == 0;
}

bool BackendLauncher::binariesAvailable()
{
    return !binaryPath(QStringLiteral("c2h_stream")).isEmpty()
        && !binaryPath(QStringLiteral("iwfg_h2c")).isEmpty();
}

QString BackendLauncher::udpBinaryPath()
{
    const QString dir = backendDirectory();
    if (dir.isEmpty()) return {};
    const QString p = dir + QStringLiteral("/udp/iwfg_c2h");
    const QFileInfo fi(p);
    return (fi.exists() && fi.isExecutable()) ? p : QString();
}

bool BackendLauncher::udpPortAvailable(int port, QString* who)
{
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return true;                 // cannot tell: do not block
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(quint16(port));
    const bool ok = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    ::close(fd);
    if (!ok && who) *who = QString::fromLocal8Bit(strerror(errno));
    return ok;
}

QString BackendLauncher::buildUdpBinary(bool* ok)
{
    if (ok) *ok = false;
    const QString dir = backendDirectory();
    if (dir.isEmpty()) return QStringLiteral("backend/ directory not found");
    QProcess p;
    p.setWorkingDirectory(dir + QStringLiteral("/udp"));
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(QStringLiteral("make"), QStringList{});
    if (!p.waitForStarted(3000)) return QStringLiteral("could not run `make`");
    p.waitForFinished(180000);
    const QString out = QString::fromLocal8Bit(p.readAll());
    if (ok) *ok = !udpBinaryPath().isEmpty();
    return out;
}

// ---------------------------------------------------------------------------
// RoCEv2 receiver.
//
// The supplied stack under reference/roce-iq-holoscan owns the network side;
// the GUI only reads the shared-memory ring it publishes. Starting it from
// here is the difference between "select RoCEv2 and press Start" and "open a
// terminal, run rdma_rx, then come back" -- and when nothing is publishing the
// operator otherwise just sees "cannot open /dev/shm/iqring".
//
// The reference tree is vendored rather than built into backend/, so it is
// looked up separately from backendDirectory().
// ---------------------------------------------------------------------------
QString BackendLauncher::roceDirectory()
{
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        exeDir + QStringLiteral("/reference/roce-iq-holoscan"),
        exeDir + QStringLiteral("/../reference/roce-iq-holoscan"),   // build/
        QDir::currentPath() + QStringLiteral("/reference/roce-iq-holoscan"),
    };
    for (const QString& c : candidates) {
        QDir d(c);
        if (d.exists() && QFileInfo::exists(d.absoluteFilePath(QStringLiteral("Makefile"))))
            return d.absolutePath();
    }
    return {};
}

QString BackendLauncher::roceReceiverPath(bool gpu)
{
    const QString dir = roceDirectory();
    if (dir.isEmpty()) return {};
    const QString p = dir + (gpu ? QStringLiteral("/rdma_rx_gpu")
                                 : QStringLiteral("/rdma_rx"));
    const QFileInfo fi(p);
    return (fi.exists() && fi.isExecutable()) ? p : QString();
}

QString BackendLauncher::buildRoceReceiver(bool gpu, bool* ok)
{
    if (ok) *ok = false;
    const QString dir = roceDirectory();
    if (dir.isEmpty())
        return QStringLiteral("reference/roce-iq-holoscan not found");
    QProcess p;
    p.setWorkingDirectory(dir);
    p.setProcessChannelMode(QProcess::MergedChannels);
    // `make gpu` builds rdma_rx_gpu, which additionally needs the CUDA
    // toolkit; plain `make` builds rdma_rx and the scale helpers.
    p.start(QStringLiteral("make"),
            gpu ? QStringList{QStringLiteral("gpu")} : QStringList{});
    if (!p.waitForStarted(3000)) return QStringLiteral("could not run `make`");
    p.waitForFinished(300000);
    const QString out = QString::fromLocal8Bit(p.readAll());
    if (ok) *ok = !roceReceiverPath(gpu).isEmpty();
    return out;
}

bool BackendLauncher::roceRingPresent()
{
    return QFileInfo::exists(roceRingPath());
}

QString BackendLauncher::buildBinaries(bool* ok)
{
    if (ok) *ok = false;
    const QString dir = backendDirectory();
    if (dir.isEmpty()) return QStringLiteral("backend/ directory not found");

    QProcess p;
    p.setWorkingDirectory(dir);
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(QStringLiteral("make"), QStringList{});
    if (!p.waitForStarted(3000)) return QStringLiteral("could not run `make`");
    p.waitForFinished(120000);

    const QString out = QString::fromLocal8Bit(p.readAll());
    const bool built = (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0)
                       && binariesAvailable();
    if (ok) *ok = built;
    return out;
}

// ----------------------------------------------------------- lifecycle

BackendLauncher::BackendLauncher(QObject* parent) : QObject(parent)
{
    m_stopTimer = new QTimer(this);
    m_stopTimer->setInterval(100);
    connect(m_stopTimer, &QTimer::timeout, this, &BackendLauncher::stopTick);
    // A device reset whose restart fails is over, too.
    connect(this, &BackendLauncher::startFailed, this, [this] {
        if (!m_resetting) return;
        m_resetting = false;
        emit deviceReset(false, m_resetWhy + QStringLiteral(" — the restart failed"));
    });
}

BackendLauncher::~BackendLauncher()
{
    // Never leave children holding the device node open — an orphaned
    // c2h_stream keeps /dev/iwfg0 busy and the next run fails to open it.
    // Bounded: ~2 s at most, then anything still alive is left behind rather
    // than blocking the exit forever in QProcess's destructor.
    m_stopping = true;
    ++m_generation;
    QList<QProcess*> all = m_reaping;
    for (const BackendProcess& bp : m_procs)
        if (bp.proc) all << bp.proc;
    for (QProcess* p : all) p->disconnect(this);

    auto running = [&all] {
        int n = 0;
        for (QProcess* p : all) if (p->state() != QProcess::NotRunning) ++n;
        return n;
    };
    QElapsedTimer t; t.start();
#ifdef Q_OS_UNIX
    // Transmit first, as in stop(): see the header.
    QList<QProcess*> playback;
    for (const BackendProcess& bp : m_procs)
        if (bp.proc && bp.role == BackendProcess::Playback) playback << bp.proc;
    for (QProcess* p : playback)
        if (p->state() != QProcess::NotRunning)
            ::kill(static_cast<pid_t>(p->processId()), SIGINT);
    auto playbackRunning = [&playback] {
        for (QProcess* p : playback) if (p->state() != QProcess::NotRunning) return true;
        return false;
    };
    while (playbackRunning() && t.elapsed() < 700)
        for (QProcess* p : playback)
            if (p->state() != QProcess::NotRunning) p->waitForFinished(50);
    for (QProcess* p : all)
        if (p->state() != QProcess::NotRunning && !playback.contains(p))
            ::kill(static_cast<pid_t>(p->processId()), SIGINT);
#endif
    while (running() && t.elapsed() < 1500)
        for (QProcess* p : all)
            if (p->state() != QProcess::NotRunning) p->waitForFinished(50);
    for (QProcess* p : all)
        if (p->state() != QProcess::NotRunning) p->kill();
    t.restart();
    while (running() && t.elapsed() < 700)
        for (QProcess* p : all)
            if (p->state() != QProcess::NotRunning) p->waitForFinished(50);
    for (QProcess* p : all)
        if (p->state() != QProcess::NotRunning)
            p->setParent(nullptr);   // deliberately leaked: deleting would block
    m_procs.clear();
    m_reaping.clear();
}

bool BackendLauncher::h2cRunning() const
{
    for (const BackendProcess& bp : m_procs)
        if (bp.role == BackendProcess::Playback) return true;
    return false;
}

void BackendLauncher::setH2cState(H2cState s, const QString& detail)
{
    if (s == m_h2cState && detail == m_h2cDetail) return;
    m_h2cState = s;
    m_h2cDetail = detail;
    emit h2cStateChanged(s, detail);
}

void BackendLauncher::start(const sdr::BackendConfig& cfg)
{
    if (m_stopping) {
        m_pendingStart = true;
        m_pendingCfg = cfg;
        emit statusMessage(QStringLiteral(
            "The previous backend is still exiting — starting as soon as it has."));
        return;
    }
    if (isRunning()) {
        emit startFailed(QStringLiteral("Backend is already running."));
        return;
    }
    m_cfg = cfg;
    ++m_generation;
    m_c2hRestarts.clear();
    m_h2cBottomedByStall = false;
    m_h2cPromoted = false;
    m_h2cRetriedSize = 0;
    m_h2cReopens.clear();
    if (!m_reaping.isEmpty())
        emit statusMessage(QStringLiteral(
            "Note: %1 earlier helper process(es) have not exited yet (stuck in the "
            "driver); the device may still be busy.").arg(m_reaping.size()));

    if (cfg.mode == BackendMode::RoceRx) {
        // The ring is checked FIRST, before the binary. If a receiver is
        // already publishing, whether this tree has a built receiver is
        // irrelevant and "run `make`" is the wrong instruction -- the
        // operator needs to be told to use the ring that is already there.
        // Refuse rather than collide: rdma_rx creates /dev/shm/iqring and dies
        // on a ring it cannot own, and a second receiver would also find the
        // rdma_cm port taken.
        if (roceRingPresent()) {
            emit startFailed(QStringLiteral(
                "%1 already exists, so a receiver is already publishing. "
                "Stop it before starting another, or select RoCEv2 and press "
                "Start to read the ring it is already filling.")
                .arg(roceRingPath()));
            return;
        }

        const QString bin = roceReceiverPath(cfg.roceGpu);
        if (bin.isEmpty()) {
            emit startFailed(QStringLiteral(
                "RoCEv2 receiver not built. Expected %1 — run `make%2` in "
                "reference/roce-iq-holoscan.")
                .arg(cfg.roceGpu ? QStringLiteral("rdma_rx_gpu")
                                 : QStringLiteral("rdma_rx"),
                     cfg.roceGpu ? QStringLiteral(" gpu") : QString()));
            return;
        }

        QStringList args;
        if (!cfg.roceBindAddr.isEmpty())
            args << QStringLiteral("-a") << cfg.roceBindAddr;
        args << QStringLiteral("-p") << QString::number(cfg.rocePort);
        if (!cfg.roceTransport.isEmpty())
            args << QStringLiteral("-w") << cfg.roceTransport;

        emit statusMessage(QStringLiteral("Starting RoCEv2 receiver (%1) on port %2…")
            .arg(cfg.roceGpu ? QStringLiteral("rdma_rx_gpu, GPUDirect")
                             : QStringLiteral("rdma_rx, host ring"))
            .arg(cfg.rocePort));
        QString err;
        if (!launchOne(QStringLiteral("RoCEv2 receiver"), bin, args,
                       BackendProcess::RoceRx, &err)) {
            teardown();
            emit startFailed(QStringLiteral(
                "RoCEv2 receiver failed to start: %1\n\n"
                "It needs the rdma_cm and ib_core modules loaded and an IP "
                "configured on the listening port — see "
                "reference/roce-iq-holoscan/scripts/02_setup_network.sh.")
                .arg(err));
            return;
        }
        if (!m_readyTimer) {
            m_readyTimer = new QTimer(this);
            m_readyTimer->setSingleShot(true);
        }
        m_readyTimer->disconnect(this);
        connect(m_readyTimer, &QTimer::timeout, this, [this] {
            if (!isRunning() || m_stopping) return;
            // Readiness here means the ring exists: the receiver creates it
            // before it listens, so this is the thing the GUI actually needs.
            if (!roceRingPresent()) {
                emit statusMessage(QStringLiteral(
                    "RoCEv2 receiver started but %1 has not appeared yet.")
                    .arg(roceRingPath()));
                return;
            }
            emit statusMessage(QStringLiteral("RoCEv2 receiver ready — ring %1")
                                   .arg(roceRingPath()));
            // becameReady carries the path the source should read; for this
            // transport that is the ring, not a capture FIFO.
            emit becameReady(roceRingPath());
        });
        m_readyTimer->start(kReadySettleMs);
        return;
    }

    if (cfg.mode == BackendMode::UdpStream) {
        const QString bin = udpBinaryPath();
        if (bin.isEmpty()) {
            emit startFailed(QStringLiteral(
                "UDP receiver not built. Expected backend/udp/iwfg_c2h — "
                "run `make` in backend/udp."));
            return;
        }
        QStringList args;
        args << QStringLiteral("-p") << QString::number(cfg.udpPort)
             << QStringLiteral("-f") << cfg.udpFifo
             << QStringLiteral("-s") << QString::number(cfg.udpPayload);
        if (!cfg.udpRawIface.isEmpty()) {
            args << QStringLiteral("--raw") << cfg.udpRawIface;
        } else if (!cfg.udpBindAddr.isEmpty()) {
            args << QStringLiteral("-b") << cfg.udpBindAddr;
        }
        if (cfg.udpRxCpu >= 0) args << QStringLiteral("--rx-cpu") << QString::number(cfg.udpRxCpu);
        if (cfg.udpWrCpu >= 0) args << QStringLiteral("--wr-cpu") << QString::number(cfg.udpWrCpu);
        if (cfg.udpHugepages)  args << QStringLiteral("--hugepages");

        // --raw opens an AF_PACKET socket, which needs root. Elevate through
        // pkexec so the operator authorises once in a dialog.
        QString program = bin;
        if (!cfg.udpRawIface.isEmpty() && ::geteuid() != 0) {
            if (!QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty()) {
                args.prepend(bin);
                program = QStringLiteral("pkexec");
            } else {
                emit startFailed(QStringLiteral(
                    "Raw capture needs root and no polkit agent is available.\n"
                    "Run manually:  sudo %1 %2")
                    .arg(bin, args.join(QLatin1Char(' '))));
                return;
            }
        }

        emit statusMessage(QStringLiteral("Starting UDP receiver on port %1%2…")
            .arg(cfg.udpPort)
            .arg(cfg.udpRawIface.isEmpty() ? QString()
                                           : QStringLiteral(" (raw, %1)").arg(cfg.udpRawIface)));
        QString err;
        if (!launchOne(QStringLiteral("UDP receiver"), program, args,
                       BackendProcess::Udp, &err)) {
            teardown();
            emit startFailed(QStringLiteral("UDP receiver failed to start: %1").arg(err));
            return;
        }
        if (!m_readyTimer) {
            m_readyTimer = new QTimer(this);
            m_readyTimer->setSingleShot(true);
        }
        m_readyTimer->disconnect(this);
        connect(m_readyTimer, &QTimer::timeout, this, [this] {
            if (!isRunning() || m_stopping) return;
            emit statusMessage(QStringLiteral("UDP receiver ready — FIFO %1")
                                   .arg(m_cfg.udpFifo));
            emit becameReady(m_cfg.udpFifo);
        });
        m_readyTimer->start(kUdpReadySettleMs);
        return;
    }

    const QString c2h = binaryPath(QStringLiteral("c2h_stream"));
    const QString h2c = binaryPath(QStringLiteral("iwfg_h2c"));

    if (c2h.isEmpty()) {
        emit startFailed(QStringLiteral(
            "C2H helper not built. Expected backend/bin/c2h_stream — "
            "run `make` in the backend directory."));
        return;
    }
    if (cfg.mode == BackendMode::H2C_C2H && h2c.isEmpty()) {
        emit startFailed(QStringLiteral(
            "H2C helper not built. Expected backend/bin/iwfg_h2c — "
            "run `make` in the backend directory."));
        return;
    }

    // Pre-flight the helpers themselves. A helper that cannot load is repaired
    // once, automatically: the sources are right here.
    {
        QStringList needed{QStringLiteral("c2h_stream")};
        if (cfg.mode == BackendMode::H2C_C2H) needed << QStringLiteral("iwfg_h2c");

        for (const QString& nm : needed) {
            const QString path = binaryPath(nm);
            if (path.isEmpty()) continue;
            QString why;
            if (helperLoads(path, &why)) continue;

            emit statusMessage(QStringLiteral(
                "%1 cannot load on this host — rebuilding it from source…").arg(nm));
            emit logLine(QStringLiteral("[backend] %1: %2").arg(nm, why));

            QString buildLog;
            const bool rebuilt = rebuildHelpers(&buildLog);
            if (!buildLog.isEmpty()) emit logLine(QStringLiteral("[make] %1").arg(buildLog));

            QString why2;
            if (!rebuilt || !helperLoads(binaryPath(nm), &why2)) {
                emit startFailed(QStringLiteral(
                    "%1 cannot run on this machine and the automatic rebuild "
                    "did not fix it.\n\nOriginal error:\n  %2\n\n"
                    "This usually means the shipped binary was built against a "
                    "different C library. Build it yourself:\n\n"
                    "    cd %3\n    make -B\n\n%4")
                    .arg(nm, why, backendDirectory(),
                         buildLog.isEmpty() ? QString()
                                            : QStringLiteral("make said:\n  %1").arg(buildLog)));
                return;
            }
            emit statusMessage(QStringLiteral("%1 rebuilt successfully.").arg(nm));
        }
    }

    // Pre-flight the device nodes. The two nodes are NOT interchangeable: the
    // driver derives the transfer direction from which one was opened —
    // /dev/iwfg0 is C2H, /dev/iwfg1 is H2C.
    // Transmit is optional: a playback stage that cannot run is reported,
    // and the capture starts regardless (it used to abort the whole start).
    QString h2cProblem;
    {
        QString why;
        if (!checkDeviceNode(cfg.c2hDevice, QStringLiteral("C2H capture"), &why)) {
            emit startFailed(why);
            return;
        }
        if (cfg.mode == BackendMode::H2C_C2H
            && !checkDeviceNode(cfg.h2cDevice, QStringLiteral("H2C playback"), &why))
            h2cProblem = why;
    }

    emit statusMessage(QStringLiteral("Starting %1 backend…")
                           .arg(backendModeName(cfg.mode)));

    // Playback first in loopback mode: the card should be consuming before
    // the capture side starts.
    if (cfg.mode == BackendMode::H2C_C2H) {
        QString err;
        if (h2cProblem.isEmpty() && !launchH2c(cfg.h2cChunkBytes, &err))
            h2cProblem = QStringLiteral("iwfg_h2c failed to start: %1").arg(err);
        if (!h2cProblem.isEmpty()) {
            setH2cState(H2cState::Failed, h2cProblem);
            const quint64 gen = m_generation;
            QTimer::singleShot(0, this, [this, gen, h2cProblem] {
                if (gen != m_generation) return;
                emit processFailed(QStringLiteral("H2C playback"), -1,
                    h2cProblem + QStringLiteral("\n\nCapture starts without transmit."),
                    /*fatal=*/false);
            });
        }
    } else {
        setH2cState(H2cState::NotRunning,
                    QStringLiteral("the data-flow mode is C2H only — no transmit helper runs"));
    }

    QString err;
    if (!launchC2h(&err)) {
        teardown();
        emit startFailed(QStringLiteral("C2H capture failed to start: %1").arg(err));
        return;
    }

    if (!m_readyTimer) {
        m_readyTimer = new QTimer(this);
        m_readyTimer->setSingleShot(true);
    }
    m_readyTimer->disconnect(this);
    connect(m_readyTimer, &QTimer::timeout, this, [this] {
        if (!isRunning() || m_stopping) return;
        emit statusMessage(QStringLiteral("Backend ready — capture FIFO %1")
                               .arg(m_cfg.c2hFifo));
        emit becameReady(m_cfg.c2hFifo);
        if (m_resetting) {
            m_resetting = false;
            emit statusMessage(QStringLiteral(
                "DMA device reset complete — capture and transmit are running again."));
            emit deviceReset(false, m_resetWhy);
        }
    });
    m_readyTimer->start(kReadySettleMs);
}

bool BackendLauncher::launchOne(const QString& label, const QString& program,
                                const QStringList& args, BackendProcess::Role role,
                                QString* error)
{
    auto* p = new QProcess(this);
    p->setProcessChannelMode(QProcess::MergedChannels);

    QString prog = program;
    QStringList argv = args;
    const QString wrap = lineBufferWrapper();
    if (role == BackendProcess::Capture && !wrap.isEmpty()) {
        // stdbuf exec()s the helper, so the PID (and SIGINT) is the helper's.
        argv.prepend(program);
        argv.prepend(QStringLiteral("-oL"));
        prog = wrap;
    }
    p->setProgram(prog);
    p->setArguments(argv);

    connect(p, &QProcess::readyReadStandardOutput, this, &BackendLauncher::onReadyRead);
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &BackendLauncher::onProcessFinished);
    connect(p, &QProcess::errorOccurred, this, &BackendLauncher::onProcessError);

    emit statusMessage(QStringLiteral("%1: %2 %3")
                           .arg(label, program, args.join(QLatin1Char(' '))));
    p->start();
    if (!p->waitForStarted(3000)) {
        if (error) *error = p->errorString();
        p->disconnect(this);
        if (p->state() == QProcess::NotRunning) p->deleteLater();
        else release(p);
        return false;
    }

    BackendProcess bp;
    bp.label = label;
    bp.program = program;
    bp.args = args;
    bp.proc = p;
    bp.role = role;
    bp.essential = (role != BackendProcess::Playback);
    m_procs.push_back(bp);
    return true;
}

bool BackendLauncher::launchC2h(QString* error)
{
    return launchOne(QStringLiteral("C2H capture"), binaryPath(QStringLiteral("c2h_stream")),
                     {m_cfg.c2hDevice, m_cfg.c2hFifo}, BackendProcess::Capture, error);
}

bool BackendLauncher::launchH2c(int chunkBytes, QString* error)
{
    const QString h2c = binaryPath(QStringLiteral("iwfg_h2c"));
    if (!launchOne(QStringLiteral("H2C playback"), h2c,
                   {m_cfg.h2cDevice, m_cfg.h2cFifo, QString::number(chunkBytes)},
                   BackendProcess::Playback, error))
        return false;
    m_procs.last().h2cChunkBytes = chunkBytes;
    m_cfg.h2cChunkBytes = chunkBytes;
    setH2cState(H2cState::WaitingForData,
                QStringLiteral("iwfg_h2c started with a %1 buffer — waiting for the IQ "
                               "generator to connect to %2")
                    .arg(byteSizeText(chunkBytes), m_cfg.h2cFifo));
    emit h2cChunkBytesChanged(chunkBytes);
    return true;
}

void BackendLauncher::relaunchC2h()
{
    QString err;
    if (!launchC2h(&err)) {
        emit processFailed(QStringLiteral("C2H capture"), -1,
                           QStringLiteral("The capture helper could not be restarted: %1").arg(err),
                           /*fatal=*/true);
        stop();
        return;
    }
    emit statusMessage(QStringLiteral(
        "Capture helper restarted — the reader re-attaches to its new FIFO by itself."));
}

bool BackendLauncher::c2hRestartAllowed()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QVector<qint64> recent;
    for (qint64 t : m_c2hRestarts)
        if (now - t < kC2hRestartWindowMs) recent << t;
    m_c2hRestarts = recent;
    return m_c2hRestarts.size() < std::max(0, m_cfg.c2hRestartLimit);
}

void BackendLauncher::stopStalledH2c(const QString& why)
{
    if (m_h2cState != H2cState::Stalled) return;
    QProcess* h2c = nullptr;
    for (const BackendProcess& bp : m_procs)
        if (bp.role == BackendProcess::Playback) h2c = bp.proc;
    if (!h2c) return;
    // A transmit helper that is not streaming keeps kicking transfers and
    // cycling /dev/iwfg1. When the capture fails at the same moment, that is
    // the first suspect: stop it rather than let it keep resetting the device
    // under a capture that is being restarted.
    release(h2c);
    setH2cState(H2cState::Failed, QStringLiteral(
        "Transmit stopped to protect the capture: iwfg_h2c was stalled (the card "
        "was not consuming H2C data) when the capture failed (%1). Press "
        "Retry transmit once the DAC path is ready.").arg(why));
}

// A playback that never streamed most likely could not use the buffer size.
// Try the next rung down rather than giving up on transmit altogether.
bool BackendLauncher::retryH2cSmaller(QProcess* p, const QString& why)
{
    const int idx = indexOf(p);
    if (idx < 0) return false;
    const BackendProcess bp = m_procs[idx];
    if (!bp.h2cChunkBytes || bp.streamed) return false;
    const int next = nextSmallerH2cChunk(bp.h2cChunkBytes);
    if (!next) {
        if (p->state() != QProcess::NotRunning) {
            m_h2cBottomedByStall = true;
            // Every size has been tried: this is not a buffer-size problem.
            // iwfg_h2c resumes by itself once the card consumes, so it is
            // left running; the operator is told what to check.
            setH2cState(H2cState::Stalled, QStringLiteral(
                "The card does not take H2C data even with a %1 buffer, so this is "
                "not a buffer-size problem. iwfg_h2c keeps retrying and resumes by "
                "itself as soon as the card consumes. Check: DAC input = Host on the "
                "RFDC tab; the RF DAC is running (RFDC ▸ Start); the iwfg driver "
                "enables the PL H2C datapath (dmesg | grep iwfg). Last message: %2")
                .arg(byteSizeText(bp.h2cChunkBytes), why));
        }
        return false;
    }

    const QString msg = QStringLiteral(
        "H2C playback: a %1 buffer did not work (%2) — retrying with %3.")
        .arg(byteSizeText(bp.h2cChunkBytes), why, byteSizeText(next));
    setH2cState(H2cState::Restarting, msg);
    emit statusMessage(msg);

    const quint64 gen = m_generation;
    const QString label = bp.label;
    release(p, [this, next, gen, label] {
        if (gen != m_generation || m_stopping || !isRunning()) return;
        QString err;
        if (!launchH2c(next, &err)) {
            setH2cState(H2cState::Failed, err);
            emit processFailed(label, -1,
                QStringLiteral("H2C playback could not be restarted: %1").arg(err),
                /*fatal=*/false);
        }
    });
    return true;
}

void BackendLauncher::queueH2cRecovery(QProcess* p, const QString& why)
{
    BackendProcess* r = record(p);
    if (!r || r->h2cStepQueued) return;
    r->h2cStepQueued = true;
    QPointer<QProcess> pp(p);
    const bool streamed = r->streamed;
    const int chunk = r->h2cChunkBytes;
    QMetaObject::invokeMethod(this, [this, pp, why, streamed, chunk] {
        if (!pp || m_stopping) return;
        if (!streamed) {
            // First stall at this size: reopen at the same size. A stall is
            // at least as often the card not consuming (H2C datapath left
            // disabled by the driver's C2H-overflow reset) as a size the
            // card cannot take; 1.2.4 stepped straight down and walked the
            // whole ladder, cycling /dev/iwfg1 at every rung.
            if (m_h2cRetriedSize != chunk) {
                m_h2cRetriedSize = chunk;
                reopenH2c(pp, why);
            } else {
                retryH2cSmaller(pp, why);
            }
            return;
        }
        if (h2cReopenAllowed()) {
            reopenH2c(pp, why);
        } else if (BackendProcess* rr = record(pp)) {
            rr->h2cStepQueued = false;     // leave it to iwfg_h2c's own recovery
        }
    }, Qt::QueuedConnection);
}

bool BackendLauncher::h2cReopenAllowed()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QVector<qint64> recent;
    for (qint64 t : m_h2cReopens)
        if (now - t < kH2cReopenWindowMs) recent << t;
    m_h2cReopens = recent;
    return m_h2cReopens.size() < kH2cReopenMax;
}

void BackendLauncher::reopenH2c(QProcess* p, const QString& why)
{
    const int idx = indexOf(p);
    if (idx < 0) return;
    const BackendProcess bp = m_procs[idx];
    const int chunk = bp.h2cChunkBytes;
    m_h2cReopens << QDateTime::currentMSecsSinceEpoch();
    const QString msg = QStringLiteral(
        "H2C playback: transfers are not completing (%1) — reopening /dev/iwfg1 with "
        "the same %2 buffer. A reopen rebuilds the H2C queue and re-enables the PL H2C "
        "datapath, which the driver's C2H-overflow recovery can leave disabled.")
        .arg(why, byteSizeText(chunk));
    setH2cState(H2cState::Restarting, msg);
    emit statusMessage(msg);
    const quint64 gen = m_generation;
    const QString label = bp.label;
    release(p, [this, chunk, gen, label] {
        if (gen != m_generation || m_stopping || !isRunning()) return;
        QString err;
        if (!launchH2c(chunk, &err)) {
            setH2cState(H2cState::Failed, err);
            emit processFailed(label, -1,
                QStringLiteral("H2C playback could not be reopened: %1").arg(err),
                /*fatal=*/false);
        }
    });
}

void BackendLauncher::resetDevice(const QString& why)
{
    // Neither the UDP receiver nor the RoCEv2 receiver is a C2H capture
    // helper, so the capture watchdog does not apply to them.
    if (m_stopping || !isRunning()
     || m_cfg.mode == BackendMode::UdpStream
     || m_cfg.mode == BackendMode::RoceRx) return;
    const BackendConfig cfg = m_cfg;       // incl. the buffer size that works
    m_resetting = true;
    m_resetWhy = why;
    emit deviceReset(true, why);
    emit statusMessage(QStringLiteral(
        "Resetting the DMA device (%1): transmit is closed first, then capture — the "
        "driver soft-resets the QDMA when the last stream closes — then both start "
        "again.").arg(why));
    stop();
    m_pendingStart = true;
    m_pendingCfg = cfg;
}

bool BackendLauncher::ensureH2c()
{
    if (h2cRunning()) return true;
    if (m_stopping || !isRunning()) {
        emit statusMessage(QStringLiteral(
            "Transmit needs the backend: start acquisition first — the H2C playback "
            "helper is started with it."));
        return false;
    }
    emit statusMessage(QStringLiteral(
        "Transmit requested in a session launched without H2C playback — starting it."));
    restartH2c(0);
    return h2cRunning() || m_h2cState == H2cState::Restarting;
}

void BackendLauncher::restartH2c(int chunkBytes)
{
    if (m_stopping || !isRunning()) {
        emit statusMessage(QStringLiteral(
            "Transmit can only be (re)started while the backend is running — press Start."));
        return;
    }
    {
        const QString h2c = binaryPath(QStringLiteral("iwfg_h2c"));
        QString why;
        if (h2c.isEmpty()) {
            why = QStringLiteral("backend/bin/iwfg_h2c is not built — run `make` in the "
                                 "backend directory.");
        } else if (!helperLoads(h2c, &why)) {
            why = QStringLiteral("iwfg_h2c cannot load on this host: %1").arg(why);
        } else {
            checkDeviceNode(m_cfg.h2cDevice, QStringLiteral("H2C playback"), &why);
        }
        if (!why.isEmpty()) {
            setH2cState(H2cState::Failed, why);
            emit processFailed(QStringLiteral("H2C playback"), -1, why, /*fatal=*/false);
            return;
        }
    }
    if (m_cfg.mode == BackendMode::C2H) m_cfg.mode = BackendMode::H2C_C2H;
    const int bytes = chunkBytes > 0 ? chunkBytes : defaultH2cChunkBytes();
    setH2cState(H2cState::Restarting,
                QStringLiteral("restarting iwfg_h2c with a %1 buffer").arg(byteSizeText(bytes)));
    QProcess* old = nullptr;
    for (const BackendProcess& bp : m_procs)
        if (bp.role == BackendProcess::Playback) old = bp.proc;
    const quint64 gen = m_generation;
    auto go = [this, bytes, gen] {
        if (gen != m_generation || m_stopping || !isRunning()) return;
        QString err;
        if (!launchH2c(bytes, &err)) {
            setH2cState(H2cState::Failed, err);
            emit processFailed(QStringLiteral("H2C playback"), -1,
                QStringLiteral("H2C playback could not be restarted: %1").arg(err),
                /*fatal=*/false);
        }
    };
    if (old) release(old, go);
    else     go();
}

// -------------------------------------------------------------- stopping

void BackendLauncher::stop()
{
    m_pendingStart = false;
    ++m_generation;
    if (m_readyTimer) m_readyTimer->stop();

    if (m_procs.isEmpty()) {
        if (!m_stopping) {
            setH2cState(H2cState::NotRunning, QString());
            emit stopped();
        }
        return;
    }
    if (m_stopping) return;                    // already on its way down
    m_stopping = true;

    emit statusMessage(QStringLiteral("Stopping backend…"));

    // SIGINT: both applications install handlers that unwind the DMA path
    // and close the device cleanly. Transmit first; capture once transmit
    // has gone (stopTick), so the capture's close is the last one and the
    // driver's QDMA soft reset leaves the device clean for the next start.
    m_stopClock.start();
    bool playbackRunning = false;
    for (BackendProcess& bp : m_procs) {
        bp.stopSignalAt = -1;
        bp.stopPhase = 0;
        if (bp.role == BackendProcess::Playback && bp.proc
            && bp.proc->state() != QProcess::NotRunning) {
            signalStop(bp);
            playbackRunning = true;
        }
    }
    if (!playbackRunning)
        for (BackendProcess& bp : m_procs)
            if (bp.stopSignalAt < 0) signalStop(bp);
    m_stopTimer->start();
    checkStopDone();
}

void BackendLauncher::signalStop(BackendProcess& bp)
{
    bp.stopSignalAt = m_stopClock.elapsed();
    bp.stopPhase = 0;
#ifdef Q_OS_UNIX
    if (bp.proc && bp.proc->state() != QProcess::NotRunning)
        ::kill(static_cast<pid_t>(bp.proc->processId()), SIGINT);
#endif
}

bool BackendLauncher::checkStopDone()
{
    if (!m_stopping) return true;
    for (const BackendProcess& bp : m_procs)
        if (bp.proc && bp.proc->state() != QProcess::NotRunning) return false;
    finishStop();
    return true;
}

void BackendLauncher::stopTick()
{
    if (checkStopDone()) return;
    const qint64 t = m_stopClock.elapsed();
    bool playbackAlive = false;
    for (const BackendProcess& bp : m_procs)
        if (bp.role == BackendProcess::Playback && bp.proc
            && bp.proc->state() != QProcess::NotRunning)
            playbackAlive = true;

    bool waiting = false;
    for (BackendProcess& bp : m_procs) {
        if (!bp.proc || bp.proc->state() == QProcess::NotRunning) continue;
        if (bp.stopSignalAt < 0) {
            if (!playbackAlive || t >= kPlaybackFirstMs) signalStop(bp);
            waiting = true;
            continue;
        }
        const qint64 dt = t - bp.stopSignalAt;
        if (bp.stopPhase == 0 && dt >= kGraceMs) {
            bp.stopPhase = 1;
            emit statusMessage(QStringLiteral("%1 did not exit on SIGINT — terminating")
                                   .arg(bp.label));
            bp.proc->terminate();
        } else if (bp.stopPhase == 1 && dt >= kGraceMs + kTermMs) {
            bp.stopPhase = 2;
            emit statusMessage(QStringLiteral("%1 unresponsive — killing").arg(bp.label));
            bp.proc->kill();
        } else if (bp.stopPhase == 2 && dt >= kGraceMs + kTermMs + kKillWaitMs) {
            bp.stopPhase = 3;
            emit statusMessage(QStringLiteral(
                "%1 is stuck inside the driver and cannot be killed — it is left to "
                "exit on its own; the device may stay busy until it does.")
                .arg(bp.label));
        }
        if (bp.stopPhase < 3) waiting = true;
    }
    if (!waiting) finishStop();
}

void BackendLauncher::finishStop()
{
    m_stopTimer->stop();
    const QVector<BackendProcess> procs = m_procs;
    m_procs.clear();
    for (const BackendProcess& bp : procs)
        if (bp.proc) release(bp.proc);
    m_stopping = false;
    setH2cState(H2cState::NotRunning, QString());
    emit statusMessage(QStringLiteral("Backend stopped."));
    emit stopped();
    if (m_pendingStart) {
        m_pendingStart = false;
        const BackendConfig cfg = m_pendingCfg;
        QTimer::singleShot(0, this, [this, cfg] { start(cfg); });
    }
}

void BackendLauncher::teardown()
{
    const QVector<BackendProcess> procs = m_procs;
    m_procs.clear();
    for (const BackendProcess& bp : procs)
        if (bp.proc) release(bp.proc);
}

void BackendLauncher::release(QProcess* p, std::function<void()> whenGone)
{
    if (!p) return;
    p->disconnect(this);
    const int idx = indexOf(p);
    if (idx >= 0) m_procs.remove(idx);

    if (p->state() == QProcess::NotRunning) {
        p->deleteLater();
        if (whenGone) QTimer::singleShot(0, this, whenGone);
        return;
    }

    // Still alive. Deleting a running QProcess blocks in its destructor until
    // the process dies — and a helper inside an uninterruptible driver call
    // does not die. Signal it and keep it until it has really exited.
    if (!m_reaping.contains(p)) m_reaping.append(p);
    auto done = std::make_shared<bool>(false);
    auto finish = [whenGone, done] {
        if (*done) return;
        *done = true;
        if (whenGone) whenGone();
    };
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, p, finish](int, QProcess::ExitStatus) {
                m_reaping.removeAll(p);
                p->deleteLater();
                finish();
            });
#ifdef Q_OS_UNIX
    ::kill(static_cast<pid_t>(p->processId()), SIGINT);
#endif
    QPointer<QProcess> pp(p);
    QTimer::singleShot(1500, this, [pp] {
        if (pp && pp->state() != QProcess::NotRunning) pp->kill();
    });
    QTimer::singleShot(kRelaunchFallbackMs, this, finish);
}

int BackendLauncher::indexOf(const QProcess* p) const
{
    for (int i = 0; i < m_procs.size(); ++i)
        if (m_procs[i].proc == p) return i;
    return -1;
}

BackendProcess* BackendLauncher::record(const QProcess* p)
{
    const int i = indexOf(p);
    return i < 0 ? nullptr : &m_procs[i];
}

/// Verify a DMA character device can actually be opened before a helper is
/// spawned on it. Returns false with a reason naming the fix.
bool BackendLauncher::checkDeviceNode(const QString& path, const QString& role,
                                      QString* why)
{
    const QFileInfo fi(path);

    if (!fi.exists()) {
        QStringList found;
        const QFileInfoList nodes =
            QDir(QStringLiteral("/dev")).entryInfoList(
                QStringList() << QStringLiteral("iwfg*"), QDir::System | QDir::Files);
        for (const QFileInfo& f : nodes) found << f.fileName();

        *why = QStringLiteral(
            "%1: the device node %2 does not exist.\n\n"
            "Present in /dev: %3\n\n"
            "The driver derives the direction from the node: /dev/iwfg0 is C2H "
            "(receive), /dev/iwfg1 is H2C (transmit). If only iwfg0 exists, the "
            "driver did not create an H2C queue — check that the module loaded "
            "with both queues enabled (dmesg | grep iwfg).")
            .arg(role, path,
                 found.isEmpty() ? QStringLiteral("(none)")
                                 : found.join(QStringLiteral(", ")));
        return false;
    }

    if (!fi.isWritable() || !fi.isReadable()) {
        *why = QStringLiteral(
            "%1: %2 exists but is not accessible to this user.\n\n"
            "Owner %3:%4, mode %5.\n\n"
            "Either add a udev rule, or add your user to the owning group. "
            "Running the whole GUI with sudo is NOT recommended — it breaks the "
            "GPU path, because a CUDA IPC handle cannot cross users.")
            .arg(role, path, fi.owner(), fi.group(),
                 QString::number(int(fi.permissions()), 16));
        return false;
    }
    return true;
}

// -------------------------------------------------------------- events

void BackendLauncher::handleC2hLine(QProcess* p, const QString& line, bool* periodic)
{
    BackendProcess* r = record(p);
    if (!r) return;
    double mib = 0.0;
    if (parseC2hRateLine(line, &mib)) {
        r->streamed = true;
        *periodic = true;
        if (++r->rateLines > 1) emit c2hThroughput(mib);
        return;
    }
    if (line.contains(QLatin1String("FIFO connected"))) r->streamed = true;
}

void BackendLauncher::handleH2cLine(QProcess* p, const QString& line, bool* periodic)
{
    BackendProcess* r = record(p);
    if (!r) return;

    double mib = 0.0; int upd = -1;
    if (parseH2cRateLine(line, &mib, &upd)) {
        r->streamed = true;
        r->h2cStallLines = 0;
        m_h2cRetriedSize = 0;
        *periodic = true;
        const int chunk = r->h2cChunkBytes;
        setH2cState(H2cState::Streaming,
                    QStringLiteral("the card is consuming — buffer %1").arg(byteSizeText(chunk)));
        if (++r->rateLines > 1) emit h2cThroughput(mib, upd);
        if (m_h2cBottomedByStall && !m_h2cPromoted && chunk < defaultH2cChunkBytes()) {
            m_h2cPromoted = true;
            const QString msg = QStringLiteral(
                "The card consumes H2C data now, on the %1 buffer the step-down ended at. "
                "That size cannot keep the DAC fed — relaunching transmit with the %2 "
                "default.").arg(byteSizeText(chunk), byteSizeText(defaultH2cChunkBytes()));
            emit statusMessage(msg);
            QMetaObject::invokeMethod(this, [this] { restartH2c(0); }, Qt::QueuedConnection);
        }
        return;
    }
    if (line.contains(QLatin1String("Waiting for GNU Radio"))) {
        setH2cState(H2cState::WaitingForData, QStringLiteral(
            "iwfg_h2c (buffer %1) is waiting for the IQ generator to connect to %2")
            .arg(byteSizeText(r->h2cChunkBytes), m_cfg.h2cFifo));
    } else if (line.contains(QLatin1String("GNU Radio connected"))
               || line.contains(QLatin1String("GNU Radio reconnected"))) {
        if (!r->streamed)
            setH2cState(H2cState::Starting, QStringLiteral(
                "generator connected — waiting for its first %1 buffer")
                .arg(byteSizeText(r->h2cChunkBytes)));
    } else if (line.contains(QLatin1String("First chunk received"))) {
        r->h2cFirstChunk = true;
        setH2cState(H2cState::Starting, QStringLiteral(
            "first %1 buffer received — transfers to the card started")
            .arg(byteSizeText(r->h2cChunkBytes)));
        // The size check that does not depend on any message: a working
        // transfer reports a rate within about a second.
        QPointer<QProcess> pp(p);
        const quint64 gen = m_generation;
        QTimer::singleShot(kH2cFirstRateMs, this, [this, pp, gen] {
            if (!pp || gen != m_generation || m_stopping) return;
            BackendProcess* rr = record(pp);
            if (!rr || rr->streamed) return;
            queueH2cRecovery(pp, QStringLiteral(
                "no transfer completed within %1 s of the first buffer")
                .arg(kH2cFirstRateMs / 1000));
        });
    } else if (isH2cStallLine(line)) {
        if (!r->streamed) {
            if (r->h2cFirstChunk) queueH2cRecovery(p, line);
        } else {
            setH2cState(H2cState::Stalled, QStringLiteral(
                "the card stopped consuming H2C data (%1)").arg(line));
            // Two in a row (about 4 s): reopen rather than wait the ~30 s
            // iwfg_h2c takes to cycle the device by itself.
            if (++r->h2cStallLines >= 2) queueH2cRecovery(p, line);
        }
    } else if (line.contains(QLatin1String("Card resumed consuming"))
               || line.contains(QLatin1String("Recovered after"))) {
        if (r->streamed)
            setH2cState(H2cState::Streaming, QStringLiteral(
                "the card is consuming again — buffer %1").arg(byteSizeText(r->h2cChunkBytes)));
    }
}

void BackendLauncher::onReadyRead()
{
    auto* p = qobject_cast<QProcess*>(sender());
    if (!p) return;

    QString label = QStringLiteral("backend");
    BackendProcess::Role role = BackendProcess::Capture;
    if (const BackendProcess* r = record(p)) { label = r->label; role = r->role; }
    else return;

    const QString chunk = QString::fromLocal8Bit(p->readAll());
    const QStringList lines = chunk.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& l : lines) {
        const QString trimmed = l.trimmed();
        if (trimmed.isEmpty()) continue;
        emit logLine(QStringLiteral("[%1] %2").arg(label, trimmed));

        bool periodic = false;
        if (role == BackendProcess::Playback)      handleH2cLine(p, trimmed, &periodic);
        else if (role == BackendProcess::Capture)  handleC2hLine(p, trimmed, &periodic);

        BackendProcess* r = record(p);         // re-lookup: handlers may emit
        if (!r) return;
        r->tail.append(trimmed);
        while (r->tail.size() > kTailLines) r->tail.removeFirst();
        if (!periodic) {
            r->notable.append(trimmed);
            while (r->notable.size() > kNotableLines) r->notable.removeFirst();
        }
    }
}

void BackendLauncher::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    auto* p = qobject_cast<QProcess*>(sender());
    if (!p) return;
    if (m_stopping) { stopTick(); return; }          // expected during stop()

    const int idx = indexOf(p);
    if (idx < 0) return;
    const BackendProcess bp = m_procs[idx];
    const QString prog = QFileInfo(bp.program).fileName();

    QString reason = (status == QProcess::CrashExit)
        ? QStringLiteral("process crashed")
        : QStringLiteral("process exited with code %1").arg(exitCode);
    const QStringList said = bp.notable.isEmpty() ? bp.tail : bp.notable;
    if (!said.isEmpty())
        reason += QStringLiteral("\n\nLast output from %1:\n  %2")
                      .arg(prog, said.join(QStringLiteral("\n  ")));
    else
        reason += QStringLiteral(
            "\n\n%1 printed nothing before exiting. Common causes:\n"
            "  - the device node is missing or already open\n"
            "  - the FIFO does not exist, or nothing is connected to it\n"
            "  - insufficient permission for the device node\n"
            "Run it by hand to see the error:\n  %2 %3")
            .arg(prog, bp.program, bp.args.join(QLatin1Char(' ')));

    // ---- H2C playback (optional) -----------------------------------------
    if (bp.role == BackendProcess::Playback) {
        // A size the driver refuses (ADD_BUF / DMA_DATA error) is retried one
        // rung down. Errors no buffer size can fix are reported at once.
        static const char* kNotSize[] = {
            "open device", "chunk_bytes must be", "NOT a FIFO", "mkfifo",
            "stat fifo", "sigaction"
        };
        bool sizeRelated = true;
        for (const QString& l : said)
            for (const char* s : kNotSize)
                if (l.contains(QLatin1String(s))) sizeRelated = false;
        if (!bp.streamed && sizeRelated && status != QProcess::CrashExit
            && retryH2cSmaller(p, said.isEmpty() ? QStringLiteral("exit %1").arg(exitCode)
                                                 : said.last()))
            return;

        m_procs.remove(idx);
        p->deleteLater();
        setH2cState(H2cState::Failed, QStringLiteral("iwfg_h2c exited: %1")
                        .arg(said.isEmpty() ? QStringLiteral("code %1").arg(exitCode)
                                            : said.last()));
        reason += QStringLiteral(
            "\n\nCapture is unaffected and is still running. Only %1 stopped — "
            "use Retry transmit in the H2C control window to start it again.")
            .arg(bp.label);
        emit processFailed(bp.label, exitCode, reason, /*fatal=*/false);
        return;
    }

    // ---- C2H capture (the source) ----------------------------------------
    if (bp.role == BackendProcess::Capture) {
        QString decisive;
        const C2hExit why = classifyC2hExit(bp.tail, &decisive);
        const QString whyText = c2hExitText(why)
            + (decisive.isEmpty() ? QString() : QStringLiteral(" (\"%1\")").arg(decisive));

        if (bp.streamed && why != C2hExit::StartupError && c2hRestartAllowed()) {
            m_procs.remove(idx);
            p->deleteLater();
            m_c2hRestarts << QDateTime::currentMSecsSinceEpoch();
            const int attempt = m_c2hRestarts.size();
            emit c2hRecovering(whyText, attempt, m_cfg.c2hRestartLimit);
            emit statusMessage(QStringLiteral(
                "Capture helper ended — %1. Restarting it (%2 of %3 per minute)…")
                .arg(whyText).arg(attempt).arg(m_cfg.c2hRestartLimit));
            stopStalledH2c(whyText);
            const quint64 gen = m_generation;
            QTimer::singleShot(kC2hRestartDelayMs, this, [this, gen] {
                if (gen != m_generation || m_stopping) return;
                relaunchC2h();
            });
            return;
        }

        QString head = QStringLiteral("The capture helper ended: %1.").arg(whyText);
        if (bp.streamed && why != C2hExit::StartupError)
            head += QStringLiteral(" It was restarted %1 time(s) in the last minute and "
                                   "keeps ending, so the session was stopped.")
                        .arg(m_c2hRestarts.size());
        reason = head + QStringLiteral("\n\n") + reason;
        emit processFailed(bp.label, exitCode, reason, /*fatal=*/true);
        stop();
        return;
    }

    // ---- UDP receiver (the source) ---------------------------------------
    emit processFailed(bp.label, exitCode, reason, /*fatal=*/true);
    stop();
}

void BackendLauncher::onProcessError(QProcess::ProcessError err)
{
    auto* p = qobject_cast<QProcess*>(sender());
    if (!p || m_stopping) return;
    // FailedToStart is reported synchronously by launchOne(); a crash also
    // arrives through finished(). This is diagnostic only.
    if (const BackendProcess* r = record(p))
        emit logLine(QStringLiteral("[%1] process error %2: %3")
                         .arg(r->label).arg(int(err)).arg(p->errorString()));
}

} // namespace sdr
