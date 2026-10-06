#include "ConfigDialog.h"
#include <QMessageBox>
#include "../core/PcapExtract.h"
#include "../core/GpuIqPipeline.h"
#include <QInputDialog>
#include <QProcess>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QFileInfo>
#include <QFile>
#include <complex>
#include <vector>
#include <cmath>
#include <algorithm>
#include "../core/ConfigValidator.h"
#include <QRadioButton>
#include "../core/GpuNetProbe.h"
#include "../core/SystemProbe.h"
#include "PcieDiscoveryDialog.h"
#include "Theme.h"

#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QScrollArea>
#include <QFrame>
#include <QScreen>
#include <QGuiApplication>
#include <QSpinBox>
#include <QCheckBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace sdr::ui {

ConfigDialog::ConfigDialog(QWidget* parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("configDialog"));
    setWindowTitle(QStringLiteral("iWave SDR Analysis Tool"));
    setMinimumWidth(900);
    // Never taller than the screen: the previous dialog could exceed it and
    // push its own actions out of reach. Sized once here rather than by
    // adjustSize(), which would grow with the content again.
    {
        const QRect avail = QGuiApplication::primaryScreen()
                                ? QGuiApplication::primaryScreen()->availableGeometry()
                                : QRect(0, 0, 1280, 800);
        // Wide two-column dialog. Width is the primary dimension now; height
        // only needs to cover the taller of the two columns.
        const int w = qBound(900, int(avail.width()  * 0.80), 1060);
        // Floor as well as ceiling: some platforms report a small available
        // geometry, and a dialog shorter than its content would clip the
        // second column exactly as the old single-column layout did.
        const int h = qBound(660, int(avail.height() * 0.90), 780);
        resize(w, h);
        setMinimumHeight(qMin(660, avail.height()));
        setMaximumHeight(avail.height());
    }

    // -----------------------------------------------------------------
    // Layout architecture: fixed header, SCROLLABLE body, fixed action bar.
    //
    // Root cause of the unreachable Launch button: every section was added
    // to a single QVBoxLayout on the dialog, so the dialog grew without
    // bound as sections were appended and eventually ran past the bottom of
    // the screen. Moving the button would only have deferred the problem to
    // the next section.
    //
    // The primary actions now live in a bar OUTSIDE the scroll area, so they
    // are reachable at any window size and no matter how many configuration
    // groups exist. The body scrolls instead of the dialog growing.
    // -----------------------------------------------------------------
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* header = new QWidget;
    header->setObjectName(QStringLiteral("dialogHeader"));
    header->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* headerLay = new QVBoxLayout(header);
    headerLay->setContentsMargins(22, 16, 22, 10);
    headerLay->setSpacing(6);
    outer->addWidget(header);

    // Two columns rather than one long scroll. The configuration groups are
    // short and independent, so side-by-side they fit a normal desktop height
    // with no scrolling at all — which reads as a proper hardware
    // configuration panel rather than a phone form.
    //
    // Left  : what the hardware IS      (device, interface, compute target)
    // Right : what this SESSION does    (session start, data flow, validation)
    //
    // A scroll area remains as a fallback so an unusually short screen
    // degrades gracefully instead of clipping controls again; on a normal
    // display it never engages.
    auto* scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("dialogScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget;
    body->setObjectName(QStringLiteral("dialogBody"));
    scroll->setWidget(body);
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    outer->addWidget(scroll, 1);

    auto* columns = new QHBoxLayout(body);
    columns->setContentsMargins(22, 14, 22, 14);
    columns->setSpacing(18);

    auto* leftCol  = new QVBoxLayout;
    auto* rightCol = new QVBoxLayout;
    leftCol->setSpacing(14);
    rightCol->setSpacing(14);
    columns->addLayout(leftCol,  1);
    columns->addLayout(rightCol, 1);

    // `root` stays the name the group-building blocks below use; it is
    // repointed per column so those blocks did not have to be rewritten.
    QVBoxLayout* root = leftCol;

    // Responsive header: the full brand plate costs ~170 px, which is most of
    // a laptop-height dialog. On short screens use the compact mark and drop
    // the subtitle so the configuration body keeps the space it needs.
    const bool compactHeader = height() < 720;

    auto* brand = new QLabel;
    brand->setPixmap(QPixmap(compactHeader
        ? QStringLiteral(":/icons/iwave_logo_compact.png")
        : QStringLiteral(":/icons/iwave_logo_large.png")));
    brand->setAlignment(Qt::AlignHCenter);
    headerLay->addWidget(brand);

    auto* heading = new QLabel(QStringLiteral("SYSTEM CONFIGURATION"));
    heading->setObjectName(QStringLiteral("dialogHeading"));
    heading->setAlignment(Qt::AlignCenter);
    headerLay->addWidget(heading);

    auto* sub = new QLabel(QStringLiteral(
        "Select the acquisition hardware and how this session should start."));
    sub->setObjectName(QStringLiteral("dialogSub"));
    sub->setAlignment(Qt::AlignCenter);
    sub->setVisible(!compactHeader);
    headerLay->addWidget(sub);

    // ---- device + transport ---------------------------------------------
    {
        auto* g = new QGroupBox(QStringLiteral("ACQUISITION DEVICE"));
        auto* grid = new QGridLayout(g);
        grid->setContentsMargins(14, 16, 14, 14);
        grid->setSpacing(9);

        // Populated in repopulateDevices(), from what is actually present on
        // this machine. It was previously a fixed list of four literals, so
        // no amount of correct back-end discovery could ever reach the user:
        // that was the real break between detection and the GUI.
        // Development kits come from HardwareDb; the GUI holds no board
        // specifications of its own (brief §14).
        m_device = new QComboBox;
        for (const hw::BoardSpec& b : hw::HardwareDb::boards())
            m_device->addItem(b.displayName, b.id);

        m_pcie = buildToggle(QStringLiteral("PCIe"), true);
        m_eth  = buildToggle(QStringLiteral("Ethernet"), false);

        // Ethernet carries two different transports. They are not
        // interchangeable — RoCEv2 is RDMA into a shared-memory ring, UDP is
        // a datagram stream into a byte FIFO — so the choice is explicit
        // rather than inferred.
        m_ethTransport = new QComboBox;
        m_ethTransport->addItem(QStringLiteral("RoCEv2 (RDMA, shm ring)"),
                                QStringLiteral("roce"));
        m_ethTransport->addItem(QStringLiteral("UDP (100G, iwfg_c2h → FIFO)"),
                                QStringLiteral("udp"));
        m_ethTransport->setToolTip(QStringLiteral(
            "RoCEv2 uses the rdma_rx receiver and a shared-memory ring.\n"
            "UDP uses the iwfg_c2h receiver, which strips headers and writes "
            "raw payloads to a FIFO."));
        m_ethTransport->setEnabled(false);   // only meaningful for Ethernet

        // UDP port. The receiver binds this; the operator changes it here
        // rather than editing a command line.
        m_udpPort = new QSpinBox;
        m_udpPort->setRange(1, 65535);
        m_udpPort->setValue(16384);
        m_udpPort->setToolTip(QStringLiteral(
            "UDP port the receiver listens on. Checked for availability "
            "before the session starts."));
        m_udpPort->setEnabled(false);

        m_udpRaw = new QCheckBox(QStringLiteral("Raw capture (AF_PACKET)"));
        m_udpRaw->setChecked(true);
        m_udpRaw->setToolTip(QStringLiteral(
            "Tap the interface before the IP stack, so frames with a foreign "
            "destination MAC or a bad IP checksum are still received.\n"
            "Requires root — the application elevates via pkexec."));
        m_udpRaw->setEnabled(false);

        // The RDMA NIC is a separate question from the development kit. The
        // kit is the same board whichever transport reaches it; the NIC only
        // exists on the Ethernet path. Folding the two into one "Device" combo
        // meant selecting Ethernet replaced the board list with a NIC list, so
        // the validator lost its board and refused to launch.
        m_rdmaNic = new QComboBox;
        m_rdmaNic->setToolTip(QStringLiteral(
            "RDMA device and port carrying the RoCEv2 stream. Discovered from "
            "the host; not a property of the development kit."));
        m_rdmaNicLabel = new QLabel(QStringLiteral("RDMA NIC"));
        m_rdmaNic->hide();
        m_rdmaNicLabel->hide();

        grid->addWidget(new QLabel(QStringLiteral("Device")), 0, 0);
        grid->addWidget(m_device, 0, 1, 1, 2);
        grid->addWidget(new QLabel(QStringLiteral("Interface")), 1, 0);
        grid->addWidget(m_pcie, 1, 1);
        grid->addWidget(m_eth,  1, 2);
        grid->addWidget(new QLabel(QStringLiteral("Transport")), 2, 0);
        grid->addWidget(m_ethTransport, 2, 1, 1, 2);
        grid->addWidget(m_rdmaNicLabel, 3, 0);
        grid->addWidget(m_rdmaNic, 3, 1, 1, 2);
        grid->addWidget(new QLabel(QStringLiteral("UDP port")), 4, 0);
        grid->addWidget(m_udpPort, 4, 1);
        grid->addWidget(m_udpRaw, 4, 2);
        root->addWidget(g);
    }

    // ---- compute ---------------------------------------------------------
    {
        auto* g = new QGroupBox(QStringLiteral("COMPUTE TARGET"));
        auto* v = new QVBoxLayout(g);
        v->setContentsMargins(14, 16, 14, 14);
        v->setSpacing(8);
        auto* h = new QHBoxLayout;
        h->setSpacing(9);
        m_cpu = buildToggle(QStringLiteral("CPU (native)"), true);
        m_gpu = buildToggle(QStringLiteral("GPU offload"), false);
        h->addWidget(m_cpu);
        h->addWidget(m_gpu);
        v->addLayout(h);
        m_gpuStatus = new QLabel;
        m_gpuStatus->setObjectName(QStringLiteral("hintLabel"));
        m_gpuStatus->setWordWrap(true);
        m_gpuStatus->hide();
        v->addWidget(m_gpuStatus);

        m_ethStatus = new QLabel;
        m_ethStatus->setObjectName(QStringLiteral("hintLabel"));
        m_ethStatus->setWordWrap(true);
        m_ethStatus->hide();
        v->addWidget(m_ethStatus);
        root->addWidget(g);
    }

    // ---- hardware status (left column) ----------------------------------
    {
        auto* g = new QGroupBox(QStringLiteral("HARDWARE STATUS"));
        auto* v = new QVBoxLayout(g);
        v->setContentsMargins(12, 14, 12, 12);
        v->setSpacing(8);

        m_capability = new QLabel;
        m_capability->setObjectName(QStringLiteral("hintLabel"));
        m_capability->setWordWrap(true);
        v->addWidget(m_capability);

        // Always visible: hiding this when the configuration was valid left a
        // blank region, and a positive confirmation keeps the group's height
        // stable so the layout does not jump as settings change.
        m_findings = new QLabel;
        m_findings->setObjectName(QStringLiteral("hintLabel"));
        m_findings->setWordWrap(true);
        m_findings->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        m_findings->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        m_findings->setMinimumHeight(64);
        v->addWidget(m_findings, 1);

        root->addWidget(g, 1);   // absorbs the left column's spare height
    }

    root = rightCol;   // remaining groups fill the second column

    // ---- session start ---------------------------------------------------
    {
        auto* g = new QGroupBox(QStringLiteral("SESSION START"));
        auto* v = new QVBoxLayout(g);
        v->setContentsMargins(14, 16, 14, 14);
        v->setSpacing(9);

        // Entry 2 depends on the selected interface and is rewritten by
        // repopulateDevices(): a PCIe session streams from the DMA node, an
        // Ethernet session from the RoCEv2 receiver's ring. Offering only the
        // PCIe option meant selecting Ethernet still launched the PCIe path
        // and hunted for /dev/iwfg0.
        m_mode = new QComboBox;
        m_mode->addItems({QStringLiteral("Simulated signal — no hardware required"),
                          QStringLiteral("Offline — analyse a stored capture"),
                          QStringLiteral("Live — stream from PCIe DMA / FIFO")});
        v->addWidget(m_mode);

        m_stack = new QStackedWidget;

        auto* simPage = new QWidget;
        auto* simForm = new QFormLayout(simPage);
        simForm->setContentsMargins(0, 6, 0, 0);
        auto* simHint = new QLabel(QStringLiteral(
            "A pulse-shaped, noisy, frequency-offset burst is generated "
            "internally so every plot is live immediately."));
        simHint->setObjectName(QStringLiteral("hintLabel"));
        simHint->setWordWrap(true);
        simForm->addRow(simHint);
        m_stack->addWidget(simPage);

        auto* filePage = new QWidget;
        auto* fileForm = new QFormLayout(filePage);
        fileForm->setContentsMargins(0, 6, 0, 0);
        auto* fileRow = new QWidget;
        auto* fh = new QHBoxLayout(fileRow);
        fh->setContentsMargins(0, 0, 0, 0);
        fh->setSpacing(5);
        m_file = new QLineEdit;
        m_file->setPlaceholderText(QStringLiteral(
            "capture.bin / .iq / .cf32  —  or a .pcap, extracted automatically"));
        m_file->setToolTip(QStringLiteral(
            "Raw sample files are plotted directly.\n"
            "A .pcap still carries Ethernet/IP/UDP headers around every "
            "payload, so it is extracted first: RoCEv2 captures via "
            "roce-extractor, plain UDP captures in-place."));
        auto* browse = new QPushButton(QStringLiteral("Browse…"));
        fh->addWidget(m_file, 1);
        fh->addWidget(browse, 0);
        fileForm->addRow(new QLabel(QStringLiteral("Capture file")), fileRow);
        m_stack->addWidget(filePage);

        auto* livePage = new QWidget;
        auto* liveForm = new QFormLayout(livePage);
        liveForm->setContentsMargins(0, 6, 0, 0);
        m_device_ = new QLineEdit(QStringLiteral("/dev/iwfg0"));
        m_device_->setPlaceholderText(QStringLiteral("/dev/iwfg0, /dev/xdma0_c2h_0, or a FIFO"));
        liveForm->addRow(new QLabel(QStringLiteral("Device / FIFO")), m_device_);
        m_stack->addWidget(livePage);

        v->addWidget(m_stack);

        auto* common = new QWidget;
        auto* cf = new QFormLayout(common);
        cf->setContentsMargins(0, 4, 0, 0);
        cf->setSpacing(7);

        m_format = new QComboBox;
        // Same data-bound population as the source panel; see Panels.cpp.
        {
            const auto order = formatOrder();
            for (int i = 0; i < order.size(); ++i) {
                if (i == formatComplexCount()) m_format->insertSeparator(m_format->count());
                m_format->addItem(formatName(order[i]), static_cast<int>(order[i]));
            }
            const int def = m_format->findData(static_cast<int>(SampleFormat::Cf32));
            if (def >= 0) m_format->setCurrentIndex(def);
        }

        // Both lists are rebuilt by refreshChannelChoices() from the selected
        // board. They were fixed lists capping at 8 channels and 245.76 MSPS,
        // which had two consequences: a 16-channel board could not be fully
        // used, and no rate reachable from the combo could ever exceed a
        // board maximum, so the rate validation was unreachable from the GUI.
        m_channels = new QComboBox;
        m_rate = new QComboBox;
        m_rate->setEditable(true);

        cf->addRow(new QLabel(QStringLiteral("Wire format")), m_format);
        cf->addRow(new QLabel(QStringLiteral("Channels")), m_channels);
        cf->addRow(new QLabel(QStringLiteral("Sample rate [MSPS]")), m_rate);
        v->addWidget(common);

        root->addWidget(g);

        connect(browse, &QPushButton::clicked, this, [this] {
            const QString p = QFileDialog::getOpenFileName(
                this, QStringLiteral("Open capture file"), m_file->text(),
                QStringLiteral("Capture files (*.bin *.dat *.iq *.cf32 *.cs16 *.raw);;"
                               "RoCEv2 packet captures (*.pcap *.pcapng);;"
                               "All files (*)"));
            if (p.isEmpty()) { refreshState(); return; }

            // A .pcap still has Ethernet/IP/UDP/BTH/RETH headers and a
            // trailing ICRC around each payload. Plotting it directly would
            // show header bytes as samples. roce-extractor already strips all
            // of that correctly, so run it rather than re-implementing the
            // decode in the GUI — there must be one parser, not two.
            if (p.endsWith(QStringLiteral(".pcap"), Qt::CaseInsensitive)
             || p.endsWith(QStringLiteral(".pcapng"), Qt::CaseInsensitive)) {
                const QString bin = extractCapture(p);
                if (bin.isEmpty()) { refreshState(); return; }
                m_file->setText(bin);
            } else {
                m_file->setText(p);
            }
            detectAndApplyFormat(m_file->text());
            refreshState();
        });
    }

    m_summary = new QLabel;
    m_summary->setObjectName(QStringLiteral("summaryLabel"));
    m_summary->setAlignment(Qt::AlignCenter);
    // ---- data flow (right column, under session start) ------------------
    {
        auto* g = new QGroupBox(QStringLiteral("DATA FLOW"));
        auto* v = new QVBoxLayout(g);
        v->setContentsMargins(12, 14, 12, 12);
        v->setSpacing(8);

        // Segmented toggles rather than plain radio buttons: a small dot is
        // easy to miss, and this reuses the PCIe/Ethernet visual language
        // already established in this dialog so the selected transfer
        // direction is unmistakable at a glance.
        m_flowAdc  = new QRadioButton(QStringLiteral("C2H\nADC Only"));
        m_flowDac  = new QRadioButton(QStringLiteral("H2C\nDAC Only"));
        m_flowBoth = new QRadioButton(QStringLiteral("C2H + H2C\nADC + DAC"));
        for (QRadioButton* b : {m_flowAdc, m_flowDac, m_flowBoth}) {
            b->setObjectName(QStringLiteral("flowToggle"));
            b->setCursor(Qt::PointingHandCursor);
            b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            b->setMinimumHeight(46);
        }
        m_flowAdc->setToolTip(QStringLiteral("Capture from the converters (C2H)"));
        m_flowDac->setToolTip(QStringLiteral("Playback to the converters (H2C)"));
        m_flowBoth->setToolTip(QStringLiteral("Duplex capture and playback (C2H + H2C)"));
        m_flowAdc->setChecked(true);

        auto* row = new QHBoxLayout;
        row->setSpacing(8);
        row->addWidget(m_flowAdc,  1);
        row->addWidget(m_flowDac,  1);
        row->addWidget(m_flowBoth, 1);
        v->addLayout(row);

        auto* hint = new QLabel(QStringLiteral(
            "H2C control is available only when a transmit path is selected."));
        hint->setObjectName(QStringLiteral("hintLabel"));
        hint->setWordWrap(true);
        v->addWidget(hint);

        root->addWidget(g);

        for (QRadioButton* b : {m_flowAdc, m_flowDac, m_flowBoth})
            connect(b, &QRadioButton::toggled, this, &ConfigDialog::refreshState);
    }

    leftCol->addStretch(1);    // keep groups top-aligned in both columns
    rightCol->addStretch(1);

    // ---- action bar: outside the scroll area, always visible -------------
    {
        auto* bar = new QWidget;
        bar->setObjectName(QStringLiteral("dialogActionBar"));
        bar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        auto* barLay = new QVBoxLayout(bar);
        barLay->setContentsMargins(22, 12, 22, 14);
        barLay->setSpacing(10);
        barLay->addWidget(m_summary);

        auto* row = new QWidget;
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(9);
        auto* cancel = new QPushButton(QStringLiteral("Cancel"));
        m_launch = new QPushButton(QStringLiteral("LAUNCH WORKSTATION"));
        m_launch->setObjectName(QStringLiteral("launchButton"));
        m_launch->setDefault(true);
        h->addStretch(1);
        h->addWidget(cancel);
        h->addWidget(m_launch);
        barLay->addWidget(row);
        outer->addWidget(bar);

        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(m_launch, &QPushButton::clicked, this, [this] {
            commit();

            // PCIe pre-flight (requirements: driver validation + link
            // discovery). Runs whenever the PCIe interface is selected for a
            // live session — regardless of CPU/GPU backend — and blocks
            // launch on a hard failure instead of dropping the operator into
            // a stream that cannot work. The dialog itself hosts the
            // integrated driver setup ("Fix driver…"), so a missing module
            // is a one-click repair, not a terminal error. Offline and
            // simulated sessions skip it: no hardware is involved.
            if (m_eth->isChecked() && !udpSelected()
                && m_cfg.initial.src.mode == SourceMode::Roce) {
                // Ethernet pre-flight: verify the discovered mapping instead
                // of running the PCIe/iwfg discovery, which is meaningless
                // for this transport.
                QString why;
                const auto sel = probe::selectRoceMapping(probe::mapNetdevsToRdma(), &why);
                if (sel.rdmaDevice.isEmpty()) {
                    QMessageBox::warning(this, QStringLiteral("RoCEv2"),
                        QStringLiteral("RoCEv2 initialisation cannot proceed.\n\n%1")
                            .arg(why));
                    return;
                }
            }
            else if (m_pcie->isChecked()
                && m_cfg.initial.src.mode == SourceMode::Dma) {
                PcieDiscoveryDialog discovery(m_cfg.initial.src.devicePath, this);
                if (discovery.exec() != QDialog::Accepted || !discovery.passed())
                    return;                       // stay in the launcher
                // Remember if the pre-flight loaded the module, so the main
                // window can rmmod it on exit and leave a clean system.
                m_cfg.driverLoadedBySession = discovery.driverLoadedHere();
            }
            accept();
        });
    }

    // ---- wiring ----------------------------------------------------------
    auto exclusive = [this](QPushButton* a, QPushButton* b) {
        connect(a, &QPushButton::clicked, this, [this, a, b] {
            a->setChecked(true);
            b->setChecked(false);
            refreshState();
        });
    };
    connect(m_device, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ConfigDialog::refreshState);

    exclusive(m_pcie, m_eth);
    exclusive(m_eth, m_pcie);

    // The device list depends on the selected interface, so rebuild it on
    // every toggle as well as at start-up.
    connect(m_pcie, &QPushButton::toggled, this, [this](bool){ repopulateDevices(); });
    connect(m_eth,  &QPushButton::toggled, this, [this](bool on){
        m_ethTransport->setEnabled(on);
        // The NIC selector only means anything on the RoCEv2 path.
        if (m_rdmaNic)      m_rdmaNic->setVisible(on);
        if (m_rdmaNicLabel) m_rdmaNicLabel->setVisible(on);
        repopulateDevices();
    });
    connect(m_udpPort, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int){ refreshState(); });
    connect(m_ethTransport, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int){
        const bool udp = udpSelected() && m_eth->isChecked();
        m_udpPort->setEnabled(udp);
        m_udpRaw->setEnabled(udp);
        repopulateDevices();
    });

    // Selecting Ethernet detects a Mellanox NIC on the spot and, when found,
    // applies the lab configuration (192.168.1.1, MTU 9000, link up). A system
    // without the adapter gets a clear status line rather than a failure.
    connect(m_eth, &QPushButton::toggled, this, [this](bool on) {
        if (!on) { m_ethStatus->hide(); return; }
        m_ethStatus->show();
        m_ethStatus->setText(QStringLiteral("Detecting Mellanox adapter…"));
        const auto e = probe::detectMellanox();
        if (!e.present) {
            m_ethStatus->setText(QStringLiteral("✗ %1").arg(e.detail));
            m_ethStatus->setStyleSheet(QStringLiteral("color:#f5b442;"));
            return;
        }
        const auto r = probe::configureMellanox(e.name);
        if (r.ok) {
            m_ethStatus->setText(QStringLiteral("✓ %1 configured — %2, MTU %3")
                                     .arg(r.iface.name, r.iface.ipAddress).arg(r.iface.mtu));
            m_ethStatus->setStyleSheet(QStringLiteral("color:#33d17a;"));
        } else {
            m_ethStatus->setText(QStringLiteral("⚠ %1 found; %2")
                                     .arg(e.name, r.failure));
            m_ethStatus->setStyleSheet(QStringLiteral("color:#f5b442;"));
        }
    });
    exclusive(m_cpu, m_gpu);
    exclusive(m_gpu, m_cpu);

    // GPU selection triggers detection there and then, so the operator finds
    // out about a missing driver in the launcher, not mid-acquisition. The
    // probe runs synchronously but is bounded at 2.5 s by SystemProbe.
    connect(m_gpu, &QPushButton::toggled, this, [this](bool on) {
        if (!on) { m_gpuStatus->hide(); return; }
        m_gpuStatus->setText(QStringLiteral("Detecting GPU…"));
        m_gpuStatus->show();
        const auto g = probe::detectGpu();
        if (g.present) {
            m_gpuStatus->setText(QStringLiteral("✓ %1").arg(g.detail));
            m_gpuStatus->setStyleSheet(QStringLiteral("color:#33d17a;"));
        } else {
            // Fall back to CPU rather than launching a mode that cannot work.
            m_gpuStatus->setText(QStringLiteral(
                "✗ %1 — falling back to CPU").arg(g.detail));
            m_gpuStatus->setStyleSheet(QStringLiteral("color:#f5b442;"));
            m_cpu->setChecked(true);
        }
        refreshState();
    });

    connect(m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
        m_stack->setCurrentIndex(i);
        refreshState();
    });
    connect(m_device, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ConfigDialog::refreshState);
    connect(m_file, &QLineEdit::textChanged, this, &ConfigDialog::refreshState);
    connect(m_rate, &QComboBox::currentTextChanged, this, &ConfigDialog::refreshState);
    connect(m_channels, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ConfigDialog::refreshState);

    repopulateDevices();   // fill Device/Port from discovered hardware
    refreshState();
}

