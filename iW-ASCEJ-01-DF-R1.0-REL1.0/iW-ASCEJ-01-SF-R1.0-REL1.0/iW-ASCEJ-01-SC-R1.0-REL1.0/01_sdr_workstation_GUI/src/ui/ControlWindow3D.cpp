#include "ControlWindow3D.h"
#include "../core/BackendLauncher.h"
#include "../core/H2cControl.h"
#include "../core/RfdcControl.h"
#include "../core/IqGenerator.h"
#include "../core/pcie_regs.h"
#include "../core/Types.h"
#include "../core/Waveform.h"
#include "Theme.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QLineEdit>
#include <QGridLayout>
#include <QThread>
#include <QTimer>
#include <QProgressBar>
#include <QVariantMap>
#include <QMessageBox>
#include <cmath>
#include <algorithm>
#include <QScrollArea>

namespace sdr::ui {

ControlWindow3D::ControlWindow3D(QWidget* parent)
    : QDialog(parent, Qt::Window)
{
    setObjectName(QStringLiteral("controlWindow3D"));
    setWindowTitle(tr("H2C Transmit && RF Data Converter Control"));
    // Non-modal: the brief requires the main GUI to stay usable while this is
    // open, so it must not block the event loop.
    setModal(false);
    // Resizable with a corner grip; each tab scrolls (see addControlPanel), so
    // the window can be shrunk freely and the full contents stay reachable
    // rather than running off the bottom edge.
    setSizeGripEnabled(true);
    resize(680, 700);
    setMinimumSize(460, 420);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* heading = new QLabel(tr("H2C TRANSMIT CONTROL"));
    heading->setObjectName(QStringLiteral("dialogHeading"));
    root->addWidget(heading);

    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("viewportFrame"));
    frame->setFrameShape(QFrame::StyledPanel);
    m_viewportLayout = new QVBoxLayout(frame);
    m_viewportLayout->setContentsMargins(0, 0, 0, 0);

    // One line, not a paragraph: until a 3D view is installed this frame is
    // scaffolding, and it should not take space from the controls that do
    // work. setViewport() replaces it and the frame then takes the stretch.
    m_placeholder = new QLabel(
        tr("3D viewport not installed — install one with setViewport()."));
    m_placeholder->setObjectName(QStringLiteral("hintLabel"));
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);
    m_viewportLayout->setContentsMargins(8, 6, 8, 6);
    m_viewportLayout->addWidget(m_placeholder);
    frame->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    root->addWidget(frame);

    m_panels = new QTabWidget;
    m_panels->setObjectName(QStringLiteral("controlPanels"));
    m_panels->hide();          // shown once a panel is added
    root->addWidget(m_panels, 1);

    m_h2cChunkBytes = sdr::defaultH2cChunkBytes();

    m_ctl = new sdr::H2cControl(this);
    connect(m_ctl, &sdr::H2cControl::statusMessage, this, &ControlWindow3D::log);
    connect(m_ctl, &sdr::H2cControl::errorMessage,  this, &ControlWindow3D::log);

    // The RFDC transaction busy-polls the ring register until the firmware
    // acknowledges — up to 15 s for a PLL reprogram. Run it on its own thread
    // so the event loop keeps turning; every call below is queued, never direct.
    m_rfThread = new QThread(this);
    m_rfThread->setObjectName(QStringLiteral("rfdcCtl"));
    m_rfdc = new sdr::RfdcControl;            // no parent: owned by the thread
    m_rfdc->moveToThread(m_rfThread);
    connect(m_rfThread, &QThread::finished, m_rfdc, &QObject::deleteLater);
    m_rfThread->start();

    connect(m_rfdc, &sdr::RfdcControl::busyChanged,
            this, &ControlWindow3D::onRfdcBusyChanged);
    connect(m_rfdc, &sdr::RfdcControl::snapshotReady,
            this, &ControlWindow3D::onRfdcSnapshot);
    connect(m_rfdc, &sdr::RfdcControl::attachedChanged,
            this, [this](bool a){ m_rfAttached = a; });
    connect(m_rfdc, &sdr::RfdcControl::attachResult,
            this, &ControlWindow3D::onRfdcAttachResult);
    connect(m_rfdc, &sdr::RfdcControl::registersRead,
            this, &ControlWindow3D::onRfdcRegisters);
    connect(m_rfdc, &sdr::RfdcControl::syncingChanged,
            this, &ControlWindow3D::onRfdcSyncing);
    connect(m_rfdc, &sdr::RfdcControl::syncFinished,
            this, &ControlWindow3D::onRfdcSyncFinished);
    connect(m_rfdc, &sdr::RfdcControl::verifyResult,
            this, &ControlWindow3D::onRfdcVerify);
    connect(m_rfdc, &sdr::RfdcControl::resetFinished,
            this, &ControlWindow3D::onRfdcResetFinished);
    connect(m_rfdc, &sdr::RfdcControl::statusMessage, this, &ControlWindow3D::log);
    connect(m_rfdc, &sdr::RfdcControl::errorMessage,  this, &ControlWindow3D::log);
    // After every DAC input change the device's own answer is shown, so the
    // combo can never claim a routing the switch did not take.
    connect(m_rfdc, &sdr::RfdcControl::dacSourceConfirmed, this, [this](int src){
        const int i = m_rfDacSrc ? m_rfDacSrc->findData(src) : -1;
        if (i >= 0) m_rfDacSrc->setCurrentIndex(i);
        log(tr("DAC input confirmed by the device: %1")
                .arg(i >= 0 ? m_rfDacSrc->itemText(i) : tr("nothing routed")));
        m_devDacSrc = src;
        publishTxState();
    });

    buildTransmitPanel();
    buildSignalChainPanel();
    buildRfdcPanel();
    attachControl();
    attachRfdc();          // best-effort; the panel reports if it cannot bind
}

