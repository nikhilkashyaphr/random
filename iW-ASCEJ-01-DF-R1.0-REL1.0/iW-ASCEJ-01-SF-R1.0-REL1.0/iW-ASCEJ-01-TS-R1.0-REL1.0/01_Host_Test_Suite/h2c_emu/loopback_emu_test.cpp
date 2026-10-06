// ---------------------------------------------------------------------------
// The whole send-and-receive chain, with only the card replaced:
//
//   IqGenerator -> FIFO -> iwfg_h2c -> [fake H2C DMA  ==  "DAC"]
//                                         | loop file = RF cable DAC -> ADC
//   DmaSource   <- FIFO <- c2h_stream <- [fake C2H DMA  ==  "ADC"]
//
// iwfg_h2c and c2h_stream are the REAL helpers built from backend/, launched
// and supervised by the REAL BackendLauncher; the generator and the reader
// are the GUI's own classes. fake_iwfg.so (LD_PRELOAD) stands in for the
// driver and can inject the faults seen on the bench:
//
//   A  send and receive: the tone generated is the tone received
//   B  capture helper dies (driver error) -> restarted, reader re-attaches,
//      transmit untouched, no dialog
//   C  H2C buffer too large for the card -> stepped down by itself
//   D  card never consumes H2C -> stops at the bottom rung, capture unaffected
//   E  helpers that ignore SIGINT/SIGTERM -> stop() still never blocks
//   F  start() while a stop is in progress is queued, not refused
//   G  the helper pre-flight does not touch the device and is quick
//   H  the reader does not spin while the capture helper is away
//   I  a missing H2C node does not stop the capture from starting
//   J  a capture-only session starts the transmit helper on demand
//   K  card consumes only later -> transmit goes back to the default buffer
//   P  a stall that a reopen cures -> reopened at the same size, no step-down
//   M  stop closes transmit before capture (the driver resets on the last close)
//   N  the capture FIFO gets a larger pipe buffer
//   O  the card stops delivering C2H -> resetDevice() brings everything back
// ---------------------------------------------------------------------------
#include "BackendLauncher.h"
#include "IqGenerator.h"
#include "Sources.h"
#include "spectrum.h"
#include "CaptureBudget.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QStandardPaths>
#include <QTimer>
#include <atomic>
#include <cstdio>
#include <functional>
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

using sdr::BackendLauncher;
using sdr::H2cState;
using sdr::IqGenerator;
using sdr::cf32;

static int g_fail = 0;
#define CHECK(c, ...) do { std::printf((c) ? "  PASS  " : "  FAIL  "); std::printf(__VA_ARGS__); \
                           std::printf("\n"); std::fflush(stdout); if (!(c)) ++g_fail; } while (0)

static QString RIG, SHIM, C2H_BIN, H2C_BIN;
static constexpr double kFs = 200e6;
static constexpr double kToneHz = 10e6;

static void pump(int ms)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) { QCoreApplication::processEvents(QEventLoop::AllEvents, 5); QThread::msleep(1); }
}
static bool waitFor(const std::function<bool()>& cond, int ms)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) {
        if (cond()) return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return cond();
}
static double cpuSeconds()
{
    struct rusage u {};
    getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_stime.tv_sec + (u.ru_utime.tv_usec + u.ru_stime.tv_usec) * 1e-6;
}

static const char* kFaultVars[] = {
    "FAKE_IWFG_H2C_STALL_ABOVE", "FAKE_IWFG_H2C_STALL_ALL", "FAKE_IWFG_C2H_FAIL_AFTER",
    "FAKE_IWFG_C2H_FAIL_ONCE", "FAKE_IWFG_BLOCK_STOP", "FAKE_IWFG_OPEN_LOG",
    "FAKE_IWFG_H2C_STALL_FILE", "FAKE_IWFG_H2C_STALL_ONCE", "FAKE_IWFG_C2H_STALL_AFTER",
    "FAKE_IWFG_C2H_STALL_ONCE"
};

/// One backend session with generator and reader, as the GUI wires them.
struct Session : QObject {
    BackendLauncher L;
    sdr::BackendConfig cfg;
    sdr::DmaSource src;
    QThread genThread;
    IqGenerator* gen = new IqGenerator;
    std::atomic<bool> genRunning{false};
    int  pendingChunk = 0;
    int  chunk = 0;
    int  chunkChanges = 0;

    // what happened
    int ready = 0, stoppedN = 0, fatal = 0, optional = 0, recovering = 0, startFailed = 0;
    int resetsBegun = 0, resetsDone = 0;
    QVector<H2cState> states;
    double h2cMib = 0, c2hMib = 0;
    QStringList log;
    QString lastFailure;

