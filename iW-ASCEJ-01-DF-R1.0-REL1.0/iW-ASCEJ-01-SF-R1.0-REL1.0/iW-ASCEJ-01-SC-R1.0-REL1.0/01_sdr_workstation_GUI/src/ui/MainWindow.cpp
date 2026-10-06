#include "MainWindow.h"
#include "../core/DspEngine.h"
#include "../core/BackendLauncher.h"
#include "../core/CaptureBudget.h"
#include "../core/DriverManager.h"
#include "../core/RoceShmSource.h"
#include "../core/Recorder.h"
#include "../core/Sources.h"
#include "BackendModeDialog.h"
#include "TitleBar.h"
#include "ControlWindow3D.h"
#include <QPushButton>
#include "ResourceBar.h"
#include "Panels.h"
#include "Plots.h"
#include "Theme.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTime>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QMenu>
#include <QWindow>
#include <QEvent>
#include <QMouseEvent>
#include <QMenuBar>
#include <QInputDialog>
#include <QMessageBox>
#include <QProgressBar>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QThread>
#include <QToolButton>
#include <QToolBar>
#include <QVBoxLayout>
#include <algorithm>
#include <fstream>

namespace sdr::ui {

namespace {
constexpr int kTableUpdateMs = 120;   // measurement tables need not run at 30 fps

QString humanBytes(quint64 b)
{
    if (b >= (1ull << 30)) return QStringLiteral("%1 GiB").arg(b / double(1ull << 30), 0, 'f', 2);
    if (b >= (1ull << 20)) return QStringLiteral("%1 MiB").arg(b / double(1ull << 20), 0, 'f', 1);
    if (b >= (1ull << 10)) return QStringLiteral("%1 KiB").arg(b / double(1ull << 10), 0, 'f', 1);
    return QStringLiteral("%1 B").arg(b);
}
} // namespace

// ============================================================== lifecycle

MainWindow::MainWindow(const sdr::SystemConfig& sys, QWidget* parent)
    : QMainWindow(parent), m_sys(sys), m_cfg(sys.initial)
{
    if (m_sys.driverLoadedBySession)
        m_driverMgr = new sdr::DriverManager(this);

    setWindowTitle(QStringLiteral("iWave SDR Analysis Tool"));


    {
        QIcon brand;
        for (int px : {16, 24, 32, 48, 64, 128, 256})
            brand.addFile(QStringLiteral(":/icons/iwave_icon_%1.png").arg(px),
                          QSize(px, px));
        setWindowIcon(brand);
    }
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks
                   | QMainWindow::AllowTabbedDocks);
    setDockNestingEnabled(true);
    resize(1620, 960);

    buildActions();
    buildCentral();
    buildDocks();
    buildMenus();
    // Resource bar and H2C control are created here but installed on the
    // TOOLBAR, not the menu bar: the menu bar is a text row sized for menu
    // items, so a pill button and a metrics strip crowded it and sat
    // visually apart from the transport controls they belong beside.
    m_resMon = new sdr::ResourceMonitor(this);
    m_resBar = new ResourceBar;
    m_resBar->setMonitor(m_resMon);

    connect(m_resMon, &sdr::ResourceMonitor::alert, this,
            [this](const sdr::ResourceAlert& a) {
                const QString msg = a.fix.isEmpty()
                    ? QStringLiteral("%1 — %2").arg(a.title, a.detail)
                    : QStringLiteral("%1 — %2  →  %3").arg(a.title, a.detail, a.fix);
                statusBar()->showMessage(msg,
                    a.level == sdr::Level::Critical ? 12000 : 8000);
            });
    m_resMon->start(1000);

    m_act3d = new QAction(QStringLiteral("H2C CONTROL"), this);
    connect(m_act3d, &QAction::triggered, this, [this] {
        if (!m_window3d) {
            m_window3d = new ControlWindow3D(this);
            // The transmit side (DAC input, DDS, IQ generator) is published to
            // the acquisition source. Only the simulator acts on it — as an RF
            // loopback — so Simulation shows what those controls would do.
            connect(m_window3d, &ControlWindow3D::txStateChanged, this,
                    [this](const sdr::TxState& tx) {
                const bool started = tx.hostRunning && !m_txState.hostRunning;
                const bool ended   = !tx.hostRunning && m_txState.hostRunning;
                m_txState = tx;
                if (started) transmitGuard(true);
                if (ended) transmitGuard(false);
                if (m_source)
                    QMetaObject::invokeMethod(m_source, "setTxState", Qt::QueuedConnection,
                                              Q_ARG(sdr::TxState, tx));
                // The generator needs iwfg_h2c on the other end of its FIFO. A
                // session launched as capture only never started it, and the
                // generator then waited for a reader that did not exist.
                if (started && m_cfg.src.mode == SourceMode::Dma && m_backend
                    && m_backend->isRunning() && !m_backend->h2cRunning())
                    m_backend->ensureH2c();
            });
            m_window3d->setSimulationMode(m_cfg.src.mode == SourceMode::Simulated);
            if (m_h2cChunkBytes > 0) m_window3d->setH2cChunkBytes(m_h2cChunkBytes);
            m_window3d->setRdmaControlVisible(m_cfg.src.mode == SourceMode::Roce);
            publishHelperState();
            m_window3d->setH2cFifoPath(m_backend ? m_backend->config().h2cFifo
                : qEnvironmentVariable("IWFG_H2C_FIFO", QStringLiteral("/tmp/iwfg_h2c.fifo")));
            // Operator recovery actions from the Transmit tab.
            connect(m_window3d, &ControlWindow3D::h2cRestartRequested, this, [this] {
                if (m_backend) m_backend->restartH2c(sdr::defaultH2cChunkBytes());
            });
            connect(m_window3d, &ControlWindow3D::captureChannelsRequested, this,
                    [this](int n) { setCaptureChannels(n); });
            // The device's stream clocks are authoritative. A display running
            // at the wrong rate scales every frequency (15 MHz at 250 MSPS for
            // a 200 MSPS stream reads as 18.75 MHz), which made a received
            // tone look like something other than what was transmitted.
            connect(m_window3d, &ControlWindow3D::deviceRatesRead, this,
                    [this](double adcMsps, double /*dacMsps*/) {
                if (m_cfg.src.mode != SourceMode::Dma || adcMsps <= 1.0) return;
                const double was = m_cfg.acq.sampleRateMsps;
                if (std::abs(was - adcMsps) <= 1e-3 * adcMsps) return;
                m_cfg.acq.sampleRateMsps = adcMsps;
                m_sourcePanel->loadFrom(m_cfg);
                pushConfig();
                const QString msg = QStringLiteral(
                    "Display sample rate set to %1 MSPS from the device's ADC stream "
                    "clock (it was %2 MSPS), so received frequencies read correctly.")
                    .arg(adcMsps, 0, 'f', 3).arg(was, 0, 'f', 3);
                statusBar()->showMessage(msg, 8000);
                m_window3d->appendLog(msg);
            });
        }
        m_window3d->show();
        m_window3d->raise();
        m_window3d->activateWindow();
    });

    m_btnH2c = new QToolButton;
    m_btnH2c->setObjectName(QStringLiteral("h2cControlButton"));
    m_btnH2c->setDefaultAction(m_act3d);
    m_btnH2c->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_btnH2c->setCursor(Qt::PointingHandCursor);

    // The window hosts two independent surfaces: H2C transmit control and RFDC
    // (PCIe) converter control. RFDC configuration is valid in every data-flow
    // mode, so the window is always reachable; the Transmit tab self-disables
    // with a reason when there is no H2C path, rather than the whole window
    // being hidden and the converter controls made unreachable with it.
    m_act3d->setEnabled(true);
    m_btnH2c->setToolTip(m_sys.dataFlowMode != 0
        ? QStringLiteral("Open the H2C transmit and RF Data Converter control")
        : QStringLiteral("Open the H2C transmit and RF Data Converter control (ADC Only "
                         "session: the transmit helper is started when you start the "
                         "generator)"));

    buildToolBar();

    buildStatusBar();
    buildPipeline();

    m_sourcePanel->loadFrom(m_cfg);
    // Same sidecar restore for a file supplied on the command line or chosen
    // in the launcher, so every route into offline mode behaves identically.
    if (m_cfg.src.mode == SourceMode::File && !m_cfg.src.filePath.isEmpty())
        m_sourcePanel->applySidecar(m_cfg.src.filePath);
    m_displayPanel->loadFrom(m_cfg);

    // Render cadence is owned by the GUI thread and capped at the configured
    // refresh rate; the DSP stage may run ahead of it freely.
    m_renderGate = std::make_shared<QAtomicInt>(0);
    m_dsp->setRenderGate(m_renderGate);

    m_renderTimer = new QTimer(this);
    m_renderTimer->setTimerType(Qt::PreciseTimer);
    connect(m_renderTimer, &QTimer::timeout, this, &MainWindow::renderPending);
    m_renderTimer->start(1000 / std::clamp(m_cfg.disp.refreshFps, 1, 120));

    m_fpsClock.start();
    m_tableClock.start();

    restoreSettings();
    onConfigChanged();
    createSource();
    onStart();
}

MainWindow::~MainWindow()
{
    destroySource();
    if (m_backend) {
        // Bounded (~2 s) and never waits on a helper stuck in the driver.
        delete m_backend;
        m_backend = nullptr;
    }

    if (m_recorder)
        QMetaObject::invokeMethod(m_recorder, "stopRecording", Qt::BlockingQueuedConnection);

    for (QThread* t : {m_sourceThread, m_dspThread, m_recThread}) {
        if (!t) continue;
        t->quit();
        t->wait(2000);
    }

    // Driver teardown is last, and only for a module this session loaded:
    // the acquisition thread is already stopped and its file descriptors
    // closed by destroySource() above, so rmmod cannot race a live DMA.
    // A module the operator had already inserted is deliberately left in
    // place — we clean up only what we created.
    if (m_driverMgr && m_sys.driverLoadedBySession) {
        const auto r = m_driverMgr->removeModule();
        if (!r.ok)
            qWarning("driver unload: %s", qUtf8Printable(r.detail));
    }
}