// ---------------------------------------------------------------------------
// RF Data Converter panel — talks to the bare-metal application on the A53
// over the PCIe control registers. This is a DIFFERENT surface from the
// transmit panel above: that one paces the host stream, this one configures
// the converters it lands in.
// ---------------------------------------------------------------------------
void ControlWindow3D::buildRfdcPanel()
{
    QWidget* page = addControlPanel(tr("RFDC"));
    auto* v = qobject_cast<QVBoxLayout*>(page->layout());

    // ---- connection ----
    auto* conn = new QGroupBox(tr("PCIe control interface"));
    auto* cf = new QFormLayout(conn);

    m_rfBdf = new QComboBox;
    m_rfBdf->setEditable(true);
    for (const QString& b : sdr::RfdcControl::discover())
        m_rfBdf->addItem(b);
    m_rfBdf->setToolTip(tr("PCIe endpoint. Xilinx devices (vendor 10ee) are "
                           "listed automatically; type a BDF to override."));

    m_rfBar = new QSpinBox;
    m_rfBar->setRange(0, 5);
    m_rfBar->setValue(2);
    m_rfBar->setToolTip(tr("BAR holding the control block. On this design the "
                           "4 KB BAR2 carries ring/config; BAR0 is the QDMA "
                           "control space and must not be written here."));

    m_rfRegBase = new QLineEdit(QStringLiteral("0x0"));
    m_rfRegBase->setToolTip(tr("Offset of the control block inside the BAR. "
                               "0 unless the block sits inside a larger window."));

    m_rfAttach = new QPushButton(tr("Attach"));
    m_rfState  = new QLabel(tr("Not attached"));
    m_rfState->setWordWrap(true);

    cf->addRow(tr("Endpoint (BDF)"), m_rfBdf);
    cf->addRow(tr("BAR"), m_rfBar);
    cf->addRow(tr("Register base"), m_rfRegBase);
    cf->addRow(QString(), m_rfAttach);
    cf->addRow(m_rfState);
    v->addWidget(conn);

    // ---- simulation (visible only when the acquisition source is simulated)
    m_simBox = new QGroupBox(tr("Simulation — no hardware is touched"));
    auto* sl = new QVBoxLayout(m_simBox);
    m_simLoopback = new QCheckBox(tr("Show the RF loopback in the plots"));
    m_simLoopback->setToolTip(tr("Model the DAC -> ADC loopback from the settings "
                                 "below instead of the simulator's modem demo."));
    auto* simHint = new QLabel(tr(
        "DAC input source, DDS frequency and the IQ generator (Transmit tab) "
        "drive the simulated loopback the way they drive the board: DDS shows "
        "the DDS tone; Host shows the generator's waveform while it runs; Host "
        "with nothing transmitting shows only noise. Unticked, the simulator "
        "shows its modem demo signal."));
    simHint->setObjectName(QStringLiteral("hintLabel"));
    simHint->setWordWrap(true);
    sl->addWidget(m_simLoopback);
    sl->addWidget(simHint);
    m_simBox->setVisible(false);
    v->addWidget(m_simBox);
    connect(m_simLoopback, &QCheckBox::toggled, this, [this](bool on){
        log(on ? tr("Simulation: plots show the RF loopback of the transmit settings.")
               : tr("Simulation: plots show the modem demo signal."));
        publishTxState();
    });

    // ---- target selection ----
    auto* tgt = new QGroupBox(tr("Target"));
    auto* tf = new QFormLayout(tgt);

    m_rfTarget = new QComboBox;
    m_rfTarget->addItem(tr("ADC"), int(sdr::RfdcControl::Adc));
    m_rfTarget->addItem(tr("DAC"), int(sdr::RfdcControl::Dac));
    m_rfTarget->addItem(tr("ADC + DAC"), int(sdr::RfdcControl::AdcAndDac));

    m_rfTile = new QComboBox;
    m_rfTile->addItem(tr("All tiles"), int(sdr::RfdcControl::AllTiles));
    for (int i = 0; i < 4; ++i) m_rfTile->addItem(tr("Tile %1").arg(i), i);

    m_rfChannel = new QComboBox;
    m_rfChannel->addItem(tr("Both channels"), int(sdr::RfdcControl::BothCh));
    m_rfChannel->addItem(tr("Channel 0"), int(sdr::RfdcControl::Ch0));
    m_rfChannel->addItem(tr("Channel 1"), int(sdr::RfdcControl::Ch1));

    tf->addRow(tr("Converter"), m_rfTarget);
    tf->addRow(tr("Tile"), m_rfTile);
    tf->addRow(tr("Channel"), m_rfChannel);
    v->addWidget(tgt);

    // ---- parameters (per target / tile / channel) ----
    // Each control is applied on its own the moment you finish editing it —
    // there is no single "apply everything" button, because one that re-sent
    // the whole group would push unrelated fields (DAC source, decimation, DDS)
    // back to their box values every time you changed one thing.
    m_rfEditHint = new QLabel(tr("Not attached. Values are read from the device "
                                 "on attach; each edit is then sent as soon as "
                                 "you finish editing it."));
    auto* parNote = m_rfEditHint;
    parNote->setObjectName(QStringLiteral("hintLabel"));
    parNote->setWordWrap(true);
    v->addWidget(parNote);

    auto* par = new QGroupBox(tr("Parameters — applied on edit"));
    auto* pf = new QFormLayout(par);

    // Decimation / interpolation combos carry ONLY the factors the register
    // map accepts (pcie_regs.h PCIE_FACTOR_*). Binding the value as item data
    // means the panel can never send a factor the firmware will reject — the
    // previous path read these off the Signal-chain tab, whose 1..4096 range
    // let an invalid factor (e.g. 7) reach the device.
    const int kFactors[] = {1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 40};
    m_rfDecim  = new QComboBox;
    m_rfInterp = new QComboBox;
    for (int f : kFactors) {
        m_rfDecim->addItem(tr("x%1").arg(f),  f);
        m_rfInterp->addItem(tr("x%1").arg(f), f);
    }
    m_rfDecim->setToolTip(tr("ADC decimation factor (applied when the target "
                             "includes the ADC)."));
    m_rfInterp->setToolTip(tr("DAC interpolation factor (applied when the "
                              "target includes the DAC)."));

    m_rfNco = new QDoubleSpinBox;
    m_rfNco->setRange(-10000.0, 10000.0);
    m_rfNco->setDecimals(3);
    m_rfNco->setSuffix(tr(" MHz"));
    m_rfNco->setValue(3100.0);

    m_rfNcoPhase = new QSpinBox;
    m_rfNcoPhase->setRange(-180, 180);
    m_rfNcoPhase->setSuffix(tr(" °"));
    m_rfNcoPhase->setToolTip(tr("NCO phase offset, integer degrees (-180..180)."));

    m_rfDds = new QDoubleSpinBox;
    m_rfDds->setRange(0.001, 75.0);
    m_rfDds->setDecimals(3);
    m_rfDds->setSuffix(tr(" MHz"));
    m_rfDds->setValue(10.0);

    m_rfDsa = new QDoubleSpinBox;
    m_rfDsa->setRange(0.0, 27.0);
    m_rfDsa->setSingleStep(0.5);
    m_rfDsa->setDecimals(1);
    m_rfDsa->setSuffix(tr(" dB"));
    m_rfDsa->setToolTip(tr("ADC digital step attenuator, 0..27 dB (ADC target; "
                           "0 leaves it unchanged)."));

    m_rfQmc = new QDoubleSpinBox;
    m_rfQmc->setRange(0.0, 2.0);
    m_rfQmc->setSingleStep(0.01);
    m_rfQmc->setDecimals(4);
    m_rfQmc->setToolTip(tr("QMC gain correction (0 leaves it unchanged)."));

    m_rfVop = new QDoubleSpinBox;
    m_rfVop->setRange(2.25, 40.5);   // device minimum; 0 mA is rejected
    m_rfVop->setSingleStep(0.5);
    m_rfVop->setDecimals(3);
    m_rfVop->setSuffix(tr(" mA"));
    m_rfVop->setToolTip(tr("DAC output current (VOP), DAC target (0 leaves it "
                           "unchanged)."));

    // Item data is the LOGICAL source (pcie_regs.h PCIE_DAC_SRC_*); the
    // firmware maps it to the switch port. See platform_ids.h for the wiring.
    m_rfDacSrc = new QComboBox;
    m_rfDacSrc->addItem(tr("DDS compiler"), int(PCIE_DAC_SRC_DDS));
    m_rfDacSrc->addItem(tr("Host / GNU Radio stream"), int(PCIE_DAC_SRC_HOST));
    m_rfDacSrc->setToolTip(tr("What the DAC transmits. DDS compiler: the internal "
                              "tone at the DDS frequency. Host / GNU Radio stream: "
                              "samples sent over PCIe (H2C) — from the IQ generator "
                              "on the Transmit tab, or your own GNU Radio flowgraph "
                              "writing /tmp/iwfg_h2c.fifo."));

    pf->addRow(tr("Decimation"), m_rfDecim);
    pf->addRow(tr("Interpolation"), m_rfInterp);
    pf->addRow(tr("NCO frequency"), m_rfNco);
    pf->addRow(tr("NCO phase"), m_rfNcoPhase);
    pf->addRow(tr("ADC DSA"), m_rfDsa);
    pf->addRow(tr("QMC gain"), m_rfQmc);
    pf->addRow(tr("DAC VOP"), m_rfVop);
    pf->addRow(tr("DAC input source"), m_rfDacSrc);
    pf->addRow(tr("DDS frequency"), m_rfDds);
    v->addWidget(par);

    // ---- global / heavy operations ----
    // These do not belong in the per-channel "Apply signal chain" bundle: the
    // sampling-rate change reprograms the PLL (seconds, not ms), Nyquist zone
    // switches the analogue band, and routing is structural. Each gets its own
    // apply so it is issued deliberately.
    auto* glob = new QGroupBox(tr("PLL, Nyquist && routing"));
    auto* gf = new QFormLayout(glob);

    m_rfSampMhz = new QSpinBox;
    m_rfSampMhz->setRange(1, 12000);
    m_rfSampMhz->setSuffix(tr(" MHz"));
    m_rfSampMhz->setValue(2000);
    m_rfSampMhz->setToolTip(tr("Sampling rate for the target converter. "
                               "Reprograms the PLL and re-aligns MTS."));
    m_rfApplyRate = new QPushButton(tr("Apply rate"));
    auto* rateRow = new QWidget;
    auto* rrl = new QHBoxLayout(rateRow);
    rrl->setContentsMargins(0, 0, 0, 0);
    rrl->addWidget(m_rfSampMhz, 1);
    rrl->addWidget(m_rfApplyRate);

    m_rfNyquist = new QComboBox;
    m_rfNyquist->addItem(tr("Zone 1"), 1);
    m_rfNyquist->addItem(tr("Zone 2"), 2);
    m_rfApplyNyq = new QPushButton(tr("Apply zone"));
    auto* nyqRow = new QWidget;
    auto* nrl = new QHBoxLayout(nyqRow);
    nrl->setContentsMargins(0, 0, 0, 0);
    nrl->addWidget(m_rfNyquist, 1);
    nrl->addWidget(m_rfApplyNyq);

    auto* axisRow = new QWidget;
    auto* axl = new QHBoxLayout(axisRow);
    axl->setContentsMargins(0, 0, 0, 0);
    for (int i = 0; i < 4; ++i) {
        m_rfAxis[i] = new QSpinBox;
        m_rfAxis[i]->setRange(0, 15);
        m_rfAxis[i]->setValue(i);
        m_rfAxis[i]->setToolTip(tr("Source stream for AXIS output %1 (0..15).").arg(i));
        axl->addWidget(m_rfAxis[i], 1);
    }
    m_rfApplyRoute = new QPushButton(tr("Apply"));
    axl->addWidget(m_rfApplyRoute);

    m_rfPlGpio = new QComboBox;
    m_rfPlGpio->addItem(tr("Mode 1"), 1);
    m_rfPlGpio->addItem(tr("Mode 2"), 2);
    m_rfPlGpio->addItem(tr("Mode 3"), 3);
    m_rfApplyGpio = new QPushButton(tr("Apply"));
    auto* gpioRow = new QWidget;
    auto* grl = new QHBoxLayout(gpioRow);
    grl->setContentsMargins(0, 0, 0, 0);
    grl->addWidget(m_rfPlGpio, 1);
    grl->addWidget(m_rfApplyGpio);

    gf->addRow(tr("Sampling rate"), rateRow);
    gf->addRow(tr("Nyquist zone"), nyqRow);
    gf->addRow(tr("AXIS routing"), axisRow);
    gf->addRow(tr("PL GPIO routing"), gpioRow);
    v->addWidget(glob);

    // ---- actions ----
    auto* act = new QGroupBox(tr("Actions"));
    auto* g = new QGridLayout(act);
    m_rfPing    = new QPushButton(tr("Ping"));
    m_rfRefresh = new QPushButton(tr("Read registers"));
    m_rfReadHw  = new QPushButton(tr("Read from hardware"));
    m_rfApplyDirty = new QPushButton(tr("Apply changed values"));
    m_rfApplyDirty->setEnabled(false);
    m_rfApplyDirty->setVisible(false);
    m_rfApplyDirty->setToolTip(tr("Send only the fields you have edited. Used "
                                  "when the device cannot be read back, so an "
                                  "untouched field is never transmitted."));
    m_rfApplyDirty->setStyleSheet(QStringLiteral(
        "QPushButton{background:#78350f;color:#fde68a;font-weight:bold;padding:6px;}"
        "QPushButton:disabled{background:#3f3f46;color:#71717a;}"));

    m_rfFullReset = new QPushButton(tr("COMPLETE RESET"));
    m_rfFullReset->setToolTip(tr("Restore every parameter to its default state, "
                                 "then re-read the device. Asks for confirmation."));
    m_rfFullReset->setStyleSheet(QStringLiteral(
        "QPushButton{background:#7f1d1d;color:#fecaca;font-weight:bold;padding:6px;}"
        "QPushButton:hover{background:#991b1b;}"));
    m_rfSyncState = new QLabel(tr("Synchronising with device…"));
    m_rfSyncState->setStyleSheet(QStringLiteral("color:#fbbf24;"));
    m_rfSyncState->setVisible(false);
    m_rfAbort   = new QPushButton(tr("Abort current command"));
    m_rfAbort->setVisible(false);
    m_rfAbort->setToolTip(tr("Stop waiting for the acknowledgement. The device "
                             "may still complete the command."));
    m_rfBusyBar = new QProgressBar;
    m_rfBusyBar->setRange(0, 0);          // indeterminate
    m_rfBusyBar->setTextVisible(false);
    m_rfBusyBar->setFixedHeight(6);
    m_rfBusyBar->setVisible(false);
    m_rfStart   = new QPushButton(tr("Start"));
    m_rfStop    = new QPushButton(tr("Stop"));
    m_rfMts     = new QPushButton(tr("Re-align MTS"));
    m_rfReset   = new QPushButton(tr("Soft reset"));
    m_rfDefaults = new QPushButton(tr("Reset all to defaults"));

    m_rfPing->setToolTip(tr("Handshake test. Changes no hardware, so a success "
                            "proves the whole path end to end."));
    m_rfStop->setToolTip(tr("Disables all converter FIFOs. There will be no "
                            "data at all until Start."));
    m_rfReset->setToolTip(tr("Firmware soft reset: MTS re-align, NCO restore, "
                             "routing restore. Does NOT power-cycle the "
                             "converters."));
    m_rfDefaults->setToolTip(tr("Put every control on this tab back to its "
                                "default value, and (if attached) apply the "
                                "default signal chain — one click to a known "
                                "baseline instead of re-entering each field."));

    g->addWidget(m_rfPing,     0, 0);
    g->addWidget(m_rfRefresh,  0, 1);
    g->addWidget(m_rfReadHw,   3, 0, 1, 2);
    g->addWidget(m_rfStart,    1, 0);
    g->addWidget(m_rfStop,     1, 1);
    g->addWidget(m_rfMts,      2, 0);
    g->addWidget(m_rfReset,    2, 1);
    g->addWidget(m_rfDefaults, 4, 0, 1, 2);
    g->addWidget(m_rfApplyDirty, 4, 0, 1, 2);
    g->addWidget(m_rfFullReset,  5, 0, 1, 2);
    g->addWidget(m_rfAbort,     6, 0, 1, 2);
    g->addWidget(m_rfSyncState, 7, 0, 1, 2);
    g->addWidget(m_rfBusyBar,   8, 0, 1, 2);
    v->addWidget(act);

    m_rfRegs = new QLabel(tr("ring = ----  cfg = ----"));
    m_rfRegs->setStyleSheet(QStringLiteral("font-family:monospace;color:#9ca3af;"));
    v->addWidget(m_rfRegs);
    v->addStretch(1);

    connect(m_rfAttach,  &QPushButton::clicked, this, &ControlWindow3D::onRfdcAttach);
    connect(m_rfPing,    &QPushButton::clicked, this, &ControlWindow3D::onRfdcPing);
    connect(m_rfReadHw,  &QPushButton::clicked, this, &ControlWindow3D::onRfdcReadFromHardware);
    connect(m_rfAbort,     &QPushButton::clicked, this, [this]{ m_rfdc->requestAbort(); });
    connect(m_rfFullReset,  &QPushButton::clicked, this, &ControlWindow3D::onRfdcCompleteReset);
    connect(m_rfApplyDirty, &QPushButton::clicked, this, &ControlWindow3D::onRfdcApplyDirty);
    connect(m_rfRefresh, &QPushButton::clicked, this, &ControlWindow3D::onRfdcRefresh);
    connect(m_rfStart, &QPushButton::clicked, this, [this]{ rfdcInvoke([this]{ m_rfdc->start(); }); });
    connect(m_rfStop, &QPushButton::clicked, this, [this]{ rfdcInvoke([this]{ m_rfdc->stop(); }); });
    connect(m_rfMts, &QPushButton::clicked, this, [this]{ rfdcInvoke([this]{ m_rfdc->realignMts(); }); });
    connect(m_rfReset, &QPushButton::clicked, this, [this]{ rfdcInvoke([this]{ m_rfdc->softReset(); }); });
    connect(m_rfDefaults, &QPushButton::clicked, this, &ControlWindow3D::onRfdcResetDefaults);

    // ---- per-control apply-on-edit ----
    // editingFinished (spin boxes) and activated (combos) fire only on a
    // deliberate user commit, never on programmatic setup, so attaching or
    // reading back registers does not spuriously re-issue commands. Each sends
    // exactly one event, so changing one parameter touches only that parameter.
    auto curTarget = [this]{ return m_rfTarget->currentData().toInt(); };
    auto curTile   = [this]{ return quint32(m_rfTile->currentData().toInt()); };
    auto curChan   = [this]{ return quint32(m_rfChannel->currentData().toInt()); };

    connect(m_rfDecim, QOverload<int>::of(&QComboBox::activated), this, [this, curTile, curChan](int){
        if (!sendOnEdit(m_rfDecim)) return;
        const auto t = curTile(); const auto c = curChan();
        const quint32 f = quint32(m_rfDecim->currentData().toInt());
        // Decimation + MTS as ONE queued job: they must not be interleaved with
        // another command, and MTS alone is slow enough to freeze the UI.
        rfdcInvoke([this,t,c,f]{
            if (m_rfdc->setDecimation(t, c, f)) m_rfdc->realignMts();
        });
        onRfdcRefresh();
    });
    connect(m_rfInterp, QOverload<int>::of(&QComboBox::activated), this, [this, curTile, curChan](int){
        if (!sendOnEdit(m_rfInterp)) return;
        const auto t = curTile(); const auto c = curChan();
        const quint32 f = quint32(m_rfInterp->currentData().toInt());
        rfdcInvoke([this,t,c,f]{
            if (m_rfdc->setInterpolation(t, c, f)) m_rfdc->realignMts();
        });
        onRfdcRefresh();
    });
    connect(m_rfNco, &QDoubleSpinBox::editingFinished, this, [this, curTarget, curTile, curChan]{
        if (!sendOnEdit(m_rfNco)) return;
        const int tg = curTarget(); const auto t = curTile(); const auto c = curChan();
        const double val = m_rfNco->value();
        rfdcInvoke([this,tg,t,c,val]{ m_rfdc->setNcoFrequency(tg, t, c, val); });
        onRfdcRefresh();
    });
    connect(m_rfNcoPhase, &QSpinBox::editingFinished, this, [this, curTarget, curTile, curChan]{
        if (!sendOnEdit(m_rfNcoPhase)) return;
        const int tg = curTarget(); const auto t = curTile(); const auto c = curChan();
        const int d = m_rfNcoPhase->value();
        rfdcInvoke([this,tg,t,c,d]{ m_rfdc->setNcoPhase(tg, t, c, d); });
        onRfdcRefresh();
    });
    connect(m_rfDsa, &QDoubleSpinBox::editingFinished, this, [this, curTile, curChan]{
        if (!sendOnEdit(m_rfDsa)) return;
        const auto t = curTile(); const auto c = curChan();
        const double val = m_rfDsa->value();                          // ADC-intrinsic
        rfdcInvoke([this,t,c,val]{ m_rfdc->setAdcDsa(t, c, val); });
        onRfdcRefresh();
    });
    connect(m_rfQmc, &QDoubleSpinBox::editingFinished, this, [this, curTarget, curTile, curChan]{
        if (!sendOnEdit(m_rfQmc)) return;
        const int tg = curTarget(); const auto t = curTile(); const auto c = curChan();
        const double val = m_rfQmc->value();
        rfdcInvoke([this,tg,t,c,val]{ m_rfdc->setQmcGain(tg, t, c, val); });
        onRfdcRefresh();
    });
    connect(m_rfVop, &QDoubleSpinBox::editingFinished, this, [this, curTile, curChan]{
        if (!sendOnEdit(m_rfVop)) return;
        const auto t = curTile(); const auto c = curChan(); const double a = m_rfVop->value();
        rfdcInvoke([this,t,c,a]{ m_rfdc->setDacVop(t, c, a); });
    });
    connect(m_rfDacSrc, QOverload<int>::of(&QComboBox::activated), this, [this](int){
        const int src = m_rfDacSrc->currentData().toInt();
        if (m_simMode) enableSimLoopback();
        publishTxState();
        if (!m_rfAttached) {                    // simulation without a device
            log(tr("Simulation: DAC input -> %1").arg(m_rfDacSrc->currentText()));
            return;
        }
        if (!sendOnEdit(m_rfDacSrc)) return;
        rfdcInvoke([this,src]{ m_rfdc->setDacSource(src); });
        const bool genOn = m_genRunning || m_simGenRunning;
        if (src == int(PCIE_DAC_SRC_HOST) && !genOn)
            log(tr("The DAC now takes the host stream. Start the IQ generator "
                   "(Transmit tab) or your GNU Radio flowgraph — until something "
                   "feeds the H2C FIFO the DAC outputs nothing."));
        if (src == int(PCIE_DAC_SRC_DDS) && genOn)
            log(tr("The IQ generator is still running, but the DAC now takes the "
                   "DDS, so the generator's samples are not transmitted."));
    });
    connect(m_rfDds, &QDoubleSpinBox::editingFinished, this, [this]{
        if (m_simMode) enableSimLoopback();
        publishTxState();
        if (!m_rfAttached) return;              // simulation without a device
        if (!sendOnEdit(m_rfDds)) return;
        const double f = m_rfDds->value();
        rfdcInvoke([this,f]{ m_rfdc->setDdsFrequency(f); });
    });

    connect(m_rfApplyRate, &QPushButton::clicked, this, [this]{
        const int t = m_rfTarget->currentData().toInt();
        const quint32 f = quint32(m_rfSampMhz->value());
        // Write -> read back -> verify. 25 MHz tolerance: the PLL synthesises
        // RefClk x FBDIV / OutDiv and legitimately lands up to ~25 MHz away.
        const quint32 tile = quint32(m_rfTile->currentData().toInt());
        const quint32 chan = quint32(m_rfChannel->currentData().toInt());
        rfdcInvoke([this,t,f,tile,chan]{
            m_rfdc->setSamplingRateVerified(t, f, 25, tile, chan);
        });

    });
    connect(m_rfApplyNyq, &QPushButton::clicked, this, [this]{
        const int t = m_rfTarget->currentData().toInt();
        const quint32 z = quint32(m_rfNyquist->currentData().toInt());
        rfdcInvoke([this,t,z]{ m_rfdc->setNyquistZone(t, z); });
    });
    connect(m_rfApplyRoute, &QPushButton::clicked, this, [this]{
        const int a0=m_rfAxis[0]->value(), a1=m_rfAxis[1]->value();
        const int a2=m_rfAxis[2]->value(), a3=m_rfAxis[3]->value();
        rfdcInvoke([this,a0,a1,a2,a3]{ m_rfdc->setAxisRouting(a0,a1,a2,a3); });
    });
    connect(m_rfApplyGpio, &QPushButton::clicked, this, [this]{
        const int m = m_rfPlGpio->currentData().toInt();
        rfdcInvoke([this,m]{ m_rfdc->setPlGpioRouting(m); });
    });

    // Everything that acts on the device: disabled until attached, so an edit
    // can never reach a BAR that is not mapped. Target/Tile/Channel are plain
    // selectors that issue nothing, so they stay usable.
    m_rfControls = { m_rfDecim, m_rfInterp, m_rfNco, m_rfNcoPhase, m_rfDsa,
                     m_rfQmc, m_rfVop, m_rfDacSrc, m_rfDds, m_rfSampMhz,
                     m_rfNyquist, m_rfAxis[0], m_rfAxis[1], m_rfAxis[2],
                     m_rfAxis[3], m_rfPlGpio, m_rfApplyRate, m_rfApplyNyq,
                     m_rfApplyRoute, m_rfApplyGpio, m_rfPing, m_rfRefresh,
                     m_rfStart, m_rfStop, m_rfMts, m_rfReset,
                     m_rfReadHw, m_rfDefaults, m_rfFullReset };
    for (QWidget* w : m_rfControls)
        if (w) w->setEnabled(false);
}

