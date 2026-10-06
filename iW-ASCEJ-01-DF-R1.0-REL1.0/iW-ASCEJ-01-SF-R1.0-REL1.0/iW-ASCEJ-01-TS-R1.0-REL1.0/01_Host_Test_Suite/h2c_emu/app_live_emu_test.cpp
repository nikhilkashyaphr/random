// ---------------------------------------------------------------------------
// The WHOLE application in a live PCIe session, with only the card replaced
// by fake_iwfg.so (DAC looped back into the ADC). Everything else is real:
// MainWindow, BackendLauncher, c2h_stream, iwfg_h2c, DmaSource, DspEngine,
// the H2C control window and its generator.
//
// It drives the application the way the operator does — Start, open H2C
// CONTROL, Start generator, Verify loopback, Stop, Start — and checks:
//   * the tone generated comes back through the "ADC" and is verified;
//   * the Transmit tab shows the helper streaming and the capture rate;
//   * a capture helper that dies mid-session is restarted with no dialog,
//     and the loopback verifies again afterwards;
//   * a card that stops delivering C2H data is noticed within ~5 s and the
//     device is reset (ordered close, restart) with no dialog;
//   * the window never freezes: a 20 ms heartbeat runs throughout, and
//     Stop / Start / close return at once.
// ---------------------------------------------------------------------------
#include "MainWindow.h"
#include "ControlWindow3D.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <functional>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(c, ...) do { std::printf((c) ? "  PASS  " : "  FAIL  "); std::printf(__VA_ARGS__); \
                           std::printf("\n"); std::fflush(stdout); if (!(c)) ++g_fail; } while (0)

static QElapsedTimer g_beatClock;
static qint64 g_lastBeat = 0, g_maxGap = 0;

static void pump(int ms)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) { QApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(2); }
}
static bool waitFor(const std::function<bool()>& c, int ms)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) { if (c()) return true; QApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(2); }
    return c();
}
template <class T, class P> static T* find(QWidget& w, P p)
{ for (T* c : w.findChildren<T*>()) if (p(c)) return c; return nullptr; }

static QLabel* labelStarting(QWidget& w, const QString& s)
{ return find<QLabel>(w, [&](QLabel* l){ return l->text().startsWith(s); }); }
static QLabel* labelContaining(QWidget& w, const QString& s)
{ return find<QLabel>(w, [&](QLabel* l){ return l->text().contains(s); }); }

static QString verify(QWidget& cw)
{
    auto* btn = find<QPushButton>(cw, [](QPushButton* b){ return b->text().startsWith(QStringLiteral("Verify loopback")); });
    if (!btn) return QString();
    for (QLabel* l : cw.findChildren<QLabel*>())
        if (l->text().startsWith(QStringLiteral("✓")) || l->text().startsWith(QStringLiteral("✗"))) l->clear();
    btn->click();
    QElapsedTimer t; t.start();
    while (t.elapsed() < 20000) { pump(100); if (btn->isEnabled() && t.elapsed() > 500) break; }
    for (QLabel* l : cw.findChildren<QLabel*>())
        if (l->text().startsWith(QStringLiteral("✓")) || l->text().startsWith(QStringLiteral("✗")))
            return l->text();
    return QString();
}