bool MainWindow::eventFilter(QObject* o, QEvent* e)
{

    // Edge resizing for the frameless window. Qt provides startSystemResize()
    // which hands the drag back to the compositor, so snapping and tiling keep
    // working exactly as they did with the native frame.
    if (o == this && m_titleBar && !isMaximized()) {
        constexpr int kEdge = 6;
        if (e->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(e);
            if (me->button() == Qt::LeftButton) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                const QPoint p = me->position().toPoint();
#else
                const QPoint p = me->pos();
#endif
                Qt::Edges edges;
                if (p.x() <= kEdge)             edges |= Qt::LeftEdge;
                if (p.x() >= width()  - kEdge)  edges |= Qt::RightEdge;
                if (p.y() <= kEdge)             edges |= Qt::TopEdge;
                if (p.y() >= height() - kEdge)  edges |= Qt::BottomEdge;
                if (edges) {
                    if (QWindow* w = windowHandle()) {
                        w->startSystemResize(edges);
                        return true;
                    }
                }
            }
        } else if (e->type() == QEvent::MouseMove) {
            auto* me = static_cast<QMouseEvent*>(e);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            const QPoint p = me->position().toPoint();
#else
            const QPoint p = me->pos();
#endif
            const bool l = p.x() <= kEdge, r = p.x() >= width()  - kEdge;
            const bool t = p.y() <= kEdge, b = p.y() >= height() - kEdge;
            if ((l && t) || (r && b))      setCursor(Qt::SizeFDiagCursor);
            else if ((r && t) || (l && b)) setCursor(Qt::SizeBDiagCursor);
            else if (l || r)               setCursor(Qt::SizeHorCursor);
            else if (t || b)               setCursor(Qt::SizeVerCursor);
            else                           unsetCursor();
        }
    }
    return QMainWindow::eventFilter(o, e);
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    saveSettings();
    QMainWindow::closeEvent(e);
}

// ================================================================ actions

void MainWindow::buildActions()
{
    m_actStart = new QAction(QIcon(QStringLiteral(":/icons/play.png")),
                             QStringLiteral("Start"), this);
    m_actStart->setShortcut(QKeySequence(Qt::Key_F5));
    m_actStart->setStatusTip(QStringLiteral("Begin acquisition"));

    m_actPause = new QAction(QIcon(QStringLiteral(":/icons/pause.png")),
                             QStringLiteral("Pause"), this);
    m_actPause->setCheckable(true);
    m_actPause->setShortcut(QKeySequence(Qt::Key_F6));
    m_actPause->setStatusTip(QStringLiteral("Hold the display without tearing down the stream"));

    m_actStop = new QAction(QIcon(QStringLiteral(":/icons/stop.png")),
                            QStringLiteral("Stop"), this);
    m_actStop->setShortcut(QKeySequence(Qt::Key_F7));
    m_actStop->setStatusTip(QStringLiteral("Stop acquisition and release the device"));

    m_actRecord = new QAction(QIcon(QStringLiteral(":/icons/record.png")),
                              QStringLiteral("Record"), this);
    m_actRecord->setCheckable(true);
    m_actRecord->setShortcut(QKeySequence(Qt::Key_F9));
    m_actRecord->setStatusTip(QStringLiteral("Write the live stream to disk while plotting"));

    m_actOpen = new QAction(QIcon(QStringLiteral(":/icons/open.png")),
                            QStringLiteral("Open capture…"), this);
    m_actOpen->setShortcut(QKeySequence::Open);
    m_actOpen->setStatusTip(QStringLiteral("Switch to offline mode and load a stored capture"));

    connect(m_actStart,  &QAction::triggered, this, &MainWindow::onStart);
    connect(m_actPause,  &QAction::toggled,   this, &MainWindow::onPause);
    connect(m_actStop,   &QAction::triggered, this, &MainWindow::onStop);
    connect(m_actRecord, &QAction::toggled,   this, &MainWindow::onToggleRecord);
    connect(m_actOpen,   &QAction::triggered, this, &MainWindow::onOpenCapture);
}

// ================================================================ central

void MainWindow::buildCentral()
{
    m_timePlot      = new TimeDomainPlot;
    m_spectrumPlot  = new SpectrumPlot;
    m_waterfallPlot = new WaterfallPlot;
    m_constPlot     = new ConstellationPlot;

    m_topSplit = new QSplitter(Qt::Horizontal);
    m_topSplit->addWidget(m_timePlot);
    m_topSplit->addWidget(m_spectrumPlot);
    m_topSplit->setSizes({600, 700});

    m_bottomSplit = new QSplitter(Qt::Horizontal);
    m_bottomSplit->addWidget(m_waterfallPlot);
    m_bottomSplit->addWidget(m_constPlot);
    m_bottomSplit->setSizes({760, 540});

    m_mainSplit = new QSplitter(Qt::Vertical);
    m_mainSplit->addWidget(m_topSplit);
    m_mainSplit->addWidget(m_bottomSplit);
    m_mainSplit->setSizes({430, 430});

    auto* central = new QWidget;
    auto* cl = new QVBoxLayout(central);
    cl->setContentsMargins(4, 4, 4, 4);
    cl->addWidget(m_mainSplit);
    setCentralWidget(central);

    // Spectrum and waterfall share a frequency axis, so a zoom on either has
    // to move the other or the two stop lining up.
    connect(m_spectrumPlot, &PlotBase::xViewChanged, this, [this](double a, double b) {
        if (m_linking) return;
        m_linking = true;
        m_waterfallPlot->setViewX(a, b);
        m_linking = false;
    });
    connect(m_waterfallPlot, &PlotBase::xViewChanged, this, [this](double a, double b) {
        if (m_linking) return;
        m_linking = true;
        m_spectrumPlot->setViewX(a, b);
        m_linking = false;
    });

    for (InstrumentPlot* p : plots())
        connect(p, &PlotBase::readoutChanged, this, [this](const QString& t) {
            m_sbReadout->setText(t.isEmpty()
                ? QStringLiteral("Ctrl+click a plot to drop a marker  ·  wheel zooms  ·  drag pans")
                : t);
        });
}

QList<InstrumentPlot*> MainWindow::plots() const
{
    return {m_timePlot, m_spectrumPlot, m_waterfallPlot, m_constPlot};
}

// ================================================================== docks

void MainWindow::buildDocks()
{
    auto makeDock = [this](const QString& title, const QString& objName,
                           QWidget* body, Qt::DockWidgetArea area, int fixedWidth) {
        auto* dock = new QDockWidget(title, this);
        dock->setObjectName(objName);
        dock->setAllowedAreas(Qt::AllDockWidgetAreas);

        auto* scroll = new QScrollArea;
        scroll->setWidget(body);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        if (fixedWidth > 0) scroll->setMinimumWidth(fixedWidth);
        dock->setWidget(scroll);

        addDockWidget(area, dock);
        return dock;
    };

    m_sourcePanel  = new SourcePanel;
    m_displayPanel = new DisplayPanel;
    m_statusPanel  = new StatusPanel;
    m_measurePanel = new MeasurementPanel;

    m_sourceDock  = makeDock(QStringLiteral("Source & Signal Chain"),
                             QStringLiteral("sourceDock"), m_sourcePanel,
                             Qt::LeftDockWidgetArea, 268);
    m_displayDock = makeDock(QStringLiteral("Display & Analysis"),
                             QStringLiteral("displayDock"), m_displayPanel,
                             Qt::RightDockWidgetArea, 268);
    m_statusDock  = makeDock(QStringLiteral("Acquisition Status"),
                             QStringLiteral("statusDock"), m_statusPanel,
                             Qt::RightDockWidgetArea, 268);

    auto* measureDock = new QDockWidget(QStringLiteral("Measurements"), this);
    measureDock->setObjectName(QStringLiteral("measureDock"));
    measureDock->setWidget(m_measurePanel);
    addDockWidget(Qt::BottomDockWidgetArea, measureDock);
    m_measureDock = measureDock;

    // Status under Display on the right, rather than stealing vertical space
    // from the plots.
    splitDockWidget(m_displayDock, m_statusDock, Qt::Vertical);

    connect(m_sourcePanel, &SourcePanel::configChanged, this, &MainWindow::onConfigChanged);
    connect(m_sourcePanel, &SourcePanel::sourceRestartRequired,
            this, &MainWindow::onSourceRestartRequired);
    connect(m_sourcePanel, &SourcePanel::seekRequested, this, [this](double frac) {
        if (m_source) QMetaObject::invokeMethod(m_source, "seekFraction",
                                                Qt::QueuedConnection, Q_ARG(double, frac));
    });

    // Give the docks sensible proportions before any saved state is applied;
    // the defaults let the measurement dock swallow half the plot area.
    resizeDocks({m_sourceDock, m_displayDock}, {320, 300}, Qt::Horizontal);
    resizeDocks({m_measureDock}, {268}, Qt::Vertical);
    resizeDocks({m_displayDock, m_statusDock}, {430, 430}, Qt::Vertical);

    connect(m_displayPanel, &DisplayPanel::configChanged, this, &MainWindow::onConfigChanged);
    connect(m_displayPanel, &DisplayPanel::resetAveragingRequested, this, [this] {
        if (m_dsp) QMetaObject::invokeMethod(m_dsp, "clearAverages", Qt::QueuedConnection);
        for (InstrumentPlot* p : plots()) p->clearTraces();
        statusBar()->showMessage(QStringLiteral("Averaging and hold traces cleared"), 2500);
    });
}

// ================================================================== menus