    // what arrived: the last 2^18 samples of channel 0
    std::vector<cf32> ring = std::vector<cf32>(1u << 18);
    quint64 received = 0;

    explicit Session(const QStringList& faults = {})
    {
        for (const char* v : kFaultVars) qunsetenv(v);
        for (const QString& kv : faults)
            qputenv(kv.section('=', 0, 0).toLocal8Bit(), kv.section('=', 1).toLocal8Bit());

        cfg.mode = sdr::BackendMode::H2C_C2H;
        cfg.c2hDevice = RIG + "/iwfg0";
        cfg.h2cDevice = RIG + "/iwfg1";
        cfg.c2hFifo = RIG + "/c2h.fifo";
        cfg.h2cFifo = RIG + "/h2c.fifo";
        QFile::remove(RIG + "/dac.loop");

        connect(&L, &BackendLauncher::becameReady, this, [this](const QString& fifo) {
            ++ready;
            sdr::Config c;
            c.src.mode = sdr::SourceMode::Dma;
            c.src.devicePath = fifo;
            c.src.format = sdr::SampleFormat::Cs16;
            c.src.streamChannels = 1;
            c.src.blockSamples = 65536;
            src.applyConfig(c);
            src.start();
        });
        connect(&L, &BackendLauncher::stopped, this, [this] { ++stoppedN; });
        connect(&L, &BackendLauncher::startFailed, this, [this](const QString& w) { ++startFailed; lastFailure = w; });
        connect(&L, &BackendLauncher::processFailed, this,
                [this](const QString&, int, const QString& why, bool f) { (f ? fatal : optional)++; lastFailure = why; });
        connect(&L, &BackendLauncher::c2hRecovering, this, [this](const QString&, int, int) { ++recovering; });
        connect(&L, &BackendLauncher::deviceReset, this, [this](bool begun, const QString&) {
            (begun ? resetsBegun : resetsDone)++; });
        connect(&L, &BackendLauncher::h2cStateChanged, this, [this](H2cState s, const QString&) { states << s; });
        connect(&L, &BackendLauncher::h2cThroughput, this, [this](double m, int) { h2cMib = m; });
        connect(&L, &BackendLauncher::c2hThroughput, this, [this](double m) { c2hMib = m; });
        connect(&L, &BackendLauncher::statusMessage, this, [this](const QString& l) { log << l; });
        connect(&L, &BackendLauncher::logLine, this, [this](const QString& l) { log << l; });
        connect(&src, &sdr::ISignalSource::statusMessage, this, [this](const QString& l) { log << "[reader] " + l; });
        connect(&src, &sdr::ISignalSource::blockReady, this, [this](const sdr::SampleBlock& b) {
            const auto* w = reinterpret_cast<const qint16*>(b.raw.constData());
            const std::size_t n = b.samplesPerChannel();
            const int nch = std::max(1, b.rawChannels);
            for (std::size_t i = 0; i < n; ++i) {
                ring[std::size_t(received % ring.size())] =
                    cf32(float(w[2 * i * nch]) / 32764.0f, float(w[2 * i * nch + 1]) / 32764.0f);
                ++received;
            }
        });

        // The generator's loop must equal iwfg_h2c's chunk: restart it when
        // the launcher steps the buffer down (as the control window does).
        connect(&L, &BackendLauncher::h2cChunkBytesChanged, this, [this](int bytes) {
            ++chunkChanges;
            if (genRunning && bytes != chunk) { pendingChunk = bytes; gen->stop(); }
            else chunk = bytes;
        });
        gen->moveToThread(&genThread);
        connect(gen, &IqGenerator::stopped, this, [this] {
            genRunning = false;
            if (pendingChunk) { const int c = pendingChunk; pendingChunk = 0; startGen(c); }
        }, Qt::QueuedConnection);
        genThread.start();
    }
    ~Session() override
    {
        gen->stop();
        waitFor([this] { return !genRunning; }, 3000);
        genThread.quit(); genThread.wait(3000); delete gen;
        src.stop();
        L.stop();
        waitFor([this] { return !L.isRunning(); }, 9000);
        for (const char* v : kFaultVars) qunsetenv(v);
    }
    void startGen(int bytes)
    {
        chunk = bytes;
        IqGenerator::Config c;
        c.fifoPath = cfg.h2cFifo; c.sampleRateSps = kFs; c.frequencyHz = kToneHz; c.chunkBytes = bytes;
        genRunning = true;
        QMetaObject::invokeMethod(gen, [this, c] { gen->start(c); }, Qt::QueuedConnection);
    }
    double expectedHz() const
    {
        IqGenerator::Config c; c.sampleRateSps = kFs; c.frequencyHz = kToneHz; c.chunkBytes = chunk;
        return IqGenerator::transmittedHz(c);
    }
    std::vector<cf32> latest() const
    {
        std::vector<cf32> x(ring.size());
        const quint64 start = received - ring.size();
        for (std::size_t i = 0; i < ring.size(); ++i)
            x[i] = ring[std::size_t((start + i) % ring.size())];
        return x;
    }
    bool sawState(H2cState s) const { return states.contains(s); }
    bool logHas(const QString& needle) const
    { for (const QString& l : log) if (l.contains(needle)) return true; return false; }
    void dumpLog(int last = 25) const
    {
        for (int i = std::max(0, int(log.size()) - last); i < log.size(); ++i)
            std::printf("        | %s\n", qPrintable(log[i]));
    }
    /// Received tone after `afterSamples` more samples have arrived.
    t::Spectrum receivedTone(quint64 moreSamples, int ms, bool* ok)
    {
        const quint64 target = received + moreSamples;
        *ok = waitFor([&] { return received >= target; }, ms);
        return t::analyse(latest(), kFs);
    }
};