QPushButton* ConfigDialog::buildToggle(const QString& text, bool checked)
{
    auto* b = new QPushButton(text);
    b->setCheckable(true);
    b->setChecked(checked);
    b->setProperty("toggle", true);
    return b;
}

// Run roce-extractor over a .pcap to produce the payload-only .bin the
// plotter consumes. Returns the .bin path, or empty on failure/cancel.
namespace {

/// Spectral flatness of `raw` decoded as `fmt`: geometric mean over arithmetic
/// mean of the power spectrum. ~1.0 is noise-like (wrong format), << 1.0 is
/// structured (right one). Returns 1.0 for anything non-finite, so a float
/// reading of integer bytes cannot win by producing inf.
double spectralFlatness(const QByteArray& raw, SampleFormat fmt, int ch)
{
    constexpr int N = 4096;
    std::vector<std::complex<double>> a(N, {0.0, 0.0});

    const char*  p = raw.constData();
    const size_t bytes = static_cast<size_t>(raw.size());
    const int    c = qMax(1, ch);

    auto rd = [&](size_t i) -> std::complex<double> {
        switch (fmt) {
        case SampleFormat::Cs16: {
            const size_t k = i * 2 * c;
            if ((k + 2) * 2 > bytes) return {};
            const qint16* v = reinterpret_cast<const qint16*>(p);
            return { double(v[k]), double(v[k + 1]) };
        }
        case SampleFormat::Cs32: {
            const size_t k = i * 2 * c;
            if ((k + 2) * 4 > bytes) return {};
            const qint32* v = reinterpret_cast<const qint32*>(p);
            return { double(v[k]), double(v[k + 1]) };
        }
        case SampleFormat::Rs16: {
            const size_t k = i * c;
            if ((k + 1) * 2 > bytes) return {};
            return { double(reinterpret_cast<const qint16*>(p)[k]), 0.0 };
        }
        case SampleFormat::Rs32: {
            const size_t k = i * c;
            if ((k + 1) * 4 > bytes) return {};
            return { double(reinterpret_cast<const qint32*>(p)[k]), 0.0 };
        }
        case SampleFormat::Cf32: {
            const size_t k = i * 2 * c;
            if ((k + 2) * 4 > bytes) return {};
            const float* v = reinterpret_cast<const float*>(p);
            if (!std::isfinite(v[k]) || !std::isfinite(v[k + 1])) return {};
            return { double(v[k]), double(v[k + 1]) };
        }
        case SampleFormat::Cs8: {
            const size_t k = i * 2 * c;
            if (k + 2 > bytes) return {};
            const qint8* v = reinterpret_cast<const qint8*>(p);
            return { double(v[k]), double(v[k + 1]) };
        }
        default: return {};
        }
    };

    for (int i = 0; i < N; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / (N - 1)); // Hann
        a[size_t(i)] = rd(size_t(i)) * w;
    }