void MainWindow::buildMenus()
{
    auto* file = menuBar()->addMenu(QStringLiteral("&File"));
    file->addAction(m_actOpen);
    file->addAction(m_actRecord);
    file->addSeparator();
    file->addAction(QStringLiteral("Export displayed I/Q…"), this, &MainWindow::onExportIq);
    file->addAction(QStringLiteral("Save screenshot…"), this, &MainWindow::onScreenshot);
    file->addSeparator();
    file->addAction(QStringLiteral("Quit"), this, &QWidget::close)
        ->setShortcut(QKeySequence::Quit);

    auto* acq = menuBar()->addMenu(QStringLiteral("&Acquisition"));
    acq->addAction(m_actStart);
    acq->addAction(m_actPause);
    acq->addAction(m_actStop);

    auto* view = menuBar()->addMenu(QStringLiteral("&View"));
    for (QDockWidget* d : {m_sourceDock, m_displayDock, m_statusDock, m_measureDock})
        view->addAction(d->toggleViewAction());

    view->addSeparator();
    auto* plotMenu = view->addMenu(QStringLiteral("Plots"));
    struct { InstrumentPlot* plot; const char* name; } entries[] = {
        {m_timePlot,      "Time domain"},
        {m_spectrumPlot,  "Spectrum"},
        {m_waterfallPlot, "Waterfall"},
        {m_constPlot,     "Constellation"}};
    for (auto& e : entries) {
        QAction* a = plotMenu->addAction(QString::fromLatin1(e.name));
        a->setCheckable(true);
        a->setChecked(true);
        InstrumentPlot* p = e.plot;
        connect(a, &QAction::toggled, this, [p](bool on) { p->setVisible(on); });
    }

    view->addSeparator();
    auto* layout = view->addMenu(QStringLiteral("Layout"));
    const QStringList presets = {QStringLiteral("Four-up (default)"),
                                 QStringLiteral("Spectrum focus"),
                                 QStringLiteral("Time domain focus"),
                                 QStringLiteral("Spectrum + waterfall")};
    for (int i = 0; i < presets.size(); ++i)
        layout->addAction(presets[i], this, [this, i] { applyLayoutPreset(i); });

    view->addAction(QStringLiteral("Reset dock layout"), this, &MainWindow::onResetLayout);
    view->addSeparator();
    view->addAction(QStringLiteral("Reset all plot views"), this, [this] {
        for (InstrumentPlot* p : plots()) p->resetView();
    });
    view->addAction(QStringLiteral("Clear all markers"), this, [this] {
        for (InstrumentPlot* p : plots()) p->clearMarkers();
    });

    auto* help = menuBar()->addMenu(QStringLiteral("&Help"));
    help->addAction(QStringLiteral("About"), this, [this] {
        QMessageBox::about(this, QStringLiteral("iWave SDR Analysis Tool"),
            QStringLiteral(
                "<b>iWave SDR Analysis Tool</b><br>"
                "<span style='color:#7d8b9a'>iWave Global</span><br><br>"
                "Live PCIe DMA / FIFO acquisition and offline capture analysis "
                "through a shared visualisation pipeline.<br><br>"
                "<b>Plot interaction</b><br>"
                "Wheel — zoom X (Ctrl: Y, Shift: both)<br>"
                "Drag — pan &nbsp;·&nbsp; Shift+drag — box zoom<br>"
                "Ctrl+click — marker &nbsp;·&nbsp; Double-click — reset view<br>"
                "Right-click — plot menu"));
    });
}

// =============================================================== tool bar

void MainWindow::buildToolBar()
{
    auto* tb = addToolBar(QStringLiteral("Main"));
    tb->setObjectName(QStringLiteral("mainToolBar"));
    tb->setMovable(false);
    // Icon beside text: the transport controls are the most-used affordance
    // in the window and read faster with a glyph than as a row of words.
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    tb->setIconSize(QSize(15, 15));

    m_actStart->setProperty("accent", QStringLiteral("go"));
    m_actStop->setProperty("accent", QStringLiteral("stop"));
    m_actRecord->setProperty("accent", QStringLiteral("rec"));

    tb->addAction(m_actStart);
    tb->addAction(m_actPause);
    tb->addAction(m_actStop);
    tb->addSeparator();
    tb->addAction(m_actRecord);
    tb->addAction(m_actOpen);
    tb->addSeparator();

    auto* device = new QLabel(QStringLiteral("  %1  ·  %2  ")
                                  .arg(m_sys.device, m_sys.interfaceName));
    device->setObjectName(QStringLiteral("deviceLabel"));
    tb->addWidget(device);

    auto* spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);

    // Metrics then H2C control, immediately before the run-state pill, so the
    // whole right-hand group reads as one status cluster.
    if (m_resBar) {
        tb->addWidget(m_resBar);
        tb->addSeparator();
    }
    if (m_btnH2c) {
        tb->addWidget(m_btnH2c);
        tb->addSeparator();
    }

    m_sbState = new QLabel(QStringLiteral("STOPPED"));
    m_sbState->setObjectName(QStringLiteral("statePill"));
    m_sbState->setAlignment(Qt::AlignCenter);
    m_sbState->setMinimumWidth(96);
    tb->addWidget(m_sbState);
}

// ============================================================= status bar

void MainWindow::buildStatusBar()
{
    m_sbReadout = new QLabel(
        QStringLiteral("Ctrl+click a plot to drop a marker  ·  wheel zooms  ·  drag pans"));
    m_sbReadout->setObjectName(QStringLiteral("readoutLabel"));

    m_sbSource  = new QLabel(QStringLiteral("—"));
    m_sbThrough = new QLabel(QStringLiteral("0.0 MB/s"));
    m_sbDrops   = new QLabel(QStringLiteral("drops 0"));
    for (QLabel* l : {m_sbSource, m_sbThrough, m_sbDrops}) {
        l->setObjectName(QStringLiteral("sbValue"));
        l->setFont(theme::monoFont(11));
    }

    m_sbBuffer = new QProgressBar;
    m_sbBuffer->setObjectName(QStringLiteral("sbBuffer"));
    m_sbBuffer->setRange(0, 100);
    // Font-relative rather than a literal 110x12: at 150 % scaling a fixed
    // bar is half the height of its neighbours and its text no longer fits.
    {
        const QFontMetrics fm(m_sbBuffer->font());
        m_sbBuffer->setFixedSize(fm.horizontalAdvance(QLatin1Char('0')) * 12,
                                 std::max(12, fm.height() - 2));
    }
    m_sbBuffer->setTextVisible(false);

    auto* bufWrap = new QWidget;
    auto* bh = new QHBoxLayout(bufWrap);
    bh->setContentsMargins(0, 0, 0, 0);
    bh->setSpacing(6);
    auto* bufLabel = new QLabel(QStringLiteral("buffer"));
    bufLabel->setObjectName(QStringLiteral("sbCaption"));
    bh->addWidget(bufLabel);
    bh->addWidget(m_sbBuffer);

    statusBar()->addWidget(m_sbReadout, 1);
    m_sbCompute = new QLabel(QStringLiteral("  CPU  "));
    m_sbCompute->setToolTip(QStringLiteral("Where the FFT runs"));
    m_sbCompute->setStyleSheet(QStringLiteral("color:#e5e7eb;background:#374151;"
        "font-weight:bold;border-radius:3px;padding:1px 6px;"));
    statusBar()->addPermanentWidget(m_sbCompute);
    statusBar()->addPermanentWidget(m_sbSource);
    statusBar()->addPermanentWidget(m_sbThrough);
    statusBar()->addPermanentWidget(bufWrap);
    statusBar()->addPermanentWidget(m_sbDrops);
    statusBar()->setSizeGripEnabled(false);
}

// =============================================================== pipeline

void MainWindow::buildPipeline()
{
    qRegisterMetaType<sdr::SampleBlock>("sdr::SampleBlock");
    qRegisterMetaType<sdr::FrameResult>("sdr::FrameResult");
    qRegisterMetaType<sdr::Config>("sdr::Config");
    qRegisterMetaType<sdr::StreamStats>("sdr::StreamStats");
    qRegisterMetaType<sdr::RunState>("sdr::RunState");
    qRegisterMetaType<sdr::TxState>("sdr::TxState");

    m_backPressure = std::make_shared<BackPressure>();
    m_backPressure->capacity = 24;

    m_sourceThread = new QThread(this);
    m_dspThread    = new QThread(this);
    m_recThread    = new QThread(this);
    m_sourceThread->setObjectName(QStringLiteral("acquisition"));
    m_dspThread->setObjectName(QStringLiteral("dsp"));
    m_recThread->setObjectName(QStringLiteral("recorder"));

    m_dsp = new DspEngine;
    m_dsp->setBackPressure(m_backPressure);
    m_dsp->moveToThread(m_dspThread);
    connect(m_dspThread, &QThread::finished, m_dsp, &QObject::deleteLater);
    connect(m_dsp, &DspEngine::frameReady, this, &MainWindow::onFrameReady);

    // Report where the FFT ACTUALLY runs. The toggle only expresses intent;
    // this is the fact — including a silent fallback to CPU, which would
    // otherwise look identical on screen.
    connect(m_dsp, &DspEngine::computeStatus, this,
            [this](bool onGpu, const QString& detail) {
        m_sbCompute->setText(onGpu ? QStringLiteral("  GPU  ")
                                   : QStringLiteral("  CPU  "));
        m_sbCompute->setToolTip(detail);
        m_sbCompute->setStyleSheet(onGpu
            ? QStringLiteral("color:#0b0f14;background:#33d17a;font-weight:bold;"
                             "border-radius:3px;padding:1px 6px;")
            : QStringLiteral("color:#e5e7eb;background:#374151;font-weight:bold;"
                             "border-radius:3px;padding:1px 6px;"));
        statusBar()->showMessage(detail, 8000);
    });

    m_recorder = new Recorder;
    m_recorder->moveToThread(m_recThread);
    connect(m_recThread, &QThread::finished, m_recorder, &QObject::deleteLater);
    connect(m_recorder, &Recorder::recordingStarted, this, [this](const QString& p) {
        statusBar()->showMessage(QStringLiteral("Recording to %1").arg(p), 4000);
    });
    connect(m_recorder, &Recorder::recordingProgress, this, [this](quint64 bytes, quint64) {
        m_recordedBytes = bytes;
        // Session size is the authoritative input to the guard: bytes written
        // during THIS session, not a cumulative or filesystem figure.
        if (m_recGuard && m_recGuard->active()) {
            const sdr::RecStatus st =
                m_recGuard->update(bytes, m_resMon ? m_resMon->disk() : sdr::DiskSample{});
            if (m_resBar) m_resBar->setRecordingStatus(st);
        }
    });
    connect(m_recorder, &Recorder::recordingStopped, this,
            [this](const QString& p, quint64 bytes, quint64 samples) {
        m_recordedBytes = bytes;
        statusBar()->showMessage(QStringLiteral("Recorded %1 (%2 samples/ch) to %3")
                                     .arg(humanBytes(bytes)).arg(samples).arg(p), 6000);
    });
    connect(m_recorder, &Recorder::recorderError, this, [this](const QString& t) {
        m_actRecord->setChecked(false);
        showNotice(QStringLiteral("recorder"), QMessageBox::Warning,
                   QStringLiteral("Recorder"), t);
    });

    // Acquisition, DSP and recording all run below the GUI thread. On a busy
    // or core-limited machine the window must stay interactive even when the
    // pipeline is saturated; losing a frame is preferable to losing the UI.
    m_sourceThread->start(QThread::LowPriority);
    m_dspThread->start(QThread::LowPriority);
    m_recThread->start(QThread::LowPriority);
}