void ControlWindow3D::attachRfdc(const QString& bdf, int bar)
{
    QString target = bdf;
    if (target.isEmpty() && m_rfBdf && m_rfBdf->count() > 0)
        target = m_rfBdf->currentText();
    if (target.isEmpty()) {
        if (m_rfState) {
            m_rfState->setText(tr("No Xilinx PCIe endpoint found."));
            m_rfState->setStyleSheet(QStringLiteral("color:#f87171;"));
        }
        return;
    }

    const quint64 base = m_rfRegBase
        ? m_rfRegBase->text().trimmed().toULongLong(nullptr, 0) : 0;
    // open() and mmap() must run on the thread that will own the descriptor,
    // otherwise every later register access is a cross-thread use of it.
    if (m_rfState) {
        m_rfState->setText(tr("Attaching to %1 BAR%2...").arg(target).arg(bar));
        m_rfState->setStyleSheet(QStringLiteral("color:#9ca3af;"));
    }
    // Attach AND read every register before the UI shows or accepts anything.
    const quint32 tile = quint32(m_rfTile->currentData().toInt());
    const quint32 chan = quint32(m_rfChannel->currentData().toInt());
    const bool    adc  = m_rfTarget->currentData().toInt() != int(sdr::RfdcControl::Dac);
    QMetaObject::invokeMethod(m_rfdc, [this, target, bar, base, tile, chan, adc]{
        m_rfdc->attachAndSync(target, bar, base, tile, chan, adc);
    }, Qt::QueuedConnection);
}

void ControlWindow3D::onRfdcAttachResult(bool ok, const QString& detail)
{
    m_rfAttached = ok;
    for (QWidget* w : m_rfControls)
        if (w) w->setEnabled(ok);
    refreshSimControls();

    if (!m_rfState) return;
    if (ok) {
        m_rfState->setText(tr("Attached: %1").arg(detail));
        m_rfState->setStyleSheet(QStringLiteral("color:#33d17a;"));
    } else {
        m_rfState->setText(detail);
        m_rfState->setStyleSheet(QStringLiteral("color:#f87171;"));
    }
}

void ControlWindow3D::onRfdcAttach()
{
    attachRfdc(m_rfBdf->currentText(), m_rfBar->value());
}

// Select the combo entry whose userData equals `value`; leaves the combo
// untouched if there is no such entry, so an unexpected readback cannot
// silently land on the wrong item.
static void setComboByData(QComboBox* box, int value)
{
    if (!box) return;
    const int i = box->findData(value);
    if (i >= 0) box->setCurrentIndex(i);
}

void ControlWindow3D::onRfdcResetDefaults()
{
    // Whole-panel action: put every control back to its default value. Because
    // this is deliberate (not an incremental edit), it also pushes the default
    // signal chain to an attached device in one go, so the user reaches a known
    // baseline without re-entering — or re-applying — each field by hand.
    //
    // setValue()/setCurrentIndex() do NOT emit editingFinished/activated, so
    // restoring the widgets does not trigger the per-control apply as a side
    // effect; the device write below is the single, explicit one.
    // These MUST match the firmware's boot configuration (PCIE_DEF_* in
    // pcie_regs.h), otherwise "defaults" and a power cycle disagree — which
    // they previously did: decimation was x1 against a boot value of x24, VOP
    // 0 mA against 32 mA, routing 0/1/2/3 against 7/3/2/1, and a sampling rate
    // of 2000 MHz that the PLL cannot even synthesise.
    m_rfTarget->setCurrentIndex(0);        // ADC
    m_rfTile->setCurrentIndex(0);          // All tiles
    m_rfChannel->setCurrentIndex(0);       // Both channels
    setComboByData(m_rfDecim,  int(PCIE_DEF_DECIMATION));
    setComboByData(m_rfInterp, int(PCIE_DEF_INTERPOLATION));
    m_rfNco->setValue(double(PCIE_DEF_NCO_MHZ));
    m_rfNcoPhase->setValue(0);
    m_rfDsa->setValue(double(PCIE_DEF_ADC_DSA_DB10) / 10.0);
    m_rfQmc->setValue(1.0);      // unity, not silence
    m_rfVop->setValue(double(PCIE_DEF_DAC_VOP_UA) / 1000.0);
    setComboByData(m_rfDacSrc, int(PCIE_DEF_DAC_SOURCE));
    m_rfDds->setValue(double(PCIE_DEF_DDS_MHZ));
    m_rfSampMhz->setValue(int(PCIE_DEF_ADC_FS_MHZ));
    m_rfNyquist->setCurrentIndex(0);
    for (int i = 0; i < 4; ++i)
        m_rfAxis[i]->setValue((PCIE_DEF_AXIS_ROUTING >> (i * 4)) & 0xF);
    setComboByData(m_rfPlGpio, int(PCIE_DEF_PL_GPIO_MODE));
    publishTxState();

    if (!m_rfAttached) {
        log(tr("Controls reset to defaults. Attach to apply them to a device."));
        return;
    }

    // One firmware command restores the whole boot configuration, rather than
    // this side replaying a dozen writes and hoping they add up to the same
    // thing. The device is the authority on what "default" means.
    rfdcInvoke([this]{
        if (m_rfdc->restoreDefaults()) {   // runs on the worker thread
            // Show what the device actually did, not what we asked for.
            m_rfdc->readSnapshot(0, 2, true);
        }
    });
}

// ---------------------------------------------------------------------------
// Pull the live values out of the firmware and display them.
//
// Until this existed the panel could only ever show what had last been TYPED,
// which is misleading precisely when it matters: the converter PLL quantises
// sampling-rate requests (2000 MHz becomes 2006.25 MHz on a 300 MHz reference),
// so the achieved rate routinely differs from the requested one.
// ---------------------------------------------------------------------------
void ControlWindow3D::onRfdcReadFromHardware()
{
    const quint32 tile = quint32(m_rfTile->currentData().toInt());
    const quint32 chan = quint32(m_rfChannel->currentData().toInt());
    const bool    adc  = m_rfTarget->currentData().toInt() != int(sdr::RfdcControl::Dac);
    // One queued pass instead of thirteen blocking round trips from this thread.
    rfdcInvoke([this, tile, chan, adc]{ m_rfdc->readSnapshot(tile, chan, adc); });
}

// Queue work onto the RFDC thread. Never call RfdcControl directly from here:
// its transactions block, and this is the GUI thread.
void ControlWindow3D::rfdcInvoke(const std::function<void()>& job)
{
    if (!m_rfAttached) { log(tr("Not attached to the RFDC control interface.")); return; }
    if (m_rfBusy)      { log(tr("A command is already in flight — wait or press Abort.")); return; }
    QMetaObject::invokeMethod(m_rfdc, job, Qt::QueuedConnection);
}

void ControlWindow3D::onRfdcPing()
{
    rfdcInvoke([this]{ m_rfdc->ping(); });
}

void ControlWindow3D::onRfdcBusyChanged(bool busy)
{
    m_rfBusy = busy;
    if (m_rfBusyBar) m_rfBusyBar->setVisible(busy);
    if (m_rfAbort)   m_rfAbort->setVisible(busy);

    // Disable the device-touching controls while one is in flight, so a second
    // command cannot be queued behind a 15 s PLL reprogram.
    for (QWidget* w : {static_cast<QWidget*>(m_rfPing),   static_cast<QWidget*>(m_rfRefresh),
                       static_cast<QWidget*>(m_rfStart),  static_cast<QWidget*>(m_rfStop),
                       static_cast<QWidget*>(m_rfMts),    static_cast<QWidget*>(m_rfReset),
                       static_cast<QWidget*>(m_rfReadHw), static_cast<QWidget*>(m_rfDefaults),
                       static_cast<QWidget*>(m_rfApplyRate), static_cast<QWidget*>(m_rfApplyNyq),
                       static_cast<QWidget*>(m_rfApplyRoute), static_cast<QWidget*>(m_rfApplyGpio)})
        if (w) w->setEnabled(!busy && m_rfAttached);

    if (!busy) onRfdcRefresh();
}

// Populate every field from one aggregated read, so the panel shows what the
// hardware IS rather than what was last typed.
void ControlWindow3D::onRfdcSnapshot(const QVariantMap& v)
{
    if (v.value(QStringLiteral("count")).toInt() == 0) {
        log(v.value(QStringLiteral("supported")).toBool()
                ? tr("Read from hardware returned nothing.")
                : tr("This firmware build does not support readback — fields "
                     "still show the last values sent."));
        return;
    }

    auto has = [&](const char* k){ return v.contains(QLatin1String(k)); };
    auto u   = [&](const char* k){ return v.value(QLatin1String(k)).toUInt(); };

    if (has("fs_mhz"))        m_rfSampMhz->setValue(int(u("fs_mhz")));
    if (has("decimation") && u("decimation"))    setComboByData(m_rfDecim,  int(u("decimation")));
    if (has("interpolation") && u("interpolation")) setComboByData(m_rfInterp, int(u("interpolation")));
    if (has("nco_khz"))       m_rfNco->setValue(double(qint32(u("nco_khz"))) / 1000.0);
    if (has("dds_hz") && u("dds_hz"))  m_rfDds->setValue(double(u("dds_hz")) / 1e6);
    if (has("dsa_db10"))      m_rfDsa->setValue(double(u("dsa_db10")) / 10.0);
    if (has("vop_ua") && u("vop_ua"))  m_rfVop->setValue(double(u("vop_ua")) / 1000.0);
    if (has("qmc_10000"))     m_rfQmc->setValue(double(u("qmc_10000")) / 10000.0);
    if (has("dac_source"))    setComboByData(m_rfDacSrc, int(u("dac_source")));
    if (has("pl_gpio") && u("pl_gpio")) setComboByData(m_rfPlGpio, int(u("pl_gpio")));
    if (has("axis"))
        for (int i = 0; i < 4; ++i) m_rfAxis[i]->setValue(int((u("axis") >> (i * 4)) & 0xF));
    if (has("nyquist") && (u("nyquist") == 1 || u("nyquist") == 2))
        m_rfNyquist->setCurrentIndex(int(u("nyquist")) - 1);

    // Single source of truth: the device snapshot also drives the Signal chain
    // tab and the transmit sample rate, so no two views can disagree about
    // what the hardware is doing.
    if (has("decimation") && u("decimation") && m_decim)
        m_decim->setValue(int(u("decimation")));
    if (has("interpolation") && u("interpolation") && m_interp)
        m_interp->setValue(int(u("interpolation")));

    if (has("stream_khz") && u("stream_khz")) {
        const double mhz = double(u("stream_khz")) / 1000.0;
        log(tr("Stream clock: %1 MHz").arg(mhz, 0, 'f', 3));
    }
    // The DAC stream clock IS the host transmit sample rate — one I/Q pair per
    // clock. It used to be taken from "stream_khz", which is the ADC clock when
    // the panel targets the ADC (the default); the two only agree at the boot
    // configuration, so any decimation change silently rescaled every
    // generated frequency.
    if (has("dac_stream_khz") && u("dac_stream_khz")) {
        const double mhz = double(u("dac_stream_khz")) / 1000.0;
        if (m_rate && mhz > 0.0) m_rate->setValue(mhz);
        if (m_genRate && mhz > 0.0 && std::abs(m_genRate->value() - mhz) > 1e-6) {
            m_genRate->setValue(mhz);        // valueChanged -> hot update
            log(tr("IQ generator sample rate follows the DAC stream clock: %1 MSPS")
                    .arg(mhz, 0, 'f', 3));
        }
    }
    // Device cache for the live transmit chain (Transmit tab).
    if (has("dac_source"))      m_devDacSrc = int(u("dac_source"));
    if (has("qmc_10000"))       m_devAdcQmc = double(u("qmc_10000")) / 10000.0;
    if (has("dac_fs_mhz"))      m_devDacFs = double(u("dac_fs_mhz"));
    if (has("adc_fs_mhz"))      m_devAdcFs = double(u("adc_fs_mhz"));
    if (has("dac_stream_khz"))  m_devDacStream = double(u("dac_stream_khz")) / 1000.0;
    if (has("adc_stream_khz"))  m_devAdcStream = double(u("adc_stream_khz")) / 1000.0;
    if (has("interpolation"))   m_devInterp = int(u("interpolation"));
    if (has("decimation"))      m_devDecim = int(u("decimation"));
    if (has("dac_nco_khz") && has("adc_nco_khz")) {
        m_devDacNco = double(qint32(u("dac_nco_khz"))) / 1000.0;
        m_devAdcNco = double(qint32(u("adc_nco_khz"))) / 1000.0;
        m_devNcoKnown = true;
    }
    if (m_devAdcStream > 0.0 || m_devDacStream > 0.0)
        emit deviceRatesRead(m_devAdcStream, m_devDacStream);

    publishTxState();

    log(tr("Read %1 live values from the device.").arg(v.value(QStringLiteral("count")).toInt()));

    // ---- advisory checks on the configuration just read ------------------
    //
    // These are the two states that silently destroy a measurement. Both were
    // hit in practice: ENOB fell from 11 to 6.5 bits and nothing on screen
    // said why, because each setting is individually legal.
    QStringList advice;

    // Decimation is a low-pass then a downsample: it discards out-of-band
    // noise while keeping the signal, so it is worth 10*log10(D) of SNR.
    // Dropping 24x to 1x throws away 13.8 dB, which is 2.3 ENOB bits.
    if (has("decimation")) {
        const uint d = u("decimation");
        if (d <= 1 && PCIE_DEF_DECIMATION > 1)
            advice << tr("Decimation is %1x; this design boots at %2x. "
                         "Decimation provides 10*log10(%2) = %3 dB of processing "
                         "gain, so running at 1x costs about %4 ENOB bits.")
                       .arg(d).arg(PCIE_DEF_DECIMATION)
                       .arg(10.0 * std::log10(double(PCIE_DEF_DECIMATION)), 0, 'f', 1)
                       .arg(10.0 * std::log10(double(PCIE_DEF_DECIMATION)) / 6.02, 0, 'f', 1);
    }

    // A DAC fed from the host stream with no transmitter running produces
    // nothing at all. The ADC then measures its own noise floor, "signal"
    // becomes the largest noise bin, and SINAD - hence ENOB - is meaningless.
    if (has("dac_source") && u("dac_source") == PCIE_DAC_SRC_HOST
        && !m_genRunning)
        advice << tr("DAC input is the HOST stream, not the internal DDS. "
                     "If H2C playback is not running there is no signal at "
                     "all, and any SNR/ENOB reading is measuring noise. "
                     "Set it to DDS compiler to test the converters alone.");
    if (has("dac_source") && u("dac_source") == PCIE_DAC_SRC_NONE)
        advice << tr("No input is routed to the DAC at all (the switch reports "
                     "none). Select DDS compiler or Host / GNU Radio stream.");

    if (has("qmc_10000")) {
        const double g = double(u("qmc_10000")) / 10000.0;
        if (g < 0.05)
            advice << tr("QMC gain is %1 — essentially zero, which mutes the "
                         "signal. The boot value is 1.0.").arg(g, 0, 'f', 4);
    }

    for (const QString& a : advice) log(tr("NOTE: %1").arg(a));
    if (!advice.isEmpty() && m_rfState) {
        m_rfState->setText(tr("Read OK — %1 advisory note(s), see the log.")
                               .arg(advice.size()));
        m_rfState->setStyleSheet(QStringLiteral("color:#fbbf24;"));
    }
}