static void makeRig()
{
    QDir().mkpath(RIG + "/backend/bin");
    QFile mk(RIG + "/backend/Makefile");
    mk.open(QIODevice::WriteOnly); mk.write("all:\n"); mk.close();
    QFile::remove(RIG + "/backend/bin/c2h_stream");
    QFile::remove(RIG + "/backend/bin/iwfg_h2c");
    QFile::copy(C2H_BIN, RIG + "/backend/bin/c2h_stream");
    QFile::copy(H2C_BIN, RIG + "/backend/bin/iwfg_h2c");
    for (const char* n : {"/backend/bin/c2h_stream", "/backend/bin/iwfg_h2c"})
        QFile::setPermissions(RIG + n, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
                                           | QFile::ReadGroup | QFile::ExeGroup);
    for (const char* n : {"/iwfg0", "/iwfg1"}) {
        QFile f(RIG + n); f.open(QIODevice::WriteOnly); f.close();
    }
    QDir::setCurrent(RIG);                                   // backendDirectory(): ./backend
    qputenv("LD_PRELOAD", SHIM.toLocal8Bit());
    qputenv("FAKE_IWFG_C2H_NODE", (RIG + "/iwfg0").toLocal8Bit());
    qputenv("FAKE_IWFG_H2C_NODE", (RIG + "/iwfg1").toLocal8Bit());
    qputenv("FAKE_IWFG_LOOP_FILE", (RIG + "/dac.loop").toLocal8Bit());
    qputenv("FAKE_IWFG_C2H_CHANNELS", "1");
    qputenv("FAKE_IWFG_ADC_BPS", "200e6");
    qunsetenv("IWFG_H2C_CHUNK_BYTES");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    SHIM    = qEnvironmentVariable("FAKE_IWFG_SO");
    C2H_BIN = qEnvironmentVariable("C2H_BIN");
    H2C_BIN = qEnvironmentVariable("H2C_BIN");
    RIG     = qEnvironmentVariable("TMPDIR", QStringLiteral("/tmp")) +
              QStringLiteral("/loopback_emu_%1").arg(QCoreApplication::applicationPid());
    std::printf("Send-and-receive chain: generator -> iwfg_h2c -> [fake card, DAC->ADC loop] "
                "-> c2h_stream -> reader\n\n");
    if (!QFileInfo(SHIM).exists() || !QFileInfo(C2H_BIN).isExecutable() || !QFileInfo(H2C_BIN).isExecutable()) {
        std::printf("  FAIL  FAKE_IWFG_SO / C2H_BIN / H2C_BIN not set\n");
        return 2;
    }
    makeRig();
    const bool haveStdbuf = !QStandardPaths::findExecutable(QStringLiteral("stdbuf")).isEmpty();

    // ---- G: pre-flight must not touch the device, and must be quick -------
    {
        const QString openLog = RIG + "/opens.log";
        QFile::remove(openLog);
        qputenv("FAKE_IWFG_OPEN_LOG", openLog.toLocal8Bit());
        QElapsedTimer t; t.start();
        QString why1, why2;
        const bool a = BackendLauncher::helperLoads(BackendLauncher::binaryPath("c2h_stream"), &why1);
        const bool b = BackendLauncher::helperLoads(BackendLauncher::binaryPath("iwfg_h2c"), &why2);
        const qint64 ms = t.elapsed();
        qunsetenv("FAKE_IWFG_OPEN_LOG");
        CHECK(a && b, "G  both helpers load (c2h_stream: %s, iwfg_h2c: %s)",
              a ? "ok" : qPrintable(why1), b ? "ok" : qPrintable(why2));
        CHECK(ms < 1500, "G  pre-flight took %lld ms (1.2.3: ~6000 ms with the window frozen)", (long long)ms);
        CHECK(!QFileInfo::exists(openLog), "G  pre-flight opened no device node");
    }

    // ---- A: send and receive ------------------------------------------------
    {
        std::printf("\nA  send and receive\n");
        Session s;
        s.L.start(s.cfg);
        CHECK(waitFor([&] { return s.ready > 0; }, 6000), "A  backend ready (capture FIFO %s)", qPrintable(s.cfg.c2hFifo));
        CHECK(s.chunk == (1 << 20), "A  iwfg_h2c launched with the 1 MiB default buffer (got %d)", s.chunk);
        s.startGen(s.chunk);
        const bool streaming = waitFor([&] { return s.L.h2cState() == H2cState::Streaming; }, 8000);
        CHECK(streaming, "A  H2C playback reports Streaming");
        // The rate itself is h2c_emu_test's subject (measured unloaded); here
        // it only has to be reported to the GUI.
        CHECK(waitFor([&] { return s.h2cMib > 0.0; }, 4000),
              "A  iwfg_h2c's delivery reported to the GUI: %.1f MiB/s (the DAC needs %.1f)",
              s.h2cMib, kFs * 4 / 1048576.0);
        bool got = false;
        const t::Spectrum sp = s.receivedTone(3u << 20, 10000, &got);
        CHECK(got, "A  %llu samples received through the loopback", (unsigned long long)s.received);
        CHECK(std::abs(sp.peakHz - s.expectedHz()) < kFs / double(1u << 18),
              "A  received tone %.6f MHz = transmitted %.6f MHz", sp.peakHz / 1e6, s.expectedHz() / 1e6);
        CHECK(sp.sfdrDb > 70.0, "A  received tone is clean: SFDR %.1f dB", sp.sfdrDb);
        if (haveStdbuf) {
            CHECK(waitFor([&] { return s.c2hMib > 0; }, 3000),
                  "A  c2h_stream's own rate arrives live (%.1f MiB/s; stdbuf line buffering)", s.c2hMib);
            if (s.c2hMib <= 0) for (const QString& l : s.log) if (l.contains("C2H")) std::printf("        | %s\n", qPrintable(l));
        }
        else
            std::printf("  SKIP  stdbuf not installed: c2h_stream rate lines arrive only at exit\n");
        CHECK(s.fatal == 0 && s.optional == 0, "A  no failure reported");
        if (!streaming || !got) s.dumpLog();

        // ---- H: reader idles without spinning once the writer is gone -----
        s.gen->stop();
        waitFor([&] { return !s.genRunning; }, 3000);
        s.L.stop();
        CHECK(waitFor([&] { return s.stoppedN > 0; }, 8000), "H  backend stopped (asynchronously)");
        pump(300);
        const double c0 = cpuSeconds();
        pump(1500);
        const double used = cpuSeconds() - c0;
        CHECK(used < 0.35, "H  reader with no writer used %.2f s CPU in 1.5 s (1.2.3 spun a whole core)", used);
        CHECK(s.logHas("Nothing is writing to"), "H  the reader says it is waiting for the capture helper");
        s.src.stop();
    }

    // ---- B: capture helper dies -> restarted, reader re-attaches ----------
    {
        std::printf("\nB  capture helper fails mid-stream (driver error)\n");
        const QString marker = RIG + "/c2h_failed_once";
        QFile::remove(marker);
        Session s({QStringLiteral("FAKE_IWFG_C2H_FAIL_AFTER=15"),
                   QStringLiteral("FAKE_IWFG_C2H_FAIL_ONCE=%1").arg(marker)});
        s.L.start(s.cfg);
        waitFor([&] { return s.ready > 0; }, 6000);
        s.startGen(s.chunk);
        CHECK(waitFor([&] { return s.recovering > 0; }, 10000),
              "B  the launcher saw c2h_stream end and restarted it");
        CHECK(s.logHas("DMA driver failed a C2H transfer"),
              "B  the reason is named (not just \"exit 0\")");
        const quint64 before = s.received;
        CHECK(waitFor([&] { return s.logHas("Re-attached to"); }, 6000),
              "B  the reader re-attached to the new FIFO by itself");
        bool got = false;
        const t::Spectrum sp = s.receivedTone(2u << 20, 10000, &got);
        CHECK(got && s.received > before, "B  data flows again after the restart (%llu new samples)",
              (unsigned long long)(s.received - before));
        CHECK(std::abs(sp.peakHz - s.expectedHz()) < kFs / double(1u << 18) && sp.sfdrDb > 70.0,
              "B  and it is the transmitted tone (%.6f MHz, SFDR %.1f dB)", sp.peakHz / 1e6, sp.sfdrDb);
        CHECK(s.fatal == 0, "B  no fatal failure, no dialog");
        CHECK(s.chunkChanges == 1 && s.L.h2cRunning(), "B  transmit was not disturbed (iwfg_h2c launched once)");
        if (!got || s.recovering == 0) s.dumpLog();
    }

    // ---- C: buffer too large for the card -> stepped down -----------------
    {
        std::printf("\nC  card does not complete transfers above 256 KiB\n");
        Session s({QStringLiteral("FAKE_IWFG_H2C_STALL_ABOVE=262144")});
        s.L.start(s.cfg);
        waitFor([&] { return s.ready > 0; }, 6000);
        s.startGen(s.chunk);
        const bool down = waitFor([&] { return s.chunk == (256 << 10) && s.L.h2cState() == H2cState::Streaming; }, 20000);
        CHECK(down, "C  stepped down 1 MiB -> 256 KiB and streams (state %s)", qPrintable(sdr::h2cStateName(s.L.h2cState())));
        CHECK(s.sawState(H2cState::Restarting), "C  the step-down was reported");
        CHECK(s.logHas("reopening /dev/iwfg1 with the same 1 MiB buffer"),
              "C  it first reopened at the same size, and stepped down only when that stalled too");
        bool got = false;
        const t::Spectrum sp = s.receivedTone(2u << 20, 10000, &got);
        CHECK(got && std::abs(sp.peakHz - s.expectedHz()) < kFs / double(1u << 18) && sp.sfdrDb > 70.0,
              "C  the generator followed the new loop length; tone received %.6f MHz, SFDR %.1f dB",
              sp.peakHz / 1e6, sp.sfdrDb);
        CHECK(s.fatal == 0 && s.optional == 0, "C  no failure dialog");
        if (!down) s.dumpLog();
    }

    // ---- D: card never consumes -> bottom rung, capture unaffected ---------
    {
        std::printf("\nD  card never consumes H2C data\n");
        Session s({QStringLiteral("FAKE_IWFG_H2C_STALL_ALL=1")});
        s.L.start(s.cfg);
        waitFor([&] { return s.ready > 0; }, 6000);
        s.startGen(s.chunk);
        const bool bottom = waitFor([&] { return s.chunk == (16 << 10) && s.L.h2cState() == H2cState::Stalled; }, 60000);
        CHECK(bottom, "D  steps down to 16 KiB, then reports Stalled (not a size problem)");
        CHECK(s.L.h2cRunning(), "D  iwfg_h2c is left running: it resumes by itself when the card consumes");
        CHECK(s.L.h2cStateDetail().contains("not a buffer-size problem"), "D  the operator is told what to check");
        const quint64 r0 = s.received;
        pump(1000);
        CHECK(s.received > r0 && s.fatal == 0, "D  capture keeps running meanwhile (%llu samples in 1 s)",
              (unsigned long long)(s.received - r0));
        CHECK(s.optional == 0, "D  no dialog");
        if (!bottom) s.dumpLog();
    }

    // ---- K: card starts consuming only later (DAC routed to Host late) ---------
    {
        std::printf("\nK  card consumes nothing at first, then starts (DAC input switched late)\n");
        const QString gate = RIG + "/card_not_consuming";
        { QFile f(gate); f.open(QIODevice::WriteOnly); }
        Session s({QStringLiteral("FAKE_IWFG_H2C_STALL_FILE=%1").arg(gate)});
        s.L.start(s.cfg);
        waitFor([&] { return s.ready > 0; }, 6000);
        s.startGen(s.chunk);
        const bool bottom = waitFor([&] { return s.chunk == (16 << 10) && s.L.h2cState() == H2cState::Stalled; }, 60000);
        CHECK(bottom, "K  walked down to 16 KiB and reports Stalled");
        QFile::remove(gate);                          // the operator fixes the DAC path
        const bool back = waitFor([&] { return s.chunk == (1 << 20) && s.L.h2cState() == H2cState::Streaming; }, 20000);
        CHECK(back, "K  once the card consumes, transmit is relaunched at the 1 MiB default (not left on 16 KiB)");
        bool got = false;
        const t::Spectrum sp = s.receivedTone(2u << 20, 10000, &got);
        CHECK(got && std::abs(sp.peakHz - s.expectedHz()) < kFs / double(1u << 18) && sp.sfdrDb > 70.0,
              "K  tone received %.6f MHz, SFDR %.1f dB", sp.peakHz / 1e6, sp.sfdrDb);
        CHECK(s.fatal == 0 && s.optional == 0, "K  no dialog");
        if (!back) s.dumpLog();
    }

    // ---- P: a stall a reopen cures -> no step-down -------------------------------
    {
        std::printf("\nP  first transmit helper never completes a transfer; a reopen cures it\n");
        const QString marker = RIG + "/h2c_stalled_once";
        QFile::remove(marker);
        Session s({QStringLiteral("FAKE_IWFG_H2C_STALL_ONCE=%1").arg(marker)});
        s.L.start(s.cfg);
        waitFor([&] { return s.ready > 0; }, 6000);
        s.startGen(s.chunk);
        const bool ok = waitFor([&] { return QFileInfo::exists(marker) && s.L.h2cState() == H2cState::Streaming; }, 15000);
        CHECK(ok && s.chunk == (1 << 20), "P  streaming again at 1 MiB after one same-size reopen (buffer %d KiB)", s.chunk / 1024);
        CHECK(s.logHas("reopening /dev/iwfg1 with the same 1 MiB buffer") && !s.logHas("retrying with"),
              "P  no step-down (1.2.4 walked straight down the ladder)");
        bool got = false;
        const t::Spectrum sp = s.receivedTone(2u << 20, 10000, &got);
        CHECK(got && std::abs(sp.peakHz - s.expectedHz()) < kFs / double(1u << 18),
              "P  tone received %.6f MHz", sp.peakHz / 1e6);
        if (!ok) s.dumpLog();
    }

    // ---- M/N: ordered stop, capture FIFO pipe size -----------------------------
    {
        std::printf("\nM  stop closes transmit before capture; N  capture FIFO pipe enlarged\n");
        const QString openLog = RIG + "/order.log";
        QFile::remove(openLog);
        Session s({QStringLiteral("FAKE_IWFG_OPEN_LOG=%1").arg(openLog)});
        s.L.start(s.cfg);
        waitFor([&] { return s.ready > 0; }, 6000);
        s.startGen(s.chunk);
        waitFor([&] { return s.L.h2cState() == H2cState::Streaming; }, 8000);
        pump(300);
        int pipeSz = -1;
        {
            const int fd = ::open(s.cfg.c2hFifo.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK);
            if (fd >= 0) { pipeSz = ::fcntl(fd, F_GETPIPE_SZ); ::close(fd); }
        }
        int maxSz = 1 << 20;
        { QFile f("/proc/sys/fs/pipe-max-size"); if (f.open(QIODevice::ReadOnly)) maxSz = std::min(QString(f.readAll()).trimmed().toInt(), 16 << 20); }
        CHECK(pipeSz >= std::min(maxSz, 1 << 20), "N  capture FIFO pipe buffer %d KiB (default 64 KiB; max allowed %d KiB)",
              pipeSz / 1024, maxSz / 1024);
        s.gen->stop();
        waitFor([&] { return !s.genRunning; }, 3000);
        s.L.stop();
        waitFor([&] { return s.stoppedN > 0; }, 10000);
        QFile f(openLog);
        f.open(QIODevice::ReadOnly);
        const QStringList lines = QString::fromLocal8Bit(f.readAll()).split('\n', Qt::SkipEmptyParts);
        int h2cClose = -1, c2hClose = -1;
        for (int i = 0; i < lines.size(); ++i) {
            if (lines[i].endsWith(" close h2c")) h2cClose = i;
            if (lines[i].endsWith(" close c2h")) c2hClose = i;
        }
        CHECK(h2cClose >= 0 && c2hClose > h2cClose,
              "M  /dev/iwfg1 closed before /dev/iwfg0 (so the driver's QDMA reset runs on the last close)");
        s.src.stop();
    }

    // ---- O: card stops delivering C2H -> device reset brings it back -----------
    {
        std::printf("\nO  card stops delivering C2H data mid-stream (wedged)\n");
        const QString marker = RIG + "/c2h_stalled_once";
        QFile::remove(marker);
        Session s({QStringLiteral("FAKE_IWFG_C2H_STALL_AFTER=10"),
                   QStringLiteral("FAKE_IWFG_C2H_STALL_ONCE=%1").arg(marker)});
        s.L.start(s.cfg);
        waitFor([&] { return s.ready > 0; }, 6000);
        s.startGen(s.chunk);
        CHECK(waitFor([&] { return QFileInfo::exists(marker); }, 10000), "O  the capture stalled (c2h_stream waits in the driver)");
        // What MainWindow's watchdog does after ~5 s of nothing:
        pump(500);                                    // drain what was already in the pipe
        quint64 r0 = s.received;
        pump(2000);
        CHECK(s.received - r0 < 70000, "O  nothing arrives meanwhile (%llu samples in 2 s)", (unsigned long long)(s.received - r0));
        s.L.resetDevice(QStringLiteral("test: no capture data"));
        CHECK(s.resetsBegun == 1, "O  resetDevice() reported as begun");
        CHECK(waitFor([&] { return s.resetsDone == 1; }, 15000), "O  ... and finished: capture and transmit restarted");
        CHECK(s.logHas("Resetting the DMA device"), "O  the reason is logged");
        r0 = s.received;
        bool got = false;
        const t::Spectrum sp = s.receivedTone(2u << 20, 12000, &got);
        CHECK(got && s.received > r0 && std::abs(sp.peakHz - s.expectedHz()) < kFs / double(1u << 18),
              "O  data flows again and it is the transmitted tone (%.6f MHz)", sp.peakHz / 1e6);
        CHECK(waitFor([&] { return s.L.h2cState() == H2cState::Streaming; }, 8000), "O  transmit streaming again");
        CHECK(s.fatal == 0, "O  no fatal failure");
        if (!got) s.dumpLog();
    }

    // ---- E: stop() never blocks ---------------------------------------------
    {
        std::printf("\nE  helpers that ignore SIGINT and SIGTERM\n");
        Session s({QStringLiteral("FAKE_IWFG_BLOCK_STOP=1")});
        s.L.start(s.cfg);
        CHECK(waitFor([&] { return s.ready > 0; }, 6000), "E  backend ready");
        pump(500);
        QElapsedTimer tick; tick.start();
        qint64 maxGap = 0, last = 0;
        QTimer beat;
        QObject::connect(&beat, &QTimer::timeout, [&] {
            const qint64 now = tick.elapsed(); maxGap = std::max(maxGap, now - last); last = now; });
        beat.start(20);
        QElapsedTimer t; t.start();
        s.L.stop();
        const qint64 callMs = t.elapsed();
        last = tick.elapsed();
        const bool done = waitFor([&] { return s.stoppedN > 0; }, 10000);
        const qint64 totalMs = t.elapsed();
        CHECK(callMs < 50, "E  stop() returned in %lld ms (1.2.3 blocked up to 10 s)", (long long)callMs);
        CHECK(done && totalMs > 3500, "E  stopped() after %lld ms: SIGINT, SIGTERM ignored, SIGKILL worked", (long long)totalMs);
        CHECK(maxGap < 250, "E  the event loop kept running throughout (longest gap %lld ms)", (long long)maxGap);
        CHECK(!s.L.isRunning() && s.L.lingeringProcesses() == 0, "E  nothing left behind");
        s.src.stop();
    }

    // ---- F: start while stopping is queued ----------------------------------
    {
        std::printf("\nF  Start pressed while the previous backend is still exiting\n");
        Session s;
        s.L.start(s.cfg);
        waitFor([&] { return s.ready > 0; }, 6000);
        s.L.stop();
        s.L.start(s.cfg);
        CHECK(s.startFailed == 0, "F  not refused");
        CHECK(waitFor([&] { return s.stoppedN > 0 && s.ready > 1; }, 12000),
              "F  the queued start ran after the stop completed (ready %d times)", s.ready);
    }

    // ---- I: transmit node missing -> capture still starts --------------------
    {
        std::printf("\nI  H2C device node missing\n");
        Session s;
        s.cfg.h2cDevice = RIG + "/no_such_iwfg1";
        s.L.start(s.cfg);
        CHECK(waitFor([&] { return s.ready > 0; }, 6000) && s.startFailed == 0,
              "I  the capture starts anyway (1.2.3 refused to start at all)");
        CHECK(waitFor([&] { return s.optional == 1; }, 2000) && s.fatal == 0,
              "I  transmit reported as unavailable, non-fatal: \"%s\"",
              qPrintable(s.lastFailure.section('\n', 0, 0).left(70)));
        const quint64 r0 = s.received;
        pump(800);
        CHECK(s.received > r0, "I  capture delivers (%llu samples)", (unsigned long long)(s.received - r0));
    }

    // ---- J: capture-only session, transmit requested later --------------------
    {
        std::printf("\nJ  session launched as C2H only, generator started later\n");
        Session s;
        s.cfg.mode = sdr::BackendMode::C2H;
        s.L.start(s.cfg);
        waitFor([&] { return s.ready > 0; }, 6000);
        CHECK(!s.L.h2cRunning() && s.L.h2cState() == H2cState::NotRunning, "J  no transmit helper at first");
        CHECK(s.L.ensureH2c() && s.L.h2cRunning(), "J  ensureH2c() starts iwfg_h2c on demand");
        s.startGen(s.chunk);
        CHECK(waitFor([&] { return s.L.h2cState() == H2cState::Streaming; }, 8000), "J  and it streams");
        bool got = false;
        const t::Spectrum sp = s.receivedTone(2u << 20, 10000, &got);
        CHECK(got && std::abs(sp.peakHz - s.expectedHz()) < kFs / double(1u << 18),
              "J  tone received %.6f MHz", sp.peakHz / 1e6);
    }

    // ---- unit checks ------------------------------------------------------
    {
        std::printf("\nParsing and policy\n");
        QString d;
        CHECK(sdr::classifyC2hExit({"Streaming: 10 MB/s", "IWFG_IOCTL_DMA_DATA (C2H): Input/output error",
                                    "Stopping stream...", "Cleanup complete."}, &d) == sdr::C2hExit::DriverError
              && d.contains("Input/output"), "c2h exit: driver error recognised");
        CHECK(sdr::classifyC2hExit({"FIFO reader disconnected, shutting down...", "Stopping stream..."}, nullptr)
              == sdr::C2hExit::ReaderClosed, "c2h exit: reader closed recognised");
        CHECK(sdr::classifyC2hExit({"Transfer interrupted by signal, shutting down..."}, nullptr)
              == sdr::C2hExit::Interrupted, "c2h exit: signal recognised");
        CHECK(sdr::classifyC2hExit({"open device: No such file or directory"}, nullptr)
              == sdr::C2hExit::StartupError, "c2h exit: start-up failure recognised (not restarted)");
        double m = 0;
        CHECK(sdr::parseC2hRateLine("Streaming: 2416.93 MB/s (total: 51234.00 MB)", &m) && std::abs(m - 2416.93) < 1e-9,
              "c2h rate line parsed: %.2f MiB/s", m);
        CHECK(sdr::isH2cStallLine("[H2C] stall kick 1 (reopen cycle 0) - retrying transfer")
              && sdr::isH2cStallLine("[H2C] DMA_DATA ETIMEDOUT (1/15) - card not consuming, retrying")
              && !sdr::isH2cStallLine("[H2C] 762.94 MB/s  total=100.0 MB  GR_updates=0/s"),
              "H2C stall lines recognised");
        CHECK(sdr::defaultH2cChunkBytes() == (1 << 20) && sdr::nextSmallerH2cChunk(1 << 20) == (256 << 10)
              && sdr::nextSmallerH2cChunk(16 << 10) == 0, "ladder: 1 MiB default -> 256 KiB -> 64 KiB -> 16 KiB -> stop");
        CHECK(sdr::captureChannelsWithinBudget(4, 200.0, 4) == 2 && sdr::captureChannelsWithinBudget(2, 200.0, 4) == 2
              && sdr::captureChannelsWithinBudget(1, 200.0, 4) == 1 && sdr::captureChannelsWithinBudget(4, 250.0, 4) == 1,
              "transmit guard: 4 ch x 200 MSPS -> 2 ch; 2 and 1 left alone; 4 x 250 MSPS -> 1 ch");
        CHECK(sdr::plGpioModeForChannels(1) == 1 && sdr::plGpioModeForChannels(2) == 2 && sdr::plGpioModeForChannels(4) == 3,
              "PL GPIO modes: 1 ch -> 1, 2 ch -> 2, 4 ch -> 3");
        int n = 0;
        const bool two = sdr::impliedCaptureChannels(1500.0, 200.0, 4, &n) && n == 2;
        const bool one = sdr::impliedCaptureChannels(760.0, 200.0, 4, &n) && n == 1;
        const bool four = sdr::impliedCaptureChannels(3040.0, 200.0, 4, &n) && n == 4;
        CHECK(two && one && four && !sdr::impliedCaptureChannels(2417.0, 200.0, 4, &n),
              "measured rate -> channels: 1500 -> 2, 760 -> 1, 3040 -> 4; an overloaded 2417 is not read as a count");
    }

    QDir(RIG).removeRecursively();
    std::printf("\nRESULT: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
