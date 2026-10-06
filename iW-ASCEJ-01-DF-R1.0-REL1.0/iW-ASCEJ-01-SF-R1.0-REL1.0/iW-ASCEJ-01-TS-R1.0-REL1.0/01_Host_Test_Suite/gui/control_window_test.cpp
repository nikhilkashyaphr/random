// ---------------------------------------------------------------------------
// The H2C / RFDC control window, driven as a user would (offscreen):
// simulation mode keeps the transmit controls usable with no device, every
// change is published as a TxState, and the generator reports the frequency
// it really transmits. Also checks that live mode without a device never
// pretends to route the DAC.
// ---------------------------------------------------------------------------
#include "ControlWindow3D.h"
#include "pcie_regs.h"
#include "BackendLauncher.h"
#include "IqGenerator.h"

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLineEdit>
#include <QThread>
#include <QTimer>
#include <QElapsedTimer>
#include <cstdio>
#include <unistd.h>

using namespace sdr;

static int g_fail = 0;
#define CHECK(c, ...) do { std::printf((c) ? "  PASS  " : "  FAIL  "); std::printf(__VA_ARGS__); \
                           std::printf("\n"); if (!(c)) ++g_fail; } while (0)

template <class T, class Pred> static T* find(QWidget& w, Pred p)
{
    for (T* c : w.findChildren<T*>()) if (p(c)) return c;
    return nullptr;
}