void ControlWindow3D::onRfdcRefresh()
{
    // Queue the read; the answer arrives via registersRead(). Reading the BAR
    // from this thread would race the worker for the same descriptor.
    if (!m_rfAttached) { m_rfRegs->setText(tr("ring = ----  cfg = ----")); return; }
    QMetaObject::invokeMethod(m_rfdc, [this]{ m_rfdc->pollRegisters(); },
                              Qt::QueuedConnection);
}

// While a sync or reset owns the device the panel is locked: the spec requires
// that the UI cannot issue conflicting operations during those sequences.
// Returns true if this edit should be transmitted immediately.
//
// Apply-on-edit is only safe when the fields hold values that came FROM the
// device. If readback is unavailable the fields hold software defaults, and
// auto-sending them is exactly how QMC 0.0000 (silence) and DAC VOP 0 mA got
// written to the hardware. In that mode the edit is recorded as dirty and the
// operator commits explicitly, so an untouched field can never be sent.
bool ControlWindow3D::sendOnEdit(QWidget* w)
{
    if (m_rfConfirmed) return true;

    if (w) {
        m_rfDirty.insert(w);
        w->setStyleSheet(QStringLiteral("border:1px solid #fbbf24;"));
    }
    if (m_rfApplyDirty) {
        m_rfApplyDirty->setEnabled(true);
        m_rfApplyDirty->setText(tr("Apply %1 changed value(s)").arg(m_rfDirty.size()));
    }
    log(tr("Recorded (not sent): readback is unavailable, so values are only "
           "transmitted when you press Apply."));
    return false;
}

// Send ONLY the fields the operator actually edited.
void ControlWindow3D::onRfdcApplyDirty()
{
    if (m_rfDirty.isEmpty()) return;

    const int  tg = m_rfTarget->currentData().toInt();
    const auto t  = quint32(m_rfTile->currentData().toInt());
    const auto c  = quint32(m_rfChannel->currentData().toInt());

    // Order matters: rates move the stream clock, so they go first and MTS
    // follows; the DDS is derived from the DAC stream clock and goes last.
    const bool dDecim  = m_rfDirty.contains(m_rfDecim);
    const bool dInterp = m_rfDirty.contains(m_rfInterp);
    const quint32 fDec = quint32(m_rfDecim->currentData().toInt());
    const quint32 fInt = quint32(m_rfInterp->currentData().toInt());
    const bool dNco    = m_rfDirty.contains(m_rfNco);
    const double vNco  = m_rfNco->value();
    const bool dPh     = m_rfDirty.contains(m_rfNcoPhase);
    const int    vPh   = m_rfNcoPhase->value();
    const bool dDsa    = m_rfDirty.contains(m_rfDsa);
    const double vDsa  = m_rfDsa->value();
    const bool dQmc    = m_rfDirty.contains(m_rfQmc);
    const double vQmc  = m_rfQmc->value();
    const bool dVop    = m_rfDirty.contains(m_rfVop);
    const double vVop  = m_rfVop->value();
    const bool dSrc    = m_rfDirty.contains(m_rfDacSrc);
    const int    vSrc  = m_rfDacSrc->currentData().toInt();
    const bool dDds    = m_rfDirty.contains(m_rfDds);
    const double vDds  = m_rfDds->value();

    log(tr("Applying %1 changed value(s)...").arg(m_rfDirty.size()));

    rfdcInvoke([=]{
        bool rates = false;
        if (dDecim)  rates |= m_rfdc->setDecimation(t, c, fDec);
        if (dInterp) rates |= m_rfdc->setInterpolation(t, c, fInt);
        if (rates)   m_rfdc->realignMts();
        if (dNco)    m_rfdc->setNcoFrequency(tg, t, c, vNco);
        if (dPh)     m_rfdc->setNcoPhase(tg, t, c, vPh);
        if (dDsa)    m_rfdc->setAdcDsa(t, c, vDsa);
        if (dQmc)    m_rfdc->setQmcGain(tg, t, c, vQmc);
        if (dVop)    m_rfdc->setDacVop(t, c, vVop);
        if (dSrc)    m_rfdc->setDacSource(vSrc);
        if (dDds)    m_rfdc->setDdsFrequency(vDds);   // last: derived value
    });

    for (QWidget* w : m_rfDirty) if (w) w->setStyleSheet(QString());
    m_rfDirty.clear();
    m_rfApplyDirty->setEnabled(false);
    m_rfApplyDirty->setText(tr("Apply changed values"));
}

void ControlWindow3D::onRfdcSyncing(bool syncing)
{
    m_rfSyncing = syncing;
    if (m_rfBusyBar) m_rfBusyBar->setVisible(syncing || m_rfBusy);
    if (m_rfAbort)   m_rfAbort->setVisible(syncing || m_rfBusy);
    if (m_rfSyncState) {
        m_rfSyncState->setText(syncing ? tr("Synchronising with device…") : QString());
        m_rfSyncState->setVisible(syncing);
    }
    for (QWidget* w : m_rfControls)
        if (w) w->setEnabled(!syncing && !m_rfBusy && m_rfAttached && m_rfConfirmed);
    refreshSimControls();
}

void ControlWindow3D::onRfdcSyncFinished(bool ok, const QString& detail)
{
    log(detail);
    if (m_rfState) {
        m_rfState->setText(detail);
        m_rfState->setStyleSheet(ok ? QStringLiteral("color:#33d17a;")
                                    : QStringLiteral("color:#fbbf24;"));
    }
    // Spec: "default UI values must not overwrite existing device values".
    // If we could not read the device, the fields hold software defaults, and
    // the edit-on-change controls would push those defaults into the hardware
    // the moment anything is touched. That has already been observed writing
    // QMC gain 0.0000 (silence) and DAC VOP 0 mA (out of range). So on a failed
    // sync the panel stays READ-ONLY.
    m_rfConfirmed = ok;

    // The panel stays USABLE either way. What changes is whether an edit is
    // transmitted immediately (values came from the device) or merely recorded
    // until the operator commits (values are software defaults).
    for (QWidget* w : m_rfControls)
        if (w) w->setEnabled(m_rfAttached && !m_rfSyncing && !m_rfBusy);
    refreshSimControls();

    if (m_rfApplyDirty) {
        m_rfApplyDirty->setVisible(!ok);
        m_rfApplyDirty->setEnabled(false);
    }
    if (m_rfEditHint) {
        m_rfEditHint->setText(ok
            ? tr("Values were read from the device. Each edit is sent as soon "
                 "as you finish editing it.")
            : tr("⚠ Values below are SOFTWARE DEFAULTS — the device could not "
                 "be read. Edits are recorded, not sent, until you press "
                 "\"Apply changed values\". Untouched fields are never "
                 "transmitted."));
        m_rfEditHint->setStyleSheet(ok ? QStringLiteral("color:#9ca3af;")
                                       : QStringLiteral("color:#fbbf24;"));
    }
}

// Never report success on an unverified write.
void ControlWindow3D::onRfdcVerify(const QString& name, bool ok, quint32 req,
                                   quint32 act, const QString& detail)
{
    log(tr("%1: %2").arg(name, detail));

    if (ok) {
        // Show what the DEVICE holds, not what was typed.
        if (name.startsWith(tr("Sampling")) && act) m_rfSampMhz->setValue(int(act));
        if (m_rfState) {
            m_rfState->setText(tr("%1 confirmed: %2").arg(name).arg(act));
            m_rfState->setStyleSheet(QStringLiteral("color:#33d17a;"));
        }
    } else {
        if (m_rfState) {
            m_rfState->setText(tr("%1 NOT confirmed — %2").arg(name, detail));
            m_rfState->setStyleSheet(QStringLiteral("color:#f87171;"));
        }
        QMessageBox::warning(this, tr("Verification failed"),
            tr("<b>%1 could not be confirmed.</b><br><br>"
               "Requested: %2<br>Device reports: %3<br><br>%4")
                .arg(name).arg(req).arg(act).arg(detail));
    }
    onRfdcRefresh();
}

void ControlWindow3D::onRfdcResetFinished(bool ok, const QString& detail)
{
    log(detail);
    if (ok) {
        QMessageBox::information(this, tr("Complete Reset"),
            tr("<b>Reset completed successfully.</b><br><br>%1").arg(detail));
    } else {
        QMessageBox::critical(this, tr("Complete Reset failed"),
            tr("<b>The reset did not complete.</b><br><br>%1<br><br>"
               "The device may be in a partially reset state — check the log "
               "and the serial console.").arg(detail));
    }
    onRfdcRefresh();
}

// Separated from the ordinary controls because it changes many settings at once.
void ControlWindow3D::onRfdcCompleteReset()
{
    const auto answer = QMessageBox::question(this, tr("Complete Reset"),
        tr("<b>Restore every RF Data Converter parameter to its default "
           "state?</b><br><br>"
           "This will stop the datapath, restore the firmware's boot "
           "configuration (4800/9600 MHz, ×24, NCO 3100 MHz, DDS 10 MHz, "
           "32 mA VOP, routing 7/3/2/1), restart the datapath and re-read "
           "every register.<br><br>"
           "It takes several seconds and affects all channels."),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);

    if (answer != QMessageBox::Yes) { log(tr("Complete reset cancelled.")); return; }

    const quint32 tile = quint32(m_rfTile->currentData().toInt());
    const quint32 chan = quint32(m_rfChannel->currentData().toInt());
    const bool    adc  = m_rfTarget->currentData().toInt() != int(sdr::RfdcControl::Dac);
    rfdcInvoke([this, tile, chan, adc]{ m_rfdc->completeReset(tile, chan, adc); });
}

void ControlWindow3D::onRfdcRegisters(quint32 ring, quint32 cfg)
{
    // Decode the status the firmware embeds in the acknowledge, so a rejected
    // command is visible here and not only in the log.
    QString extra;
    if (RING_ACK_VALID(ring)) {
        const unsigned e = RING_ACK_ERR(ring);
        extra = e ? tr("   ACK err=0x%1").arg(e, 0, 16) : tr("   ACK ok");
    }
    m_rfRegs->setText(tr("ring = 0x%1   cfg = 0x%2   NEW_CMD=%3%4")
                          .arg(ring, 8, 16, QLatin1Char('0'))
                          .arg(cfg,  8, 16, QLatin1Char('0'))
                          .arg((ring >> 31) & 1)
                          .arg(extra));
}