void MainWindow::createSource()
{
    destroySource();

    switch (m_cfg.src.mode) {
    case SourceMode::File: m_source = new FileSource;      break;
    case SourceMode::Dma:  m_source = new DmaSource;       break;
    // RoCEv2: the supplied rdma_rx receiver owns the network side; this
    // source only reads the shared-memory ring it publishes.
    case SourceMode::Roce: m_source = new RoceShmSource;    break;
    default:               m_source = new SimulatedSource; break;
    }

    m_sourceKind = m_cfg.src.mode;
    m_source->setBackPressure(m_backPressure);
    m_source->moveToThread(m_sourceThread);

    connect(m_source, &ISignalSource::blockReady, m_dsp, &DspEngine::processBlock);
    // The recorder is fed from the source, not from the DSP stage: a recording
    // must capture every block even while the display is throttled.
    connect(m_source, &ISignalSource::blockReady, m_recorder, &Recorder::writeBlock);
    connect(m_source, &ISignalSource::statsUpdated, m_dsp, &DspEngine::updateStats);
    connect(m_source, &ISignalSource::statsUpdated, this, &MainWindow::onStats);
    connect(m_source, &ISignalSource::runStateChanged, this, &MainWindow::onRunState);
    connect(m_source, &ISignalSource::sourceError, this, &MainWindow::onSourceError);
    connect(m_source, &ISignalSource::statusMessage, this, [this](const QString& t) {
        statusBar()->showMessage(t, 4000);
    });

    QMetaObject::invokeMethod(m_source, "applyConfig", Qt::QueuedConnection,
                              Q_ARG(sdr::Config, m_cfg));
    QMetaObject::invokeMethod(m_source, "setTxState", Qt::QueuedConnection,
                              Q_ARG(sdr::TxState, m_txState));
    if (m_window3d) {
        m_window3d->setSimulationMode(m_cfg.src.mode == SourceMode::Simulated);
        m_window3d->setRdmaControlVisible(m_cfg.src.mode == SourceMode::Roce);
    }
}

void MainWindow::destroySource()
{
    if (!m_source) return;
    // Queued, never blocking. 1.2.3 used a BlockingQueuedConnection here: the
    // GUI thread waited for the acquisition thread, and if that thread was
    // busy — or parked in a blocking read — the whole window hung. The stop
    // and the delete are queued to the source's own thread in that order,
    // so the device is still closed before the object goes away.
    ISignalSource* old = m_source;
    m_source = nullptr;
    old->disconnect();
    QMetaObject::invokeMethod(old, "stop", Qt::QueuedConnection);
    old->deleteLater();
    // Anything still queued belongs to the source that just went away.
    if (m_backPressure) m_backPressure->queued.storeRelease(0);
}

// ================================================================== slots

void MainWindow::pushConfig()
{
    if (m_source) QMetaObject::invokeMethod(m_source, "applyConfig", Qt::QueuedConnection,
                                            Q_ARG(sdr::Config, m_cfg));
    if (m_dsp)    QMetaObject::invokeMethod(m_dsp, "applyConfig", Qt::QueuedConnection,
                                            Q_ARG(sdr::Config, m_cfg));
}

void MainWindow::onConfigChanged()
{
    if (m_renderTimer) {
        const int ms = 1000 / std::clamp(m_cfg.disp.refreshFps, 1, 120);
        if (m_renderTimer->interval() != ms) m_renderTimer->start(ms);
    }
    m_sourcePanel->applyTo(m_cfg);
    m_cfg.disp = m_displayPanel->displayConfig();

    m_displayPanel->setChannelCount(m_cfg.src.streamChannels);
    m_statusPanel->setChannelInfo(m_cfg);

    for (InstrumentPlot* p : plots()) p->setDisplayConfig(m_cfg.disp);

    // Offline mode has no live device to record from.
    m_actRecord->setEnabled(m_cfg.src.mode != SourceMode::File);

    pushConfig();
}

void MainWindow::onSourceRestartRequired()
{
    if (m_restarting) return;
    m_restarting = true;

    const bool wasRunning = (m_lastStats.state == RunState::Running
                          || m_lastStats.state == RunState::Paused);

    m_sourcePanel->applyTo(m_cfg);
    m_cfg.disp = m_displayPanel->displayConfig();

    // A live PCIe/UDP reader is reconfigured IN PLACE. Recreating it closes
    // the capture FIFO, and c2h_stream exits the moment its reader goes
    // (EPIPE -> "Stopping stream...", exit 0): an edit in the Source panel —
    // or just focus leaving the device-path field — ended the capture.
    // DmaSource applies a new format, channel count, block size or path
    // itself (applyConfig).
    if (m_source && m_sourceKind == SourceMode::Dma && m_cfg.src.mode == SourceMode::Dma) {
        pushConfig();
        for (InstrumentPlot* p : plots()) p->clearTraces();
        m_measurePanel->clear();
        m_restarting = false;
        return;
    }

    // Leaving live mode: the helpers would otherwise keep running with no
    // reader, and the capture helper would be restarted into the void.
    if (m_cfg.src.mode != SourceMode::Dma && m_backend && m_backend->isRunning()) {
        m_backend->stop();
        m_backendReady = false;
    }

    createSource();
    for (InstrumentPlot* p : plots()) p->clearTraces();
    m_measurePanel->clear();

    if (wasRunning) onStart();
    m_restarting = false;
}

void MainWindow::onStart()
{
    // Live mode routes through the backend applications. This is the only
    // behavioural change to the existing UI: a mode popup, then the pipeline
    // starts once the backend reports its capture FIFO is ready. Simulated
    // and offline modes are untouched.
    if (m_cfg.src.mode == SourceMode::Dma && !m_backendReady) {
        // UDP is a live transport too: start its receiver automatically
        // rather than asking the operator to run iwfg_c2h in a terminal.
        if (m_sys.udpTransport) { startUdpThenAcquire(); return; }
        startBackendThenAcquire();
        return;
    }

    // RoCEv2: the receiver owns the network side and publishes the ring this
    // source reads. Start it here when nothing is publishing yet, for the same
    // reason the UDP receiver is started here -- otherwise selecting RoCEv2
    // and pressing Start just reports "cannot open /dev/shm/iqring". A ring
    // that already exists belongs to a receiver the operator started, so it is
    // used as it is and nothing is launched.
    if (m_cfg.src.mode == SourceMode::Roce && m_sys.roceTransport
        && !m_backendReady && !sdr::BackendLauncher::roceRingPresent()) {
        startRoceThenAcquire();
        return;
    }

    if (!m_source) createSource();
    m_actPause->setChecked(false);
    if (m_dsp) QMetaObject::invokeMethod(m_dsp, "resetCounters", Qt::QueuedConnection);
    QMetaObject::invokeMethod(m_source, "applyConfig", Qt::QueuedConnection,
                              Q_ARG(sdr::Config, m_cfg));
    QMetaObject::invokeMethod(m_source, "start", Qt::QueuedConnection);
}

void MainWindow::onPause(bool paused)
{
    if (!m_source) return;
    QMetaObject::invokeMethod(m_source, "setPaused", Qt::QueuedConnection,
                              Q_ARG(bool, paused));
    m_actPause->setText(paused ? QStringLiteral("Resume") : QStringLiteral("Pause"));
}

void MainWindow::onStop()
{
    if (m_source) {
        m_actPause->setChecked(false);
        QMetaObject::invokeMethod(m_source, "stop", Qt::QueuedConnection);
    }
    // Tear the backend down after the reader has been told to stop, so the
    // capture app is not writing into a FIFO nobody is draining.
    if (m_backend && m_backend->isRunning()) m_backend->stop();
    m_backendReady = false;
}

// ------------------------------------------------------ backend bridge


void MainWindow::showNotice(const QString& key, int icon, const QString& title,
                            const QString& text, const QString& details)
{
    QPointer<QMessageBox>& box = m_notices[key];
    if (!box) {
        box = new QMessageBox(this);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setWindowModality(Qt::NonModal);
        box->setStandardButtons(QMessageBox::Ok);
    }
    box->setIcon(static_cast<QMessageBox::Icon>(icon));
    box->setWindowTitle(title);
    box->setText(text);
    box->setDetailedText(details);
    box->show();
    box->raise();
    backendLog(QStringLiteral("[notice] %1: %2").arg(title, text));
}