    // iterative radix-2
    for (int i = 1, j = 0; i < N; ++i) {
        int b = N >> 1;
        for (; j & b; b >>= 1) j ^= b;
        j ^= b;
        if (i < j) std::swap(a[size_t(i)], a[size_t(j)]);
    }
    for (int len = 2; len <= N; len <<= 1) {
        const double ang = -2.0 * M_PI / len;
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (int i = 0; i < N; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (int k = 0; k < len / 2; ++k) {
                const auto u = a[size_t(i + k)];
                const auto v = a[size_t(i + k + len / 2)] * w;
                a[size_t(i + k)]             = u + v;
                a[size_t(i + k + len / 2)]   = u - v;
                w *= wl;
            }
        }
    }

    double lg = 0.0, ar = 0.0;
    for (int i = 0; i < N; ++i) {
        double pw = std::norm(a[size_t(i)]);
        if (!std::isfinite(pw) || pw < 1e-20) pw = 1e-20;
        lg += std::log(pw);
        ar += pw;
    }
    if (!std::isfinite(lg) || !std::isfinite(ar) || ar <= 0.0) return 1.0;
    const double r = std::exp(lg / N) / (ar / N);
    return std::isfinite(r) ? r : 1.0;
}

} // namespace

// A .pcap can hold either encapsulation, and they need different treatment:
//
//   RoCEv2 : Ethernet/IPv4/UDP:4791/BTH[/RETH]/payload/ICRC
//            -> roce-extractor, which also does PSN ordering, dedup, loss
//               detection and ICRC validation
//   UDP    : Ethernet/IPv4/UDP/payload
//            -> handled in-process; nothing else does this offline
//
// The Transport selector supplies the default, so the common case is one
// click, but the operator can override: a capture's encapsulation is a
// property of the file, not of how this session happens to be configured.
QString ConfigDialog::extractCapture(const QString& pcap)
{
    const bool roceDefault = !udpSelected();

    QStringList choices{
        QStringLiteral("RoCEv2  —  strip Ethernet/IP/UDP/BTH/RETH and ICRC"),
        QStringLiteral("UDP  —  strip Ethernet/IP/UDP headers only")};
    bool okPressed = false;
    const QString pick = QInputDialog::getItem(this,
        QStringLiteral("Extract capture"),
        QStringLiteral("How are the payloads encapsulated in\n%1 ?")
            .arg(QFileInfo(pcap).fileName()),
        choices, roceDefault ? 0 : 1, false, &okPressed);
    if (!okPressed) return QString();

    if (pick.startsWith(QStringLiteral("RoCEv2")))
        return extractPcap(pcap);              // roce-extractor
    return extractUdp(pcap);                   // in-process
}

