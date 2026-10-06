// ---------------------------------------------------------------------------
// The whole application in Simulation: the real MainWindow, its DSP chain and
// the simulated RF loopback, driven through the real H2C window. Proves the
// end-to-end path the bench depends on — generator -> "DAC" -> "ADC" ->
// spectrum -> loopback check — and that the check tells DDS from generator.
// ---------------------------------------------------------------------------
#include "MainWindow.h"
#include "ControlWindow3D.h"
#include "pcie_regs.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include <QThread>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c, ...) do { std::printf((c) ? "  PASS  " : "  FAIL  "); std::printf(__VA_ARGS__); \
                           std::printf("\n"); if (!(c)) ++g_fail; } while (0)

static void pump(int ms)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) { QApplication::processEvents(); QThread::msleep(5); }
}
template <class T, class P> static T* find(QWidget& w, P p)
{ for (T* c : w.findChildren<T*>()) if (p(c)) return c; return nullptr; }

static QString verify(QWidget& cw)
{
    auto* btn = find<QPushButton>(cw, [](QPushButton* b){ return b->text().startsWith(QStringLiteral("Verify loopback")); });
    if (!btn) return QString();
    btn->click();
    QElapsedTimer t; t.start();
    while (t.elapsed() < 15000) { pump(100); if (btn->isEnabled() && t.elapsed() > 500) break; }
    for (QLabel* l : cw.findChildren<QLabel*>())
        if (l->text().startsWith(QStringLiteral("✓")) || l->text().startsWith(QStringLiteral("✗")))
            return l->text();
    return QString();
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    std::printf("Whole application, Simulation: generator -> loopback -> spectrum -> check\n\n");

    sdr::SystemConfig cfg;
    cfg.initial.src.mode = sdr::SourceMode::Simulated;
    cfg.initial.src.streamChannels = 1;
    auto* w = new sdr::ui::MainWindow(cfg);
    w->show();
    pump(800);

    QAction* act = nullptr;
    for (QAction* a : w->findChildren<QAction*>()) if (a->text() == QStringLiteral("H2C CONTROL")) act = a;
    CHECK(act != nullptr, "H2C CONTROL action present");
    if (!act) return 1;
    act->trigger();
    pump(300);
    auto* cw = w->findChild<sdr::ui::ControlWindow3D*>();
    CHECK(cw != nullptr, "control window opened");
    if (!cw) return 1;

    auto* dac = find<QComboBox>(*cw, [](QComboBox* c){ return c->findText(QStringLiteral("DDS compiler")) >= 0; });
    dac->setCurrentIndex(dac->findData(int(PCIE_DAC_SRC_HOST)));
    emit dac->activated(dac->currentIndex());
    pump(1500);

    QString r = verify(*cw);
    CHECK(r.startsWith(QStringLiteral("✓ Loopback verified")),
          "Host + generator: the received tone follows it -> \"%s\"", qPrintable(r.left(100)));

    dac->setCurrentIndex(dac->findData(int(PCIE_DAC_SRC_DDS)));
    emit dac->activated(dac->currentIndex());
    pump(1500);
    r = verify(*cw);
    CHECK(r.contains(QStringLiteral("did NOT move")) && r.contains(QStringLiteral("DDS compiler")),
          "DAC on the DDS: the check says the ADC is not seeing the generator, and why");

    delete w;
    std::printf("\nRESULT: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