void MainWindow::backendLog(const QString& line)
{
    if (!m_backendLogFile) {
        const QString path = QDir::tempPath() + QStringLiteral("/sdr_backend.log");
        // Keep one previous session; cap this one so a long run cannot fill /tmp.
        if (QFileInfo(path).size() > (8 << 20)) {
            QFile::remove(path + QStringLiteral(".1"));
            QFile::rename(path, path + QStringLiteral(".1"));
        }
        m_backendLogFile = new QFile(path, this);
        if (!m_backendLogFile->open(QIODevice::Append | QIODevice::Text)) return;
        m_backendLogFile->write(QStringLiteral("\n==== %1 session start ====\n")
            .arg(QDateTime::currentDateTime().toString(Qt::ISODate)).toUtf8());
    }
    if (!m_backendLogFile->isOpen()) return;
    if (m_backendLogFile->size() > (16 << 20)) return;
    m_backendLogFile->write(QStringLiteral("%1 %2\n")
        .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")), line).toUtf8());
    m_backendLogFile->flush();
}

void MainWindow::publishHelperState()
{
    if (!m_window3d) return;
    sdr::H2cState st = m_h2cState;
    QString detail = m_h2cDetail;
    const bool running = m_backend && m_backend->isRunning();
    if (!running) {
        st = sdr::H2cState::NotRunning;
        if (m_cfg.src.mode != SourceMode::Dma)
            detail = QStringLiteral("no live PCIe session (the source is simulation, a file or RoCE)");
        else if (m_backend && m_backend->isStopping())
            detail = QStringLiteral("backend stopping");
        else
            detail = QStringLiteral("backend not running — press Start; the H2C playback "
                                    "helper starts with the capture");
    }
    m_window3d->setH2cHelperState(int(st), detail, running);
}

void MainWindow::ensureBackend()
{
    if (m_backend) return;
    m_backend = new sdr::BackendLauncher(this);
    const QString title = m_sys.udpTransport  ? QStringLiteral("UDP receiver")
                        : m_sys.roceTransport ? QStringLiteral("RoCEv2 receiver")
                                              : QStringLiteral("Backend");

    connect(m_backend, &sdr::BackendLauncher::statusMessage, this, [this](const QString& l) {
        statusBar()->showMessage(l, 5000);
        backendLog(QStringLiteral("[launcher] ") + l);
        if (m_window3d) m_window3d->appendLog(l);
    });
    // Every helper line goes to the log file. The once-a-second rate lines stay
    // out of the status bar, where they overwrote every other message.
    connect(m_backend, &sdr::BackendLauncher::logLine, this, [this](const QString& l) {
        backendLog(l);
        if (l.contains(QLatin1String(" MB/s"))) return;
        statusBar()->showMessage(l, 6000);
    });

    // H2C playback: the generator's loop must match the chunk iwfg_h2c
    // really runs with (the launcher may step it down), and its delivery
    // rate is shown against what the DAC consumes.
    connect(m_backend, &sdr::BackendLauncher::h2cChunkBytesChanged, this, [this](int bytes) {
        m_h2cChunkBytes = bytes;
        if (m_window3d) m_window3d->setH2cChunkBytes(bytes);
    });
    connect(m_backend, &sdr::BackendLauncher::h2cThroughput, this, [this](double mib, int upd) {
        if (m_window3d) m_window3d->setH2cThroughput(mib, upd);
    });
    connect(m_backend, &sdr::BackendLauncher::h2cStateChanged, this,
            [this](sdr::H2cState st, const QString& detail) {
        m_h2cState = st;
        m_h2cDetail = detail;
        backendLog(QStringLiteral("[launcher] H2C %1: %2").arg(sdr::h2cStateName(st), detail));
        publishHelperState();
    });
    connect(m_backend, &sdr::BackendLauncher::c2hRecovering, this,
            [this](const QString& why, int attempt, int limit) {
        const QString msg = QStringLiteral(
            "Capture helper ended — %1. Restarted automatically (%2 of %3 per minute); "
            "the display re-attaches by itself.").arg(why).arg(attempt).arg(limit);
        statusBar()->showMessage(msg, 12000);
        if (m_window3d) m_window3d->appendLog(msg);
    });
    connect(m_backend, &sdr::BackendLauncher::stopped, this, [this] { publishHelperState(); });
    connect(m_backend, &sdr::BackendLauncher::deviceReset, this,
            [this](bool inProgress, const QString& why) {
        m_captureZero = 0;
        const QString msg = inProgress
            ? QStringLiteral("Resetting the DMA device: %1. Capture and transmit restart in a "
                             "few seconds.").arg(why)
            : QStringLiteral("DMA device reset finished (%1).").arg(why);
        statusBar()->showMessage(msg, 10000);
        backendLog(QStringLiteral("[monitor] ") + msg);
        if (m_window3d) m_window3d->appendLog(msg);
    });

    connect(m_backend, &sdr::BackendLauncher::becameReady, this, [this](const QString& fifo) {
        // Point the existing DmaSource at the capture FIFO and start it
        // exactly as a manually configured live run.
        m_cfg.src.devicePath = fifo;
        m_sourcePanel->loadFrom(m_cfg);
        m_backendReady = true;
        onSourceRestartRequired();
        onStart();
        // A generator already running in a capture-only session gets its
        // playback helper now.
        if (m_txState.hostRunning && m_cfg.src.mode == SourceMode::Dma
            && !m_backend->h2cRunning())
            m_backend->ensureH2c();
        publishHelperState();
    });

    connect(m_backend, &sdr::BackendLauncher::startFailed, this, [this, title](const QString& why) {
        m_backendReady = false;
        showNotice(QStringLiteral("backend-start"), QMessageBox::Warning, title, why);
        publishHelperState();
    });

    connect(m_backend, &sdr::BackendLauncher::processFailed, this,
            [this, title](const QString& label, int code, const QString& why, bool fatal) {
        // Only a FATAL failure stops acquisition. An optional stage dying
        // must not take a working capture with it.
        if (fatal) {
            m_backendReady = false;
            if (m_source)
                QMetaObject::invokeMethod(m_source, "stop", Qt::QueuedConnection);
        }
        showNotice(fatal ? QStringLiteral("backend-fatal") : QStringLiteral("backend-optional"),
                   QMessageBox::Warning,
                   fatal ? title : QStringLiteral("%1 — capture continues").arg(title),
                   code >= 0 ? QStringLiteral("%1 stopped (exit %2).\n%3").arg(label).arg(code).arg(why)
                             : QStringLiteral("%1: %2").arg(label, why));
        if (m_window3d) m_window3d->appendLog(QStringLiteral("%1 stopped: %2").arg(label, why));
    });
}

// What the capture delivers against what the configured stream needs. A
// 4-channel 200 MSPS int16 stream is 3052 MiB/s; the bench measured ~2417.
// The shortfall is the FPGA's C2H FIFO overflowing, and the driver's overflow
// recovery soft-resets the QDMA — which also rebuilds the H2C queue, so an
// overloaded capture interrupts transmit. Nothing on screen said so.
void MainWindow::checkCaptureRate(const sdr::StreamStats& stats)
{
    const bool backendUp = m_cfg.src.mode == SourceMode::Dma && m_backend
                           && m_backend->isRunning() && !m_backend->isStopping()
                           && !m_backend->isResetting() && m_backendReady;
    const bool live = backendUp && stats.state == RunState::Running && stats.dmaMBps > 0.0;
    const int ch = std::max(1, m_cfg.src.streamChannels);
    const int bps = bytesPerSample(m_cfg.src.format);
    const double needMib = sdr::captureNeedMib(ch, m_cfg.acq.sampleRateMsps, bps);
    if (m_window3d)
        m_window3d->setCaptureRate(live ? stats.dmaMBps : 0.0, needMib, ch,
                                   m_cfg.acq.sampleRateMsps, bps);

    // ---- stalled capture: the card stopped delivering ---------------------
    // c2h_stream waits in the driver indefinitely when no C2H data arrives, so
    // a wedged device showed only "0.0 MB/s" forever. After ~5 s with nothing,
    // reset the device (ordered close lets the driver soft-reset the QDMA) and
    // start again; if two resets in three minutes do not help, say so.
    if (backendUp && stats.state == RunState::Running && stats.dmaMBps <= 0.01) {
        if (++m_captureZero >= 12) {
            m_captureZero = 0;
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            QVector<qint64> recent;
            for (qint64 t : m_deviceResets) if (now - t < 180000) recent << t;
            m_deviceResets = recent;
            if (m_deviceResets.size() < 2) {
                m_deviceResets << now;
                m_backend->resetDevice(QStringLiteral("the capture delivered nothing for 5 s"));
            } else if (!m_deadNoticeShown) {
                m_deadNoticeShown = true;
                showNotice(QStringLiteral("device-dead"), QMessageBox::Warning,
                    QStringLiteral("Capture stopped"),
                    QStringLiteral(
                        "The card has stopped delivering C2H data, and two device resets "
                        "did not bring it back.\n\nThe QDMA is most likely wedged — the "
                        "iwfg driver's C2H-overflow recovery can leave it so when it fires "
                        "during an H2C transfer. Stop acquisition, reload the driver "
                        "(sudo rmmod iwfg; sudo insmod iwfg.ko) or power-cycle the host, "
                        "then start again with fewer capture channels.\n\nDetails: %1")
                        .arg(QDir::tempPath() + QStringLiteral("/sdr_backend.log")));
            }
        }
    } else {
        m_captureZero = 0;
        if (stats.dmaMBps > 0.01) m_deadNoticeShown = false;
    }

    if (!live || needMib <= 0.0) { m_captureLow = m_captureHigh = 0; return; }

    const double ratio = stats.dmaMBps / needMib;
    m_captureLow  = ratio < 0.90 ? m_captureLow + 1 : 0;
    m_captureHigh = ratio > 1.30 ? m_captureHigh + 1 : 0;

    // A channel change is checked against the measured rate. With few enough
    // channels the capture is not overloaded, so the rate says exactly how
    // many channels the stream now carries — whatever the PL GPIO mode's
    // real meaning is — and the display follows it.
    if (m_chanSwitchTo > 0 && m_chanSwitchClock.isValid() && m_chanSwitchClock.elapsed() > 3000) {
        const int from = m_chanSwitchFrom, to = m_chanSwitchTo;
        m_chanSwitchTo = 0;
        int n = 0;
        const bool counted = sdr::impliedCaptureChannels(stats.dmaMBps, m_cfg.acq.sampleRateMsps,
                                                         bps, &n);
        // Still more data than the new count can carry, but not a whole
        // count (an overloaded 4-channel stream reads as ~3.2): the switch
        // did not take effect.
        if (!counted && ratio > 1.30 && from > to) n = from;
        if (counted || (ratio > 1.30 && from > to)) {
            if (n == to) {
                if (m_window3d)
                    m_window3d->appendLog(QStringLiteral(
                        "Capture now %1 channel(s): %2 MiB/s for %3 MiB/s needed — confirmed.")
                        .arg(to).arg(stats.dmaMBps, 0, 'f', 0).arg(needMib, 0, 'f', 0));
            } else if (n >= from && from > to) {
                const QString msg = QStringLiteral(
                    "The capture still delivers %1 MiB/s — %2 channels' worth — after the "
                    "switch to %3 channel(s), so the PL GPIO mode did not reduce the stream. "
                    "Display and PL GPIO mode restored.")
                    .arg(stats.dmaMBps, 0, 'f', 0).arg(n).arg(to);
                if (m_window3d) m_window3d->appendLog(msg);
                showNotice(QStringLiteral("capture-rate"), QMessageBox::Warning,
                           QStringLiteral("Capture channels"), msg);
                if (m_window3d) {
                    if (m_guardActive)
                        m_window3d->applyPlGpioMode(m_guardPrevMode, QStringLiteral("restored"));
                    else
                        m_window3d->restorePlGpioModeFor(from);
                }
                m_guardActive = false;
                setCaptureChannels(from, /*fromCheck=*/true);
                return;
            } else {
                const QString msg = QStringLiteral(
                    "The capture delivers %1 MiB/s — exactly %2 channels' worth, not %3: "
                    "the display now deinterleaves %2 channels.")
                    .arg(stats.dmaMBps, 0, 'f', 0).arg(n).arg(to);
                if (m_window3d) m_window3d->appendLog(msg);
                backendLog(QStringLiteral("[monitor] ") + msg);
                m_chanSwitchTo = 0;
                m_cfg.src.streamChannels = n;
                if (m_cfg.disp.activeChannel >= n) m_cfg.disp.activeChannel = 0;
                m_sourcePanel->loadFrom(m_cfg);
                onConfigChanged();
                return;
            }
        }
    }

    const int state = m_captureLow >= 5 ? -1 : (m_captureHigh >= 5 ? +1 : (ratio >= 0.9 && ratio <= 1.3 ? 0 : m_captureState));
    if (state == m_captureState) return;
    m_captureState = state;
    if (state < 0) {
        const QString msg = QStringLiteral(
            "Capture overloaded: %1 MiB/s delivered, %2 MiB/s needed (%3 ch × %4 MSPS × %5 B). "
            "The C2H FIFO overflows, and each overflow recovery resets the QDMA — which "
            "also interrupts H2C transmit. Capture fewer channels (H2C control ▸ Transmit).")
            .arg(stats.dmaMBps, 0, 'f', 0).arg(needMib, 0, 'f', 0).arg(ch)
            .arg(m_cfg.acq.sampleRateMsps, 0, 'f', 2).arg(bps);
        statusBar()->showMessage(msg, 15000);
        backendLog(QStringLiteral("[monitor] ") + msg);
        if (m_window3d) m_window3d->appendLog(msg);
    } else if (state > 0) {
        const QString msg = QStringLiteral(
            "The capture delivers %1 MiB/s but %2 ch × %3 MSPS × %4 B needs only %5 MiB/s: "
            "the stream carries more channels (or a higher rate) than configured, so the "
            "display deinterleaves it wrongly. Check Channels in the Source panel.")
            .arg(stats.dmaMBps, 0, 'f', 0).arg(ch).arg(m_cfg.acq.sampleRateMsps, 0, 'f', 2)
            .arg(bps).arg(needMib, 0, 'f', 0);
        statusBar()->showMessage(msg, 15000);
        backendLog(QStringLiteral("[monitor] ") + msg);
        if (m_window3d) m_window3d->appendLog(msg);
    }
}

void MainWindow::transmitGuard(bool starting)
{
    if (m_cfg.src.mode != SourceMode::Dma) return;
    if (!starting) {
        if (!m_guardActive) return;
        m_guardActive = false;
        m_chanSwitchTo = 0;                  // a pending check is moot now
        const QString msg = QStringLiteral(
            "Generator stopped — capture back to %1 channels (PL GPIO mode %2).")
            .arg(m_guardPrevCh).arg(m_guardPrevMode);
        if (m_window3d && m_window3d->rfdcAttached())
            m_window3d->applyPlGpioMode(m_guardPrevMode, QStringLiteral("transmit ended"));
        setCaptureChannels(m_guardPrevCh, /*fromCheck=*/true);
        statusBar()->showMessage(msg, 8000);
        if (m_window3d) m_window3d->appendLog(msg);
        return;
    }
    if (!m_backend || !m_backend->isRunning()) return;

    const int ch = std::max(1, m_cfg.src.streamChannels);
    const int bps = bytesPerSample(m_cfg.src.format);
    const double rate = m_cfg.acq.sampleRateMsps;
    const int target = sdr::captureChannelsWithinBudget(ch, rate, bps);
    if (target >= ch) return;

    const double need = sdr::captureNeedMib(ch, rate, bps);
    if (!m_window3d || !m_window3d->rfdcAttached()) {
        const QString msg = QStringLiteral(
            "Transmit with a %1-channel capture: it needs %2 MiB/s, more than this host "
            "drains (budget %3 MiB/s). Every C2H overflow makes the driver reset the QDMA, "
            "which interrupts H2C transmit and can wedge the device. The GUI cannot reduce "
            "the capture because the RFDC tab is not attached. Attach it, or set PL GPIO "
            "mode %4 (serial menu 10) and Channels = %5 in the Source panel.")
            .arg(ch).arg(need, 0, 'f', 0).arg(sdr::kCaptureBudgetMib, 0, 'f', 0)
            .arg(sdr::plGpioModeForChannels(target)).arg(target);
        showNotice(QStringLiteral("capture-guard"), QMessageBox::Warning,
                   QStringLiteral("Capture overload while transmitting"), msg);
        if (m_window3d) m_window3d->appendLog(msg);
        return;
    }

    m_guardActive = true;
    m_guardPrevCh = ch;
    m_guardPrevMode = m_window3d->plGpioMode() > 0 ? m_window3d->plGpioMode()
                                                   : sdr::plGpioModeForChannels(ch);
    const int mode = sdr::plGpioModeForChannels(target);
    const QString msg = QStringLiteral(
        "Transmit guard: capture reduced from %1 to %2 channel(s) (PL GPIO mode %3) while "
        "the generator runs. %1 × %4 MSPS needs %5 MiB/s, more than this host drains, and "
        "every C2H overflow makes the driver reset the QDMA under the H2C transfer. "
        "Kept: stream channel(s) 0%6 — AXIS output(s) 0%6; ADC Channel Routing on the "
        "RFDC tab decides which ADC inputs those are, so route your loopback ADC there. "
        "Restored when the generator stops.")
        .arg(ch).arg(target).arg(mode).arg(rate, 0, 'f', 2).arg(need, 0, 'f', 0)
        .arg(target > 1 ? QStringLiteral("–%1").arg(target - 1) : QString());
    m_window3d->applyPlGpioMode(mode, QStringLiteral("transmit guard"));
    setCaptureChannels(target);
    statusBar()->showMessage(msg, 12000);
    backendLog(QStringLiteral("[monitor] ") + msg);
    m_window3d->appendLog(msg);
}

void MainWindow::setCaptureChannels(int n, bool fromCheck)
{
    n = std::clamp(n, 1, 8);
    const int was = std::max(1, m_cfg.src.streamChannels);
    if (n == was) return;
    m_cfg.src.streamChannels = n;
    if (m_cfg.disp.activeChannel >= n) m_cfg.disp.activeChannel = 0;
    m_sourcePanel->loadFrom(m_cfg);
    onConfigChanged();                       // DmaSource re-frames in place
    m_captureLow = m_captureHigh = 0;
    m_captureState = 0;
    if (!fromCheck) {                         // verify it against the measured rate
        m_chanSwitchFrom = was;
        m_chanSwitchTo = n;
        m_chanSwitchClock.restart();
    }
    const QString msg = QStringLiteral("Capture set to %1 channel(s) (was %2).").arg(n).arg(was);
    statusBar()->showMessage(msg, 6000);
    if (m_window3d) m_window3d->appendLog(msg);
}

void MainWindow::startUdpThenAcquire()
{
    ensureBackend();
    if (m_backend->isRunning() && !m_backend->isStopping()) {
        statusBar()->showMessage(QStringLiteral("UDP receiver is starting…"), 4000);
        return;
    }

    // Build on first use, so a fresh checkout does not need a separate step.
    if (sdr::BackendLauncher::udpBinaryPath().isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Building UDP receiver…"));
        QApplication::setOverrideCursor(Qt::WaitCursor);
        bool ok = false;
        const QString log = sdr::BackendLauncher::buildUdpBinary(&ok);
        QApplication::restoreOverrideCursor();
        if (!ok) {
            showNotice(QStringLiteral("backend-build"), QMessageBox::Warning,
                       QStringLiteral("UDP receiver"),
                       QStringLiteral("Could not build the UDP receiver."), log);
            return;
        }
    }

    // A busy port makes the receiver exit immediately; say so plainly rather
    // than surfacing an opaque exit code.
    QString why;
    if (!m_backend->isStopping() && !sdr::BackendLauncher::udpPortAvailable(m_sys.udpPort, &why)) {
        showNotice(QStringLiteral("udp-port"), QMessageBox::Warning,
            QStringLiteral("UDP port unavailable"),
            QStringLiteral("Port %1 is already in use (%2).\n\n"
                           "Another receiver may already be running. Choose a "
                           "different port in the launcher, or stop the process "
                           "holding it.").arg(m_sys.udpPort).arg(why));
        return;
    }

    sdr::BackendConfig cfg;
    cfg.mode        = sdr::BackendMode::UdpStream;
    cfg.udpPort     = m_sys.udpPort;
    cfg.udpFifo     = m_sys.udpFifo.isEmpty() ? QStringLiteral("/tmp/iwfg_c2h.fifo")
                                              : m_sys.udpFifo;
    cfg.udpRawIface = m_sys.udpInterface;      // empty = plain UDP socket
    m_backend->start(cfg);
}

void MainWindow::startRoceThenAcquire()
{
    ensureBackend();
    if (m_backend->isRunning() && !m_backend->isStopping()) {
        statusBar()->showMessage(QStringLiteral("RoCEv2 receiver is starting…"), 4000);
        return;
    }

    const bool gpu = m_cfg.src.useGpu;

    if (sdr::BackendLauncher::roceDirectory().isEmpty()) {
        showNotice(QStringLiteral("roce-missing"), QMessageBox::Warning,
                   QStringLiteral("RoCEv2 receiver"),
                   QStringLiteral(
                       "The supplied RoCEv2 stack was not found next to the "
                       "application (reference/roce-iq-holoscan).\n\n"
                       "Start a receiver yourself, or run the GUI from a tree "
                       "that still has it."));
        return;
    }

    // Build on first use, so a fresh checkout does not need a separate step.
    if (sdr::BackendLauncher::roceReceiverPath(gpu).isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Building RoCEv2 receiver…"));
        QApplication::setOverrideCursor(Qt::WaitCursor);
        bool ok = false;
        const QString log = sdr::BackendLauncher::buildRoceReceiver(gpu, &ok);
        QApplication::restoreOverrideCursor();
        if (!ok) {
            showNotice(QStringLiteral("roce-build"), QMessageBox::Warning,
                       QStringLiteral("RoCEv2 receiver"),
                       gpu ? QStringLiteral(
                                 "Could not build rdma_rx_gpu. It needs the CUDA "
                                 "toolkit as well as rdma-core; without it, use "
                                 "the host ring (rdma_rx).")
                           : QStringLiteral(
                                 "Could not build rdma_rx. It needs rdma-core "
                                 "development files (libibverbs, librdmacm) — see "
                                 "reference/roce-iq-holoscan/scripts/01_install_base.sh."),
                       log);
            return;
        }
    }

    // A launched receiver always publishes at /dev/shm/iqring: rdma_rx
    // hard-codes RING_SHM_NAME, so the source has to be pointed there
    // whatever the device field held.
    const QString ring = sdr::roceRingPath();
    if (m_cfg.src.devicePath != ring) {
        m_cfg.src.devicePath = ring;
        if (m_source)
            QMetaObject::invokeMethod(m_source, "applyConfig", Qt::QueuedConnection,
                                      Q_ARG(sdr::Config, m_cfg));
    }

    sdr::BackendConfig cfg;
    cfg.mode          = sdr::BackendMode::RoceRx;
    cfg.roceGpu       = gpu;
    cfg.rocePort      = m_sys.rocePort > 0 ? m_sys.rocePort : 7471;
    cfg.roceBindAddr  = m_sys.roceBindAddr;
    m_backend->start(cfg);
}

void MainWindow::startBackendThenAcquire()
{
    ensureBackend();
    // Start pressed again before the backend reported ready: it is on its way.
    if (m_backend->isRunning() && !m_backend->isStopping()) {
        statusBar()->showMessage(QStringLiteral("Backend is starting…"), 4000);
        return;
    }

    // The data-flow mode is already chosen in the launcher (DATA FLOW:
    // C2H / H2C / C2H+H2C), so asking again here was duplicate work — and its
    // Cancel path returned silently, leaving the application idle with no
    // explanation. The launcher's choice is authoritative.
    // "H2C DAC Only" used to map to capture-only, so the transmit helper was
    // never launched and the generator had nobody to send to. Any selection
    // with a transmit path now runs iwfg_h2c (the capture runs too: it is
    // what shows the loopback). An ADC-only session starts it on demand when
    // the generator is started (BackendLauncher::ensureH2c).
    m_backendMode = (m_sys.dataFlowMode != 0) ? sdr::BackendMode::H2C_C2H
                                              : sdr::BackendMode::C2H;
    statusBar()->showMessage(QStringLiteral("Starting %1 acquisition…")
        .arg(sdr::backendModeName(m_backendMode)));

    // Build the helpers on first use rather than making the operator do it.
    if (!sdr::BackendLauncher::binariesAvailable()) {
        statusBar()->showMessage(QStringLiteral("Building backend helpers…"));
        QApplication::setOverrideCursor(Qt::WaitCursor);
        bool ok = false;
        const QString log = sdr::BackendLauncher::buildBinaries(&ok);
        QApplication::restoreOverrideCursor();
        if (!ok) {
            showNotice(QStringLiteral("backend-build"), QMessageBox::Warning,
                       QStringLiteral("Backend"),
                       QStringLiteral("Could not build the backend helper applications."),
                       log);
            statusBar()->showMessage(
                QStringLiteral("Live acquisition not started — backend helpers "
                               "could not be built."), 10000);
            return;
        }
    }

    sdr::BackendConfig cfg;
    cfg.mode = m_backendMode;
    // Honour the device node already configured in the source panel.
    if (!m_cfg.src.devicePath.isEmpty()
        && m_cfg.src.devicePath.startsWith(QLatin1String("/dev/")))
        cfg.c2hDevice = m_cfg.src.devicePath;
    // Environment overrides, for a second card / other minors, or a second
    // instance that must not share the /tmp FIFOs.
    cfg.c2hDevice = qEnvironmentVariable("IWFG_C2H_DEVICE", cfg.c2hDevice);
    cfg.h2cDevice = qEnvironmentVariable("IWFG_H2C_DEVICE", cfg.h2cDevice);
    cfg.c2hFifo   = qEnvironmentVariable("IWFG_C2H_FIFO",   cfg.c2hFifo);
    cfg.h2cFifo   = qEnvironmentVariable("IWFG_H2C_FIFO",   cfg.h2cFifo);
    if (m_window3d) m_window3d->setH2cFifoPath(cfg.h2cFifo);

    // Check the node before launching. Without this, c2h_stream fails to open
    // it and exits immediately, and the only symptom is a window that never
    // plots anything — which is far harder to diagnose than a message.
    if (!QFileInfo::exists(cfg.c2hDevice)) {
        showNotice(QStringLiteral("device-missing"), QMessageBox::Warning,
            QStringLiteral("Device node missing"),
            QStringLiteral("%1 does not exist, so the capture application "
                           "cannot open it.\n\n"
                           "Load the iwfg driver (Acquisition ▸ PCIe discovery "
                           "offers to do this), or correct the device path in "
                           "the Source panel.").arg(cfg.c2hDevice));
        statusBar()->showMessage(
            QStringLiteral("Live acquisition not started — %1 missing.")
                .arg(cfg.c2hDevice), 10000);
        return;
    }

    m_backend->start(cfg);
}

void MainWindow::onRunState(sdr::RunState state)
{
    m_lastStats.state = state;

    const bool live = (state == RunState::Running || state == RunState::Paused);
    m_actStart->setEnabled(!live);
    m_actStop->setEnabled(live);
    m_actPause->setEnabled(live);
    m_sourcePanel->setRunState(state);

    m_sbState->setText(runStateName(state));
    QColor c = theme::textDim;
    switch (state) {
    case RunState::Running: c = theme::ok;     break;
    case RunState::Paused:  c = theme::warn;   break;
    case RunState::Error:   c = theme::danger; break;
    case RunState::Stopped: c = theme::textDim; break;
    }
    m_sbState->setStyleSheet(QStringLiteral(
        "QLabel#statePill { color: %1; border: 1px solid %1; border-radius: 3px; "
        "padding: 3px 10px; font-weight: bold; }").arg(c.name()));

    m_statusPanel->setStats(m_lastStats, m_displayFps);
}

void MainWindow::onSourceError(const QString& text)
{
    statusBar()->showMessage(text, 8000);
    showNotice(QStringLiteral("acquisition"), QMessageBox::Warning,
               QStringLiteral("Acquisition"), text);
}

void MainWindow::onStats(const sdr::StreamStats& stats)
{
    // ---- silent-idle watchdog ------------------------------------------
    // A live session that receives nothing shows an empty window and no
    // error, which is the hardest possible thing to diagnose. If the source
    // is running but no block has arrived after a grace period, say so once
    // and list the causes worth checking, in order of likelihood.
    if (stats.state == RunState::Running) {
        if (stats.blocksIn > 0) {
            m_idleWatchStart = 0;          // data flowing: reset
            m_idleWarned = false;
        } else {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (m_idleWatchStart == 0) m_idleWatchStart = now;
            else if (!m_idleWarned && now - m_idleWatchStart > 6000) {
                m_idleWarned = true;
                const QString src = m_cfg.src.devicePath;
                statusBar()->showMessage(
                    QStringLiteral("No data received from %1 after 6 s — see the "
                                   "warning for what to check.").arg(src), 15000);
                showNotice(QStringLiteral("no-data"), QMessageBox::Warning,
                    QStringLiteral("No data received"),
                    QStringLiteral(
                        "Acquisition is running but no samples have arrived from "
                        "%1.\n\nCheck, in order:\n\n"
                        "1. Is the capture application still running? Its output "
                        "appears in the status bar and in %2.\n"
                        "2. Is the FPGA actually transmitting? A card that is "
                        "armed but idle produces no DMA completions.\n"
                        "3. Does the wire format match the hardware? A mismatch "
                        "usually plots noise, but a wrong channel count can stall "
                        "the framing.\n"
                        "4. For UDP: is traffic reaching the port? Confirm with "
                        "tcpdump, and use Raw capture if the destination MAC or "
                        "IP checksum is not the host's.")
                        .arg(src, QDir::tempPath() + QStringLiteral("/sdr_backend.log")));
            }
        }
    } else {
        m_idleWatchStart = 0;
        m_idleWarned = false;
    }

    checkCaptureRate(stats);

    m_lastStats = stats;
    m_lastStats.recording     = m_actRecord->isChecked();
    m_lastStats.recordedBytes = m_recordedBytes;

    m_statusPanel->setStats(m_lastStats, m_displayFps);
    m_sourcePanel->setStats(m_lastStats);

    m_sbSource->setText(stats.sourceName);
    m_sbThrough->setText(QStringLiteral("%1 MB/s").arg(stats.dmaMBps, 0, 'f', 1));
    // Rate, not just the running total. Some discard is inherent: the display
    // consumes ~0.25 % of a 200 MSPS stream, so a non-zero count is normal and
    // colouring it red on the first dropped block cries wolf. Alarm above 5 %,
    // which is where the pipeline is genuinely failing to keep up.
    const double dropPct = stats.dropRatePct();
    m_sbDrops->setText(QStringLiteral("drops %1 (%2%)")
                           .arg(stats.samplesDropped).arg(dropPct, 0, 'f', 2));
    m_sbDrops->setStyleSheet(dropPct >= 5.0
        ? QStringLiteral("color: %1;").arg(theme::danger.name())
        : (dropPct >= 1.0 ? QStringLiteral("color: %1;").arg(theme::warn.name())
                          : QString()));
    m_sbBuffer->setValue(static_cast<int>(std::clamp(stats.bufferPct, 0.0, 100.0)));
}

void MainWindow::onFrameReady(const sdr::FrameResult& frame)
{
    // Store and mark dirty only. Painting happens on the render timer, so if
    // the DSP stage delivers faster than the compositor can keep up, the extra
    // frames coalesce instead of queueing repaints the user has to wait out.
    // This is what made the window go unresponsive under fast acquisition:
    // every frame synchronously repainted four plots on the GUI thread.
    m_lastFrame    = frame;
    m_framePending = true;
}

void MainWindow::renderPending()
{
    if (!m_framePending) {
        if (m_renderGate) m_renderGate->storeRelease(0);
        return;
    }
    m_framePending = false;

    const sdr::FrameResult& frame = m_lastFrame;

    for (InstrumentPlot* p : plots())
        if (p->isVisible()) p->setFrame(frame);

    // The tables carry far more text than the plots and do not need to be
    // rewritten at the full frame rate to look live.
    if (m_tableClock.elapsed() >= kTableUpdateMs) {
        m_tableClock.restart();
        m_measurePanel->setActiveChannel(m_cfg.disp.activeChannel);
        m_measurePanel->setFrame(frame);

        // What the receiver sees, for the H2C window's loopback view: it can
        // then say whether the received tone is the one being transmitted.
        if (m_window3d && !frame.channels.empty()) {
            const sdr::ChannelFrame* ch = &frame.channels.front();
            for (const auto& c : frame.channels)
                if (c.channel == m_cfg.disp.activeChannel) { ch = &c; break; }
            const double bin = frame.displayRateHz / std::max(1, m_cfg.disp.fftSize);
            m_window3d->setReceivedPeak(ch->metrics.peakFreqHz - frame.centerFreqHz,
                                        ch->metrics.peakDbfs, ch->metrics.toneValid,
                                        m_cfg.acq.sampleRateHz(), bin);
        }
    }

    // The engine may queue the next frame now that this one is on screen.
    if (m_renderGate) m_renderGate->storeRelease(0);

    ++m_framesSinceTick;
    if (m_fpsClock.elapsed() >= 1000) {
        m_displayFps = m_framesSinceTick * 1000.0 / m_fpsClock.elapsed();
        m_framesSinceTick = 0;
        m_fpsClock.restart();

        StreamStats s = frame.stats;
        s.recording     = m_actRecord->isChecked();
        s.recordedBytes = m_recordedBytes;
        s.state         = m_lastStats.state;
        m_statusPanel->setStats(s, m_displayFps);
    }
}

// ================================================================ file I/O

void MainWindow::onOpenCapture()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open capture file"), QString(),
        QStringLiteral("Capture files (*.bin *.dat *.iq *.cf32 *.cs16 *.raw);;All files (*)"));
    if (path.isEmpty()) return;

    onStop();
    m_sourcePanel->loadFrom([&] {
        Config c = m_cfg;
        c.src.mode     = SourceMode::File;
        c.src.filePath = path;
        return c;
    }());

    // Restore the rate, format and resampling the capture was taken with;
    // otherwise a file recorded at a decimated rate is replayed and
    // down-converted against whatever happens to be on screen.
    m_sourcePanel->applySidecar(path);

    onSourceRestartRequired();
    onStart();
    statusBar()->showMessage(QStringLiteral("Offline mode — %1")
                                 .arg(QFileInfo(path).fileName()), 5000);
}