// Plain UDP: strip Ethernet/IP/UDP and keep the payload. Done here because
// iwfg_c2h only does this on the LIVE path — no tool turns a stored .pcap
// into raw UDP payloads.
QString ConfigDialog::extractUdp(const QString& pcap)
{
    const QString bin = QFileInfo(pcap).absolutePath() + QLatin1Char('/')
                      + QFileInfo(pcap).completeBaseName()
                      + QStringLiteral("_udp.bin");

    const quint16 port = static_cast<quint16>(m_udpPort->value());
    const auto useFilter = QMessageBox::question(this,
        QStringLiteral("Extract UDP"),
        QStringLiteral("Keep only datagrams on port %1?\n\n"
                       "Choose No to accept every UDP datagram in the capture.")
            .arg(port),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

    sdr::PcapExtractStats st;
    const bool ok = sdr::extractUdpPayloads(
        pcap, bin, useFilter == QMessageBox::Yes ? port : quint16(0), st);

    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("Extract UDP"),
            QStringLiteral("%1\n\n%2").arg(st.error, sdr::describe(st)));
        return QString();
    }
    QMessageBox::information(this, QStringLiteral("Extract UDP"),
        QStringLiteral("Wrote %1\n\n%2")
            .arg(QFileInfo(bin).fileName(), sdr::describe(st)));
    return bin;
}