// ---------------------------------------------------------------------------
// IQ generator panel.
//
// The playback chain is  <producer> -> /tmp/iwfg_h2c.fifo -> iwfg_h2c -> DAC.
// iwfg_h2c is only the FIFO->card mover; until now nothing wrote INTO the FIFO,
// so the pipe had no producer and the DAC received nothing. The control block
// at /dev/shm/iqctl carries pacing and frequency metadata, not samples — which
// is why transmit looked configured but silent.
// ---------------------------------------------------------------------------
void ControlWindow3D::buildGeneratorPanel(QWidget* page)
{
    auto* v = qobject_cast<QVBoxLayout*>(page->layout());
    if (!v) return;

    m_genThread = new QThread(this);
    m_genThread->setObjectName(QStringLiteral("iqGen"));
    m_gen = new sdr::IqGenerator;             // no parent: owned by the thread
    m_gen->moveToThread(m_genThread);
    connect(m_genThread, &QThread::finished, m_gen, &QObject::deleteLater);
    m_genThread->start();

    auto* box = new QGroupBox(tr("IQ Signal Generator  (Host / GNU Radio stream → H2C FIFO)"));
    auto* f = new QFormLayout(box);

    m_genFifo = new QLineEdit(QStringLiteral("/tmp/iwfg_h2c.fifo"));
    m_genFifo->setToolTip(tr("The FIFO iwfg_h2c reads from. Start the H2C "
                             "backend first — it is this FIFO's consumer."));

    // The generator's clock is the DAC stream clock, NOT the transmit panel's
    // iq_ctl rate (that belongs to the RDMA transmitter and is 0 when no
    // control block exists — which made the generator run at 1 kSPS).
    m_genRate = new QDoubleSpinBox;
    m_genRate->setRange(1.0, 20000.0);
    m_genRate->setDecimals(3);
    m_genRate->setSuffix(tr(" MSPS"));
    m_genRate->setKeyboardTracking(false);
    m_genRate->setValue(double(PCIE_DEF_DAC_FS_MHZ)
                        / (2.0 * double(PCIE_DEF_INTERPOLATION)));
    m_genRate->setToolTip(tr("DAC stream clock: one I/Q pair per clock. Filled in "
                             "from the device when the RFDC panel is attached "
                             "(200 MSPS at the boot configuration, 9600 MSPS / "
                             "(24 x 2)). Every generated frequency is relative to "
                             "this rate."));

    m_genWave = new QComboBox;
    // Numbering matches iq_source_v9, so the behaviour is identical to the
    // GNU Radio flowgraph operators already know.
    m_genWave->addItem(tr("Sine"),     int(sdr::IqGenerator::Sine));
    m_genWave->addItem(tr("Cosine"),   int(sdr::IqGenerator::Cosine));
    m_genWave->addItem(tr("Square"),   int(sdr::IqGenerator::Square));
    m_genWave->addItem(tr("Sawtooth"), int(sdr::IqGenerator::Saw));
    m_genWave->addItem(tr("Triangle"), int(sdr::IqGenerator::Triangle));
    m_genWave->addItem(tr("Noise"),    int(sdr::IqGenerator::Noise));

    // keyboardTracking off: a value is taken when committed (Enter, focus out,
    // arrows), so typing "25" never transmits 2 MHz on the way.
    m_genTone = new QDoubleSpinBox;
    m_genTone->setRange(-100.0, 100.0);
    m_genTone->setDecimals(6);
    m_genTone->setSuffix(tr(" MHz"));
    m_genTone->setKeyboardTracking(false);
    m_genTone->setValue(10.0);

    m_genTone2 = new QDoubleSpinBox;
    m_genTone2->setRange(-180.0, 180.0);
    m_genTone2->setDecimals(3);
    m_genTone2->setSuffix(tr(" deg"));
    m_genTone2->setKeyboardTracking(false);
    m_genTone2->setValue(-90.0);
    m_genTone2->setToolTip(tr("IQ phase difference in degrees. -90 is standard "
                              "IQ; other values deliberately unbalance I/Q."));

    m_genAmp = new QDoubleSpinBox;
    m_genAmp->setRange(0.0, 1.0);
    m_genAmp->setSingleStep(0.05);
    m_genAmp->setDecimals(2);
    m_genAmp->setKeyboardTracking(false);
    m_genAmp->setValue(0.90);
    m_genAmp->setToolTip(tr("Fraction of full scale. Full scale is 8191<<2 = "
                            "32764, i.e. 14-bit MSB-aligned, which is the "
                            "packing the RFSoC DAC requires."));

    m_genCoherent = new QCheckBox(tr("Loop-coherent (clean tone)"));
    m_genCoherent->setChecked(true);
    m_genCoherent->setToolTip(tr(
        "iwfg_h2c replays the chunk it last received as a loop. With this on, "
        "the generator builds one seamless loop the size of the H2C buffer "
        "(the tone is placed on the nearest frequency that fits it — about "
        "48 Hz steps with the 16 MiB buffer), writes it once, and the card "
        "replays it with nothing further crossing the FIFO. Off: a new chunk "
        "is streamed continuously, as the GNU Radio chain did; iwfg_h2c must "
        "copy each one between transfers, which interrupts the DAC feed. Not "
        "recommended."));

    m_genActual = new QLabel;
    m_genActual->setObjectName(QStringLiteral("hintLabel"));
    m_genActual->setWordWrap(true);

    m_genLink = new QLabel(tr("H2C link: no reading yet (iwfg_h2c reports once "
                              "a second while it is transferring)."));
    m_genLink->setObjectName(QStringLiteral("hintLabel"));
    m_genLink->setWordWrap(true);
    m_genLink->setToolTip(tr("iwfg_h2c's own measured delivery to the card, "
                             "against what the DAC consumes (one 32-bit I/Q "
                             "word per DAC stream clock). The DAC input has no "
                             "flow control: any shortfall is played as stale "
                             "data, which shows as broadband interference and "
                             "a line at 0 Hz."));

    m_genBtn = new QPushButton(tr("Start generator"));
    m_genState = new QLabel(tr("Stopped"));
    m_genState->setWordWrap(true);
    m_genState->setStyleSheet(QStringLiteral("color:#9ca3af;"));

    f->addRow(tr("FIFO"), m_genFifo);
    f->addRow(tr("Sample rate"), m_genRate);
    f->addRow(tr("Waveform"), m_genWave);
    f->addRow(tr("Tone"), m_genTone);
    f->addRow(QString(), m_genActual);
    f->addRow(tr("IQ phase diff"), m_genTone2);
    f->addRow(tr("Amplitude"), m_genAmp);
    f->addRow(QString(), m_genCoherent);
    f->addRow(QString(), m_genBtn);
    f->addRow(m_genState);
    f->addRow(m_genLink);
    v->addWidget(box);

    connect(m_genBtn, &QPushButton::clicked, this, &ControlWindow3D::onGenToggle);
    connect(m_gen, &sdr::IqGenerator::progress, this, &ControlWindow3D::onGenProgress);
    connect(m_gen, &sdr::IqGenerator::connectedChanged, this, [this](bool c){
        m_genConnected = c;
        if (!m_genRunning) return;
        if (c) {
            log(tr("H2C FIFO connected — iwfg_h2c is reading."));
            setGenState(tr("Transmitting"), "#33d17a");
        } else {
            log(tr("H2C FIFO reader detached."));
            setGenState(tr("Waiting for iwfg_h2c to open %1 — is the H2C backend "
                           "running?").arg(m_genFifo->text()), "#fbbf24");
        }
        publishTxState();
    });
    connect(m_gen, &sdr::IqGenerator::started, this, [this]{
        m_genRunning = true;
        m_genConnected = false;
        m_genBtn->setText(tr("Stop generator"));
        setGenState(tr("Waiting for iwfg_h2c to open %1 — is the H2C backend "
                       "running?").arg(m_genFifo->text()), "#fbbf24");
        publishTxState();
    });
    connect(m_gen, &sdr::IqGenerator::stopped, this, [this]{
        m_genRunning = false;
        m_genConnected = false;
        m_genBtn->setText(tr("Start generator"));
        setGenState(tr("Stopped"), "#9ca3af");
        publishTxState();
        if (m_genRestartPending) {             // H2C buffer changed under us
            m_genRestartPending = false;
            const sdr::IqGenerator::Config cfg = genConfig();
            QMetaObject::invokeMethod(m_gen, [this, cfg]{ m_gen->start(cfg); },
                                      Qt::QueuedConnection);
        }
    });
    connect(m_gen, &sdr::IqGenerator::loopLoaded, this, [this](quint64 loads){
        const int bytes = m_h2cChunkBytes;
        log(tr("Loop %1 handed to iwfg_h2c: %2 MiB, %3 samples, seamless — the "
               "card replays it with nothing more crossing the FIFO.")
                .arg(loads).arg(double(bytes) / 1048576.0, 0, 'f', 2).arg(bytes / 4));
    });
    connect(m_gen, &sdr::IqGenerator::silenced, this, [this]{
        log(tr("DAC silenced: a zero chunk was sent, so the card is not left "
               "replaying the last tone."));
    });
    connect(m_gen, &sdr::IqGenerator::error, this, [this](const QString& why){
        log(why);
        setGenState(why, "#f87171");
    });
    connect(m_gen, &sdr::IqGenerator::transmitting, this,
            [this](double req, double act, int pairs, bool coherent){
        if (coherent && std::abs(act - req) >= 0.5)
            log(tr("Generator: %1 MHz requested, transmitting %2 MHz — the nearest "
                   "frequency that loops seamlessly in the %3-sample H2C buffer.")
                    .arg(req / 1e6, 0, 'f', 6).arg(act / 1e6, 0, 'f', 6).arg(pairs));
        else
            log(tr("Generator: transmitting %1 MHz.").arg(act / 1e6, 0, 'f', 6));
    });

    // Parameter edits: preview always, hot-apply while running, publish for
    // the simulator. valueChanged fires only on commit (keyboardTracking off).
    connect(m_genWave, QOverload<int>::of(&QComboBox::activated),
            this, [this](int){ onGenParamsEdited(); });
    for (QDoubleSpinBox* w : {m_genRate, m_genTone, m_genTone2, m_genAmp})
        connect(w, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double){ onGenParamsEdited(); });
    connect(m_genCoherent, &QCheckBox::toggled, this, [this](bool){ onGenParamsEdited(); });

    updateGenPreview();
}

void ControlWindow3D::onGenToggle()
{
    // Simulation: the generator is modelled, never written to the FIFO — the
    // plots show the simulated loopback of exactly these parameters.
    if (m_simMode) {
        m_simGenRunning = !m_simGenRunning;
        if (m_simGenRunning) {
            enableSimLoopback();
            if (m_rfDacSrc && m_rfDacSrc->currentData().toInt() != int(PCIE_DAC_SRC_HOST)) {
                setComboByData(m_rfDacSrc, int(PCIE_DAC_SRC_HOST));
                log(tr("Simulation: DAC input switched to Host / GNU Radio stream, "
                       "so the loopback shows the generator."));
            }
            m_genBtn->setText(tr("Stop generator"));
            setGenState(tr("Running (simulated) — nothing is written to the H2C FIFO"),
                        "#33d17a");
            log(tr("Simulation: IQ generator started."));
        } else {
            m_genBtn->setText(tr("Start generator"));
            setGenState(tr("Stopped"), "#9ca3af");
            log(tr("Simulation: IQ generator stopped."));
        }
        publishTxState();
        return;
    }

    if (m_genRunning) {
        m_gen->stop();                       // atomic flag; safe cross-thread
        return;
    }

    const sdr::IqGenerator::Config cfg = genConfig();
    if (cfg.fifoPath.isEmpty()) {
        setGenState(tr("Enter the H2C FIFO path first."), "#f87171");
        return;
    }

    // The generator is pointless unless the DAC takes the host stream.
    ensureHostRouting();
    // Show the real chain it will run through (rates, NCOs, DAC input).
    if (m_rfAttached && !m_rfBusy && !m_rfSyncing) onRfdcReadFromHardware();

    log(tr("Starting IQ generator: %1, %2 MSPS, into %3")
            .arg(m_genWave->currentText()).arg(m_genRate->value(), 0, 'f', 3)
            .arg(cfg.fifoPath));

    QMetaObject::invokeMethod(m_gen, [this, cfg]{ m_gen->start(cfg); },
                              Qt::QueuedConnection);
}

void ControlWindow3D::onGenProgress(quint64 total, double msps,
                                    quint64 chunks, quint64 reconnects)
{
    // Proof that data is actually leaving the application, which is what the
    // brief asked for: a count the operator can watch increase.
    Q_UNUSED(total);
    QString t;
    const auto cfg = genConfig();
    const double hz = sdr::IqGenerator::transmittedHz(cfg);
    const QString what = cfg.waveform == sdr::IqGenerator::Noise
        ? tr("noise") : tr("%1 MHz %2").arg(hz / 1e6, 0, 'f', 6)
                                         .arg(m_genWave->currentText().toLower());
    if (cfg.coherent) {
        // Every chunk is the same seamless loop; iwfg_h2c replays it at the
        // full DAC rate between refreshes, so the refresh count is the proof
        // of life, not a throughput figure.
        Q_UNUSED(msps);
        const int pairs = sdr::IqGenerator::loopPairsFor(cfg.chunkBytes);
        t = tr("Transmitting %1 — one seamless %2-sample loop, replayed by the "
               "card (loaded %3×)").arg(what).arg(pairs).arg(chunks);
    } else {
        t = tr("Transmitting %1 — %2 MSPS written, %3 chunks")
                .arg(what).arg(msps, 0, 'f', 2).arg(chunks);
    }
    if (reconnects) t += tr("  (reconnects: %1)").arg(reconnects);
    setGenState(t, "#33d17a");
}

void ControlWindow3D::buildTransmitPanel()
{
    QWidget* page = addControlPanel(tr("Transmit"));
    auto* v = qobject_cast<QVBoxLayout*>(page->layout());
    v->setContentsMargins(12, 12, 12, 12);
    v->setSpacing(10);

    // ---- the whole transmit chain, as the device reports it ---------------
    // H2C transmit was shown as a generator in isolation, so nothing on screen
    // said whether its samples reached the DAC, at what rate, through which
    // NCO, or whether they came back through the ADC. This group states each
    // of those from the device, and proves the last one on demand.
    auto* chain = new QGroupBox(tr("H2C TRANSMIT CHAIN — live from the device"));
    auto* cfm = new QFormLayout(chain);
    cfm->setContentsMargins(12, 14, 12, 12);
    cfm->setSpacing(6);
    auto mk = [](){ auto* l = new QLabel(QStringLiteral("—")); l->setWordWrap(true);
                    l->setTextInteractionFlags(Qt::TextSelectableByMouse); return l; };
    m_chSrc = mk(); m_chDac = mk(); m_chNco = mk(); m_chAdc = mk();
    m_chLink = mk(); m_chRx = mk(); m_chH2c = mk(); m_chC2h = mk();
    cfm->addRow(tr("DAC input"), m_chSrc);
    cfm->addRow(tr("DAC"), m_chDac);
    cfm->addRow(tr("NCO / RF"), m_chNco);
    cfm->addRow(tr("ADC (loopback)"), m_chAdc);
    cfm->addRow(tr("H2C playback"), m_chH2c);
    cfm->addRow(tr("H2C to card"), m_chLink);
    cfm->addRow(tr("C2H capture"), m_chC2h);
    cfm->addRow(tr("Received"), m_chRx);

    // Recovery actions, shown when they apply.
    m_retryTxBtn = new QPushButton(tr("Retry transmit"));
    m_retryTxBtn->setToolTip(tr(
        "Relaunch the H2C playback helper (iwfg_h2c) with the default buffer. Use it "
        "after fixing the DAC path, or when the chain shows the helper stalled, failed "
        "or stepped down to a small buffer. The generator reconnects by itself."));
    m_oneChBtn = new QPushButton(tr("Capture 1 channel (PL GPIO mode 1)"));
    m_oneChBtn->setToolTip(tr(
        "Sets PL GPIO routing mode 1 (a single channel in the PL) and the display to one "
        "channel. The capture then needs a quarter of the PCIe bandwidth, so the C2H FIFO "
        "stops overflowing — and stops resetting the QDMA under H2C transmit. The channel "
        "kept is AXIS output 0 (ADC Channel Routing on the RFDC tab). The measured rate is "
        "checked afterwards and the change undone if the stream did not shrink."));
    {
        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(m_retryTxBtn);
        h->addWidget(m_oneChBtn);
        h->addStretch(1);
        cfm->addRow(QString(), row);
    }
    connect(m_retryTxBtn, &QPushButton::clicked, this, [this] {
        log(tr("Retry transmit: relaunching iwfg_h2c with the default buffer."));
        emit h2cRestartRequested();
    });
    connect(m_oneChBtn, &QPushButton::clicked, this, [this] {
        if (!m_rfAttached || !m_rfdc) {
            log(tr("Attach the RFDC tab first: the PL GPIO mode is set over PCIe."));
            return;
        }
        m_plGpioBefore = m_rfPlGpio ? m_rfPlGpio->currentData().toInt() : 3;
        applyPlGpioMode(1, tr("single channel; display → 1 channel"));
        emit captureChannelsRequested(1);
    });

    m_verifyBtn = new QPushButton(tr("Verify loopback — hop the tone and watch the ADC"));
    m_verifyBtn->setToolTip(tr(
        "Moves the generator's tone by a few MHz and back, and checks that the "
        "tone received through the ADC moves with it. The only proof that the "
        "generated samples reach the DAC: a DMA that completes, or a tone that "
        "happens to be on screen, could be something else (e.g. the DDS)."));
    m_chVerify = new QLabel;
    m_chVerify->setWordWrap(true);
    m_chVerify->setTextInteractionFlags(Qt::TextSelectableByMouse);
    cfm->addRow(QString(), m_verifyBtn);
    cfm->addRow(m_chVerify);
    v->addWidget(chain);
    connect(m_verifyBtn, &QPushButton::clicked, this, &ControlWindow3D::onVerifyLoopback);

    m_lcTimer = new QTimer(this);
    m_lcTimer->setInterval(100);
    connect(m_lcTimer, &QTimer::timeout, this, &ControlWindow3D::loopCheckTick);

    buildGeneratorPanel(page);

    // ---- RDMA transmitter control block (RoCEv2 path only) ----------------
    // The three fields `rdma_tx` polls from its iq_ctl block. They do NOT
    // affect the PCIe H2C generator above, and showing them unlabelled in a
    // PCIe session (100 MSPS / 2450 MHz beside a 200 MSPS generator) read as
    // "these are my transmit parameters". Shown only for the RoCEv2 path.
    m_rdmaBox = new QGroupBox(tr("RDMA TRANSMITTER — rdma_tx control block "
                                 "(RoCEv2 path; not used by PCIe H2C)"));
    auto* rv = new QVBoxLayout(m_rdmaBox);

    m_ctlState = new QLabel;
    m_ctlState->setObjectName(QStringLiteral("hintLabel"));
    m_ctlState->setWordWrap(true);
    rv->addWidget(m_ctlState);

    auto* g = new QWidget;
    auto* f = new QFormLayout(g);
    f->setContentsMargins(0, 4, 0, 4);
    f->setSpacing(8);

    m_rate = new QDoubleSpinBox;
    m_rate->setRange(0.001, 20000.0);
    m_rate->setDecimals(3);
    m_rate->setSuffix(tr(" MSPS"));
    m_rate->setToolTip(tr("Transmitter sample rate (iq_ctl.sample_rate_hz)."));

    m_centre = new QDoubleSpinBox;
    // Signed: the reference transmits at negative baseband offsets, and the
    // receiver reports them as such, so clamping to >= 0 would make a valid
    // configuration unreachable.
    m_centre->setRange(-30000.0, 30000.0);
    m_centre->setDecimals(3);
    m_centre->setSuffix(tr(" MHz"));
    m_centre->setToolTip(tr("Transmitter centre frequency (iq_ctl.center_freq_hz). "
                            "Negative values are valid."));

    m_pace = new QDoubleSpinBox;
    m_pace->setRange(0.01, 400.0);
    m_pace->setDecimals(2);
    m_pace->setSuffix(tr(" Gb/s"));
    m_pace->setToolTip(tr("Transmit pacing (iq_ctl.pace_gbps)."));

    m_unlimited = new QCheckBox(tr("Unlimited"));
    m_unlimited->setToolTip(tr("Send pace_gbps = 0, which the transmitter "
                               "treats as unpaced."));
    connect(m_unlimited, &QCheckBox::toggled, this, [this](bool on) {
        m_pace->setEnabled(!on);
    });

    auto* paceRow = new QWidget;
    auto* ph = new QHBoxLayout(paceRow);
    ph->setContentsMargins(0, 0, 0, 0);
    ph->setSpacing(8);
    ph->addWidget(m_pace, 1);
    ph->addWidget(m_unlimited);

    f->addRow(new QLabel(tr("Sample rate")), m_rate);
    f->addRow(new QLabel(tr("Centre frequency")), m_centre);
    f->addRow(new QLabel(tr("Pacing")), paceRow);
    rv->addWidget(g);

    m_derived = new QLabel;
    m_derived->setObjectName(QStringLiteral("hintLabel"));
    m_derived->setWordWrap(true);
    rv->addWidget(m_derived);

    auto* buttons = new QHBoxLayout;
    m_revert = new QPushButton(tr("Revert"));
    m_revert->setToolTip(tr("Reload the values currently in the control block."));
    m_apply  = new QPushButton(tr("Apply"));
    m_apply->setObjectName(QStringLiteral("launchButton"));
    m_apply->setToolTip(tr("Write the parameters and bump the sequence; the "
                           "transmitter applies them on its next poll."));
    for (QDoubleSpinBox* w : {m_rate, m_centre})
        connect(w, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this](double){ updateDerived(); });

    connect(m_revert, &QPushButton::clicked, this, &ControlWindow3D::onRevert);
    connect(m_apply,  &QPushButton::clicked, this, &ControlWindow3D::onApply);
    buttons->addStretch(1);
    buttons->addWidget(m_revert);
    buttons->addWidget(m_apply);
    rv->addLayout(buttons);
    m_rdmaBox->setVisible(false);          // MainWindow shows it for RoCEv2
    v->addWidget(m_rdmaBox);

    m_log = new QPlainTextEdit;
    m_log->setObjectName(QStringLiteral("controlLog"));
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(200);   // bounded: this window stays open
    m_log->setMinimumHeight(90);
    v->addWidget(m_log, 1);

    refreshChain();
}


