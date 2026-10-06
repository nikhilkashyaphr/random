#pragma once
#include "../core/RecordingGuard.h"
#include "../core/ResourceMonitor.h"
#include "../core/Types.h"
#include "../core/BackendLauncher.h"
#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QAtomicInt>
#include <QPointer>
#include <memory>

class QAction;
class QDockWidget;
class QTimer;
class QPushButton;
class QToolButton;
class QLabel;
class QProgressBar;
class QSplitter;
class QThread;
class QMessageBox;
class QFile;

namespace sdr {
class ISignalSource;
class DspEngine;
class Recorder;
class DriverManager;
}

namespace sdr::ui {

class TitleBar;
class ControlWindow3D;
class ResourceBar;

class SourcePanel;
class DisplayPanel;
class MeasurementPanel;
class StatusPanel;
class InstrumentPlot;
class TimeDomainPlot;
class SpectrumPlot;
class WaterfallPlot;
class ConstellationPlot;

/// The workstation shell: dockable panels around a resizable plot grid, with
/// the acquisition and DSP pipeline living on their own threads.
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(const sdr::SystemConfig& sys, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private slots:
    void onFrameReady(const sdr::FrameResult& frame);
    /// Paints the most recent frame. Decoupled from onFrameReady so a burst of
    /// frames costs one repaint, not one repaint each.
    void renderPending();
    void onStats(const sdr::StreamStats& stats);
    void onRunState(sdr::RunState state);
    void onSourceError(const QString& text);
    void onConfigChanged();
    void onSourceRestartRequired();

    void onStart();
    void startBackendThenAcquire();
    void startUdpThenAcquire();
    void onPause(bool paused);
    void onStop();
    void onToggleRecord(bool on);
    void onOpenCapture();
    void onExportIq();
    void onScreenshot();
    void onResetLayout();

private:
    void buildActions();
    void buildMenus();
    void buildToolBar();
    void buildDocks();
    void buildCentral();
    void buildStatusBar();
    void buildPipeline();

    void createSource();
    void destroySource();
    void pushConfig();
    /// Create the backend launcher once, with every connection. The UDP and
    /// PCIe paths used to wire it separately, and differently.
    void ensureBackend();
    /// A non-modal, de-duplicated notice. The modal boxes these replace
    /// blocked every other window until dismissed, and a burst of backend
    /// events stacked one box per event.
    void showNotice(const QString& key, int icon, const QString& title,
                    const QString& text, const QString& details = QString());
    /// Append to the backend log file (QDir::tempPath()/sdr_backend.log).
    void backendLog(const QString& line);
    /// Compare what the capture delivers with what the configured stream
    /// needs (channels x rate x sample size) and warn on a shortfall.
    void checkCaptureRate(const sdr::StreamStats& stats);
    /// Control-window request: capture `n` channels (PL GPIO mode set there).
    void setCaptureChannels(int n, bool fromCheck = false);
    /// Generator started / stopped in a live session: keep the capture
    /// within what the host drains while transmitting (see CaptureBudget.h),
    /// and put it back afterwards.
    void transmitGuard(bool starting);
    void publishHelperState();
    void applyLayoutPreset(int preset);
    void setPlotsVisible(bool time, bool spectrum, bool waterfall, bool constellation);
    void saveSettings();
    void restoreSettings();
    QList<InstrumentPlot*> plots() const;

    sdr::SystemConfig    m_sys;
    sdr::DriverManager*  m_driverMgr = nullptr;  ///< owns exit-time rmmod
    sdr::BackendLauncher* m_backend   = nullptr;  ///< external C2H/H2C apps
    TitleBar*            m_titleBar   = nullptr;  ///< our own, logo-bearing
    QAction*             m_act3d      = nullptr;  ///< menu-bar H2C control
    ResourceBar*         m_resBar     = nullptr;
    QToolButton*         m_btnH2c     = nullptr;
    sdr::ResourceMonitor* m_resMon    = nullptr;
    sdr::RecordingGuard*  m_recGuard  = nullptr;
    ControlWindow3D*     m_window3d   = nullptr;  ///< created on first use
    /// Last transmit-side state published by the control window. Kept here so
    /// a source created later (mode switch, restart) is told immediately.
    sdr::TxState         m_txState;
    /// Chunk size the backend launched iwfg_h2c with (0 = not launched). The
    /// control window's generator must use the same loop length.
    int                  m_h2cChunkBytes = 0;
    sdr::BackendMode     m_backendMode = sdr::BackendMode::C2H;
    bool                 m_backendReady = false;
    qint64               m_idleWatchStart = 0;   ///< watchdog for a silent live source
    bool                 m_idleWarned = false;
    sdr::Config       m_cfg;

    QThread* m_sourceThread = nullptr;
    QThread* m_dspThread    = nullptr;
    QThread* m_recThread    = nullptr;

    sdr::ISignalSource* m_source   = nullptr;
    sdr::DspEngine*     m_dsp      = nullptr;
    sdr::Recorder*      m_recorder = nullptr;
    sdr::BackPressurePtr m_backPressure;

    SourcePanel*      m_sourcePanel = nullptr;
    DisplayPanel*     m_displayPanel= nullptr;
    MeasurementPanel* m_measurePanel= nullptr;
    StatusPanel*      m_statusPanel = nullptr;

    QDockWidget* m_sourceDock  = nullptr;
    QDockWidget* m_displayDock = nullptr;
    QDockWidget* m_statusDock  = nullptr;
    QDockWidget* m_measureDock = nullptr;

    TimeDomainPlot*    m_timePlot      = nullptr;
    SpectrumPlot*      m_spectrumPlot  = nullptr;
    WaterfallPlot*     m_waterfallPlot = nullptr;
    ConstellationPlot* m_constPlot     = nullptr;
    QSplitter*         m_topSplit      = nullptr;
    QSplitter*         m_bottomSplit   = nullptr;
    QSplitter*         m_mainSplit     = nullptr;

    QAction* m_actStart   = nullptr;
    QAction* m_actPause   = nullptr;
    QAction* m_actStop    = nullptr;
    QAction* m_actRecord  = nullptr;
    QAction* m_actOpen    = nullptr;

    QLabel*       m_sbState    = nullptr;
    QLabel*       m_sbSource   = nullptr;
    QLabel*   m_sbCompute = nullptr;   ///< CPU / GPU, from DspEngine
    QLabel*       m_sbThrough  = nullptr;
    QLabel*       m_sbDrops    = nullptr;
    QLabel*       m_sbReadout  = nullptr;
    QProgressBar* m_sbBuffer   = nullptr;

    QElapsedTimer m_fpsClock;
    QElapsedTimer m_tableClock;
    int           m_framesSinceTick = 0;
    double        m_displayFps      = 0.0;
    sdr::FrameResult m_lastFrame;
    bool             m_framePending = false;
    std::shared_ptr<QAtomicInt> m_renderGate;
    QTimer*          m_renderTimer  = nullptr;
    sdr::StreamStats m_lastStats;
    quint64          m_recordedBytes = 0;
    bool             m_linking = false;
    bool             m_restarting = false;

    // ---- 1.2.4: backend supervision ---------------------------------------
    QHash<QString, QPointer<QMessageBox>> m_notices;
    QFile*           m_backendLogFile = nullptr;
    /// Which kind of source m_source is (its mode when it was created).
    sdr::SourceMode  m_sourceKind = sdr::SourceMode::Simulated;
    sdr::H2cState    m_h2cState = sdr::H2cState::NotRunning;
    QString          m_h2cDetail;
    /// Consecutive stats updates with the capture below / above what the
    /// configured stream needs.
    int              m_captureLow  = 0;
    int              m_captureHigh = 0;
    int              m_captureState = 0;      ///< -1 short, 0 ok, +1 excess
    /// A channel-count change made from the control window, verified against
    /// the measured capture rate a few seconds later.
    int              m_chanSwitchFrom = 0;
    int              m_chanSwitchTo   = 0;
    QElapsedTimer    m_chanSwitchClock;
    // ---- 1.2.5 ------------------------------------------------------------
    bool             m_guardActive   = false;   ///< capture reduced for transmit
    int              m_guardPrevCh   = 0;
    int              m_guardPrevMode = 0;
    int              m_captureZero   = 0;       ///< consecutive stats with no data
    QVector<qint64>  m_deviceResets;            ///< ms since epoch
    bool             m_deadNoticeShown = false;
};

} // namespace sdr::ui