static void pump(int ms)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) { QApplication::processEvents(); QThread::msleep(5); }
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    qRegisterMetaType<sdr::TxState>("sdr::TxState");
    std::printf("Control window (offscreen)\n\n");

    ui::ControlWindow3D w;
    TxState last; int published = 0;
    QObject::connect(&w, &ui::ControlWindow3D::txStateChanged,
                     [&](const TxState& t){ last = t; ++published; });

    auto* dac = find<QComboBox>(w, [](QComboBox* c){ return c->findText(QStringLiteral("DDS compiler")) >= 0; });
    auto* genBtn = find<QPushButton>(w, [](QPushButton* b){ return b->text() == QStringLiteral("Start generator"); });
    auto* tone = find<QDoubleSpinBox>(w, [](QDoubleSpinBox* s){ return s->suffix() == QStringLiteral(" MHz") && s->decimals() == 6; });
    auto* fifo = find<QLineEdit>(w, [](QLineEdit* e){ return e->text().endsWith(QStringLiteral("iwfg_h2c.fifo")); });
    auto* logBox = find<QPlainTextEdit>(w, [](QPlainTextEdit* e){ return e->objectName() == QStringLiteral("controlLog"); });
    auto* simBox = find<QGroupBox>(w, [](QGroupBox* g){ return g->title().startsWith(QStringLiteral("Simulation")); });
    CHECK(dac && genBtn && tone && fifo && logBox && simBox, "widgets found");
    if (!(dac && genBtn && tone && fifo && logBox && simBox)) return 1;

    CHECK(dac->findData(int(PCIE_DAC_SRC_DDS)) == dac->findText(QStringLiteral("DDS compiler"))
          && dac->findData(int(PCIE_DAC_SRC_HOST)) == dac->findText(QStringLiteral("Host / GNU Radio stream")),
          "combo items carry the logical PCIE_DAC_SRC_* values");

    // ---- simulation -------------------------------------------------------
    w.setSimulationMode(true);
    CHECK(!simBox->isHidden(), "simulation box shown in simulation mode");
    CHECK(dac->isEnabled(), "DAC input source usable with no device in simulation");

    dac->setCurrentIndex(dac->findData(int(PCIE_DAC_SRC_HOST)));
    emit dac->activated(dac->currentIndex());
    CHECK(last.loopback && last.dacSource == int(PCIE_DAC_SRC_HOST) && !last.hostRunning,
          "select Host: loopback on, host source, nothing transmitting yet");

    genBtn->click();
    CHECK(last.hostRunning && genBtn->text() == QStringLiteral("Stop generator"),
          "Start generator (simulated): host running");

    tone->setValue(10.0);
    tone->setValue(12.0); tone->setValue(10.0);      // commit a change
    sdr::IqGenerator::Config ref;
    ref.sampleRateSps = 200e6; ref.frequencyHz = 10e6; ref.chunkBytes = sdr::defaultH2cChunkBytes();
    const double resHz = 200e6 / (sdr::defaultH2cChunkBytes() / 4.0);
    CHECK(std::abs(last.hostToneHz - sdr::IqGenerator::transmittedHz(ref)) < 1e-3
          && std::abs(last.hostToneHz - 10e6) <= resHz / 2,
          "tone 10 MHz -> published %.6f MHz with the %d KiB loop (what is really sent, within %.0f Hz)",
          last.hostToneHz / 1e6, sdr::defaultH2cChunkBytes() / 1024, resHz / 2);
    CHECK(last.loopPairs == sdr::defaultH2cChunkBytes() / 4, "loop length = H2C buffer / 4 (%d samples)", last.loopPairs);

    w.setH2cChunkBytes(16384);                       // backend stepped down
    CHECK(last.loopPairs == 4096 && std::abs(last.hostToneHz - 10.009765625e6) < 1.0,
          "backend on 16 KiB: loop follows (%d samples, %.6f MHz)", last.loopPairs, last.hostToneHz / 1e6);
    w.setH2cChunkBytes(sdr::defaultH2cChunkBytes());

    auto* link = find<QLabel>(w, [](QLabel* l){ return l->text().startsWith(QStringLiteral("H2C link")); });
    CHECK(link != nullptr, "H2C link monitor present");
    if (link) {
        w.setH2cThroughput(640.58, 48);              // the bench reading
        CHECK(link->text().contains(QStringLiteral("runs dry")) && link->text().contains(QStringLiteral("762.9")),
              "640.58 MiB/s flagged: \"%s\"", qPrintable(link->text().left(90)));
        w.setH2cThroughput(762.9, 0);
        CHECK(link->text().contains(QStringLiteral("kept full")), "762.9 MiB/s reported as keeping the DAC full");
    }

    dac->setCurrentIndex(dac->findData(int(PCIE_DAC_SRC_DDS)));
    emit dac->activated(dac->currentIndex());
    CHECK(last.dacSource == int(PCIE_DAC_SRC_DDS), "select DDS: published");

    genBtn->click();
    CHECK(!last.hostRunning && genBtn->text() == QStringLiteral("Start generator"), "Stop generator (simulated)");

    genBtn->click();                                     // running again, DAC on DDS
    CHECK(last.hostRunning && last.dacSource == int(PCIE_DAC_SRC_HOST),
          "starting the generator in simulation routes the DAC to Host");

    w.setSimulationMode(false);
    CHECK(!last.hostRunning && !last.loopback, "leaving simulation stops the modelled generator");
    CHECK(simBox->isHidden(), "simulation box hidden in live mode");
    CHECK(!dac->isEnabled(), "live, no device: DAC input source disabled (nothing to send to)");

    // ---- live transmit chain + loopback verification (fake receiver) -------
    // A receiver stand-in that behaves like the RF loopback: it reports the
    // transmitted tone, spectrum-inverted, while the DAC takes the host stream
    // and the generator runs; otherwise the DDS. `scale` mimics a display
    // running at the wrong sample rate.
    {
        w.setSimulationMode(true);
        auto* rdma = find<QGroupBox>(w, [](QGroupBox* g){ return g->title().startsWith(QStringLiteral("RDMA TRANSMITTER")); });
        CHECK(rdma && rdma->isHidden(), "rdma_tx control block hidden outside RoCEv2");
        w.setRdmaControlVisible(true);
        CHECK(rdma && !rdma->isHidden(), "  ...and shown for RoCEv2");
        w.setRdmaControlVisible(false);

        auto* verify = find<QPushButton>(w, [](QPushButton* b){ return b->text().startsWith(QStringLiteral("Verify loopback")); });
        CHECK(verify != nullptr, "Verify loopback button present");
        auto* dds = find<QDoubleSpinBox>(w, [](QDoubleSpinBox* s){ return s->suffix() == QStringLiteral(" MHz") && s->maximum() == 75.0; });

        enum class Rx { Loopback, DdsOnly, Scaled };
        Rx mode = Rx::Loopback;
        QTimer rx;
        QObject::connect(&rx, &QTimer::timeout, [&]{
            const bool host = last.dacSource == int(PCIE_DAC_SRC_HOST) && last.hostRunning;
            double f = host ? -last.hostToneHz : (dds ? dds->value() * 1e6 : 10e6);
            if (mode == Rx::DdsOnly) f = dds ? dds->value() * 1e6 : 10e6;
            if (mode == Rx::Scaled && host) f *= 1.25;
            w.setReceivedPeak(f, -40.0, true, 200e6, 200e6 / 2048);
        });
        rx.start(100);
        pump(400);                                        // receiver feed is live
        auto runVerify = [&]{
            auto* res = find<QLabel>(w, [](QLabel* l){ return l->text().startsWith(QStringLiteral("Checking")); });
            Q_UNUSED(res);
            verify->click();
            QElapsedTimer t; t.start();
            while (t.elapsed() < 12000) {
                pump(100);
                if (verify->isEnabled() && t.elapsed() > 500) break;
            }
            for (QLabel* l : w.findChildren<QLabel*>())
                if (l->text().startsWith(QStringLiteral("✓")) || l->text().startsWith(QStringLiteral("✗")))
                    return l->text();
            return QString();
        };

        dac->setCurrentIndex(dac->findData(int(PCIE_DAC_SRC_HOST))); emit dac->activated(dac->currentIndex());
        const double toneBefore = tone->value();
        QString r = runVerify();
        CHECK(r.startsWith(QStringLiteral("✓ Loopback verified")) && r.contains(QStringLiteral("inverts")),
              "loopback follows the generator -> verified (inversion reported)");
        CHECK(std::abs(tone->value() - toneBefore) < 1e-9, "tone restored after the check (%.6f MHz)", tone->value());

        pump(300);
        auto* rxRow = find<QLabel>(w, [](QLabel* l){ return l->text().contains(QStringLiteral("the generator's frequency")); });
        if (!rxRow) for (QLabel* l : w.findChildren<QLabel*>())
            if (l->text().contains(QStringLiteral("dBFS"))) std::printf("        (received row: %s)\n", qPrintable(l->text()));
        CHECK(rxRow != nullptr, "Received row: identifies the generator's tone");

        mode = Rx::DdsOnly;
        dac->setCurrentIndex(dac->findData(int(PCIE_DAC_SRC_DDS))); emit dac->activated(dac->currentIndex());
        r = runVerify();
        CHECK(r.contains(QStringLiteral("did NOT move")) && r.contains(QStringLiteral("DDS")),
              "receiver stuck on the DDS -> NOT verified, cause named: \"%s\"", qPrintable(r.left(80)));

        mode = Rx::Scaled;
        dac->setCurrentIndex(dac->findData(int(PCIE_DAC_SRC_HOST))); emit dac->activated(dac->currentIndex());
        r = runVerify();
        CHECK(r.contains(QStringLiteral("sample rate does not match")),
              "display at the wrong rate -> reported as scaling, not as a transmit failure");
        rx.stop();
        genBtn->click();                                  // stop the simulated generator
        w.setSimulationMode(false);
    }

    // ---- live mode, no device: generator writes the FIFO, says it waits ----
    const QString path = QStringLiteral("%1/cw_test_fifo_%2")
                             .arg(qEnvironmentVariable("TMPDIR", QStringLiteral("/tmp"))).arg(::getpid());
    fifo->setText(path);
    logBox->clear();
    genBtn->click();
    pump(400);
    const QString log = logBox->toPlainText();
    CHECK(log.contains(QStringLiteral("RFDC control is not attached")),
          "live, not attached: says the DAC input could not be set (does not pretend)");
    auto* state = find<QLabel>(w, [](QLabel* l){ return l->text().startsWith(QStringLiteral("Waiting for iwfg_h2c")); });
    CHECK(state != nullptr, "live, no reader: state is \"Waiting for iwfg_h2c…\", not \"Running\"");
    genBtn->click();                                     // stop
    pump(600);
    CHECK(genBtn->text() == QStringLiteral("Start generator"), "stopped cleanly");
    ::unlink(path.toLocal8Bit().constData());

    // ---- 1.2.4: the helpers behind the chain, and the recovery actions ----
    {
        std::printf("\n  helper state and recovery actions\n");
        auto* retry = find<QPushButton>(w, [](QPushButton* b){ return b->text() == QStringLiteral("Retry transmit"); });
        auto* oneCh = find<QPushButton>(w, [](QPushButton* b){ return b->text().startsWith(QStringLiteral("Capture 1 channel")); });
        CHECK(retry && oneCh, "Retry transmit and Capture 1 channel buttons exist");
        if (retry && oneCh) {
            w.setH2cHelperState(int(H2cState::NotRunning), QStringLiteral("backend not running — press Start"), false);
            CHECK(retry->isHidden(), "no backend: Retry transmit hidden");
            w.setH2cHelperState(int(H2cState::Stalled), QStringLiteral("the card does not take H2C data"), true);
            auto* row = find<QLabel>(w, [](QLabel* l){ return l->text().startsWith(QStringLiteral("stalled — the card does not take")); });
            CHECK(row != nullptr, "H2C playback row shows the helper state and why");
            CHECK(!retry->isHidden() && retry->isEnabled(), "backend running: Retry transmit offered");
            int asked = 0;
            QObject::connect(&w, &ui::ControlWindow3D::h2cRestartRequested, [&]{ ++asked; });
            retry->click();
            CHECK(asked == 1, "Retry transmit asks the main window to relaunch iwfg_h2c");

            w.setCaptureRate(2417.0, 3051.8, 4, 200.0, 4);
            auto* c2h = find<QLabel>(w, [](QLabel* l){ return l->text().contains(QStringLiteral("OVERLOADED")); });
            CHECK(c2h != nullptr, "C2H row: 2417 of 3052 MiB/s -> OVERLOADED, says it interrupts H2C");
            CHECK(!oneCh->isHidden() && !oneCh->isEnabled(), "Capture 1 channel offered, disabled until RFDC attached");
            int chans = 0;
            QObject::connect(&w, &ui::ControlWindow3D::captureChannelsRequested, [&](int n){ chans = n; });
            logBox->clear();
            QMetaObject::invokeMethod(oneCh, "click");
            pump(50);
            CHECK(chans == 0, "not attached: channel count left alone (PL GPIO cannot be set)");
            w.setCaptureRate(760.0, 762.9, 1, 200.0, 4);
            CHECK(oneCh->isHidden(), "1 channel, 760 of 763 MiB/s: no overload, button hidden");
            w.setCaptureRate(0.0, 762.9, 1, 200.0, 4);
            CHECK(find<QLabel>(w, [](QLabel* l){ return l->text() == QStringLiteral("not capturing"); }) != nullptr,
                  "not live: C2H row says so");
        }
    }

    std::printf("\n  (%d TxState updates published)\nRESULT: %s\n", published, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