void ControlWindow3D::buildSignalChainPanel()
{
    // These parameters are NOT part of the iq_ctl block, so they are not
    // applied live by rdma_tx. They configure the H2C feed the session builds
    // and take effect when acquisition is (re)started. The panel says so
    // rather than implying an immediacy the backend does not provide.
    QWidget* page = addControlPanel(tr("Signal chain"));
    auto* v = qobject_cast<QVBoxLayout*>(page->layout());
    v->setContentsMargins(12, 12, 12, 12);
    v->setSpacing(10);

    auto* note = new QLabel(tr(
        "Applied when acquisition is started or restarted. The transmit "
        "parameters on the previous tab are the only values rdma_tx accepts "
        "live."));
    note->setObjectName(QStringLiteral("hintLabel"));
    note->setWordWrap(true);
    v->addWidget(note);

    auto* g = new QGroupBox(tr("H2C SIGNAL CHAIN"));
    auto* f = new QFormLayout(g);
    f->setContentsMargins(12, 14, 12, 12);
    f->setSpacing(8);

    m_nco = new QDoubleSpinBox;
    m_nco->setRange(-30000.0, 30000.0);      // signed: up- and down-conversion
    m_nco->setDecimals(4);
    m_nco->setSuffix(tr(" MHz"));
    m_nco->setToolTip(tr("NCO / DUC offset applied in the transmit chain. "
                         "Negative values are valid."));

    m_interp = new QSpinBox;
    m_interp->setRange(1, 4096);
    m_interp->setValue(1);
    m_interp->setToolTip(tr("Interpolation factor. Output rate = "
                            "sample rate x interpolation / decimation."));

    m_decim = new QSpinBox;
    m_decim->setRange(1, 4096);
    m_decim->setValue(1);
    m_decim->setToolTip(tr("Decimation factor."));

    m_channels = new QComboBox;
    for (int n : {1, 2, 4, 8, 16}) m_channels->addItem(QString::number(n));
    m_channels->setCurrentText(QStringLiteral("1"));
    m_channels->setToolTip(tr("Transmit channels interleaved on the wire."));

    m_format = new QComboBox;
    m_format->addItems(sdr::formatNames());
    m_format->setToolTip(tr("Wire format of the transmit stream."));

    m_chunk = new QSpinBox;
    m_chunk->setRange(1024, 1048576);
    m_chunk->setSingleStep(1024);
    m_chunk->setValue(16384);
    m_chunk->setSuffix(tr(" B"));
    m_chunk->setToolTip(tr("Chunk size handed to iwfg_h2c per write."));

    f->addRow(new QLabel(tr("NCO / DUC")), m_nco);
    f->addRow(new QLabel(tr("Interpolation")), m_interp);
    f->addRow(new QLabel(tr("Decimation")), m_decim);
    f->addRow(new QLabel(tr("Channels")), m_channels);
    f->addRow(new QLabel(tr("Wire format")), m_format);
    f->addRow(new QLabel(tr("Chunk size")), m_chunk);
    v->addWidget(g);
    v->addStretch(1);

    auto emitChain = [this] {
        emit signalChainChanged(m_nco->value(), m_interp->value(), m_decim->value(),
                                m_channels->currentText().toInt(), m_chunk->value());
        updateDerived();
    };
    connect(m_nco, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [emitChain](double){ emitChain(); });
    for (QSpinBox* w : {m_interp, m_decim, m_chunk})
        connect(w, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [emitChain](int){ emitChain(); });
    connect(m_channels, &QComboBox::currentTextChanged,
            this, [emitChain](const QString&){ emitChain(); });
}

void ControlWindow3D::updateDerived()
{
    if (!m_derived || !m_rate) return;
    const double fs = m_rate->value();                        // MSPS
    const int itp = m_interp ? m_interp->value() : 1;
    const int dec = m_decim  ? m_decim->value()  : 1;
    const double out = fs * double(itp) / double(dec <= 0 ? 1 : dec);
    const double nco = m_nco ? m_nco->value() : 0.0;
    const double ctr = m_centre ? m_centre->value() : 0.0;

    // Show what the settings actually produce, so the interaction between
    // rate, resampling and the NCO is visible instead of implied.
    m_derived->setText(tr("Output rate %1 MSPS  ·  transmit centre %2 MHz  "
                          "·  span ±%3 MHz")
                           .arg(out, 0, 'f', 3)
                           .arg(ctr + nco, 0, 'f', 3)
                           .arg(out / 2.0, 0, 'f', 3));
}

void ControlWindow3D::attachControl(const QString& path)
{
    const bool ok = m_ctl->attach(path);
    for (QWidget* w : {static_cast<QWidget*>(m_rate), static_cast<QWidget*>(m_centre),
                       static_cast<QWidget*>(m_pace), static_cast<QWidget*>(m_unlimited),
                       static_cast<QWidget*>(m_apply), static_cast<QWidget*>(m_revert)})
        if (w) w->setEnabled(ok);

    if (ok) {
        m_ctlState->setText(tr("Control block: %1").arg(path));
        m_ctlState->setStyleSheet(QStringLiteral("color:#33d17a;"));
        onRevert();
        updateDerived();
    } else {
        // Disabled with the reason, rather than silently accepting input that
        // will never reach the transmitter.
        m_ctlState->setText(tr("Control block unavailable — %1").arg(m_ctl->lastError()));
        m_ctlState->setStyleSheet(QStringLiteral("color:#f87171;"));
    }
}

void ControlWindow3D::onRevert()
{
    if (!m_ctl->attached()) return;
    const double pace = m_ctl->paceGbps();
    m_unlimited->setChecked(pace <= 0.0);
    if (pace > 0.0) m_pace->setValue(pace);
    m_rate->setValue(double(m_ctl->sampleRateHz()) / 1e6);
    m_centre->setValue(double(m_ctl->centerFreqHz()) / 1e6);
    // Only meaningful where the rdma_tx block is in use (RoCEv2): elsewhere
    // this line read as if the H2C transmitter had been reset.
    if (m_rdmaBox && !m_rdmaBox->isHidden())
        log(tr("Reloaded from control block (seq %1)").arg(m_ctl->sequence()));
}

void ControlWindow3D::onApply()
{
    if (!m_ctl->attached()) return;
    m_ctl->apply(m_unlimited->isChecked() ? 0.0 : m_pace->value(),
                 quint64(m_rate->value() * 1e6),
                 qint64(m_centre->value() * 1e6));
}

void ControlWindow3D::log(const QString& line)
{
    if (m_log) m_log->appendPlainText(line);
}

void ControlWindow3D::setViewport(QWidget* view)
{
    if (!view) return;
    if (m_placeholder) {
        m_viewportLayout->removeWidget(m_placeholder);
        m_placeholder->deleteLater();
        m_placeholder = nullptr;
    }
    m_viewportLayout->addWidget(view);
    if (QWidget* frame = m_viewportLayout->parentWidget())
        frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_hasViewport = true;
}

QWidget* ControlWindow3D::addControlPanel(const QString& title)
{
    // The page the caller populates lives inside a scroll area, so a tab whose
    // controls are taller than the window scrolls instead of pushing its lower
    // rows off the bottom edge. widgetResizable keeps the page the full width
    // of the viewport (no horizontal scrollbar until it is genuinely too
    // narrow), and the caller still gets the plain page back — it never has to
    // know it is scrolled.
    auto* page = new QWidget;
    new QVBoxLayout(page);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setWidget(page);

    m_panels->addTab(scroll, title);
    m_panels->show();
    return page;
}

// The window is hidden rather than destroyed, so this normally runs only at
// application shutdown. Stop the worker cleanly: an in-flight transaction can
// be mid-poll, and tearing the thread down under it would abort the process.
ControlWindow3D::~ControlWindow3D()
{
    if (m_genThread) {
        if (m_gen) m_gen->stop();
        m_genThread->quit();
        if (!m_genThread->wait(3000)) { m_genThread->terminate(); m_genThread->wait(500); }
    }
    if (m_rfThread) {
        if (m_rfdc) m_rfdc->requestAbort();     // break out of any poll loop
        m_rfThread->quit();
        if (!m_rfThread->wait(3000)) {
            m_rfThread->terminate();            // last resort; logged by Qt
            m_rfThread->wait(500);
        }
    }
}

void ControlWindow3D::closeEvent(QCloseEvent* e)
{
    // Hide, do not destroy: reopening must preserve whatever state the future
    // controls hold. The parent owns the instance for the app's lifetime.
    e->ignore();
    hide();
}

// ---------------------------------------------------------------------------
// Transmit-side state: shared by the hardware path and the simulator
// ---------------------------------------------------------------------------
sdr::IqGenerator::Config ControlWindow3D::genConfig() const
{
    sdr::IqGenerator::Config cfg;
    cfg.fifoPath      = m_genFifo ? m_genFifo->text().trimmed() : cfg.fifoPath;
    cfg.waveform      = sdr::IqGenerator::Waveform(m_genWave ? m_genWave->currentData().toInt() : 0);
    cfg.sampleRateSps = (m_genRate ? m_genRate->value() : 200.0) * 1e6;
    cfg.frequencyHz   = (m_genTone ? m_genTone->value() : 10.0) * 1e6;
    cfg.iqPhaseDiff   = (m_genTone2 ? m_genTone2->value() : -90.0) * M_PI / 180.0;
    cfg.amplitude     = m_genAmp ? m_genAmp->value() : 0.9;
    // The loop must be exactly the chunk iwfg_h2c runs with, which is the one
    // the backend launcher starts it with. One source for that number.
    cfg.chunkBytes    = m_h2cChunkBytes;
    cfg.coherent      = m_genCoherent ? m_genCoherent->isChecked() : true;
    return cfg;
}

void ControlWindow3D::setGenState(const QString& text, const char* colour)
{
    if (!m_genState) return;
    m_genState->setText(text);
    m_genState->setStyleSheet(QStringLiteral("color:%1;").arg(QLatin1String(colour)));
}

// Say, before anything is transmitted, which frequency will really go out.
void ControlWindow3D::updateGenPreview()
{
    if (!m_genActual) return;
    const auto cfg = genConfig();
    const int pairs = sdr::IqGenerator::loopPairsFor(cfg.chunkBytes);
    const double step = sdr::wave::loopResolutionHz(cfg.sampleRateSps, pairs);

    QString t;
    if (cfg.waveform == sdr::IqGenerator::Noise) {
        t = tr("Noise has no carrier frequency.");
    } else if (!cfg.coherent) {
        t = tr("Streams exactly %1 MHz as a continuous series of chunks. Every "
               "new chunk makes iwfg_h2c copy it between transfers and the "
               "replay seams add spurs every %2 Hz — expect a less clean output "
               "than with Loop-coherent on.")
                .arg(cfg.frequencyHz / 1e6, 0, 'f', 6).arg(step, 0, 'f', 3);
    } else {
        const double act = sdr::IqGenerator::transmittedHz(cfg);
        const QString buf = tr("%1 MiB H2C buffer, %2-sample loop, step %3 Hz")
                                .arg(double(cfg.chunkBytes) / 1048576.0, 0, 'g', 4)
                                .arg(pairs).arg(step, 0, 'f', 3);
        if (std::abs(act - cfg.frequencyHz) < 0.5)
            t = tr("Transmits exactly %1 MHz (%2).").arg(act / 1e6, 0, 'f', 6).arg(buf);
        else
            t = tr("Transmits %1 MHz — the nearest seamless frequency (%2).")
                    .arg(act / 1e6, 0, 'f', 6).arg(buf);
        if (std::abs(act) >= 0.5 * cfg.sampleRateSps)
            t += tr("  ⚠ Beyond ±%1 MHz (half the sample rate): it will alias.")
                     .arg(cfg.sampleRateSps / 2e6, 0, 'f', 3);
    }
    m_genActual->setText(t);
}

