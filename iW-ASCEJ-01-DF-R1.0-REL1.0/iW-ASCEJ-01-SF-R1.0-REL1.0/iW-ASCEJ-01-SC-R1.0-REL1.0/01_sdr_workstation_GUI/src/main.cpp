#include "core/GpuNetProbe.h"
#include "core/RoceShmSource.h"
#include <csignal>
#include "ui/ConfigDialog.h"
#include "ui/MainWindow.h"
#include "ui/PcieDiscoveryDialog.h"

#include <QApplication>
#include <QIcon>
#include <QSize>
#include <QGuiApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QTimer>

namespace {

/// Dev/CI helper: render a widget after it has settled, write it to disk and
/// quit. Keeps regression screenshots reproducible from a headless runner.
void scheduleCapture(QWidget* w, const QString& path, int delayMs)
{
    QTimer::singleShot(delayMs, w, [w, path] {
        const bool ok = w->grab().save(path);
        qInfo("capture %s -> %s", ok ? "ok" : "FAILED", qUtf8Printable(path));
        QCoreApplication::quit();
    });
}

} // namespace

int main(int argc, char** argv)
{
    // High-DPI support must be configured BEFORE the QApplication is
    // constructed — set afterwards these attributes are silently ignored,
    // which is a common cause of clipped and mis-scaled text on 4K and
    // fractional-scaling displays. Qt 6 enables scaling unconditionally and
    // deprecates the enable flag, so it is guarded by version.
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling, true);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, true);
#endif
    // Round fractional scale factors up rather than truncating: truncation is
    // what turns a 1.5x desktop into widgets sized for 1.0x while the font is
    // still rendered at 1.5x, so labels overflow their boxes.
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    // The workstation writes to FIFOs (H2C generator, recorder) whose readers
    // are separate processes. When a reader exits, the kernel sends SIGPIPE to
    // the writer, and its default action terminates the whole GUI — so an H2C
    // helper restarting could make the application vanish. Ignore it; every
    // writer already handles the resulting EPIPE.
    std::signal(SIGPIPE, SIG_IGN);

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("iWave SDR Analysis Tool"));
    app.setOrganizationName(QStringLiteral("iWave Global"));
    // Window/taskbar icon: the iWave monogram.
    // Multi-resolution icon: the window manager draws the titlebar mark from
    // this, and picks whichever size it needs. Supplying one bitmap forces it
    // to rescale and the wordmark turns to mush at 16-24 px.
    {
        QIcon brand;
        for (int px : {16, 24, 32, 48, 64, 128, 256})
            brand.addFile(QStringLiteral(":/icons/iwave_icon_%1.png").arg(px),
                          QSize(px, px));
        app.setWindowIcon(brand);
    }
    app.setOrganizationName(QStringLiteral("SignalAnalysis"));
    app.setApplicationVersion(QStringLiteral("1.0.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Live PCIe DMA and offline capture signal analysis workstation"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption skipConfig(
        QStringLiteral("skip-config"),
        QStringLiteral("Launch straight into the workstation using defaults."));
    const QCommandLineOption modeOpt(
        QStringLiteral("mode"),
        QStringLiteral("Start mode: sim | file | dma."),
        QStringLiteral("mode"), QStringLiteral("sim"));
    const QCommandLineOption fileOpt(
        QStringLiteral("file"),
        QStringLiteral("Capture file to analyse (implies --mode file)."),
        QStringLiteral("path"));
    const QCommandLineOption roceOpt(
        QStringLiteral("roce"),
        QStringLiteral("Live RoCEv2 from the receiver's shm ring (implies --mode roce)."),
        QStringLiteral("ring"), QStringLiteral("/dev/shm/iqring"));
    const QCommandLineOption deviceOpt(
        QStringLiteral("device"),
        QStringLiteral("DMA device node or FIFO path (implies --mode dma)."),
        QStringLiteral("path"));
    const QCommandLineOption formatOpt(
        QStringLiteral("format"),
        QStringLiteral("Wire format: cs8 | cu8 | cs12 | cs16 | cs32 | cf32 | cf64."),
        QStringLiteral("fmt"), QStringLiteral("cf32"));
    const QCommandLineOption channelsOpt(
        QStringLiteral("channels"),
        QStringLiteral("Channels interleaved in the stream."),
        QStringLiteral("n"), QStringLiteral("2"));
    const QCommandLineOption rateOpt(
        QStringLiteral("rate"),
        QStringLiteral("Sample rate in MSPS."),
        QStringLiteral("msps"), QStringLiteral("122.88"));
    const QCommandLineOption shot(
        QStringLiteral("capture"),
        QStringLiteral("Render to <file> after settling, then exit."),
        QStringLiteral("file"));
    const QCommandLineOption shotDelay(
        QStringLiteral("capture-delay"),
        QStringLiteral("Milliseconds to wait before capturing (default 1500)."),
        QStringLiteral("ms"), QStringLiteral("1500"));

    const QCommandLineOption preflight(
        QStringList{QStringLiteral("preflight"), QStringLiteral("gpu-rdma-preflight")},
        QStringLiteral("Report Ethernet/RoCEv2 -> GPU capability and exit."));
    parser.addOption(preflight);
    parser.addOption(skipConfig);
    parser.addOption(modeOpt);
    parser.addOption(fileOpt);
    parser.addOption(roceOpt);
    parser.addOption(deviceOpt);
    parser.addOption(formatOpt);
    parser.addOption(channelsOpt);
    parser.addOption(rateOpt);
    parser.addOption(shot);
    parser.addOption(shotDelay);
    parser.process(app);

    // Capability preflight: prints the Ethernet/RoCEv2 -> GPU dependency
    // matrix and the evidence-based transport decision, then exits. Headless
    // and side-effect free, so it can be run on the target before any
    // pipeline work is attempted.
    if (parser.isSet(preflight)) {
        const auto rep = sdr::probe::runGpuNetPreflight();
        fputs(qUtf8Printable(sdr::probe::formatGpuNetReport(rep)), stdout);
        return rep.gpuPathViable ? 0 : 2;
    }

    QFile qss(QStringLiteral(":/style/dark.qss"));
    if (qss.open(QIODevice::ReadOnly | QIODevice::Text))
        app.setStyleSheet(QString::fromUtf8(qss.readAll()));

    const int delay = parser.value(shotDelay).toInt();

    sdr::SystemConfig cfg;
    cfg.initial.acq.sampleRateMsps = parser.value(rateOpt).toDouble();
    if (cfg.initial.acq.sampleRateMsps <= 0.0) cfg.initial.acq.sampleRateMsps = 122.88;
    cfg.initial.src.streamChannels = qBound(1, parser.value(channelsOpt).toInt(), 16);

    const QString fmt = parser.value(formatOpt).toLower();
    if      (fmt == QLatin1String("cs8"))  cfg.initial.src.format = sdr::SampleFormat::Cs8;
    else if (fmt == QLatin1String("cu8"))  cfg.initial.src.format = sdr::SampleFormat::Cu8;
    else if (fmt == QLatin1String("cs12")) cfg.initial.src.format = sdr::SampleFormat::Cs12Packed;
    else if (fmt == QLatin1String("cs16")) cfg.initial.src.format = sdr::SampleFormat::Cs16;
    else if (fmt == QLatin1String("cs32")) cfg.initial.src.format = sdr::SampleFormat::Cs32;
    else if (fmt == QLatin1String("cf64")) cfg.initial.src.format = sdr::SampleFormat::Cf64;
    else                                   cfg.initial.src.format = sdr::SampleFormat::Cf32;

    // An explicit --file or --device settles the mode without needing --mode.
    const QString mode = parser.value(modeOpt).toLower();
    if (parser.isSet(fileOpt)) {
        cfg.initial.src.mode     = sdr::SourceMode::File;
        cfg.initial.src.filePath = parser.value(fileOpt);
    } else if (parser.isSet(roceOpt)) {
        cfg.initial.src.mode       = sdr::SourceMode::Roce;
        cfg.initial.src.devicePath = parser.value(roceOpt);
        cfg.initial.src.format     = sdr::SampleFormat::Cs16;  // ring ABI
        cfg.initial.src.streamChannels = 1;
        // The ring's control block is authoritative for rate and centre.
        double rate = 0.0, ctr = 0.0;
        if (sdr::RoceShmSource::peekRingInfo(cfg.initial.src.devicePath, &rate, &ctr)) {
            if (rate > 0.0) cfg.initial.acq.sampleRateMsps = rate;
            cfg.initial.acq.centerFreqGHz = ctr;
            cfg.initial.acq.decimation = 1;
            cfg.initial.acq.interpolation = 1;
            cfg.initial.acq.ncoFreqMHz = 0.0;
        }
    } else if (parser.isSet(deviceOpt)) {
        cfg.initial.src.mode       = sdr::SourceMode::Dma;
        cfg.initial.src.devicePath = parser.value(deviceOpt);
    } else if (mode == QLatin1String("file")) {
        cfg.initial.src.mode = sdr::SourceMode::File;
    } else if (mode == QLatin1String("roce")) {
        cfg.initial.src.mode = sdr::SourceMode::Roce;
    } else if (mode == QLatin1String("dma")) {
        cfg.initial.src.mode = sdr::SourceMode::Dma;
    } else {
        cfg.initial.src.mode = sdr::SourceMode::Simulated;
    }

    const bool preconfigured = parser.isSet(skipConfig) || parser.isSet(fileOpt)
                            || parser.isSet(deviceOpt)  || parser.isSet(modeOpt)
                            || parser.isSet(roceOpt);

    if (!preconfigured) {
        sdr::ui::ConfigDialog dialog;
        if (parser.isSet(shot)) {
            dialog.show();
            scheduleCapture(&dialog, parser.value(shot), delay);
            return app.exec();
        }
        if (dialog.exec() != QDialog::Accepted) return 0;
        cfg = dialog.config();
    }

    // PCIe pre-flight: whenever the session starts in DMA mode, validate the
    // hardware and driver stack before the workstation opens. A FIFO path
    // passes with only the node check, so loopback testing needs no hardware.
    if (cfg.initial.src.mode == sdr::SourceMode::Dma) {
        if (parser.isSet(shot)) {
            // Headless capture of the discovery screen itself, for docs/CI.
            if (qEnvironmentVariableIsSet("SDR_CAPTURE_DISCOVERY")) {
                auto* d = new sdr::ui::PcieDiscoveryDialog(cfg.initial.src.devicePath);
                d->show();
                scheduleCapture(d, parser.value(shot), delay);
                return app.exec();
            }
            // Normal shot mode skips the splash so pipeline captures stay fast.
        } else {
            sdr::ui::PcieDiscoveryDialog discovery(cfg.initial.src.devicePath);
            discovery.exec();
            if (!discovery.passed()) return 1;
        }
    }

    auto* window = new sdr::ui::MainWindow(cfg);
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->show();

    if (parser.isSet(shot))
        scheduleCapture(window, parser.value(shot), delay);

    return app.exec();
}
