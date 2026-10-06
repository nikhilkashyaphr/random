// ---------------------------------------------------------------------------
// The REAL iwfg_h2c (built from backend/H2C) driven by the REAL IqGenerator,
// with the card replaced by fake_iwfg.so. Whatever iwfg_h2c hands to the
// "card" is captured: that is exactly what the DAC would play.
//
// Checks, per the bench report (tone with broadband junk, H2C 640 MiB/s):
//   * the 16 MiB buffer keeps a DAC that reads 763 MiB/s fed, while 16 KiB
//     chunks behind the same per-transfer cost cannot;
//   * the DAC stream is one seamless loop — a pure tone at the reported
//     frequency, even across loop boundaries;
//   * the loop is written once: iwfg_h2c reports GR_updates=0/s afterwards;
//   * a parameter change reaches the DAC, and stop leaves it silent;
//   * a buffer the driver refuses makes iwfg_h2c exit before any rate line,
//     which is the condition the launcher steps down on.
// ---------------------------------------------------------------------------
#include "BackendLauncher.h"
#include "IqGenerator.h"
#include "spectrum.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QThread>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
#include <csignal>
#include <atomic>

using sdr::IqGenerator;
using sdr::cf32;

static int g_fail = 0;
#define CHECK(c, ...) do { std::printf((c) ? "  PASS  " : "  FAIL  "); std::printf(__VA_ARGS__); \
                           std::printf("\n"); if (!(c)) ++g_fail; } while (0)

static QString H2C_BIN, SHIM, TMP;
static constexpr double kFs = 200e6;                 // DAC stream clock
static constexpr double kDacMiB = kFs * 4.0 / 1048576.0;

struct Rig {
    QProcess h2c;
    QThread  thread;
    IqGenerator* gen = new IqGenerator;
    QString fifo, cap;
    QStringList lines;
    std::atomic<int> stops{0};

    Rig(const QString& tag, int chunk, const QStringList& env)
    {
        fifo = TMP + QStringLiteral("/h2c_%1.fifo").arg(tag);
        cap  = TMP + QStringLiteral("/h2c_%1.cap").arg(tag);
        ::unlink(fifo.toLocal8Bit().constData());
        QFile::remove(cap);
        ::mkfifo(fifo.toLocal8Bit().constData(), 0666);
        QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
        e.insert(QStringLiteral("LD_PRELOAD"), SHIM);
        e.insert(QStringLiteral("FAKE_IWFG_CAPTURE"), cap);
        for (const QString& kv : env) e.insert(kv.section('=', 0, 0), kv.section('=', 1));
        h2c.setProcessEnvironment(e);
        h2c.setProcessChannelMode(QProcess::MergedChannels);
        QObject::connect(&h2c, &QProcess::readyRead, [this]{
            for (const QByteArray& l : h2c.readAll().split('\n'))
                if (!l.trimmed().isEmpty()) lines << QString::fromLocal8Bit(l.trimmed());
        });
        h2c.start(H2C_BIN, {QStringLiteral("/dev/iwfg1"), fifo, QString::number(chunk)});
        h2c.waitForStarted(3000);
        gen->moveToThread(&thread);
        QObject::connect(gen, &IqGenerator::stopped, gen, [this]{ stops++; }, Qt::DirectConnection);
        thread.start();
    }
    void start(const IqGenerator::Config& c)
    { QMetaObject::invokeMethod(gen, [this, c]{ gen->start(c); }, Qt::QueuedConnection); }
    void stop()
    {
        const int s = stops.load();
        gen->stop();
        QElapsedTimer t; t.start();
        while (stops.load() == s && t.elapsed() < 4000) pump(10);
    }
    void pump(int ms)
    {
        QElapsedTimer t; t.start();
        while (t.elapsed() < ms) { QCoreApplication::processEvents(); QThread::msleep(2); }
    }
    bool waitCapture(qint64 bytes, int ms)
    {
        QElapsedTimer t; t.start();
        while (t.elapsed() < ms) {
            pump(20);
            if (QFileInfo(cap).size() >= bytes) { pump(50); return true; }
        }
        return false;
    }
    // Rate lines iwfg_h2c printed: MiB/s and GR_updates/s.
    QVector<QPair<double,int>> rates() const
    {
        QVector<QPair<double,int>> r;
        for (const QString& l : lines) { double m; int u; if (sdr::parseH2cRateLine(l, &m, &u)) r.push_back({m, u}); }
        return r;
    }
    ~Rig()
    {
        gen->stop(); thread.quit(); thread.wait(3000); delete gen;
        if (h2c.state() != QProcess::NotRunning) {
            ::kill(pid_t(h2c.processId()), SIGINT);
            if (!h2c.waitForFinished(3000)) { h2c.kill(); h2c.waitForFinished(1000); }
        }
        ::unlink(fifo.toLocal8Bit().constData());
        QFile::remove(cap);
    }
};