void MainWindow::onToggleRecord(bool on)
{
    if (!on) {
        QMetaObject::invokeMethod(m_recorder, "stopRecording", Qt::QueuedConnection);
        m_actRecord->setText(QStringLiteral("Record"));
        if (m_recGuard) m_recGuard->endSession();
        if (m_resBar)   m_resBar->setRecording(false);
        return;
    }

    const QString suggested = QStringLiteral("capture_%1.bin")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Record stream to"), suggested,
        QStringLiteral("Capture files (*.bin *.dat *.iq);;All files (*)"));

    if (path.isEmpty()) {
        m_actRecord->setChecked(false);
        return;
    }

    // ---- storage-limit configuration and validation ---------------------
    // Asked BEFORE recording starts, and checked against the real free space
    // of the destination volume rather than a system-wide figure.
    const QString outDir = QFileInfo(path).absolutePath();
    if (m_resMon) m_resMon->setDiskPath(outDir);
    const sdr::DiskSample disk = m_resMon ? m_resMon->disk() : sdr::DiskSample{};
    const quint64 safe = sdr::RecordingGuard::safeCapacity(disk);

    bool okInput = false;
    const double suggestedMiB =
        std::min(5000.0, safe ? double(safe) / (1024.0 * 1024.0) * 0.5 : 5000.0);
    const double limitMiB = QInputDialog::getDouble(
        this, QStringLiteral("Recording storage limit"),
        QStringLiteral("Maximum storage this recording may consume (MB).\n"
                       "Destination: %1\nSafely available: %2")
            .arg(outDir, sdr::ResourceMonitor::formatBytes(safe)),
        suggestedMiB, 1.0, 100000000.0, 0, &okInput);
    if (!okInput) { m_actRecord->setChecked(false); return; }

    const auto findings = sdr::RecordingGuard::validateLimit(limitMiB, outDir, disk);
    bool blocking = false;
    for (const auto& f : findings) if (f.severity == sdr::RecWarning::Critical) blocking = true;
    if (!findings.isEmpty()) {
        const auto& f = findings.first();
        QMessageBox box(this);
        box.setIcon(blocking ? QMessageBox::Critical : QMessageBox::Warning);
        box.setWindowTitle(f.title);
        // What is wrong / why / how to fix — not a bare refusal.
        box.setText(QStringLiteral("<b>%1</b><br><br>%2").arg(f.title, f.what));
        box.setInformativeText(QStringLiteral("%1\n\nRecommended: %2").arg(f.why, f.fix));
        if (blocking) {
            box.setStandardButtons(QMessageBox::Ok);
            box.exec();
            m_actRecord->setChecked(false);
            return;                      // never silently accept an unsafe limit
        }
        box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
        if (box.exec() == QMessageBox::Cancel) { m_actRecord->setChecked(false); return; }
    }

    if (!m_recGuard) {
        m_recGuard = new sdr::RecordingGuard(this);
        connect(m_recGuard, &sdr::RecordingGuard::warningRaised, this,
                [this](const sdr::RecWarning& w) {
                    statusBar()->showMessage(QStringLiteral("%1 — %2").arg(w.title, w.fix),
                                             8000);
                });
        connect(m_recGuard, &sdr::RecordingGuard::limitReached, this,
                [this](quint64, quint64) {
                    // Stop cleanly at the limit rather than consuming the disk.
                    if (m_actRecord->isChecked()) m_actRecord->setChecked(false);
                    QMessageBox::information(this, QStringLiteral("Recording stopped"),
                        QStringLiteral("The configured storage limit was reached and "
                                       "the recording was stopped."));
                });
    }
    m_recGuard->beginSession(quint64(limitMiB * 1024.0 * 1024.0));
    if (m_resBar) m_resBar->setRecording(true);

    m_recordedBytes = 0;
    m_actRecord->setText(QStringLiteral("Recording…"));
    QMetaObject::invokeMethod(m_recorder, "startRecording", Qt::QueuedConnection,
                              Q_ARG(QString, path), Q_ARG(sdr::Config, m_cfg));
}