static int visibleNotices(QWidget& w)
{
    int n = 0;
    for (QMessageBox* b : w.findChildren<QMessageBox*>()) if (b->isVisible()) ++n;
    return n;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    const QString shim = qEnvironmentVariable("FAKE_IWFG_SO");
    const QString c2hBin = qEnvironmentVariable("C2H_BIN"), h2cBin = qEnvironmentVariable("H2C_BIN");
    const QString rig = qEnvironmentVariable("TMPDIR", QStringLiteral("/tmp")) +
                        QStringLiteral("/app_live_emu_%1").arg(::getpid());
    QDir().mkpath(rig + "/backend/bin");
    { QFile mk(rig + "/backend/Makefile"); mk.open(QIODevice::WriteOnly); mk.write("all:\n"); }
    QFile::copy(c2hBin, rig + "/backend/bin/c2h_stream");
    QFile::copy(h2cBin, rig + "/backend/bin/iwfg_h2c");
    for (const char* n : {"/backend/bin/c2h_stream", "/backend/bin/iwfg_h2c"})
        QFile::setPermissions(rig + n, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    for (const char* n : {"/iwfg0", "/iwfg1"}) { QFile f(rig + n); f.open(QIODevice::WriteOnly); }
    QDir::setCurrent(rig);
    const QString marker = rig + "/c2h_failed_once";
    qputenv("LD_PRELOAD", shim.toLocal8Bit());
    qputenv("FAKE_IWFG_C2H_NODE", (rig + "/iwfg0").toLocal8Bit());
    qputenv("FAKE_IWFG_H2C_NODE", (rig + "/iwfg1").toLocal8Bit());
    qputenv("FAKE_IWFG_LOOP_FILE", (rig + "/dac.loop").toLocal8Bit());
    qputenv("FAKE_IWFG_C2H_CHANNELS", "1");
    qputenv("FAKE_IWFG_ADC_BPS", "100e6");
    qputenv("IWFG_C2H_DEVICE", (rig + "/iwfg0").toLocal8Bit());
    qputenv("IWFG_H2C_DEVICE", (rig + "/iwfg1").toLocal8Bit());
    qputenv("IWFG_C2H_FIFO", (rig + "/c2h.fifo").toLocal8Bit());
    qputenv("IWFG_H2C_FIFO", (rig + "/h2c.fifo").toLocal8Bit());
    qunsetenv("IWFG_H2C_CHUNK_BYTES");
    qputenv("TMPDIR", rig.toLocal8Bit());       // sdr_backend.log lands in the rig

    QApplication app(argc, argv);
    std::printf("Whole application, live PCIe session, card emulated (DAC looped into ADC)\n\n");
    if (!QFileInfo(shim).exists() || !QFileInfo(c2hBin).isExecutable() || !QFileInfo(h2cBin).isExecutable()) {
        std::printf("  FAIL  FAKE_IWFG_SO / C2H_BIN / H2C_BIN not set\n");
        return 2;
    }

    // Heartbeat: any freeze of the GUI thread shows up as a long gap.
    g_beatClock.start();
    QTimer beat;
    QObject::connect(&beat, &QTimer::timeout, [] {
        const qint64 now = g_beatClock.elapsed();
        g_maxGap = std::max(g_maxGap, now - g_lastBeat);
        g_lastBeat = now;
    });

    sdr::SystemConfig sys;
    sys.initial.src.mode = sdr::SourceMode::Dma;
    sys.initial.src.format = sdr::SampleFormat::Cs16;
    sys.initial.src.streamChannels = 1;
    sys.initial.acq.sampleRateMsps = 200.0;
    sys.dataFlowMode = 2;                         // C2H + H2C
    auto* w = new sdr::ui::MainWindow(sys);
    w->show();
    g_lastBeat = g_beatClock.elapsed();
    beat.start(20);

    // ---- Start (automatic at launch) ----------------------------------------
    QAction* act = nullptr;
    for (QAction* a : w->findChildren<QAction*>()) if (a->text() == QStringLiteral("H2C CONTROL")) act = a;
    CHECK(act != nullptr, "H2C CONTROL action present");
    if (!act) return 1;
    act->trigger();
    pump(200);
    auto* cw = w->findChild<sdr::ui::ControlWindow3D*>();
    CHECK(cw != nullptr, "control window opened");
    if (!cw) return 1;

    CHECK(waitFor([&] { return labelStarting(*cw, QStringLiteral("waiting for the generator")) != nullptr; }, 8000),
          "Transmit tab: H2C playback helper running, waiting for the generator");
    auto* fifoEdit = find<QLineEdit>(*cw, [&](QLineEdit* e){ return e->text().endsWith(QStringLiteral(".fifo")); });
    CHECK(fifoEdit && fifoEdit->text() == rig + "/h2c.fifo", "generator FIFO follows the backend's (%s)",
          fifoEdit ? qPrintable(fifoEdit->text()) : "?");

    // ---- Start generator ------------------------------------------------------
    auto* genBtn = find<QPushButton>(*cw, [](QPushButton* b){ return b->text() == QStringLiteral("Start generator"); });
    CHECK(genBtn != nullptr, "Start generator button");
    if (!genBtn) return 1;
    genBtn->click();
    CHECK(waitFor([&] { return labelStarting(*cw, QStringLiteral("streaming")) != nullptr; }, 10000),
          "H2C playback row: streaming");
    CHECK(waitFor([&] { return labelContaining(*cw, QStringLiteral("iwfg_h2c is moving")) != nullptr; }, 5000),
          "H2C to card row: iwfg_h2c's measured rate shown");
    CHECK(waitFor([&] { return labelContaining(*cw, QStringLiteral("delivered for")) != nullptr; }, 5000),
          "C2H capture row: delivered vs needed shown");
    CHECK(waitFor([&] { return labelContaining(*cw, QStringLiteral("matches the generator's frequency")) != nullptr; }, 8000),
          "Received row: the received tone matches the generator's frequency");
    if (!labelContaining(*cw, QStringLiteral("matches the generator's frequency")))
        for (QLabel* l : cw->findChildren<QLabel*>())
            if (l->text().contains(QStringLiteral("dBFS")) || l->text().contains(QStringLiteral("carrier")))
                std::printf("        (received row: %s)\n", qPrintable(l->text()));

    QString r = verify(*cw);
    CHECK(r.startsWith(QStringLiteral("✓ Loopback verified")), "Verify loopback: \"%s\"", qPrintable(r.left(110)));

    // ---- capture helper dies: recovered silently -------------------------------
    qputenv("FAKE_IWFG_C2H_FAIL_AFTER", "5");
    qputenv("FAKE_IWFG_C2H_FAIL_ONCE", marker.toLocal8Bit());
    // Only helpers launched from now on see the fault: restart the session.
    auto* stopAct = [&]{ for (QAction* a : w->findChildren<QAction*>()) if (a->text() == QStringLiteral("Stop")) return a; return (QAction*)nullptr; }();
    auto* startAct = [&]{ for (QAction* a : w->findChildren<QAction*>()) if (a->text() == QStringLiteral("Start")) return a; return (QAction*)nullptr; }();
    CHECK(stopAct && startAct, "Start/Stop actions present");
    if (!stopAct || !startAct) return 1;
    QElapsedTimer t; t.start();
    stopAct->trigger();
    const qint64 stopMs = t.elapsed();
    CHECK(stopMs < 200, "Stop returned in %lld ms", (long long)stopMs);
    pump(300);
    t.restart();
    startAct->trigger();                          // backend may still be exiting: queued
    const qint64 startMs = t.elapsed();
    CHECK(startMs < 300, "Start returned in %lld ms (helper pre-flight no longer freezes)", (long long)startMs);
    CHECK(waitFor([&] { return QFileInfo::exists(marker); }, 15000), "the injected C2H driver error fired");
    pump(4000);
    CHECK(visibleNotices(*w) == 0, "no dialog for a recovered capture helper");
    CHECK(waitFor([&] { return labelStarting(*cw, QStringLiteral("streaming")) != nullptr; }, 10000),
          "transmit streaming after the restart");
    r = verify(*cw);
    CHECK(r.startsWith(QStringLiteral("✓ Loopback verified")), "Verify loopback after the recovery: \"%s\"",
          qPrintable(r.left(110)));
    qunsetenv("FAKE_IWFG_C2H_FAIL_AFTER");

    // ---- card stops delivering C2H (wedged): watchdog resets the device ----------
    const QString stallMarker = rig + "/c2h_stalled_once";
    qputenv("FAKE_IWFG_C2H_STALL_AFTER", "5");
    qputenv("FAKE_IWFG_C2H_STALL_ONCE", stallMarker.toLocal8Bit());
    stopAct->trigger();
    pump(300);
    startAct->trigger();
    CHECK(waitFor([&] { return QFileInfo::exists(stallMarker); }, 15000),
          "injected: the card stops delivering C2H data (c2h_stream waits in the driver)");
    auto logHas = [&](const char* needle) {
        QFile f(rig + "/sdr_backend.log");
        return f.open(QIODevice::ReadOnly) && f.readAll().contains(needle);
    };
    CHECK(waitFor([&] { return logHas("Resetting the DMA device"); }, 15000),
          "after ~5 s with no capture data the GUI resets the device by itself");
    CHECK(waitFor([&] { return logHas("DMA device reset finished"); }, 15000),
          "the reset finished: transmit closed first, capture after, both restarted");
    qunsetenv("FAKE_IWFG_C2H_STALL_AFTER");
    CHECK(waitFor([&] { return labelStarting(*cw, QStringLiteral("streaming")) != nullptr; }, 10000),
          "transmit streaming after the reset");
    pump(1500);
    CHECK(visibleNotices(*w) == 0, "no dialog for a recovered device");
    r = verify(*cw);
    CHECK(r.startsWith(QStringLiteral("✓ Loopback verified")), "Verify loopback after the reset: \"%s\"",
          qPrintable(r.left(110)));

    // ---- log file ---------------------------------------------------------------
    QFile log(rig + "/sdr_backend.log");
    const bool haveLog = log.open(QIODevice::ReadOnly);
    const QByteArray logText = haveLog ? log.readAll() : QByteArray();
    CHECK(haveLog && logText.contains("DMA driver failed a C2H transfer"),
          "sdr_backend.log records the helper output and the recovery reason");

    // ---- close ------------------------------------------------------------------
    genBtn->click();
    pump(300);
    beat.stop();
    CHECK(g_maxGap < 1000, "the GUI thread never froze (longest heartbeat gap %lld ms)", (long long)g_maxGap);
    t.restart();
    delete w;
    CHECK(t.elapsed() < 4000, "closing the window took %lld ms", (long long)t.elapsed());

    QDir(rig).removeRecursively();
    std::printf("\nRESULT: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
