#pragma once
// ---------------------------------------------------------------------------
// ControlWindow3D — floating, non-modal control panel opened from the main GUI.
//
// The controls that will live inside this window have not been specified yet.
// The brief is explicit that they arrive later, so this file deliberately does
// NOT invent them. What it provides is the architecture:
//
//   ControlWindow3D
//     ├── viewport   (QWidget*)  — where a 3D view will be installed
//     └── panels     (QWidget*)  — stacked control sections, added by name
//
// Two extension points, `setViewport()` and `addControlPanel()`, let future
// work drop in a real 3D view and its controls without touching this class,
// the main window, or anything else. Until then a placeholder states plainly
// that the window is a scaffold, rather than faking a 3D render.
//
// Behaviour required by the brief: opens on demand, closes without disturbing
// the application, reopens with its state intact (it is hidden, never
// destroyed), and is non-modal so the main GUI stays usable while it is open.
// ---------------------------------------------------------------------------

#include <QDialog>
#include <QVBoxLayout>
#include <QVariantMap>
#include <functional>
#include <QSet>
#include <QList>
#include <QElapsedTimer>
#include <QVector>

#include "../core/IqGenerator.h"
#include "../core/Types.h"

class QLabel;
class QTabWidget;
class QDoubleSpinBox;
class QSpinBox;
class QComboBox;
class QPushButton;
class QCheckBox;
class QPlainTextEdit;
class QThread;
class QProgressBar;
class QLineEdit;
class QGroupBox;
class QTimer;

namespace sdr { class H2cControl; }
namespace sdr { class RfdcControl; }

namespace sdr::ui {

class ControlWindow3D : public QDialog
{
    Q_OBJECT
public:
    explicit ControlWindow3D(QWidget* parent = nullptr);
    ~ControlWindow3D() override;

    /// Install the 3D view widget. Takes ownership. Replaces the placeholder.
    void setViewport(QWidget* view);

    /// Add a named control section. Returns the container so the caller can
    /// populate it; the window itself never needs to know what is inside.
    QWidget* addControlPanel(const QString& title);

    /// True once a real viewport has been installed.
    bool hasViewport() const { return m_hasViewport; }

    /// Attach to the transmitter's control block. Safe to call repeatedly;
    /// the controls are disabled with a reason when it cannot be reached.
    void attachControl(const QString& path = QStringLiteral("/dev/shm/iqctl"));

    /// Attach the RF Data Converter control path (PCIe BAR registers on the
    /// ZU47DR). Independent of attachControl(): that one drives the host
    /// transmitter, this one configures the converters themselves.
    void attachRfdc(const QString& bdf = QString(), int bar = 2);

    /// Tell the window the acquisition source is the simulator. In simulation
    /// the DAC input source, DDS frequency and IQ generator drive the
    /// simulated RF loopback instead of hardware, so they stay usable without
    /// a device and the generator never touches the H2C FIFO.
    void setSimulationMode(bool simulated);

    /// What the transmit side is currently configured to put on the DAC.
    sdr::TxState txState() const;

    /// The chunk size iwfg_h2c is running with (from the backend launcher).
    /// The generator's loop must be exactly this long; a running generator is
    /// restarted if it changes.
    void setH2cChunkBytes(int bytes);

    /// iwfg_h2c's measured delivery to the card (MiB/s, as it prints it),
    /// compared against what the DAC consumes.
    void setH2cThroughput(double mibPerSec, int updatesPerSec);

    /// The receiver's strongest spectral peak (active channel), fed by the
    /// main window a few times a second. `basebandHz` is relative to the
    /// display centre; `inputRateHz` is the rate the display assumes.
    void setReceivedPeak(double basebandHz, double dbfs, bool toneValid,
                         double inputRateHz, double binHz);

    /// The rdma_tx control block only matters on the RoCEv2 path.
    void setRdmaControlVisible(bool visible);

    /// Add a line to this window's log (for messages the main window owns).
    void appendLog(const QString& line) { log(line); }