void ControlWindow3D::onGenParamsEdited()
{
    updateGenPreview();
    // requestUpdate() is thread-safe and applied at the next chunk boundary.
    // A queued slot call would never arrive: start() does not return to the
    // generator thread's event loop while it is running.
    if (m_genRunning && m_gen) m_gen->requestUpdate(genConfig());
    if (m_simMode && m_simGenRunning) enableSimLoopback();
    publishTxState();
}

// Starting the generator implies the host stream must reach the DAC.
void ControlWindow3D::ensureHostRouting()
{
    if (!m_rfDacSrc) return;
    // Unattached, the combo is only the panel's belief, so it proves nothing
    // either way: say so rather than assume the DAC is on the host stream.
    if (!m_rfAttached) {
        log(tr("RFDC control is not attached, so the DAC input could not be "
               "checked or set. The generator's samples reach the DAC only when "
               "its input is \"Host / GNU Radio stream\" — select it on the "
               "RFDC tab once attached, or serial menu 16 -> 1."));
        return;
    }
    if (m_rfDacSrc->currentData().toInt() == int(PCIE_DAC_SRC_HOST))
        return;
    if (m_rfBusy || m_rfSyncing) {
        log(tr("Could not switch the DAC input to the host stream: another RFDC "
               "command is in flight. Select \"Host / GNU Radio stream\" on the "
               "RFDC tab when it completes."));
        return;
    }
    setComboByData(m_rfDacSrc, int(PCIE_DAC_SRC_HOST));
    rfdcInvoke([this]{ m_rfdc->setDacSource(int(PCIE_DAC_SRC_HOST)); });
    log(tr("DAC input switched to Host / GNU Radio stream, so the generator's "
           "samples reach the DAC."));
    publishTxState();
}

sdr::TxState ControlWindow3D::txState() const
{
    sdr::TxState t;
    const auto cfg = genConfig();
    t.loopback     = m_simMode && m_simLoopback && m_simLoopback->isChecked();
    t.dacSource    = m_rfDacSrc ? m_rfDacSrc->currentData().toInt() : int(PCIE_DAC_SRC_DDS);
    t.ddsHz        = (m_rfDds ? m_rfDds->value() : double(PCIE_DEF_DDS_MHZ)) * 1e6;
    t.dacStreamSps = cfg.sampleRateSps;
    t.hostRunning  = m_simMode ? m_simGenRunning : (m_genRunning && m_genConnected);
    t.waveform     = int(cfg.waveform);
    t.hostToneHz   = sdr::IqGenerator::transmittedHz(cfg);
    t.amplitude    = cfg.amplitude;
    t.phaseOffset  = cfg.phaseOffset;
    t.iqPhaseDiff  = cfg.iqPhaseDiff;
    t.loopPairs    = sdr::IqGenerator::loopPairsFor(cfg.chunkBytes);
    t.coherent     = cfg.coherent;
    return t;
}

void ControlWindow3D::publishTxState()
{
    emit txStateChanged(txState());
    refreshChain();
}

// In simulation the transmit-side controls must stay usable with no device.
// Every handler that disables m_rfControls ends by calling this.
void ControlWindow3D::refreshSimControls()
{
    if (!m_simMode || m_rfAttached) return;
    if (m_rfDacSrc) m_rfDacSrc->setEnabled(true);
    if (m_rfDds)    m_rfDds->setEnabled(true);
}

// Touching a transmit control in simulation means "show me what that does".
void ControlWindow3D::enableSimLoopback()
{
    if (m_simMode && m_simLoopback && !m_simLoopback->isChecked())
        m_simLoopback->setChecked(true);            // toggled -> publish
}

void ControlWindow3D::setSimulationMode(bool simulated)
{
    if (m_simMode == simulated) { publishTxState(); return; }
    m_simMode = simulated;
    if (m_simBox) m_simBox->setVisible(simulated);
    if (m_rfEditHint && !m_rfAttached)
        m_rfEditHint->setText(simulated
            ? tr("Simulation, no device attached: DAC input source and DDS "
                 "frequency drive the simulated loopback. The other parameters "
                 "need a device.")
            : tr("Not attached. Values are read from the device on attach; each "
                 "edit is then sent as soon as you finish editing it."));

    if (simulated) {
        refreshSimControls();
    } else {
        // Leaving simulation: a modelled generator is not a real one.
        if (m_simGenRunning) {
            m_simGenRunning = false;
            if (m_genBtn) m_genBtn->setText(tr("Start generator"));
            setGenState(tr("Stopped"), "#9ca3af");
        }
        for (QWidget* w : {static_cast<QWidget*>(m_rfDacSrc), static_cast<QWidget*>(m_rfDds)})
            if (w) w->setEnabled(m_rfAttached && !m_rfBusy && !m_rfSyncing);
    }
    if (m_genBtn && !m_genRunning && !m_simGenRunning)
        m_genBtn->setText(tr("Start generator"));
    if (m_genBtn)
        m_genBtn->setToolTip(simulated
            ? tr("Simulation: the generator is modelled and shown in the plots; "
                 "nothing is written to the H2C FIFO.")
            : tr("Write the waveform to the H2C FIFO for iwfg_h2c to transmit."));
    publishTxState();
}

// ---------------------------------------------------------------------------
// H2C playback buffer and link health
// ---------------------------------------------------------------------------
static QString sizeText(int bytes)
{
    if (bytes >= (1 << 20) && bytes % (1 << 20) == 0)
        return QStringLiteral("%1 MiB").arg(bytes >> 20);
    return QStringLiteral("%1 KiB").arg(bytes / 1024);
}

void ControlWindow3D::setH2cChunkBytes(int bytes)
{
    if (bytes <= 0 || bytes == m_h2cChunkBytes) return;
    m_h2cChunkBytes = bytes;
    log(tr("H2C playback buffer: %1 — the generator's loop is %2 samples.")
            .arg(sizeText(bytes)).arg(bytes / 4));
    updateGenPreview();
    publishTxState();
    if (m_genRunning && m_gen) {
        // The loop must be exactly one chunk long; a mismatch would have
        // iwfg_h2c replay a fragment of it. Restart with the new length.
        log(tr("Restarting the IQ generator so its loop matches the new buffer."));
        m_genRestartPending = true;
        m_gen->stop();
    }
}

void ControlWindow3D::setH2cThroughput(double mibPerSec, int updatesPerSec)
{
    Q_UNUSED(updatesPerSec);
    m_linkMib = mibPerSec;
    m_linkClock.restart();
    refreshChain();
    if (!m_genLink) return;
    // One 32-bit I/Q word per DAC stream clock. iwfg_h2c's "MB" is MiB.
    const double needMib = (m_genRate ? m_genRate->value() : 200.0) * 1e6 * 4.0 / 1048576.0;
    const double ratio = needMib > 0.0 ? mibPerSec / needMib : 0.0;
    const bool starved = ratio < 0.98;
    if (starved) {
        m_genLink->setText(tr("⚠ H2C link %1 MiB/s, but the DAC reads %2 MiB/s: "
                              "it runs dry about %3% of the time and plays stale "
                              "data — broadband interference and a line at 0 Hz. "
                              "H2C buffer %4.")
                               .arg(mibPerSec, 0, 'f', 1).arg(needMib, 0, 'f', 1)
                               .arg((1.0 - ratio) * 100.0, 0, 'f', 0)
                               .arg(sizeText(m_h2cChunkBytes)));
        m_genLink->setStyleSheet(QStringLiteral("color:#f87171;"));
    } else {
        m_genLink->setText(tr("H2C link %1 MiB/s for a DAC that reads %2 MiB/s — "
                              "kept full (buffer %3).")
                               .arg(mibPerSec, 0, 'f', 1).arg(needMib, 0, 'f', 1)
                               .arg(sizeText(m_h2cChunkBytes)));
        m_genLink->setStyleSheet(QStringLiteral("color:#33d17a;"));
    }
    if (starved != m_h2cStarved) {
        m_h2cStarved = starved;
        log(starved ? tr("H2C is not keeping up with the DAC (%1 of %2 MiB/s).")
                          .arg(mibPerSec, 0, 'f', 1).arg(needMib, 0, 'f', 1)
                    : tr("H2C is keeping the DAC fed (%1 of %2 MiB/s).")
                          .arg(mibPerSec, 0, 'f', 1).arg(needMib, 0, 'f', 1));
    }
}

// ---------------------------------------------------------------------------
// Live transmit chain and loopback verification
// ---------------------------------------------------------------------------
void ControlWindow3D::setRdmaControlVisible(bool visible)
{
    if (m_rdmaBox) m_rdmaBox->setVisible(visible);
}

double ControlWindow3D::rxTrueHz(double displayedHz) const
{
    // A display that assumes the wrong sample rate scales every frequency:
    // 15 MHz read at 250 MSPS for a 200 MSPS stream shows as 18.75 MHz.
    if (m_devAdcStream > 0.0 && m_rxRate > 0.0)
        return displayedHz * (m_devAdcStream * 1e6) / m_rxRate;
    return displayedHz;
}

double ControlWindow3D::expectedRxHz(double txHz) const
{
    // DAC: RF = DAC_NCO + tx. ADC: baseband = RF - ADC_NCO, possibly inverted
    // by the Nyquist zone. The magnitude is what is compared.
    const double d = m_devNcoKnown ? (m_devDacNco - m_devAdcNco) * 1e6 : 0.0;
    return std::abs(txHz + d);
}

void ControlWindow3D::setReceivedPeak(double basebandHz, double dbfs, bool toneValid,
                                      double inputRateHz, double binHz)
{
    m_rxHz = basebandHz; m_rxDb = dbfs; m_rxValid = toneValid;
    m_rxRate = inputRateHz; m_rxBin = binHz;
    m_rxClock.restart();
    if ((m_lcStep == 2 || m_lcStep == 4) && toneValid)
        m_lcSamples.push_back(rxTrueHz(basebandHz));
    refreshChain();
}

void ControlWindow3D::setH2cHelperState(int state, const QString& detail, bool backendRunning)
{
    const bool changed = state != m_h2cHelper || detail != m_h2cHelperDetail;
    m_h2cHelper = state;
    m_h2cHelperDetail = detail;
    m_backendRunning = backendRunning;
    if (changed && backendRunning && !detail.isEmpty())
        log(tr("H2C playback %1: %2").arg(sdr::h2cStateName(sdr::H2cState(state)), detail));
    refreshChain();
}

void ControlWindow3D::setCaptureRate(double deliveredMib, double needMib, int channels,
                                     double rateMsps, int bytesPerSample)
{
    m_capMib = deliveredMib;
    m_capNeedMib = needMib;
    m_capCh = channels;
    m_capRate = rateMsps;
    m_capBps = bytesPerSample;
    refreshChain();
}

void ControlWindow3D::setH2cFifoPath(const QString& path)
{
    if (!m_genFifo || path.isEmpty() || m_genFifo->text() == path) return;
    if (m_genRunning) {
        log(tr("The H2C playback FIFO is %1; the generator is writing %2 — it will use "
               "the new path when restarted.").arg(path, m_genFifo->text()));
        return;
    }
    m_genFifo->setText(path);
}

void ControlWindow3D::restorePlGpioModeFor(int channels)
{
    const int mode = m_plGpioBefore > 0 ? m_plGpioBefore
                                        : (channels >= 4 ? 3 : channels >= 2 ? 2 : 1);
    m_plGpioBefore = 0;
    applyPlGpioMode(mode, tr("restored"));
}

int ControlWindow3D::plGpioMode() const
{
    return m_rfPlGpio ? m_rfPlGpio->currentData().toInt() : 0;
}

void ControlWindow3D::applyPlGpioMode(int mode, const QString& why)
{
    if (mode < 1 || mode > 3) return;
    if (m_rfPlGpio) setComboByData(m_rfPlGpio, mode);
    if (m_rfAttached && m_rfdc) rfdcInvoke([this, mode] { m_rfdc->setPlGpioRouting(mode); });
    log(tr("PL GPIO routing → mode %1 (%2).").arg(mode).arg(why));
}