QString ConfigDialog::extractPcap(const QString& pcap)
{
    // Look beside the application first, then on PATH, so a co-located build
    // is used without the operator configuring anything.
    QString exe = QCoreApplication::applicationDirPath()
                + QStringLiteral("/roce-extractor");
    if (!QFileInfo::exists(exe)) exe = QStandardPaths::findExecutable(
                                          QStringLiteral("roce-extractor"));
    if (exe.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Extract .pcap"),
            QStringLiteral(
                "roce-extractor was not found beside the application or on "
                "PATH.\n\nA .pcap still carries Ethernet/IP/UDP/BTH headers "
                "and a trailing ICRC around every payload, so it cannot be "
                "plotted directly.\n\nExtract it first:\n\n"
                "  roce-extractor --pcap \"%1\" --output out.bin --icrc strict\n\n"
                "then open out.bin here.").arg(pcap));
        return QString();
    }

    const QString bin = QFileInfo(pcap).absolutePath() + QLatin1Char('/')
                      + QFileInfo(pcap).completeBaseName()
                      + QStringLiteral(".bin");

    if (QFileInfo::exists(bin)) {
        const auto a = QMessageBox::question(this, QStringLiteral("Extract .pcap"),
            QStringLiteral("%1 already exists.\n\nRe-extract and overwrite it?")
                .arg(QFileInfo(bin).fileName()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (a == QMessageBox::No) return bin;      // reuse what is there
    }

    QProgressDialog dlg(QStringLiteral("Extracting RoCEv2 payloads…\n%1")
                            .arg(QFileInfo(pcap).fileName()),
                        QStringLiteral("Cancel"), 0, 0, this);
    dlg.setWindowModality(Qt::WindowModal);
    dlg.show();

    QProcess proc;
    proc.start(exe, {QStringLiteral("--pcap"),   pcap,
                     QStringLiteral("--output"), bin,
                     QStringLiteral("--icrc"),   QStringLiteral("strict")});
    if (!proc.waitForStarted(5000)) {
        QMessageBox::warning(this, QStringLiteral("Extract .pcap"),
            QStringLiteral("Could not start %1.").arg(exe));
        return QString();
    }
    while (proc.state() != QProcess::NotRunning) {
        if (dlg.wasCanceled()) { proc.kill(); proc.waitForFinished(2000); return QString(); }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        proc.waitForFinished(50);
    }
    dlg.close();

    const QString errOut = QString::fromLocal8Bit(proc.readAllStandardError());
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0
        || !QFileInfo::exists(bin) || QFileInfo(bin).size() == 0) {
        QMessageBox::warning(this, QStringLiteral("Extract .pcap"),
            QStringLiteral("Extraction failed (exit %1).\n\n%2")
                .arg(proc.exitCode())
                .arg(errOut.isEmpty() ? QStringLiteral("No diagnostics.") : errOut));
        return QString();
    }
    return bin;
}