    /// The H2C playback helper (iwfg_h2c) as the backend launcher reports it:
    /// `state` is an sdr::H2cState, `backendRunning` whether a live backend is
    /// up (Retry transmit is offered only then).
    void setH2cHelperState(int state, const QString& detail, bool backendRunning);

    /// What the capture delivers (MiB/s, 0 = not live) against what the
    /// configured stream needs. Offers fewer channels when it falls short.
    void setCaptureRate(double deliveredMib, double needMib, int channels,
                        double rateMsps, int bytesPerSample);

    /// Put the PL GPIO routing back to the mode matching `channels` (after a
    /// channel reduction the main window found did not take effect).
    void restorePlGpioModeFor(int channels);

    /// True when the RFDC control path is attached (PCIe registers usable).
    bool rfdcAttached() const { return m_rfAttached && m_rfdc; }
    /// PL GPIO routing mode as last read from / written to the device.
    int  plGpioMode() const;
    /// Set the PL GPIO routing mode on the device (and the RFDC tab's combo).
    void applyPlGpioMode(int mode, const QString& why);

    /// The FIFO iwfg_h2c reads, as the backend launches it. The generator
    /// writes there; a mismatch meant the generator waited for a reader that
    /// was listening on a different path.
    void setH2cFifoPath(const QString& path);

protected:
    // Hide rather than destroy, so reopening preserves state (brief §11).
    void closeEvent(QCloseEvent* e) override;

signals:
    /// Signal-chain values the operator changed. The session applies these on
    /// (re)start; they are not part of the live iq_ctl block, so they are
    /// reported rather than written to shared memory.
    void signalChainChanged(double ncoMHz, int interp, int decim,
                            int channels, int chunkBytes);

    /// Emitted whenever anything that determines the DAC output changes: DAC
    /// input source, DDS frequency, generator state or parameters.
    void txStateChanged(const sdr::TxState& tx);

    /// Stream clocks read from the device (MSPS). The main window aligns its
    /// display rate with the ADC's, so received frequencies read correctly.
    void deviceRatesRead(double adcStreamMsps, double dacStreamMsps);

    /// The operator pressed Retry transmit (relaunch iwfg_h2c).
    void h2cRestartRequested();

    /// Capture this many channels (the PL GPIO mode has been set already).
    void captureChannelsRequested(int channels);

private slots:
    void onApply();
    void onRevert();
    void onRfdcAttach();
    void onRfdcResetDefaults();
    void onRfdcPing();
    void onRfdcRefresh();
    void onRfdcReadFromHardware();
    void onRfdcBusyChanged(bool busy);
    void onRfdcSnapshot(const QVariantMap& values);
    void onRfdcAttachResult(bool ok, const QString& detail);
    void onRfdcRegisters(quint32 ring, quint32 cfg);
    void onRfdcSyncing(bool syncing);
    void onRfdcSyncFinished(bool ok, const QString& detail);
    void onRfdcVerify(const QString& name, bool ok, quint32 req, quint32 act,
                      const QString& detail);
    void onRfdcResetFinished(bool ok, const QString& detail);
    void onRfdcCompleteReset();
    void onRfdcApplyDirty();
    void onGenToggle();
    void onVerifyLoopback();
    void loopCheckTick();
    void onGenProgress(quint64 total, double msps, quint64 chunks,
                       quint64 reconnects);

private:
    void buildTransmitPanel();
    void buildGeneratorPanel(QWidget* page);
    void buildSignalChainPanel();
    void buildRfdcPanel();
    void rfdcInvoke(const std::function<void()>& job);
    /// True if this edit may be transmitted now; otherwise records it dirty.
    bool sendOnEdit(QWidget* w);
    void updateDerived();
    void log(const QString& line);

    // Transmit-side state shared by hardware and simulation
    sdr::IqGenerator::Config genConfig() const;
    void onGenParamsEdited();       ///< preview + hot update + publish
    void updateGenPreview();
    void setGenState(const QString& text, const char* colour);
    void ensureHostRouting();       ///< generator start: DAC must take host
    void publishTxState();
    void refreshSimControls();      ///< re-enable sim-usable controls
    void enableSimLoopback();
    void refreshChain();            ///< the live transmit-chain group
    void finishLoopCheck();
    /// Received peak converted to true Hz (display-rate mismatch removed).
    double rxTrueHz(double displayedHz) const;
    /// |frequency| at which the ADC should see `txHz`, given both NCOs.
    double expectedRxHz(double txHz) const;