void MainWindow::onExportIq()
{
    const ChannelFrame* cf = m_lastFrame.channel(m_cfg.disp.activeChannel);
    if (!cf || cf->iq.empty()) {
        statusBar()->showMessage(QStringLiteral("No samples buffered yet"), 3000);
        return;
    }

    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export displayed I/Q"),
        QStringLiteral("frame_%1.cf32")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss"))),
        QStringLiteral("Interleaved complex float32 (*.cf32)"));
    if (path.isEmpty()) return;

    std::ofstream out(path.toStdString(), std::ios::binary);
    if (!out) {
        statusBar()->showMessage(QStringLiteral("Could not open %1").arg(path), 4000);
        return;
    }
    out.write(reinterpret_cast<const char*>(cf->iq.data()),
              static_cast<std::streamsize>(cf->iq.size() * sizeof(cf32)));
    statusBar()->showMessage(
        QStringLiteral("Wrote %1 samples to %2").arg(cf->iq.size()).arg(path), 4000);
}

void MainWindow::onScreenshot()
{
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save screenshot"),
        QStringLiteral("workstation_%1.png")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss"))),
        QStringLiteral("PNG image (*.png)"));
    if (path.isEmpty()) return;

    statusBar()->showMessage(grab().save(path)
        ? QStringLiteral("Screenshot written to %1").arg(path)
        : QStringLiteral("Could not write %1").arg(path), 4000);
}