// Choose the wire format by MEASURING the file, not by trusting its name.
//
// Decoded with the correct format a capture has spectral structure; decoded
// wrongly the bytes are re-partitioned into samples that mean nothing and the
// result approaches white noise. Spectral flatness separates the two.
void ConfigDialog::detectAndApplyFormat(const QString& path)
{
    if (path.isEmpty() || !QFileInfo::exists(path)) return;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray raw = f.read(2 << 20);
    f.close();
    if (raw.size() < 4096) return;

    const int ch = qMax(1, m_channels->currentText().toInt());

    struct Cand { SampleFormat fmt; const char* label; };
    static const Cand cands[] = {
        { SampleFormat::Cs16, "Complex Int 16" },
        { SampleFormat::Cs32, "Complex Int 32" },
        { SampleFormat::Rs16, "Short 16"       },
        { SampleFormat::Rs32, "Int 32"         },
        { SampleFormat::Cf32, "Complex Float 32" },
        { SampleFormat::Cs8,  "Complex Int 8"  },
    };

    double best = 2.0;
    SampleFormat bestFmt = m_format->currentData().isValid()
        ? static_cast<SampleFormat>(m_format->currentData().toInt())
        : SampleFormat::Cs16;
    for (const Cand& c : cands) {
        const double fl = spectralFlatness(raw, c.fmt, ch);
        if (fl < best) { best = fl; bestFmt = c.fmt; }
    }

    const int idx = m_format->findData(static_cast<int>(bestFmt));
    if (idx >= 0 && idx != m_format->currentIndex()) {
        m_format->setCurrentIndex(idx);
        // Say what changed and why, rather than silently moving a control the
        // operator set.
        if (m_findings)
            m_findings->setText(QStringLiteral(
                "Wire format set to \"%1\" by analysing the capture.\n"
                "Change it if you know better — this is a measurement, not a "
                "declaration in the file.").arg(formatName(bestFmt)));
    }
}