static std::vector<cf32> readCapture(const QString& path, std::size_t samples, std::size_t skip = 0)
{
    QFile f(path);
    std::vector<cf32> x;
    if (!f.open(QIODevice::ReadOnly)) return x;
    const QByteArray all = f.readAll();
    const auto* w = reinterpret_cast<const qint16*>(all.constData());
    const std::size_t n = std::size_t(all.size()) / 4;
    for (std::size_t i = skip; i < n && x.size() < samples; ++i)
        x.emplace_back(float(w[2 * i]) / 32764.0f, float(w[2 * i + 1]) / 32764.0f);
    return x;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    H2C_BIN = QString::fromLocal8Bit(qgetenv("H2C_BIN"));
    SHIM    = QString::fromLocal8Bit(qgetenv("FAKE_IWFG_SO"));
    TMP     = qEnvironmentVariable("TMPDIR", QStringLiteral("/tmp"));
    std::printf("Real iwfg_h2c + IqGenerator, card replaced by fake_iwfg.so\n\n");
    if (!QFileInfo(H2C_BIN).isExecutable() || !QFileInfo(SHIM).exists()) {
        std::printf("  FAIL  H2C_BIN / FAKE_IWFG_SO not set\n"); return 2;
    }

    // 1. Throughput: 16 KiB vs 16 MiB behind the same per-transfer cost -----
    const QString overhead = QStringLiteral("FAKE_IWFG_OVERHEAD_US=5");
    double small = 0, large = 0, mid = 0;
    for (int chunk : {16384, 1 << 20, 16 << 20}) {
        Rig r(QStringLiteral("rate%1").arg(chunk), chunk, {overhead, QStringLiteral("FAKE_IWFG_CAPTURE_BYTES=0")});
        IqGenerator::Config c; c.fifoPath = r.fifo; c.sampleRateSps = kFs; c.frequencyHz = 10e6; c.chunkBytes = chunk;
        r.start(c);
        QElapsedTimer t; t.start();
        while (r.rates().size() < 3 && t.elapsed() < 8000) r.pump(50);
        const auto rs = r.rates();
        double m = rs.isEmpty() ? 0 : rs.last().first;
        (chunk == 16384 ? small : chunk == (1 << 20) ? mid : large) = m;
        r.stop();
    }
    CHECK(small > 0 && small < 0.98 * kDacMiB,
          "16 KiB chunks, 5 us per transfer: %.1f MiB/s < %.1f MiB/s the DAC reads — starved (the bench symptom)", small, kDacMiB);
    CHECK(mid >= 0.98 * kDacMiB,
          "1 MiB chunks (the 1.2.4 default), same cost: %.1f MiB/s — the DAC is kept fed", mid);
    CHECK(large >= 0.98 * kDacMiB,
          "16 MiB chunks, same cost: %.1f MiB/s", large);

    // 2. Purity and write-once, 16 MiB --------------------------------------
    {
        const int chunk = 16 << 20;
        Rig r(QStringLiteral("pure"), chunk, {QStringLiteral("FAKE_IWFG_CAPTURE_BYTES=%1").arg(3 * chunk)});
        IqGenerator::Config c; c.fifoPath = r.fifo; c.sampleRateSps = kFs; c.frequencyHz = 10e6; c.chunkBytes = chunk;
        const double act = IqGenerator::transmittedHz(c);
        QElapsedTimer gen; gen.start();
        r.start(c);
        const bool got = r.waitCapture(3LL * chunk, 15000);
        CHECK(got, "DAC stream captured (3 loops, %lld ms from start incl. building the loop)", (long long)gen.elapsed());
        // Straddle a loop boundary: 1 M samples centred on the first seam.
        const std::size_t seam = std::size_t(chunk / 4);
        auto x = readCapture(r.cap, 1u << 20, seam - (1u << 19));
        auto s = t::analyse(x, kFs);
        CHECK(std::abs(s.peakHz - act) < kFs / double(x.size()), "tone at %.6f MHz (requested 10 MHz, transmitted %.6f)", s.peakHz / 1e6, act / 1e6);
        CHECK(s.sfdrDb > 80.0, "across a loop seam: SFDR %.1f dB — seamless, no interference", s.sfdrDb);
        CHECK(std::abs(act - 10e6) < 30.0, "16 MiB loop: 10 MHz lands within %.1f Hz", std::abs(act - 10e6));
        r.pump(2500);
        const auto rs = r.rates();
        CHECK(!rs.isEmpty() && rs.last().second == 0,
              "written once: iwfg_h2c reports GR_updates=%d/s after loading", rs.isEmpty() ? -1 : rs.last().second);
        r.stop();
    }

    // 3. Parameter change reaches the DAC ------------------------------------
    {
        const int chunk = 4 << 20;
        Rig r(QStringLiteral("upd"), chunk, {QStringLiteral("FAKE_IWFG_CAPTURE_AFTER=1"),
                                             QStringLiteral("FAKE_IWFG_CAPTURE_BYTES=%1").arg(2 * chunk)});
        IqGenerator::Config c; c.fifoPath = r.fifo; c.sampleRateSps = kFs; c.frequencyHz = 10e6; c.chunkBytes = chunk;
        r.start(c);
        r.pump(1500);
        c.frequencyHz = 23.45e6;
        r.gen->requestUpdate(c);
        const bool got = r.waitCapture(2LL * chunk, 10000);
        auto s = t::analyse(readCapture(r.cap, 1u << 20), kFs);
        CHECK(got && std::abs(s.peakHz - IqGenerator::transmittedHz(c)) < kFs / double(1u << 20),
              "tone changed to %.6f MHz while running", s.peakHz / 1e6);
        CHECK(s.sfdrDb > 80.0, "  ...and is pure (SFDR %.1f dB)", s.sfdrDb);
        r.stop();
    }

    // 4. Stop leaves the DAC silent -------------------------------------------
    {
        const int chunk = 4 << 20;
        Rig r(QStringLiteral("stop"), chunk, {QStringLiteral("FAKE_IWFG_CAPTURE_AFTER=1"),
                                              QStringLiteral("FAKE_IWFG_CAPTURE_BYTES=%1").arg(chunk)});
        IqGenerator::Config c; c.fifoPath = r.fifo; c.sampleRateSps = kFs; c.frequencyHz = 10e6; c.chunkBytes = chunk;
        r.start(c);
        r.pump(1500);
        r.stop();
        const bool got = r.waitCapture(chunk, 8000);
        auto x = readCapture(r.cap, std::size_t(chunk / 4));
        double peak = 0; for (auto& v : x) peak = std::max(peak, double(std::abs(v)));
        CHECK(got && peak == 0.0, "after Stop the card replays silence (peak %.6f)", peak);
    }

    // 5. A buffer the driver refuses: exit before any rate line --------------
    {
        const int chunk = 16 << 20;
        Rig r(QStringLiteral("refuse"), chunk, {QStringLiteral("FAKE_IWFG_MAX_BUF=4194304")});
        r.h2c.waitForFinished(5000);
        r.pump(100);
        CHECK(r.h2c.state() == QProcess::NotRunning && r.rates().isEmpty(),
              "driver refuses 16 MiB: iwfg_h2c exits (code %d) without a rate line — launcher steps down",
              r.h2c.exitCode());
        CHECK(sdr::nextSmallerH2cChunk(16 << 20) == (4 << 20) && sdr::nextSmallerH2cChunk(16 << 10) == 0,
              "step-down ladder: 16 MiB -> 4 MiB -> ... -> 16 KiB -> stop");
    }

    // 6. Parsing and the environment override ---------------------------------
    {
        double m = 0; int u = -1;
        const bool ok = sdr::parseH2cRateLine(QStringLiteral("[H2C] 640.58 MB/s  total=17934.0 MB  GR_updates=48/s"), &m, &u);
        CHECK(ok && std::abs(m - 640.58) < 1e-9 && u == 48, "parses the bench line: %.2f MiB/s, %d updates/s", m, u);
        CHECK(!sdr::parseH2cRateLine(QStringLiteral("[H2C] Waiting for GNU Radio"), nullptr, nullptr), "ignores other lines");
        CHECK(sdr::defaultH2cChunkBytes() == (1 << 20), "default H2C buffer is 1 MiB");
        qputenv("IWFG_H2C_CHUNK_BYTES", "16777216");
        CHECK(sdr::defaultH2cChunkBytes() == (16 << 20), "IWFG_H2C_CHUNK_BYTES overrides it");
        qputenv("IWFG_H2C_CHUNK_BYTES", "12345");
        CHECK(sdr::defaultH2cChunkBytes() == (1 << 20), "an invalid override is ignored");
        qunsetenv("IWFG_H2C_CHUNK_BYTES");
    }

    std::printf("\nRESULT: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