    sdr::H2cControl*  m_ctl  = nullptr;
    sdr::RfdcControl* m_rfdc = nullptr;

    // RF Data Converter panel
    QComboBox*      m_rfBdf       = nullptr;
    QSpinBox*       m_rfBar       = nullptr;
    QLineEdit*      m_rfRegBase   = nullptr;
    QPushButton*    m_rfAttach    = nullptr;
    QLabel*         m_rfState     = nullptr;
    QComboBox*      m_rfTarget    = nullptr;
    QComboBox*      m_rfTile      = nullptr;
    QComboBox*      m_rfChannel   = nullptr;
    QComboBox*      m_rfDecim     = nullptr;
    QComboBox*      m_rfInterp    = nullptr;
    QDoubleSpinBox* m_rfNco       = nullptr;
    QSpinBox*       m_rfNcoPhase  = nullptr;
    QDoubleSpinBox* m_rfDds       = nullptr;
    QDoubleSpinBox* m_rfDsa       = nullptr;
    QDoubleSpinBox* m_rfQmc       = nullptr;
    QDoubleSpinBox* m_rfVop       = nullptr;
    QComboBox*      m_rfDacSrc    = nullptr;
    QComboBox*      m_rfNyquist   = nullptr;
    QSpinBox*       m_rfSampMhz   = nullptr;
    QSpinBox*       m_rfAxis[4]   = { nullptr, nullptr, nullptr, nullptr };
    QComboBox*      m_rfPlGpio    = nullptr;
    QPushButton*    m_rfApplyRate = nullptr;
    QPushButton*    m_rfApplyNyq  = nullptr;
    QPushButton*    m_rfApplyRoute= nullptr;
    QPushButton*    m_rfApplyGpio = nullptr;
    QPushButton*    m_rfPing      = nullptr;
    QPushButton*    m_rfReadHw    = nullptr;
    QPushButton*    m_rfAbort     = nullptr;
    QProgressBar*   m_rfBusyBar   = nullptr;
    QThread*        m_rfThread    = nullptr;
    bool            m_rfAttached  = false;
    bool            m_rfBusy      = false;
    bool            m_rfSyncing   = false;
    bool            m_rfConfirmed = false;
    QSet<QWidget*>  m_rfDirty;
    QPushButton*    m_rfApplyDirty = nullptr;
    QLabel*         m_rfEditHint   = nullptr;
    QPushButton*    m_rfFullReset = nullptr;
    QLabel*         m_rfSyncState = nullptr;

    // IQ generator: the missing producer for the H2C playback FIFO
    sdr::IqGenerator* m_gen       = nullptr;
    QThread*          m_genThread = nullptr;
    QComboBox*        m_genWave   = nullptr;
    QDoubleSpinBox*   m_genTone   = nullptr;
    QDoubleSpinBox*   m_genTone2  = nullptr;
    QDoubleSpinBox*   m_genAmp    = nullptr;
    QLineEdit*        m_genFifo   = nullptr;
    QPushButton*      m_genBtn    = nullptr;
    QLabel*           m_genState  = nullptr;
    QDoubleSpinBox*   m_genRate   = nullptr;   ///< DAC stream clock, MSPS
    QCheckBox*        m_genCoherent = nullptr;
    QLabel*           m_genActual = nullptr;
    QLabel*           m_genLink   = nullptr;   ///< H2C supply vs DAC demand
    int               m_h2cChunkBytes = 0;     ///< iwfg_h2c chunk = loop length
    bool              m_genRestartPending = false;
    bool              m_h2cStarved = false;    ///< last reading was short
    bool              m_genRunning = false;
    bool              m_genConnected = false;   ///< iwfg_h2c has the FIFO open