void ConfigDialog::refreshState()
{
    const int mode = m_mode->currentIndex();
    const double rate = m_rate->currentText().toDouble();

    // Launching offline with no file selected would drop the user into an
    // error dialog immediately, so block it here instead.
    const bool ready = (mode != 1) || !m_file->text().trimmed().isEmpty();

    refreshChannelChoices();

    // ---- offline: no hardware is involved, so nothing about it is checked --
    //
    // A stored capture is self-describing: wire format, channel count and
    // sample rate come from the file and the fields beside it. Running the
    // converter/board rules here meant a machine with no RDMA NIC and no kit
    // selected could not open a .bin it had already recorded — hardware
    // validation blocking a purely offline task.
    const bool offline = (mode == 1);
    if (offline) {
        m_launch->setEnabled(ready);
        m_launch->setToolTip(ready ? QString()
            : QStringLiteral("Choose a capture file to analyse."));
        if (m_capability)
            m_capability->setText(QStringLiteral(
                "Offline analysis — no acquisition hardware is used.\n"
                "Wire format, channel count and sample rate are taken from the "
                "fields above and must match how the capture was recorded."));
        m_findings->setText(ready
            ? QStringLiteral("✓ Offline capture selected.\n"
                             "   %1 · %2 ch · %3 MSPS")
                  .arg(m_format->currentText())
                  .arg(m_channels->currentText())
                  .arg(rate, 0, 'f', 2)
            : QStringLiteral("Select a capture file to analyse."));
        m_findings->setStyleSheet(ready ? QStringLiteral("color:#33d17a;")
                                        : QStringLiteral("color:#9ca3af;"));
        if (m_ethStatus) m_ethStatus->hide();
        m_summary->setText(QStringLiteral("Offline  ·  %1  ·  %2 ch  ·  %3 MSPS")
            .arg(m_format->currentText(), m_channels->currentText())
            .arg(rate, 0, 'f', 2));
        return;
    }

    // ---- hardware validation ------------------------------------------
    // Runs on every change so limits are enforced as the user types, not at
    // launch. Rate is entered in MSPS here; the engine works in GSPS.
    hw::RequestedConfig req;
    req.boardId = m_device->currentData().toString();
    req.mode = m_flowBoth->isChecked() ? hw::DataFlowMode::AdcAndDac
             : m_flowDac->isChecked()  ? hw::DataFlowMode::DacOnly
                                       : hw::DataFlowMode::AdcOnly;
    const int ch = m_channels->currentText().toInt();
    const double gsps = rate / 1000.0;
    if (hw::usesAdc(req.mode)) { req.adcChannels = ch; req.adcRateGsps = gsps; }
    if (hw::usesDac(req.mode)) { req.dacChannels = ch; req.dacRateGsps = gsps; }

    const hw::ValidationResult vr = hw::validate(req);

    if (const hw::BoardSpec* b = hw::HardwareDb::byId(req.boardId)) {
        auto conv = [](const hw::ConverterSpec& c) {
            if (!c.present) return QStringLiteral("none");
            return QStringLiteral("%1 ch · %2-bit · max %3")
                .arg(c.channels).arg(c.resolutionBits)
                .arg(c.maxSampleRateGsps.known()
                         ? QStringLiteral("%1 GSPS").arg(c.maxSampleRateGsps.value, 0, 'f', 2)
                         : QStringLiteral("UNVERIFIED"));
        };
        m_capability->setText(
            QStringLiteral("%1 · %2\nADC: %3\nDAC: %4\nData path: %5   "
                           "Max channels this mode: %6")
                .arg(b->device, b->family, conv(b->adc), conv(b->dac),
                     hw::dataFlowDirections(req.mode))
                .arg(b->maxChannelsFor(hw::usesAdc(req.mode), hw::usesDac(req.mode))));
    }

    // Errors block launch; warnings inform but do not.
    QStringList msgs;
    for (const hw::Finding& f : vr.findings) {
        if (f.severity == hw::Finding::Info) continue;
        const bool err = f.severity == hw::Finding::Error;
        QString m = QStringLiteral("%1  %2\n%3")
                        .arg(err ? QStringLiteral("✕ ERROR") : QStringLiteral("⚠ WARNING"),
                             f.title, f.detail);
        if (!f.remedy.isEmpty()) m += QStringLiteral("\n→ %1").arg(f.remedy);
        msgs << m;
    }
    if (msgs.isEmpty()) {
        m_findings->setText(QStringLiteral(
            "✓ Configuration valid for this development kit.\n"
            "   %1 channel%2 · %3 MSPS · %4")
                .arg(ch).arg(ch == 1 ? QString() : QStringLiteral("s"))
                .arg(rate, 0, 'f', 2)
                .arg(hw::dataFlowDirections(req.mode)));
        m_findings->setStyleSheet(QStringLiteral("color:#33d17a;"));
    } else {
        m_findings->setText(msgs.join(QStringLiteral("\n\n")));
        m_findings->setStyleSheet(vr.hasErrors()
            ? QStringLiteral("color:#f87171;") : QStringLiteral("color:#f5b442;"));
    }

    m_launch->setEnabled(ready && vr.ok());
    m_launch->setToolTip(vr.ok() ? QString()
        : QStringLiteral("Resolve the configuration errors above before launching."));

    m_summary->setText(QStringLiteral("%1  ·  %2  ·  %3 ch  ·  %4 MSPS  ·  %5")
        .arg(m_device->currentText(),
             m_pcie->isChecked() ? QStringLiteral("PCIe")
                                 : QStringLiteral("Ethernet"),
             m_channels->currentText())
        .arg(rate, 0, 'f', 2)
        .arg(m_cpu->isChecked() ? QStringLiteral("CPU") : QStringLiteral("GPU")));
}

void ConfigDialog::refreshChannelChoices()
{
    const hw::BoardSpec* b = hw::HardwareDb::byId(m_device->currentData().toString());
    const bool adc = !m_flowDac->isChecked();
    const bool dac = !m_flowAdc->isChecked();

    // ---- channels: powers of two up to the board's limit ----------------
    int maxCh = b ? b->maxChannelsFor(adc, dac) : 16;
    if (maxCh <= 0) maxCh = 16;          // unknown board: do not restrict

    const QString keepCh = m_channels->currentText();
    QSignalBlocker bc(m_channels);
    m_channels->clear();
    for (int n = 1; n <= maxCh; n *= 2) m_channels->addItem(QString::number(n));
    if (maxCh > 1 && (maxCh & (maxCh - 1)) != 0)   // non power of two
        m_channels->addItem(QString::number(maxCh));
    int i = m_channels->findText(keepCh);
    if (i < 0) i = m_channels->findText(QStringLiteral("2"));
    m_channels->setCurrentIndex(i >= 0 ? i : m_channels->count() - 1);

    // ---- sample rate: keep the familiar host rates, then add the board's
    // converter rate so the ceiling is actually reachable and the validation
    // rule can be exercised from the UI.
    const QString keepRate = m_rate->currentText();
    QSignalBlocker br(m_rate);
    m_rate->clear();
    m_rate->addItems({QStringLiteral("30.72"), QStringLiteral("61.44"),
                      QStringLiteral("122.88"), QStringLiteral("200.00"),
                      QStringLiteral("200.25"), QStringLiteral("245.76"),
                      QStringLiteral("491.52")});
    const double maxG = b ? b->maxRateGspsFor(adc, dac) : 0.0;
    if (maxG > 0.0) {
        const double maxMsps = maxG * 1000.0;
        for (double f : {0.25, 0.5, 1.0}) {
            const QString t = QString::number(maxMsps * f, 'f', 2);
            if (m_rate->findText(t) < 0) m_rate->addItem(t);
        }
    }
    int ri = m_rate->findText(keepRate);
    if (ri >= 0) m_rate->setCurrentIndex(ri);
    else         m_rate->setCurrentText(keepRate.isEmpty()
                                            ? QStringLiteral("122.88") : keepRate);
}

bool ConfigDialog::udpSelected() const
{
    return m_ethTransport
        && m_ethTransport->currentData().toString() == QLatin1String("udp");
}

void ConfigDialog::syncSessionForTransport()
{
    const bool eth = m_eth && m_eth->isChecked();
    // The live-session entry and its default path must match the transport,
    // otherwise an Ethernet session still asks for /dev/iwfg0.
    if (m_mode && m_mode->count() >= 3) {
        // Three transports now, not two: the label must name the one that
        // will actually run, or the operator cannot tell RoCEv2 from UDP.
        m_mode->setItemText(2, !eth
            ? QStringLiteral("Live — stream from PCIe DMA / FIFO")
            : udpSelected()
                ? QStringLiteral("Live — stream from UDP (iwfg_c2h FIFO)")
                : QStringLiteral("Live — stream from RoCEv2 (shared-memory ring)"));
    }
    if (m_device_) {
        const QString cur = m_device_->text();
        const bool isPcieDefault = cur.isEmpty() || cur.startsWith(QLatin1String("/dev/iwfg"));
        const bool isRoceDefault = cur.isEmpty() || cur.startsWith(QLatin1String("/dev/shm/iqring"));
        const bool isUdpDefault = cur.isEmpty()
                               || cur.startsWith(QLatin1String("/tmp/iwfg_c2h"));
        if (eth && udpSelected() && (isPcieDefault || isRoceDefault))
            m_device_->setText(QStringLiteral("/tmp/iwfg_c2h.fifo"));
        else if (eth && !udpSelected() && (isPcieDefault || isUdpDefault))
            m_device_->setText(QStringLiteral("/dev/shm/iqring"));
        else if (!eth && (isRoceDefault || isUdpDefault))
            m_device_->setText(QStringLiteral("/dev/iwfg0"));
    }

}