void ControlWindow3D::refreshChain()
{
    if (!m_chSrc) return;
    auto set = [](QLabel* l, const QString& t, const char* c) {
        l->setText(t);
        l->setStyleSheet(QStringLiteral("color:%1;").arg(QLatin1String(c)));
    };
    const char* ok = "#33d17a"; const char* warn = "#fbbf24";
    const char* bad = "#f87171"; const char* dim = "#9ca3af";
    const auto cfg = genConfig();
    const double txHz = sdr::IqGenerator::transmittedHz(cfg);
    const bool genOn = m_simMode ? m_simGenRunning : m_genRunning;
    const int comboSrc = m_rfDacSrc ? m_rfDacSrc->currentData().toInt() : -1;
    const int src = m_simMode ? comboSrc : m_devDacSrc;

    // DAC input
    if (m_simMode)
        set(m_chSrc, tr("%1 (simulation)").arg(m_rfDacSrc ? m_rfDacSrc->currentText()
                                                          : QString()), dim);
    else if (src == int(PCIE_DAC_SRC_HOST))
        set(m_chSrc, tr("Host / GNU Radio stream — confirmed by the device"), ok);
    else if (src == int(PCIE_DAC_SRC_DDS))
        set(m_chSrc, genOn ? tr("DDS compiler (device) — the generator's samples are NOT played")
                           : tr("DDS compiler (device)"), genOn ? bad : dim);
    else if (src == int(PCIE_DAC_SRC_NONE))
        set(m_chSrc, tr("nothing routed (device)"), bad);
    else if (genOn)
        // Firmware 1.2.1+ boots with the DAC on the DDS. Unattached, the GUI
        // cannot switch it, and the generator's samples are then dropped at
        // the switch however healthy the H2C transfer looks.
        set(m_chSrc, tr("UNKNOWN — the RFDC tab is not attached, so the DAC may still be "
                        "on the DDS (the boot default) and the generator's samples dropped. "
                        "Attach it, or serial menu 16 → 1."), warn);
    else
        set(m_chSrc, tr("not read yet — attach the RFDC tab"), dim);

    // DAC
    if (m_devDacFs > 0.0) {
        QString t = tr("%1 MSPS, interpolation ×%2 → stream %3 MSPS")
                        .arg(m_devDacFs, 0, 'f', 2).arg(m_devInterp)
                        .arg(m_devDacStream, 0, 'f', 3);
        const bool rateOk = m_devDacStream <= 0.0
                         || std::abs(cfg.sampleRateSps / 1e6 - m_devDacStream) < 1e-3;
        if (!rateOk)
            t += tr("  ⚠ generator is set to %1 MSPS").arg(cfg.sampleRateSps / 1e6, 0, 'f', 3);
        set(m_chDac, t, rateOk ? ok : warn);
    } else {
        set(m_chDac, tr("generator at %1 MSPS (device not read)")
                         .arg(cfg.sampleRateSps / 1e6, 0, 'f', 3), dim);
    }

    // NCO / RF
    if (m_devNcoKnown)
        set(m_chNco, tr("DAC NCO %1 MHz → tone at RF %2 MHz · ADC NCO %3 MHz")
                         .arg(m_devDacNco, 0, 'f', 3)
                         .arg(m_devDacNco + txHz / 1e6, 0, 'f', 3)
                         .arg(m_devAdcNco, 0, 'f', 3), ok);
    else
        set(m_chNco, tr("not read yet"), dim);

    // ADC
    if (m_devAdcFs > 0.0) {
        QString t = tr("%1 MSPS, decimation ×%2 → %3 MSPS per channel")
                        .arg(m_devAdcFs, 0, 'f', 2).arg(m_devDecim)
                        .arg(m_devAdcStream, 0, 'f', 3);
        const bool dispOk = m_rxRate <= 0.0 || m_devAdcStream <= 0.0
                         || std::abs(m_rxRate / 1e6 - m_devAdcStream) < 1e-3;
        if (!dispOk)
            t += tr("  ⚠ the display assumes %1 MSPS").arg(m_rxRate / 1e6, 0, 'f', 3);
        // The readback gives the gain factor, not whether QMC gain is
        // enabled; 0 with gain enabled multiplies the capture by zero.
        const bool qmcZero = m_devAdcQmc >= 0.0 && m_devAdcQmc < 0.05;
        if (qmcZero)
            t += tr("  ⚠ ADC QMC gain reads %1: if QMC gain is enabled this mutes the "
                    "capture — set QMC gain 1.0 for the ADC on the RFDC tab")
                     .arg(m_devAdcQmc, 0, 'f', 4);
        set(m_chAdc, t, (dispOk && !qmcZero) ? ok : warn);
    } else {
        set(m_chAdc, tr("not read yet"), dim);
    }

    // H2C to card
    if (m_simMode) {
        set(m_chLink, tr("simulation — nothing is sent"), dim);
    } else if (m_linkClock.isValid() && m_linkClock.elapsed() < 3000) {
        const double need = cfg.sampleRateSps * 4.0 / 1048576.0;
        const bool full = m_linkMib >= 0.98 * need;
        set(m_chLink, tr("iwfg_h2c is moving %1 MiB/s to the card (the DAC reads %2) · "
                         "buffer %3 KiB")
                          .arg(m_linkMib, 0, 'f', 1).arg(need, 0, 'f', 1)
                          .arg(m_h2cChunkBytes / 1024), full ? ok : bad);
    } else {
        const auto st = sdr::H2cState(m_h2cHelper);
        QString t = tr("idle");
        if (genOn) {
            if (!m_backendRunning)
                t = tr("nothing is moved: the backend is not running — press Start");
            else if (st == sdr::H2cState::NotRunning || st == sdr::H2cState::Failed)
                t = tr("nothing is moved: iwfg_h2c is not running — press Retry transmit");
            else if (st == sdr::H2cState::WaitingForData)
                t = tr("iwfg_h2c is waiting for the generator's first full buffer");
            else
                t = tr("no transfer has completed yet (%1)").arg(sdr::h2cStateName(st));
        }
        set(m_chLink, t, genOn ? warn : dim);
    }

    // H2C playback helper
    {
        const auto st = sdr::H2cState(m_h2cHelper);
        const QString name = sdr::h2cStateName(st);
        const QString t = m_h2cHelperDetail.isEmpty() ? name
                                                      : tr("%1 — %2").arg(name, m_h2cHelperDetail);
        const char* c = dim;
        switch (st) {
        case sdr::H2cState::Streaming:      c = ok; break;
        case sdr::H2cState::Stalled:
        case sdr::H2cState::Failed:         c = bad; break;
        case sdr::H2cState::NotRunning:     c = genOn ? bad : dim; break;
        default:                            c = genOn ? warn : dim; break;
        }
        if (m_simMode) set(m_chH2c, tr("simulation — no helper runs"), dim);
        else           set(m_chH2c, t, c);
        if (m_retryTxBtn) {
            m_retryTxBtn->setVisible(!m_simMode && m_backendRunning);
            m_retryTxBtn->setEnabled(st != sdr::H2cState::Restarting);
        }
    }

    // C2H capture: what it delivers against what the stream needs
    {
        bool offerOne = false;
        if (m_simMode) {
            set(m_chC2h, tr("simulation"), dim);
        } else if (m_capMib <= 0.0 || m_capNeedMib <= 0.0) {
            set(m_chC2h, tr("not capturing"), dim);
        } else {
            const double ratio = m_capMib / m_capNeedMib;
            QString t = tr("%1 MiB/s delivered for %2 MiB/s needed (%3 ch × %4 MSPS × %5 B)")
                            .arg(m_capMib, 0, 'f', 0).arg(m_capNeedMib, 0, 'f', 0)
                            .arg(m_capCh).arg(m_capRate, 0, 'f', 2).arg(m_capBps);
            if (ratio < 0.90) {
                t += tr(" — OVERLOADED (%1%): the C2H FIFO overflows, and each overflow "
                        "recovery resets the QDMA, which interrupts H2C transmit too. "
                        "Capture fewer channels.").arg(ratio * 100.0, 0, 'f', 0);
                set(m_chC2h, t, bad);
                offerOne = m_capCh > 1;
            } else if (ratio > 1.30) {
                t += tr(" — the stream carries more data than configured (channel count or "
                        "rate mismatch): check Channels in the Source panel");
                set(m_chC2h, t, warn);
            } else {
                set(m_chC2h, t, ok);
            }
        }
        if (m_oneChBtn) {
            m_oneChBtn->setVisible(offerOne);
            m_oneChBtn->setEnabled(m_rfAttached);
        }
    }

    // Received
    if (!m_rxClock.isValid() || m_rxClock.elapsed() > 2000) {
        set(m_chRx, tr("no receive data (start acquisition)"), dim);
    } else if (!m_rxValid) {
        set(m_chRx, tr("no carrier in the received spectrum"), genOn ? warn : dim);
    } else {
        const double k = (m_devAdcStream > 0 && m_rxRate > 0) ? m_devAdcStream * 1e6 / m_rxRate : 1.0;
        const double rx = rxTrueHz(m_rxHz);
        const double tol = std::max(2.5 * m_rxBin * k, 50e3);
        const QString what = tr("%1 MHz at %2 dBFS").arg(rx / 1e6, 0, 'f', 3).arg(m_rxDb, 0, 'f', 1);
        const double expHost = expectedRxHz(txHz);
        const double expDds  = expectedRxHz((m_rfDds ? m_rfDds->value() : 0.0) * 1e6);
        const bool isHost = std::abs(std::abs(rx) - expHost) < tol;
        const bool isDds  = std::abs(std::abs(rx) - expDds) < tol;
        if (genOn && src == int(PCIE_DAC_SRC_HOST) && isHost) {
            const bool inv = (rx < 0) != (txHz < 0);
            set(m_chRx, what + tr(" — the generator's frequency%1%2")
                                   .arg(inv ? tr(" (spectrum inverted)") : QString())
                                   .arg(isDds ? tr(" — the DDS is at the same frequency, "
                                                   "press Verify to be sure") : QString()),
                isDds ? warn : ok);
        } else if (genOn && src < 0 && isHost) {
            // DAC input not read (RFDC tab not attached): the frequency matches,
            // which is evidence but not proof. 1.2.3 called this "NOT the
            // generator's" (or "the DDS tone") because the source was unknown.
            set(m_chRx, what + (isDds
                    ? tr(" — matches the generator's frequency, and the DDS's: the DAC input "
                         "is not read, so press Verify loopback to tell them apart")
                    : tr(" — matches the generator's frequency (DAC input not read; press "
                         "Verify loopback to prove it)")), isDds ? warn : ok);
        } else if (isDds && src != int(PCIE_DAC_SRC_HOST)) {
            set(m_chRx, what + tr(" — the DDS tone"), dim);
        } else if (genOn) {
            set(m_chRx, what + tr(" — NOT the generator's %1 MHz").arg(expHost / 1e6, 0, 'f', 3), bad);
        } else {
            set(m_chRx, what, dim);
        }
    }
    if (m_verifyBtn) m_verifyBtn->setEnabled(m_lcStep == 0);
}

void ControlWindow3D::onVerifyLoopback()
{
    if (m_lcStep != 0) return;
    if (!m_rxClock.isValid() || m_rxClock.elapsed() > 2000) {
        m_chVerify->setText(tr("Start acquisition first: the check watches the received spectrum."));
        m_chVerify->setStyleSheet(QStringLiteral("color:#fbbf24;"));
        return;
    }
    log(tr("Loopback check: starting."));
    m_lcOrigMhz = m_genTone->value();
    m_lcSamples.clear();
    const bool genOn = m_simMode ? m_simGenRunning : m_genRunning;
    if (!genOn) onGenToggle();                  // start it; step 1 waits for it
    else if (!m_simMode) ensureHostRouting();
    m_lcStep = 1;
    m_lcClock.restart();
    m_chVerify->setText(tr("Checking… (about 6 s; the tone is moved and then restored)"));
    m_chVerify->setStyleSheet(QStringLiteral("color:#9ca3af;"));
    m_lcTimer->start();
    refreshChain();
}

void ControlWindow3D::loopCheckTick()
{
    constexpr int kSettleMs = 1500, kMeasureMs = 1200, kStartMs = 8000;
    auto median = [](QVector<double> v) {
        if (v.isEmpty()) return 0.0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    const bool genOn = m_simMode ? m_simGenRunning : (m_genRunning && m_genConnected);

    switch (m_lcStep) {
    case 1:     // generator transmitting, then settle on tone A
        if (!genOn) {
            if (m_lcClock.elapsed() > kStartMs) {
                m_lcStep = 0; m_lcTimer->stop();
                m_chVerify->setText(tr("✗ The generator did not start transmitting — "
                                       "iwfg_h2c never opened the FIFO. Is the H2C "
                                       "backend running?"));
                m_chVerify->setStyleSheet(QStringLiteral("color:#f87171;"));
                refreshChain();
            }
            return;
        }
        if (m_lcClock.elapsed() < kSettleMs) return;
        m_lcTxA = sdr::IqGenerator::transmittedHz(genConfig());
        m_lcSamples.clear();
        m_lcStep = 2; m_lcClock.restart();
        return;
    case 2: {   // measure A, then move the tone
        if (m_lcClock.elapsed() < kMeasureMs) return;
        m_lcPa = median(m_lcSamples);
        m_lcSamples.clear();
        // A step the DDS cannot be mistaken for, inside the displayed band.
        const double half = 0.4 * std::min(genConfig().sampleRateSps,
                                           m_rxRate > 0 ? m_rxRate : 1e12);
        double step = 7.3e6;
        if (std::abs(m_lcTxA + step) > half) step = -step;
        const double dds = (m_rfDds ? m_rfDds->value() : 0.0) * 1e6;
        if (std::abs(std::abs(m_lcTxA + step) - dds) < 1e6) step *= 1.6;
        m_genTone->setValue((m_lcTxA + step) / 1e6);   // -> live update
        m_lcStep = 3; m_lcClock.restart();
        return;
    }
    case 3:
        if (m_lcClock.elapsed() < kSettleMs) return;
        m_lcTxB = sdr::IqGenerator::transmittedHz(genConfig());
        m_lcSamples.clear();
        m_lcStep = 4; m_lcClock.restart();
        return;
    case 4:
        if (m_lcClock.elapsed() < kMeasureMs) return;
        m_lcPb = median(m_lcSamples);
        finishLoopCheck();
        return;
    default:
        m_lcTimer->stop();
    }
}

void ControlWindow3D::finishLoopCheck()
{
    const int nB = m_lcSamples.size();
    m_lcStep = 0;
    m_lcTimer->stop();
    m_genTone->setValue(m_lcOrigMhz);           // restore

    const double dTx = m_lcTxB - m_lcTxA;
    const double dRx = m_lcPb - m_lcPa;
    const double k = (m_devAdcStream > 0 && m_rxRate > 0) ? m_devAdcStream * 1e6 / m_rxRate : 1.0;
    const double tol = std::max(3.0 * m_rxBin * k, 0.01 * std::abs(dTx));
    const int src = m_simMode ? (m_rfDacSrc ? m_rfDacSrc->currentData().toInt() : -1)
                              : m_devDacSrc;

    QString msg;
    const char* colour = "#f87171";
    if (nB < 3 || m_lcPa == 0.0) {
        msg = tr("✗ No steady carrier was received during the check.");
    } else if (std::abs(std::abs(dRx) - std::abs(dTx)) <= tol) {
        const bool inv = (dRx * dTx) < 0;
        msg = tr("✓ Loopback verified. The generator moved %1 MHz and the tone received "
                 "through the ADC moved %2 MHz%3. The generated samples ARE reaching the "
                 "DAC and coming back through the ADC.")
                  .arg(dTx / 1e6, 0, 'f', 3).arg(dRx / 1e6, 0, 'f', 3)
                  .arg(inv ? tr(" (the loopback inverts the spectrum, so received = "
                                "−transmitted)") : QString());
        colour = "#33d17a";
    } else if (std::abs(dRx) < tol) {
        msg = tr("✗ The received tone did NOT move (it stayed at %1 MHz) when the "
                 "generator moved %2 MHz, so what the ADC sees is not the generator. ")
                  .arg(m_lcPa / 1e6, 0, 'f', 3).arg(dTx / 1e6, 0, 'f', 3);
        const double dds = (m_rfDds ? m_rfDds->value() : 0.0) * 1e6;
        if (std::abs(std::abs(m_lcPa) - expectedRxHz(dds)) < tol)
            msg += tr("It is at the DDS frequency (%1 MHz). ").arg(dds / 1e6, 0, 'f', 3);
        if (src == int(PCIE_DAC_SRC_DDS))
            msg += tr("Cause: the DAC input is the DDS compiler — select Host / GNU Radio "
                      "stream on the RFDC tab.");
        else if (m_simMode)
            msg += tr("(Simulation: tick \"Show the RF loopback\" on the RFDC tab.)");
        else if (src < 0)
            msg += tr("Attach the RFDC tab so the DAC input can be read from the device.");
        else if (!m_linkClock.isValid() || m_linkClock.elapsed() > 3000)
            msg += tr("Cause: iwfg_h2c reports no transfers — the H2C backend is not "
                      "running or not moving data (start acquisition in a mode that "
                      "includes H2C).");
        else
            msg += tr("iwfg_h2c IS moving %1 MiB/s to the card, and the DAC input switch IS "
                      "on the host stream (read back from the switch). So the card's DMA "
                      "engine accepts the data but it does not reach the DAC: the card-side "
                      "H2C path (QDMA H2C stream → H2C_data_in → DDS_H2C_switch S00_AXIS) is "
                      "not delivering. That is on the FPGA / driver side — probe S00_AXIS "
                      "tvalid/tdata with the ILA, and check that the driver enables the PL "
                      "H2C datapath when /dev/iwfg1 is opened.").arg(m_linkMib, 0, 'f', 1);
    } else {
        msg = tr("✗ The received tone moved %1 MHz for a %2 MHz step. The display's sample "
                 "rate does not match the stream (ADC stream %3 MSPS, display %4 MSPS), so "
                 "frequencies are scaled.")
                  .arg(dRx / 1e6, 0, 'f', 3).arg(dTx / 1e6, 0, 'f', 3)
                  .arg(m_devAdcStream, 0, 'f', 3).arg(m_rxRate / 1e6, 0, 'f', 3);
        colour = "#fbbf24";
    }
    m_chVerify->setText(msg);
    m_chVerify->setStyleSheet(QStringLiteral("color:%1;").arg(QLatin1String(colour)));
    log(tr("Loopback check: %1").arg(msg));
    refreshChain();
}

} // namespace sdr::ui