// ================================================================= layout

void MainWindow::setPlotsVisible(bool time, bool spectrum, bool waterfall, bool constellation)
{
    m_timePlot->setVisible(time);
    m_spectrumPlot->setVisible(spectrum);
    m_waterfallPlot->setVisible(waterfall);
    m_constPlot->setVisible(constellation);
}

void MainWindow::applyLayoutPreset(int preset)
{
    switch (preset) {
    case 1:   // spectrum focus
        setPlotsVisible(false, true, true, false);
        m_mainSplit->setSizes({560, 300});
        break;
    case 2:   // time domain focus
        setPlotsVisible(true, false, false, true);
        m_mainSplit->setSizes({560, 300});
        break;
    case 3:   // spectrum + waterfall stacked
        setPlotsVisible(false, true, true, false);
        m_mainSplit->setSizes({400, 460});
        break;
    default:  // four-up
        setPlotsVisible(true, true, true, true);
        m_topSplit->setSizes({600, 700});
        m_bottomSplit->setSizes({760, 540});
        m_mainSplit->setSizes({430, 430});
        break;
    }
}

void MainWindow::onResetLayout()
{
    for (QDockWidget* d : {m_sourceDock, m_displayDock, m_statusDock, m_measureDock}) {
        d->setFloating(false);
        d->show();
    }
    addDockWidget(Qt::LeftDockWidgetArea, m_sourceDock);
    addDockWidget(Qt::RightDockWidgetArea, m_displayDock);
    addDockWidget(Qt::RightDockWidgetArea, m_statusDock);
    splitDockWidget(m_displayDock, m_statusDock, Qt::Vertical);
    addDockWidget(Qt::BottomDockWidgetArea, m_measureDock);
    applyLayoutPreset(0);
    statusBar()->showMessage(QStringLiteral("Layout reset"), 2000);
}

void MainWindow::saveSettings()
{
    QSettings s;
    s.setValue(QStringLiteral("geometry"), saveGeometry());
    s.setValue(QStringLiteral("windowState"), saveState());
    s.setValue(QStringLiteral("splitMain"), m_mainSplit->saveState());
    s.setValue(QStringLiteral("splitTop"), m_topSplit->saveState());
    s.setValue(QStringLiteral("splitBottom"), m_bottomSplit->saveState());
}

void MainWindow::restoreSettings()
{
    QSettings s;
    if (s.contains(QStringLiteral("geometry")))
        restoreGeometry(s.value(QStringLiteral("geometry")).toByteArray());
    if (s.contains(QStringLiteral("windowState")))
        restoreState(s.value(QStringLiteral("windowState")).toByteArray());
    if (s.contains(QStringLiteral("splitMain")))
        m_mainSplit->restoreState(s.value(QStringLiteral("splitMain")).toByteArray());
    if (s.contains(QStringLiteral("splitTop")))
        m_topSplit->restoreState(s.value(QStringLiteral("splitTop")).toByteArray());
    if (s.contains(QStringLiteral("splitBottom")))
        m_bottomSplit->restoreState(s.value(QStringLiteral("splitBottom")).toByteArray());
}

} // namespace sdr::ui