void ConfigDialog::repopulateDevices()
{
    const bool eth = m_eth && m_eth->isChecked();

    m_device->blockSignals(true);
    m_device->clear();

    if (eth) {
        if (udpSelected()) {
            // UDP needs a plain network interface, not an RDMA device: the
            // receiver binds a socket (or an AF_PACKET tap) to it.
            for (const probe::NetRdmaMapping& m : probe::mapNetdevsToRdma())
                if (!m.netdev.isEmpty())
                    m_device->addItem(QStringLiteral("%1  —  %2%3")
                        .arg(m.netdev,
                             m.carrier ? QStringLiteral("link up")
                                       : QStringLiteral("NO CARRIER"),
                             m.ipv4.isEmpty() ? QString()
                                              : QStringLiteral(", %1").arg(m.ipv4)),
                        m.netdev);
            if (m_device->count() == 0)
                m_device->addItem(QStringLiteral("No network interface found"), QString());
            if (m_ethStatus) {
                m_ethStatus->show();
                m_ethStatus->setText(QStringLiteral(
                    "UDP receiver writes raw %1 B payloads to a FIFO; the "
                    "workstation reads that FIFO.").arg(1472));
                m_ethStatus->setStyleSheet(QStringLiteral("color:#7d8b9a;"));
            }
            m_device->blockSignals(false);
            syncSessionForTransport();   // the UDP path used to skip this
            refreshState();
            return;
        }

        // Ethernet: the DEVICE list is unchanged — the development kit is the
        // same board whichever transport reaches it. What Ethernet adds is a
        // NIC choice, which is a property of the host, so it gets its own
        // control instead of replacing the board list.
        const auto maps = probe::mapNetdevsToRdma();
        m_rdmaNic->blockSignals(true);
        m_rdmaNic->clear();
        for (const probe::NetRdmaMapping& m : maps) {
            const QString label =
                QStringLiteral("%1 · port %2  (%3)  —  %4%5%6")
                    .arg(m.rdmaDevice).arg(m.rdmaPort)
                    .arg(m.netdev.isEmpty() ? QStringLiteral("no netdev") : m.netdev,
                         m.carrier ? QStringLiteral("link up")
                                   : QStringLiteral("NO CARRIER"),
                         m.ipv4.isEmpty() ? QString()
                                          : QStringLiteral(", %1").arg(m.ipv4),
                         m.verified() ? QString() : QStringLiteral("  [unverified]"));
            m_rdmaNic->addItem(label, m.rdmaDevice);
        }
        if (m_rdmaNic->count() == 0)
            m_rdmaNic->addItem(QStringLiteral("No RDMA device found"), QString());

        QString why;
        const probe::NetRdmaMapping sel = probe::selectRoceMapping(maps, &why);
        if (!sel.rdmaDevice.isEmpty()) {
            const int i = m_rdmaNic->findData(sel.rdmaDevice);
            if (i >= 0) m_rdmaNic->setCurrentIndex(i);
        }
        m_rdmaNic->blockSignals(false);

        if (m_ethStatus) {
            m_ethStatus->show();
            m_ethStatus->setText(why);
            m_ethStatus->setStyleSheet(sel.rdmaDevice.isEmpty()
                ? QStringLiteral("color:#f5b442;") : QStringLiteral("color:#33d17a;"));
        }

        // Board list is rebuilt identically for both transports.
        for (const hw::BoardSpec& b : hw::HardwareDb::boards())
            m_device->addItem(b.displayName, b.id);
    } else {
        // PCIe: the development kits from HardwareDb. These carry the board
        // specifications the validation engine reads, so the list must come
        // from the database rather than being re-typed here.
        for (const hw::BoardSpec& b : hw::HardwareDb::boards())
            m_device->addItem(b.displayName, b.id);
    }

    m_device->blockSignals(false);
    syncSessionForTransport();
    refreshState();
}

void ConfigDialog::commit()
{
    m_cfg.device        = m_device->currentText();
    // Link generation and lane width are properties of the enumerated device,
    // not something the user picks — showing "Gen4 x16" here was decoration
    // that could contradict the hardware actually present.
    m_cfg.interfaceName = m_pcie->isChecked() ? QStringLiteral("PCIe")
                                              : QStringLiteral("Ethernet");
    m_cfg.computeDevice = m_cpu->isChecked() ? QStringLiteral("CPU (native)")
                                             : QStringLiteral("GPU offload");

    Config c;
    switch (m_mode->currentIndex()) {
    case 1:  c.src.mode = SourceMode::File; break;
    case 2:
        // UDP terminates in a plain byte FIFO, which DmaSource already reads;
        // only RoCEv2 needs the ring-aware source.
        c.src.mode = (m_eth->isChecked() && udpSelected()) ? SourceMode::Dma
                   : m_eth->isChecked()                    ? SourceMode::Roce
                                                           : SourceMode::Dma;
        break;
    default: c.src.mode = SourceMode::Simulated; break;
    }
    c.src.filePath       = m_file->text().trimmed();
    // Carry the compute target. Until now this toggle went nowhere.
    c.src.useGpu         = m_gpu && m_gpu->isChecked();
    c.src.devicePath     = m_device_->text().trimmed();
    {
        const QVariant v = m_format->currentData();
        c.src.format = v.isValid() ? static_cast<SampleFormat>(v.toInt())
                                   : SampleFormat::Cf32;
    }
    c.src.streamChannels = m_channels->currentText().toInt();

    c.acq.sampleRateMsps = m_rate->currentText().toDouble();
    if (c.acq.sampleRateMsps <= 0.0) c.acq.sampleRateMsps = 122.88;

    m_cfg.udpTransport = m_eth->isChecked() && udpSelected();
    // RoCEv2 is the other Ethernet transport: the main window can start
    // its receiver too, so the operator does not run rdma_rx by hand.
    m_cfg.roceTransport = m_eth->isChecked() && !udpSelected();
    if (m_cfg.udpTransport) {
        m_cfg.udpPort = m_udpPort->value();
        // The device combo holds the netdev name in UDP mode; --raw needs it.
        m_cfg.udpInterface = m_udpRaw->isChecked()
                                 ? m_device->currentData().toString() : QString();
        m_cfg.udpFifo = m_device_->text().isEmpty()
                            ? QStringLiteral("/tmp/iwfg_c2h.fifo") : m_device_->text();
    }

    m_cfg.dataFlowMode = m_flowBoth->isChecked() ? 2
                       : m_flowDac->isChecked()  ? 1 : 0;
    m_cfg.initial = c;
}

} // namespace sdr::ui