    // Live transmit chain (Transmit tab), all from the device where possible
    QGroupBox*        m_rdmaBox   = nullptr;
    QLabel*           m_chSrc = nullptr, *m_chDac = nullptr, *m_chNco = nullptr,
                     *m_chAdc = nullptr, *m_chLink = nullptr, *m_chRx = nullptr,
                     *m_chVerify = nullptr;
    QPushButton*      m_verifyBtn = nullptr;
    // 1.2.4: the helpers behind the chain
    QLabel*           m_chH2c = nullptr, *m_chC2h = nullptr;
    QPushButton*      m_retryTxBtn = nullptr;
    QPushButton*      m_oneChBtn = nullptr;
    int               m_h2cHelper = 0;           ///< sdr::H2cState
    QString           m_h2cHelperDetail;
    bool              m_backendRunning = false;
    double            m_capMib = 0, m_capNeedMib = 0, m_capRate = 0;
    int               m_capCh = 0, m_capBps = 0;
    int               m_plGpioBefore = 0;        ///< mode before a 1-channel switch
    double            m_devAdcQmc = -1.0;        ///< ADC QMC gain readback, -1 unknown
    int               m_devDacSrc = -1;          ///< device-confirmed, -1 unknown
    double            m_devDacFs = 0, m_devAdcFs = 0;          ///< MHz
    double            m_devDacStream = 0, m_devAdcStream = 0;  ///< MSPS
    double            m_devDacNco = 0, m_devAdcNco = 0;        ///< MHz
    bool              m_devNcoKnown = false;
    int               m_devInterp = 0, m_devDecim = 0;
    double            m_linkMib = 0;
    QElapsedTimer     m_linkClock;               ///< since the last link reading
    double            m_rxHz = 0, m_rxDb = -999, m_rxRate = 0, m_rxBin = 0;
    bool              m_rxValid = false;
    QElapsedTimer     m_rxClock;
    // Loopback verification (tone hop)
    QTimer*           m_lcTimer = nullptr;
    int               m_lcStep = 0;              ///< 0 idle
    QElapsedTimer     m_lcClock;
    double            m_lcOrigMhz = 0, m_lcTxA = 0, m_lcTxB = 0, m_lcPa = 0, m_lcPb = 0;
    QVector<double>   m_lcSamples;

    // Simulation
    bool              m_simMode   = false;
    bool              m_simGenRunning = false;
    QGroupBox*        m_simBox    = nullptr;
    QCheckBox*        m_simLoopback = nullptr;
    QPushButton*    m_rfRefresh   = nullptr;
    QPushButton*    m_rfStart     = nullptr;
    QPushButton*    m_rfStop      = nullptr;
    QPushButton*    m_rfMts       = nullptr;
    QPushButton*    m_rfReset     = nullptr;
    QPushButton*    m_rfDefaults  = nullptr;
    QLabel*         m_rfRegs      = nullptr;
    /// Every RFDC control that only makes sense once attached. Enabled and
    /// disabled as one set, so an edit can never reach a device that is not
    /// mapped.
    QList<QWidget*> m_rfControls;
    QDoubleSpinBox*  m_pace     = nullptr;
    QCheckBox*       m_unlimited= nullptr;
    QDoubleSpinBox*  m_rate     = nullptr;
    QDoubleSpinBox*  m_centre   = nullptr;
    QDoubleSpinBox*  m_nco      = nullptr;
    QSpinBox*        m_interp   = nullptr;
    QSpinBox*        m_decim    = nullptr;
    QComboBox*       m_channels = nullptr;
    QComboBox*       m_format   = nullptr;
    QSpinBox*        m_chunk    = nullptr;
    QLabel*          m_derived  = nullptr;
    QPushButton*     m_apply    = nullptr;
    QPushButton*     m_revert   = nullptr;
    QLabel*          m_ctlState = nullptr;
    QPlainTextEdit*  m_log      = nullptr;

    QVBoxLayout* m_viewportLayout = nullptr;
    QLabel*      m_placeholder    = nullptr;
    QTabWidget*  m_panels         = nullptr;
    bool         m_hasViewport    = false;
};

} // namespace sdr::ui
