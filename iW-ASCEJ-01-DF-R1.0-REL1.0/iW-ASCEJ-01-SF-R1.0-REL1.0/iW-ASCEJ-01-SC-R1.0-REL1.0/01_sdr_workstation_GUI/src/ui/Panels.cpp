#include "Panels.h"
#include "ChannelDialog.h"
#include "ColorMap.h"
#include "Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <cmath>

namespace sdr::ui {

namespace {

/// Row height that actually fits the widget's rendered text: line height plus
/// the cell padding the stylesheet applies, with a small floor. Scales with
/// the system font and with display scaling, unlike a literal pixel value.
int tableRowHeight(const QWidget* w)
{
    const QFontMetrics fm(w->font());
    return std::max(22, fm.height() + 10);
}


/// Populate a combo with the wire formats, binding each entry to its enum
/// value as item data. Reading selection back through data (not row index)
/// means the presentation order can change without silently reinterpreting a
/// stream — an index-based read turned "Complex Int 16" into a float32 decode
/// the moment the list grew.
void fillFormatCombo(QComboBox* c)
{
    c->clear();
    const auto order = formatOrder();
    for (int i = 0; i < order.size(); ++i) {
        if (i == formatComplexCount()) {
            c->insertSeparator(c->count());
        }
        c->addItem(formatName(order[i]), static_cast<int>(order[i]));
    }
}

void selectFormat(QComboBox* c, SampleFormat f)
{
    const int idx = c->findData(static_cast<int>(f));
    if (idx >= 0) c->setCurrentIndex(idx);
}

SampleFormat formatOf(const QComboBox* c, SampleFormat fallback)
{
    const QVariant v = c->currentData();
    return v.isValid() ? static_cast<SampleFormat>(v.toInt()) : fallback;
}


QFormLayout* makeForm(QWidget* host)
{
    auto* f = new QFormLayout(host);
    f->setContentsMargins(10, 10, 10, 10);
    f->setSpacing(6);
    f->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    return f;
}

QGroupBox* makeGroup(const QString& title, QFormLayout*& formOut)
{
    auto* g = new QGroupBox(title);
    formOut = makeForm(g);
    return g;
}

QString humanBytes(quint64 b)
{
    if (b >= (1ull << 30)) return QStringLiteral("%1 GiB").arg(b / double(1ull << 30), 0, 'f', 2);
    if (b >= (1ull << 20)) return QStringLiteral("%1 MiB").arg(b / double(1ull << 20), 0, 'f', 1);
    if (b >= (1ull << 10)) return QStringLiteral("%1 KiB").arg(b / double(1ull << 10), 0, 'f', 1);
    return QStringLiteral("%1 B").arg(b);
}

QString humanTime(qint64 ms)
{
    const qint64 s = ms / 1000;
    return QStringLiteral("%1:%2:%3")
        .arg(s / 3600, 2, 10, QLatin1Char('0'))
        .arg((s / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(s % 60, 2, 10, QLatin1Char('0'));
}

QString engFreq(double hz)
{
    const double a = std::abs(hz);
    if (a >= 1e9) return QString::number(hz / 1e9, 'f', 4) + QStringLiteral(" GHz");
    if (a >= 1e6) return QString::number(hz / 1e6, 'f', 3) + QStringLiteral(" MHz");
    if (a >= 1e3) return QString::number(hz / 1e3, 'f', 2) + QStringLiteral(" kHz");
    return QString::number(hz, 'f', 1) + QStringLiteral(" Hz");
}

} // namespace

// ============================================================ SourcePanel

SourcePanel::SourcePanel(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("sourcePanel"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    // ---- Mode ----------------------------------------------------------
    {
        QFormLayout* f = nullptr;
        auto* g = makeGroup(QStringLiteral("Acquisition mode"), f);
        m_mode = new QComboBox;
        m_mode->addItems({QStringLiteral("Simulated"),
                          QStringLiteral("Offline — capture file"),
                          QStringLiteral("Live — PCIe DMA / FIFO")});
        m_mode->setObjectName(QStringLiteral("modeCombo"));
        f->addRow(m_mode);
        root->addWidget(g);
    }

    m_stack = new QStackedWidget;
    // Without this the stack claims all spare height and pushes the signal
    // chain group to the bottom of the dock with a dead gap above it.
    m_stack->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    root->addWidget(m_stack);

    // ---- page 0: simulation -------------------------------------------
    {
        auto* page = new QWidget;
        QFormLayout* f = nullptr;
        auto* g = makeGroup(QStringLiteral("Simulated signal"), f);

        m_modulation = new QComboBox;
        m_modulation->addItems({QStringLiteral("QPSK"), QStringLiteral("16-QAM"),
                                QStringLiteral("64-QAM"), QStringLiteral("Noise only")});
        m_modulation->setCurrentIndex(1);

        m_snr = new QSpinBox;
        m_snr->setRange(0, 60);
        m_snr->setValue(25);
        m_snr->setSuffix(QStringLiteral(" dB"));

        m_txOffset = new QDoubleSpinBox;
        m_txOffset->setRange(-100.0, 100.0);
        m_txOffset->setDecimals(4);
        m_txOffset->setSuffix(QStringLiteral(" MHz"));
        m_txOffset->setValue(1.2288);

        m_simChannels = new QSpinBox;
        m_simChannels->setRange(1, 8);   // simulator synthesises up to 8
        m_simChannels->setValue(2);

        f->addRow(new QLabel(QStringLiteral("Modulation")), m_modulation);
        f->addRow(new QLabel(QStringLiteral("SNR")), m_snr);
        f->addRow(new QLabel(QStringLiteral("Tx offset")), m_txOffset);
        f->addRow(new QLabel(QStringLiteral("Channels")), m_simChannels);

        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(0, 0, 0, 0);
        v->addWidget(g);
        m_stack->addWidget(page);
    }

    // ---- page 1: offline file -----------------------------------------
    {
        auto* page = new QWidget;
        QFormLayout* f = nullptr;
        auto* g = makeGroup(QStringLiteral("Capture file"), f);

        auto* pathRow = new QWidget;
        auto* ph = new QHBoxLayout(pathRow);
        ph->setContentsMargins(0, 0, 0, 0);
        ph->setSpacing(4);
        m_filePath = new QLineEdit;
        m_filePath->setPlaceholderText(QStringLiteral("capture.bin / .dat / .iq / .cf32"));
        m_fileBrowse = new QPushButton(QStringLiteral("…"));
        // Square-ish button sized to the font, so the glyph is never clipped.
        m_fileBrowse->setFixedWidth(
            std::max(30, QFontMetrics(m_fileBrowse->font()).height() + 14));
        ph->addWidget(m_filePath, 1);
        ph->addWidget(m_fileBrowse, 0);

        m_fileFormat = new QComboBox;
        fillFormatCombo(m_fileFormat);
        selectFormat(m_fileFormat, SampleFormat::Cf32);

        m_fileChannels = new QSpinBox;
        m_fileChannels->setRange(1, 16);
        m_fileChannels->setValue(2);

        m_headerBytes = new QSpinBox;
        m_headerBytes->setRange(0, 1 << 20);
        m_headerBytes->setSuffix(QStringLiteral(" B"));

        m_loop  = new QCheckBox(QStringLiteral("Loop at end"));
        m_loop->setChecked(true);
        m_paced = new QCheckBox(QStringLiteral("Replay at nominal rate"));
        m_paced->setChecked(true);

        m_speed = new QDoubleSpinBox;
        m_speed->setRange(0.05, 20.0);
        m_speed->setSingleStep(0.25);
        m_speed->setValue(1.0);
        m_speed->setPrefix(QStringLiteral("× "));

        f->addRow(new QLabel(QStringLiteral("File")), pathRow);
        f->addRow(new QLabel(QStringLiteral("Wire format")), m_fileFormat);
        f->addRow(new QLabel(QStringLiteral("Channels")), m_fileChannels);
        f->addRow(new QLabel(QStringLiteral("Header skip")), m_headerBytes);
        f->addRow(m_loop);
        f->addRow(m_paced);
        f->addRow(new QLabel(QStringLiteral("Speed")), m_speed);

        QFormLayout* pf = nullptr;
        auto* pg = makeGroup(QStringLiteral("Position"), pf);
        m_seek = new QSlider(Qt::Horizontal);
        m_seek->setRange(0, 1000);
        m_seekLabel = new QLabel(QStringLiteral("—"));
        m_seekLabel->setObjectName(QStringLiteral("valueLabel"));
        m_seekLabel->setAlignment(Qt::AlignCenter);
        pf->addRow(m_seek);
        pf->addRow(m_seekLabel);

        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(0, 0, 0, 0);
        v->addWidget(g);
        v->addWidget(pg);
        m_stack->addWidget(page);
    }

    // ---- page 2: live DMA ----------------------------------------------
    {
        auto* page = new QWidget;
        QFormLayout* f = nullptr;
        auto* g = makeGroup(QStringLiteral("PCIe DMA interface"), f);

        auto* pathRow = new QWidget;
        auto* ph = new QHBoxLayout(pathRow);
        ph->setContentsMargins(0, 0, 0, 0);
        ph->setSpacing(4);
        m_devicePath = new QLineEdit(QStringLiteral("/dev/iwfg0"));
        m_devicePath->setPlaceholderText(QStringLiteral("/dev/iwfg0 or a FIFO path"));
        m_deviceBrowse = new QPushButton(QStringLiteral("…"));
        m_deviceBrowse->setFixedWidth(
            std::max(30, QFontMetrics(m_deviceBrowse->font()).height() + 14));
        ph->addWidget(m_devicePath, 1);
        ph->addWidget(m_deviceBrowse, 0);

        m_liveFormat = new QComboBox;
        fillFormatCombo(m_liveFormat);
        // Complex Int 16 is what most DMA front ends put on the wire.
        selectFormat(m_liveFormat, SampleFormat::Cs16);

        m_liveChannels = new QSpinBox;
        m_liveChannels->setRange(1, 16);
        m_liveChannels->setValue(2);

        // Clickable field showing the current selection; clicking opens the
        // modal chooser. A button (not a label) so it is keyboard-focusable
        // and announces itself as activatable to assistive tech.
        m_channelField = new QPushButton;
        m_channelField->setObjectName(QStringLiteral("channelField"));
        m_channelField->setCursor(Qt::PointingHandCursor);
        m_channelField->setToolTip(QStringLiteral("Click to choose channels"));
        m_channelField->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(m_channelField, &QPushButton::clicked,
                this, &SourcePanel::openChannelDialog);

        // Raising the wire channel count must not silently drop a selection:
        // newly revealed channels default to selected, existing ticks persist.
        connect(m_liveChannels, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this](int n) {
                    const quint32 all = (n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
                    m_channelMask = (m_channelMask == 0u) ? all : (m_channelMask & all);
                    if (m_channelMask == 0u) m_channelMask = all;
                    refreshChannelField();
                    if (!m_loading) emit configChanged();
                });

        m_blockSize = new QComboBox;
        m_blockSize->addItems({QStringLiteral("4096"), QStringLiteral("8192"),
                               QStringLiteral("16384"), QStringLiteral("32768"),
                               QStringLiteral("65536"), QStringLiteral("131072")});
        m_blockSize->setCurrentIndex(2);

        auto* hint = new QLabel(QStringLiteral(
            "Device node or named FIFO. Acquisition starts as soon as the "
            "node is readable."));
        hint->setObjectName(QStringLiteral("hintLabel"));
        hint->setWordWrap(true);

        f->addRow(new QLabel(QStringLiteral("Device / FIFO")), pathRow);
        f->addRow(new QLabel(QStringLiteral("Wire format")), m_liveFormat);
        f->addRow(new QLabel(QStringLiteral("Channels")), m_liveChannels);
        f->addRow(new QLabel(QStringLiteral("Select")), m_channelField);
        f->addRow(new QLabel(QStringLiteral("Block samples")), m_blockSize);
        f->addRow(hint);

        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(0, 0, 0, 0);
        v->addWidget(g);
        m_stack->addWidget(page);
    }

    // ---- Signal chain ---------------------------------------------------
    {
        QFormLayout* f = nullptr;
        auto* g = makeGroup(QStringLiteral("Signal chain"), f);

        m_sampleRate = new QComboBox;
        m_sampleRate->setEditable(true);
        m_sampleRate->addItems({QStringLiteral("30.72"), QStringLiteral("61.44"),
                                QStringLiteral("122.88"), QStringLiteral("200.00"),
                                QStringLiteral("200.25"), QStringLiteral("245.76"),
                                QStringLiteral("491.52")});
        m_sampleRate->setCurrentIndex(2);
        // The rate entered here IS the frequency scale: bin spacing is Fs/N,
        // so a rate that differs from the converter's true clock produces a
        // proportional error at every frequency (0.125 % between 200.00 and
        // 200.25, i.e. 12 kHz at 10 MHz but 112 kHz at 90 MHz). Enter the
        // measured converter rate, not the nominal one.
        m_sampleRate->setToolTip(QStringLiteral(
            "Converter sample rate in MSPS. This sets the frequency scale "
            "(bin spacing = rate / FFT size), so enter the actual clock rate "
            "-- any error here scales every displayed frequency."));

        m_decimation = new QSpinBox;
        m_decimation->setRange(1, 4096);
        m_decimation->setValue(1);

        m_ncoFreq = new QDoubleSpinBox;
        m_ncoFreq->setRange(-500.0, 500.0);
        m_ncoFreq->setDecimals(4);
        m_ncoFreq->setSuffix(QStringLiteral(" MHz"));
        m_ncoFreq->setValue(AcqConfig{}.ncoFreqMHz);

        m_centerFreq = new QDoubleSpinBox;
        // Lower bound 0, not 1 Hz: 0 means "no RF front end, report baseband",
        // which is the correct description of an ADC -> DMA -> FIFO path. The
        // old 0.000001 floor made that state unreachable from the UI.
        m_centerFreq->setRange(0.0, 30.0);
        m_centerFreq->setDecimals(6);
        m_centerFreq->setSingleStep(0.001);
        m_centerFreq->setSuffix(QStringLiteral(" GHz"));
        m_centerFreq->setToolTip(QStringLiteral(
            "RF centre of the captured band. 0 = baseband: the display then "
            "reports the frequency present in the samples themselves. Set your "
            "LO here if a front end shifts the band."));
        m_centerFreq->setValue(AcqConfig{}.centerFreqGHz);

        m_bufferSize = new QComboBox;
        m_bufferSize->addItems({QStringLiteral("4 MiB"), QStringLiteral("8 MiB"),
                                QStringLiteral("16 MiB"), QStringLiteral("64 MiB")});
        m_bufferSize->setCurrentIndex(2);

        f->addRow(new QLabel(QStringLiteral("Rate [MSPS]")), m_sampleRate);

        m_interpolation = new QSpinBox;
        m_interpolation->setRange(1, 4096);
        m_interpolation->setValue(1);
        m_interpolation->setToolTip(QStringLiteral(
            "Resampler numerator. Display rate = sample rate x interp / decim."));
        f->addRow(new QLabel(QStringLiteral("Interpolation")), m_interpolation);
        f->addRow(new QLabel(QStringLiteral("Decimation")), m_decimation);
        f->addRow(new QLabel(QStringLiteral("NCO / DDC")), m_ncoFreq);
        f->addRow(new QLabel(QStringLiteral("Centre freq")), m_centerFreq);
        f->addRow(new QLabel(QStringLiteral("DMA buffer")), m_bufferSize);
        root->addWidget(g);
    }

    root->addStretch(1);

    // ---- wiring ---------------------------------------------------------
    const auto touched = [this] { if (!m_loading) emit configChanged(); };
    const auto restart = [this] { if (!m_loading) { emit configChanged(); emit sourceRestartRequired(); } };

    connect(m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, restart] {
        syncModeStack();
        restart();
    });

    for (QComboBox* c : {m_liveFormat, m_fileFormat, m_blockSize})
        connect(c, QOverload<int>::of(&QComboBox::currentIndexChanged), this, restart);
    for (QSpinBox* s : {m_liveChannels, m_fileChannels, m_simChannels, m_headerBytes})
        connect(s, QOverload<int>::of(&QSpinBox::valueChanged), this, restart);
    connect(m_filePath, &QLineEdit::editingFinished, this, restart);
    connect(m_devicePath, &QLineEdit::editingFinished, this, restart);

    for (QCheckBox* c : {m_loop, m_paced})
        connect(c, &QCheckBox::toggled, this, touched);
    connect(m_speed, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, touched);

    connect(m_modulation, QOverload<int>::of(&QComboBox::currentIndexChanged), this, touched);
    connect(m_snr, QOverload<int>::of(&QSpinBox::valueChanged), this, touched);
    connect(m_txOffset, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, touched);

    connect(m_sampleRate, &QComboBox::currentTextChanged, this, touched);
    connect(m_decimation, QOverload<int>::of(&QSpinBox::valueChanged), this, touched);
    connect(m_interpolation, QOverload<int>::of(&QSpinBox::valueChanged), this, touched);
    connect(m_ncoFreq, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, touched);
    connect(m_centerFreq, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, touched);
    connect(m_bufferSize, QOverload<int>::of(&QComboBox::currentIndexChanged), this, touched);

    connect(m_fileBrowse, &QPushButton::clicked, this, &SourcePanel::browseForFile);
    connect(m_deviceBrowse, &QPushButton::clicked, this, &SourcePanel::browseForDevice);

    // sliderReleased rather than valueChanged: seeking on every intermediate
    // value would thrash the file position while the user is still dragging.
    connect(m_seek, &QSlider::sliderReleased, this, [this] {
        emit seekRequested(m_seek->value() / 1000.0);
    });

    syncModeStack();
}

void SourcePanel::syncModeStack()
{
    m_stack->setCurrentIndex(m_mode->currentIndex());

    // A QStackedWidget normally reserves the height of its *tallest* page,
    // which leaves a dead gap under the short simulated page. Letting only
    // the visible page contribute a size hint collapses that gap.
    for (int i = 0; i < m_stack->count(); ++i)
        m_stack->widget(i)->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    if (QWidget* cur = m_stack->currentWidget()) {
        cur->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        cur->adjustSize();
    }
    m_stack->adjustSize();
}

sdr::SourceMode SourcePanel::mode() const
{
    switch (m_mode->currentIndex()) {
    case 1:  return SourceMode::File;
    // Index 2 is "live"; which live transport it means was decided in the
    // launcher and is remembered here. Returning Dma unconditionally would
    // silently downgrade a RoCEv2 session to the PCIe path on the first
    // config round-trip.
    case 2:  return m_liveMode;
    default: return SourceMode::Simulated;
    }
}

void SourcePanel::browseForFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open capture file"), m_filePath->text(),
        QStringLiteral("Capture files (*.bin *.dat *.iq *.cf32 *.cs16 *.raw);;All files (*)"));
    if (path.isEmpty()) return;

    m_filePath->setText(path);

    applySidecar(path);

    emit configChanged();
    emit sourceRestartRequired();
}

void SourcePanel::applySidecar(const QString& path)
{
    // A sidecar written by the recorder carries the rate and format, so a
    // capture reopens with the settings it was taken with instead of whatever
    // happened to be on screen.
    QFile meta(path + QStringLiteral(".meta"));
    if (meta.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // Suppress per-widget change signals; one configChanged is emitted by
        // the caller once the whole sidecar has been applied.
        const bool wasLoading = m_loading;
        m_loading = true;
        while (!meta.atEnd()) {
            const QString line = QString::fromUtf8(meta.readLine()).trimmed();
            if (line.startsWith(QLatin1Char('#'))) continue;
            const int eq = line.indexOf(QLatin1Char('='));
            if (eq < 1) continue;
            const QString key = line.left(eq);
            const QString val = line.mid(eq + 1);

            if (key == QLatin1String("format")) {
                for (SampleFormat f : formatOrder()) {
                    if (formatName(f) == val || formatTag(f) == val) {
                        selectFormat(m_fileFormat, f);
                        break;
                    }
                }
            } else if (key == QLatin1String("channels")) {
                m_fileChannels->setValue(val.toInt());
            } else if (key == QLatin1String("sample_rate_msps")) {
                m_sampleRate->setCurrentText(QString::number(val.toDouble(), 'f', 4));
            } else if (key == QLatin1String("center_freq_ghz")) {
                m_centerFreq->setValue(val.toDouble());
            } else if (key == QLatin1String("nco_freq_mhz")) {
                m_ncoFreq->setValue(val.toDouble());
            } else if (key == QLatin1String("decimation")) {
                m_decimation->setValue(std::max(1, val.toInt()));
            } else if (key == QLatin1String("channel_mask")) {
                bool okHex = false;
                const quint32 m = val.startsWith(QLatin1String("0x"))
                    ? val.mid(2).toUInt(&okHex, 16) : val.toUInt(&okHex, 16);
                if (okHex) { m_channelMask = m; refreshChannelField(); }
            } else if (key == QLatin1String("interpolation")) {
                m_interpolation->setValue(std::max(1, val.toInt()));
            } else if (key == QLatin1String("header_bytes")) {
                m_headerBytes->setValue(val.toInt());
            }
        }
        m_loading = wasLoading;
    }

}

void SourcePanel::refreshChannelField()
{
    if (!m_channelField) return;
    const int n = m_liveChannels ? m_liveChannels->value() : 1;
    m_channelField->setText(ChannelDialog::label(m_channelMask, n));
}

void SourcePanel::openChannelDialog()
{
    const int n = m_liveChannels ? m_liveChannels->value() : 1;
    ChannelDialog dlg(n, m_channelMask, this);
    if (dlg.exec() != QDialog::Accepted) return;   // Cancel preserves selection
    m_channelMask = dlg.mask();
    refreshChannelField();
    if (!m_loading) emit configChanged();
}

void SourcePanel::browseForDevice()
{
    // Device nodes and FIFOs are not regular files, so the dialog must not
    // filter them out.
    QFileDialog dlg(this, QStringLiteral("Select DMA device node or FIFO"),
                    QStringLiteral("/dev"));
    dlg.setFileMode(QFileDialog::AnyFile);
    dlg.setOption(QFileDialog::DontUseNativeDialog, true);
    dlg.setFilter(QDir::AllEntries | QDir::System | QDir::Hidden);
    if (dlg.exec() != QDialog::Accepted) return;

    const QStringList sel = dlg.selectedFiles();
    if (sel.isEmpty()) return;
    m_devicePath->setText(sel.first());
    emit configChanged();
    emit sourceRestartRequired();
}

sdr::SourceConfig SourcePanel::sourceConfig() const
{
    SourceConfig c;
    c.mode = mode();

    switch (c.mode) {
    case SourceMode::File:
        c.filePath       = m_filePath->text();
        c.format         = formatOf(m_fileFormat, SampleFormat::Cf32);
        c.streamChannels = m_fileChannels->value();
        c.headerBytes    = m_headerBytes->value();
        c.loopFile       = m_loop->isChecked();
        c.pacedReplay    = m_paced->isChecked();
        c.replaySpeed    = m_speed->value();
        c.blockSamples   = 16384;
        break;
    case SourceMode::Roce:
    case SourceMode::Dma:
        c.devicePath     = m_devicePath->text();
        c.format         = formatOf(m_liveFormat, SampleFormat::Cs16);
        c.streamChannels = m_liveChannels->value();
        c.channelMask    = m_channelMask;
        c.blockSamples   = m_blockSize->currentText().toInt();
        break;
    case SourceMode::Simulated:
    default:
        c.format         = SampleFormat::Cf32;
        c.streamChannels = m_simChannels->value();
        c.blockSamples   = 16384;
        break;
    }
    return c;
}

sdr::AcqConfig SourcePanel::acqConfig() const
{
    AcqConfig c;
    c.sampleRateMsps  = m_sampleRate->currentText().toDouble();
    if (c.sampleRateMsps <= 0.0) c.sampleRateMsps = 122.88;
    c.decimation      = m_decimation->value();
    c.interpolation   = m_interpolation->value();
    c.ncoFreqMHz      = m_ncoFreq->value();
    c.centerFreqGHz   = m_centerFreq->value();
    c.bufferMiB       = m_bufferSize->currentText().split(QLatin1Char(' ')).first().toInt();
    c.modulation      = static_cast<Modulation>(m_modulation->currentIndex());
    c.snrDb           = m_snr->value();
    c.signalOffsetMHz = m_txOffset->value();
    return c;
}

void SourcePanel::applyTo(sdr::Config& cfg) const
{
    cfg.src = sourceConfig();
    cfg.acq = acqConfig();
}

void SourcePanel::loadFrom(const sdr::Config& cfg)
{
    m_loading = true;

    m_mode->setCurrentIndex(cfg.src.mode == SourceMode::File ? 1
                          : (cfg.src.mode == SourceMode::Dma
                             || cfg.src.mode == SourceMode::Roce) ? 2 : 0);
    m_devicePath->setText(cfg.src.devicePath);
    m_filePath->setText(cfg.src.filePath);
    m_sampleRate->setCurrentText(QString::number(cfg.acq.sampleRateMsps, 'f', 2));
    m_decimation->setValue(cfg.acq.decimation);
    m_interpolation->setValue(cfg.acq.interpolation);
    m_ncoFreq->setValue(cfg.acq.ncoFreqMHz);
    m_centerFreq->setValue(cfg.acq.centerFreqGHz);
    m_simChannels->setValue(cfg.src.streamChannels);
    m_modulation->setCurrentIndex(static_cast<int>(cfg.acq.modulation));
    m_snr->setValue(static_cast<int>(cfg.acq.snrDb));
    m_txOffset->setValue(cfg.acq.signalOffsetMHz);

    // Format/channel/replay widgets exist per page; populate all of them so
    // the next applyTo() round-trips the config instead of resetting it to
    // the widgets' construction defaults (this bit: a --format cs16 launch
    // was silently decoded as float32).
    selectFormat(m_fileFormat, cfg.src.format);
    selectFormat(m_liveFormat, cfg.src.format);
    m_fileChannels->setValue(cfg.src.streamChannels);
    m_liveChannels->setValue(cfg.src.streamChannels);
    if (cfg.src.mode == SourceMode::Roce || cfg.src.mode == SourceMode::Dma)
        m_liveMode = cfg.src.mode;
    m_channelMask = cfg.src.channelMask;
    refreshChannelField();
    m_headerBytes->setValue(static_cast<int>(cfg.src.headerBytes));
    m_loop->setChecked(cfg.src.loopFile);
    m_paced->setChecked(cfg.src.pacedReplay);
    m_speed->setValue(cfg.src.replaySpeed);
    m_blockSize->setCurrentText(QString::number(cfg.src.blockSamples));

    syncModeStack();
    m_loading = false;
}

void SourcePanel::setRunState(sdr::RunState state)
{
    const bool live = (state == RunState::Running || state == RunState::Paused);
    // Wire-format and path changes need a restart; lock them while running so
    // the stream cannot be reinterpreted mid-capture.
    m_mode->setEnabled(!live);
    m_liveFormat->setEnabled(!live);
    m_fileFormat->setEnabled(!live);
    m_liveChannels->setEnabled(!live);
    m_fileChannels->setEnabled(!live);
    m_simChannels->setEnabled(!live);
    m_devicePath->setEnabled(!live);
    m_filePath->setEnabled(!live);
    m_fileBrowse->setEnabled(!live);
    m_deviceBrowse->setEnabled(!live);
    m_blockSize->setEnabled(!live);
    m_headerBytes->setEnabled(!live);
}

void SourcePanel::setStats(const sdr::StreamStats& stats)
{
    if (mode() != SourceMode::File) return;
    if (m_seek->isSliderDown()) return;

    const double frac = stats.filePosFrac();
    m_seek->setValue(static_cast<int>(frac * 1000.0));

    if (stats.fileTotal > 0) {
        m_seekLabel->setText(QStringLiteral("%1 / %2   (%3 %)")
                                 .arg(humanBytes(static_cast<quint64>(stats.filePos)),
                                      humanBytes(static_cast<quint64>(stats.fileTotal)))
                                 .arg(frac * 100.0, 0, 'f', 1));
    } else {
        m_seekLabel->setText(QStringLiteral("—"));
    }
}

// =========================================================== DisplayPanel

DisplayPanel::DisplayPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("displayPanel"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    // ---- Channels -------------------------------------------------------
    {
        QFormLayout* f = nullptr;
        auto* g = makeGroup(QStringLiteral("Channels"), f);
        m_activeChannel = new QComboBox;
        m_activeChannel->addItem(QStringLiteral("CH 0"));
        m_overlay = new QCheckBox(QStringLiteral("Overlay all channels"));
        f->addRow(new QLabel(QStringLiteral("Active")), m_activeChannel);
        f->addRow(m_overlay);
        root->addWidget(g);
    }

    // ---- FFT ------------------------------------------------------------
    {
        QFormLayout* f = nullptr;
        auto* g = makeGroup(QStringLiteral("FFT / spectrum"), f);

        m_fftSize = new QComboBox;
        m_fftSize->addItems({QStringLiteral("256"), QStringLiteral("512"),
                             QStringLiteral("1024"), QStringLiteral("2048"),
                             QStringLiteral("4096"), QStringLiteral("8192"),
                             QStringLiteral("16384")});
        m_fftSize->setCurrentIndex(3);

        m_window = new QComboBox;
        m_window->addItems(windowNames());
        m_window->setCurrentIndex(4);

        m_average = new QComboBox;
        m_average->addItems(averageNames());
        m_average->setCurrentIndex(1);

        m_avgCount = new QSpinBox;
        m_avgCount->setRange(2, 1000);
        m_avgCount->setValue(8);

        m_avgAlpha = new QDoubleSpinBox;
        m_avgAlpha->setRange(0.01, 1.0);
        m_avgAlpha->setSingleStep(0.05);
        m_avgAlpha->setDecimals(2);
        m_avgAlpha->setValue(0.35);

        m_refreshFps = new QSpinBox;
        m_refreshFps->setRange(1, 120);
        m_refreshFps->setValue(30);
        m_refreshFps->setSuffix(QStringLiteral(" fps"));

        m_resetAvg = new QPushButton(QStringLiteral("Reset averaging / holds"));

        f->addRow(new QLabel(QStringLiteral("FFT size")), m_fftSize);
        f->addRow(new QLabel(QStringLiteral("Window")), m_window);
        f->addRow(new QLabel(QStringLiteral("Averaging")), m_average);
        f->addRow(new QLabel(QStringLiteral("Average N")), m_avgCount);
        f->addRow(new QLabel(QStringLiteral("Exp. alpha")), m_avgAlpha);
        f->addRow(new QLabel(QStringLiteral("Refresh")), m_refreshFps);
        f->addRow(m_resetAvg);
        root->addWidget(g);
    }

    // ---- Amplitude ------------------------------------------------------
    {
        QFormLayout* f = nullptr;
        auto* g = makeGroup(QStringLiteral("Amplitude axis"), f);
        m_floorDb = new QSpinBox;
        m_floorDb->setRange(-220, -20);
        m_floorDb->setValue(-130);
        m_floorDb->setSuffix(QStringLiteral(" dBFS"));
        m_ceilDb = new QSpinBox;
        m_ceilDb->setRange(-100, 60);
        m_ceilDb->setValue(10);
        m_ceilDb->setSuffix(QStringLiteral(" dBFS"));
        f->addRow(new QLabel(QStringLiteral("Floor")), m_floorDb);
        f->addRow(new QLabel(QStringLiteral("Reference")), m_ceilDb);
        root->addWidget(g);
    }

    // ---- Peaks ----------------------------------------------------------
    {
        QFormLayout* f = nullptr;
        auto* g = makeGroup(QStringLiteral("Peak search"), f);
        m_peakSearch = new QCheckBox(QStringLiteral("Enabled"));
        m_peakSearch->setChecked(true);
        m_peakCount = new QSpinBox;
        m_peakCount->setRange(1, 20);
        m_peakCount->setValue(5);
        m_peakThresh = new QSpinBox;
        m_peakThresh->setRange(1, 80);
        m_peakThresh->setValue(10);
        m_peakThresh->setSuffix(QStringLiteral(" dB"));
        f->addRow(m_peakSearch);
        f->addRow(new QLabel(QStringLiteral("Max peaks")), m_peakCount);
        f->addRow(new QLabel(QStringLiteral("Above floor")), m_peakThresh);
        root->addWidget(g);
    }

    // ---- Rendering ------------------------------------------------------
    {
        QFormLayout* f = nullptr;
        auto* g = makeGroup(QStringLiteral("Rendering"), f);
        m_colorMap = new QComboBox;
        m_colorMap->addItems(ColorMap::names());
        m_wfRows = new QSpinBox;
        m_wfRows->setRange(64, 2048);
        m_wfRows->setSingleStep(64);
        m_wfRows->setValue(320);
        m_persistence = new QCheckBox(QStringLiteral("Constellation persistence"));
        m_persistence->setChecked(true);
        m_decay = new QDoubleSpinBox;
        m_decay->setRange(0.0, 0.99);
        m_decay->setSingleStep(0.05);
        m_decay->setDecimals(2);
        m_decay->setValue(0.90);
        f->addRow(new QLabel(QStringLiteral("Colour map")), m_colorMap);
        f->addRow(new QLabel(QStringLiteral("Waterfall rows")), m_wfRows);
        f->addRow(m_persistence);
        f->addRow(new QLabel(QStringLiteral("Decay")), m_decay);
        root->addWidget(g);
    }

    root->addStretch(1);

    // ---- wiring ---------------------------------------------------------
    const auto touched = [this] { if (!m_loading) emit configChanged(); };

    for (QComboBox* c : {m_fftSize, m_window, m_average, m_colorMap, m_activeChannel})
        connect(c, QOverload<int>::of(&QComboBox::currentIndexChanged), this, touched);
    for (QSpinBox* s : {m_avgCount, m_refreshFps, m_floorDb, m_ceilDb,
                        m_wfRows, m_peakCount, m_peakThresh})
        connect(s, QOverload<int>::of(&QSpinBox::valueChanged), this, touched);
    for (QDoubleSpinBox* s : {m_avgAlpha, m_decay})
        connect(s, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, touched);
    for (QCheckBox* c : {m_overlay, m_persistence, m_peakSearch})
        connect(c, &QCheckBox::toggled, this, touched);

    connect(m_resetAvg, &QPushButton::clicked, this, &DisplayPanel::resetAveragingRequested);
}

sdr::DisplayConfig DisplayPanel::displayConfig() const
{
    DisplayConfig c;
    c.fftSize      = m_fftSize->currentText().toInt();
    c.window       = static_cast<WindowType>(m_window->currentIndex());
    c.average      = static_cast<AverageMode>(m_average->currentIndex());
    c.averageCount = m_avgCount->value();
    c.averageAlpha = m_avgAlpha->value();
    c.refreshFps   = m_refreshFps->value();

    c.floorDb = m_floorDb->value();
    c.ceilDb  = m_ceilDb->value();
    // A user can drive the reference below the floor; clamp rather than let
    // every plot divide by a negative range.
    if (c.ceilDb <= c.floorDb) c.ceilDb = c.floorDb + 10.0;

    c.peakSearch   = m_peakSearch->isChecked();
    c.peakCount    = m_peakCount->value();
    c.peakThreshDb = m_peakThresh->value();

    c.colorMap      = m_colorMap->currentIndex();
    c.waterfallRows = m_wfRows->value();
    c.persistence   = m_persistence->isChecked();
    c.persistDecay  = m_decay->value();

    c.activeChannel = std::max(0, m_activeChannel->currentIndex());
    c.channelView   = m_overlay->isChecked() ? ChannelView::Overlay : ChannelView::Active;
    return c;
}

void DisplayPanel::loadFrom(const sdr::Config& cfg)
{
    m_loading = true;
    m_fftSize->setCurrentText(QString::number(cfg.disp.fftSize));
    m_window->setCurrentIndex(static_cast<int>(cfg.disp.window));
    m_average->setCurrentIndex(static_cast<int>(cfg.disp.average));
    m_refreshFps->setValue(cfg.disp.refreshFps);
    m_floorDb->setValue(static_cast<int>(cfg.disp.floorDb));
    m_ceilDb->setValue(static_cast<int>(cfg.disp.ceilDb));
    m_colorMap->setCurrentIndex(cfg.disp.colorMap);
    m_loading = false;
}

void DisplayPanel::setChannelCount(int n)
{
    n = std::max(1, n);
    if (m_activeChannel->count() == n) return;

    const int keep = m_activeChannel->currentIndex();
    m_loading = true;
    m_activeChannel->clear();
    for (int i = 0; i < n; ++i)
        m_activeChannel->addItem(QStringLiteral("CH %1").arg(i));
    m_activeChannel->setCurrentIndex(std::clamp(keep, 0, n - 1));
    m_loading = false;
}

// ======================================================= MeasurementPanel

MeasurementPanel::MeasurementPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("measurementPanel"));

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    static const QStringList kRows = {
        QStringLiteral("Peak level"),        QStringLiteral("Peak frequency"),
        QStringLiteral("Noise floor"),       QStringLiteral("Peak / floor"),
        QStringLiteral("SNR"),
        QStringLiteral("Occupied bandwidth"),QStringLiteral("Channel power"),
        QStringLiteral("RMS level"),         QStringLiteral("Peak amplitude"),
        QStringLiteral("PAPR"),              QStringLiteral("EVM"),
        QStringLiteral("Phase error"),       QStringLiteral("DC offset"),
        QStringLiteral("I/Q imbalance"),
        // --- converter metrics, the Xilinx RF Analyzer set ---------------
        QStringLiteral("SINAD"),             QStringLiteral("ENOB"),
        QStringLiteral("SFDR"),              QStringLiteral("THD"),
        QStringLiteral("H2"),                QStringLiteral("H3"),
        QStringLiteral("H4"),                QStringLiteral("H5"),
        QStringLiteral("H6"),
        QStringLiteral("Residual carrier")};

    m_params = new QTableWidget(kRows.size(), 1);
    m_params->setObjectName(QStringLiteral("paramTable"));
    m_params->setVerticalHeaderLabels(kRows);
    m_params->setHorizontalHeaderLabels({QStringLiteral("CH 0")});
    m_params->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_params->setSelectionMode(QAbstractItemView::SingleSelection);
    // Row height derived from font metrics, never a hardcoded pixel count.
    // The old fixed 20 px was smaller than the rendered line height once the
    // stylesheet's 12 px font and 5 px cell padding were applied, so every
    // row label was clipped along its bottom edge ("Peak frequency",
    // "Occupied bandwidth"). A fixed size also breaks outright under any
    // display-scaling factor above 1.0.
    const int rowH = tableRowHeight(m_params);
    m_params->verticalHeader()->setDefaultSectionSize(rowH);
    m_params->verticalHeader()->setMinimumSectionSize(rowH);
    m_params->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_params->setAlternatingRowColors(true);

    m_peaks = new QTableWidget(0, 4);
    m_peaks->setObjectName(QStringLiteral("peakTable"));
    m_peaks->setHorizontalHeaderLabels({QStringLiteral("#"), QStringLiteral("Frequency"),
                                        QStringLiteral("Level"), QStringLiteral("Δ peak")});
    m_peaks->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_peaks->verticalHeader()->setVisible(false);
    m_peaks->verticalHeader()->setDefaultSectionSize(rowH);
    m_peaks->verticalHeader()->setMinimumSectionSize(rowH);
    m_peaks->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_peaks->setAlternatingRowColors(true);

    auto wrap = [](const QString& title, QWidget* body) {
        auto* g = new QGroupBox(title);
        auto* v = new QVBoxLayout(g);
        v->setContentsMargins(8, 10, 8, 8);
        v->addWidget(body);
        return g;
    };

    m_params->setMinimumHeight(210);
    m_peaks->setMinimumHeight(210);
    root->addWidget(wrap(QStringLiteral("Signal parameters"), m_params), 3);
    root->addWidget(wrap(QStringLiteral("Detected peaks"), m_peaks), 2);

    rebuildColumns(1);
}

void MeasurementPanel::rebuildColumns(int channels)
{
    channels = std::max(1, channels);
    if (m_columns == channels) return;
    m_columns = channels;

    m_params->setColumnCount(channels);
    QStringList heads;
    for (int i = 0; i < channels; ++i) heads << QStringLiteral("CH %1").arg(i);
    m_params->setHorizontalHeaderLabels(heads);

    for (int r = 0; r < m_params->rowCount(); ++r)
        for (int c = 0; c < channels; ++c)
            if (!m_params->item(r, c)) {
                auto* item = new QTableWidgetItem(QStringLiteral("—"));
                item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                item->setFont(theme::monoFont(11));
                m_params->setItem(r, c, item);
            }
}

void MeasurementPanel::clear()
{
    for (int r = 0; r < m_params->rowCount(); ++r)
        for (int c = 0; c < m_params->columnCount(); ++c)
            if (auto* it = m_params->item(r, c)) it->setText(QStringLiteral("—"));
    m_peaks->setRowCount(0);
}

void MeasurementPanel::setFrame(const sdr::FrameResult& frame)
{
    rebuildColumns(static_cast<int>(frame.channels.size()));

    for (int c = 0; c < static_cast<int>(frame.channels.size()); ++c) {
        const Metrics& m = frame.channels[static_cast<std::size_t>(c)].metrics;
        const QStringList vals = {
            QStringLiteral("%1 dBFS").arg(m.peakDbfs, 0, 'f', 2),
            engFreq(m.peakFreqHz),
            QStringLiteral("%1 dBFS").arg(m.noiseFloorDbfs, 0, 'f', 2),
            QStringLiteral("%1 dB").arg(m.peakToFloorDb, 0, 'f', 2),
            // SNR, SINAD and ENOB are ratios against a carrier. With no
            // carrier they are undefined, not low: show a dash and the reason
            // rather than a number computed from noise, which reads as a
            // converter fault and is chased as one.
            m.toneValid ? QStringLiteral("%1 dB").arg(m.snrDb, 0, 'f', 2)
                        : QStringLiteral("—"),
            engFreq(m.occupiedBwHz),
            QStringLiteral("%1 dB").arg(m.channelPowerDb, 0, 'f', 2),
            QStringLiteral("%1").arg(m.rms, 0, 'f', 4),
            QStringLiteral("%1").arg(m.peakAmplitude, 0, 'f', 4),
            QStringLiteral("%1 dB").arg(m.paprDb, 0, 'f', 2),
            QStringLiteral("%1 %").arg(m.evm * 100.0, 0, 'f', 2),
            QStringLiteral("%1 °").arg(m.phaseErrDeg, 0, 'f', 2),
            QStringLiteral("%1").arg(m.dcOffset, 0, 'f', 5),
            QStringLiteral("%1 dB").arg(m.iqImbalanceDb, 0, 'f', 2),
            m.toneValid ? QStringLiteral("%1 dB").arg(m.sinadDb, 0, 'f', 2)
                        : QStringLiteral("—"),
            m.toneValid ? QStringLiteral("%1 bits").arg(m.enob, 0, 'f', 2)
                        : QStringLiteral("—"),
            QStringLiteral("%1 dBc").arg(m.sfdrDbc, 0, 'f', 1),
            QStringLiteral("%1 dBc").arg(m.thdDbc, 0, 'f', 1),
            QStringLiteral("%1 dBc").arg(m.harmonicsDbc[0], 0, 'f', 1),
            QStringLiteral("%1 dBc").arg(m.harmonicsDbc[1], 0, 'f', 1),
            QStringLiteral("%1 dBc").arg(m.harmonicsDbc[2], 0, 'f', 1),
            QStringLiteral("%1 dBc").arg(m.harmonicsDbc[3], 0, 'f', 1),
            QStringLiteral("%1 dBc").arg(m.harmonicsDbc[4], 0, 'f', 1),
            // Do not print an aliased estimate as if it were measured.
            m.residualValid ? engFreq(m.residualHz)
                            : QStringLiteral("— (beyond estimator range)")};

        // The row labels and this value list are two parallel sequences, and
        // nothing but care keeps them in step. Inserting a row in one and not
        // the other silently shifts every parameter below it — ENOB would
        // display SFDR's number and look like a converter fault. The loop
        // below would hide that by simply stopping early, so assert instead.
        Q_ASSERT_X(vals.size() == m_params->rowCount(), "MeasurementsPanel",
                   "RF parameter labels and values are out of step");
        if (vals.size() != m_params->rowCount())
            qWarning("RF parameters: %d labels but %d values — rows are mislabelled",
                     m_params->rowCount(), int(vals.size()));

        for (int r = 0; r < m_params->rowCount() && r < vals.size(); ++r)
            if (auto* it = m_params->item(r, c)) it->setText(vals[r]);
    }

    // Peak table follows the channel the plots are showing.
    const int active = std::clamp(m_active, 0,
                                  std::max(0, static_cast<int>(frame.channels.size()) - 1));
    const ChannelFrame* cf = frame.channel(active);
    if (!cf) return;

    const auto& peaks = cf->metrics.peaks;
    m_peaks->setRowCount(static_cast<int>(peaks.size()));
    const double ref = peaks.empty() ? 0.0 : peaks.front().levelDbfs;

    for (int i = 0; i < static_cast<int>(peaks.size()); ++i) {
        const Peak& p = peaks[static_cast<std::size_t>(i)];
        const QStringList cells = {
            QStringLiteral("P%1").arg(i + 1),
            engFreq(p.freqHz),
            QStringLiteral("%1 dBFS").arg(p.levelDbfs, 0, 'f', 2),
            QStringLiteral("%1 dB").arg(p.levelDbfs - ref, 0, 'f', 2)};

        for (int c = 0; c < cells.size(); ++c) {
            auto* it = m_peaks->item(i, c);
            if (!it) {
                it = new QTableWidgetItem;
                it->setFont(theme::monoFont(11));
                it->setTextAlignment(c == 0 ? (Qt::AlignLeft | Qt::AlignVCenter)
                                            : (Qt::AlignRight | Qt::AlignVCenter));
                m_peaks->setItem(i, c, it);
            }
            it->setText(cells[c]);
            if (i == 0) it->setForeground(theme::marker);
        }
    }
}

// ============================================================ StatusPanel

StatusPanel::StatusPanel(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("statusPanel"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    auto addRow = [](QFormLayout* f, const QString& label, const QString& init) {
        auto* v = new QLabel(init);
        v->setObjectName(QStringLiteral("valueLabel"));
        v->setFont(theme::monoFont(11));
        v->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        // Ignored width: a long value must not push the dock wider — the
        // label clips instead, and unbounded strings are elided by callers.
        v->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        v->setMinimumWidth(90);
        f->addRow(new QLabel(label), v);
        return v;
    };

    {
        QFormLayout* f = nullptr;
        auto* g = makeGroup(QStringLiteral("Acquisition"), f);
        m_state      = addRow(f, QStringLiteral("State"), QStringLiteral("STOPPED"));
        m_state->setObjectName(QStringLiteral("stateLabel"));
        m_source     = addRow(f, QStringLiteral("Source"), QStringLiteral("—"));
        m_throughput = addRow(f, QStringLiteral("DMA throughput"), QStringLiteral("0.0 MB/s"));
        m_sampleRate = addRow(f, QStringLiteral("Measured rate"), QStringLiteral("0.00 MSPS"));
        m_displayFps = addRow(f, QStringLiteral("Display rate"), QStringLiteral("0.0 fps"));
        m_elapsed    = addRow(f, QStringLiteral("Elapsed"), QStringLiteral("00:00:00"));

        m_buffer = new QProgressBar;
        m_buffer->setObjectName(QStringLiteral("bufferBar"));
        m_buffer->setRange(0, 100);
        m_buffer->setValue(0);
        m_buffer->setFormat(QStringLiteral("%p%"));
        m_buffer->setFixedHeight(
            std::max(14, QFontMetrics(m_buffer->font()).height() - 2));
        f->addRow(new QLabel(QStringLiteral("Buffer")), m_buffer);
        root->addWidget(g);
    }

    {
        QFormLayout* f = nullptr;
        auto* g = makeGroup(QStringLiteral("Integrity"), f);
        m_blocks   = addRow(f, QStringLiteral("Blocks in"), QStringLiteral("0"));
        m_dropped  = addRow(f, QStringLiteral("Samples dropped"), QStringLiteral("0"));
        m_skipped  = addRow(f, QStringLiteral("Display frames throttled"), QStringLiteral("0"));
        m_recorded = addRow(f, QStringLiteral("Recorded"), QStringLiteral("—"));
        root->addWidget(g);
    }

    {
        QFormLayout* f = nullptr;
        auto* g = makeGroup(QStringLiteral("Channel information"), f);
        m_chCount  = addRow(f, QStringLiteral("Channels"), QStringLiteral("—"));
        m_chRate   = addRow(f, QStringLiteral("Configured rate"), QStringLiteral("—"));
        m_chCenter = addRow(f, QStringLiteral("Centre"), QStringLiteral("—"));
        m_chSpan   = addRow(f, QStringLiteral("Span"), QStringLiteral("—"));
        m_chRbw    = addRow(f, QStringLiteral("RBW"), QStringLiteral("—"));
        m_chFormat = addRow(f, QStringLiteral("Wire format"), QStringLiteral("—"));
        root->addWidget(g);
    }

    root->addStretch(1);
}

void StatusPanel::setStats(const sdr::StreamStats& stats, double displayFps)
{
    m_state->setText(runStateName(stats.state));

    QColor c = theme::textDim;
    switch (stats.state) {
    case RunState::Running: c = theme::ok;     break;
    case RunState::Paused:  c = theme::warn;   break;
    case RunState::Error:   c = theme::danger; break;
    case RunState::Stopped: c = theme::textDim; break;
    }
    m_state->setStyleSheet(QStringLiteral("color: %1; font-weight: bold;").arg(c.name()));

    {
        const QString name = stats.sourceName.isEmpty() ? QStringLiteral("—")
                                                        : stats.sourceName;
        const QFontMetrics fm(m_source->font());
        m_source->setText(fm.elidedText(name, Qt::ElideMiddle,
                                        std::max(120, m_source->width() - 4)));
        m_source->setToolTip(name);
    }
    m_throughput->setText(QStringLiteral("%1 MB/s").arg(stats.dmaMBps, 0, 'f', 1));
    m_sampleRate->setText(QStringLiteral("%1 MSPS").arg(stats.sampleRateSps / 1e6, 0, 'f', 2));
    m_displayFps->setText(QStringLiteral("%1 fps").arg(displayFps, 0, 'f', 1));
    m_elapsed->setText(humanTime(stats.elapsedMs));

    m_buffer->setValue(static_cast<int>(std::clamp(stats.bufferPct, 0.0, 100.0)));
    // Colour the bar by depth: a backlog above ~70 % means the DSP stage is
    // no longer keeping up and drops are imminent.
    const QString barColor = stats.bufferPct > 85.0 ? theme::danger.name()
                           : stats.bufferPct > 60.0 ? theme::warn.name()
                                                    : theme::ok.name();
    m_buffer->setStyleSheet(
        QStringLiteral("QProgressBar#bufferBar::chunk { background: %1; }").arg(barColor));

    m_blocks->setText(QString::number(stats.blocksIn));
    const double dropPct = stats.dropRatePct();
    m_dropped->setText(QStringLiteral("%1  (%2%)")
                           .arg(stats.samplesDropped).arg(dropPct, 0, 'f', 2));
    m_dropped->setStyleSheet(dropPct >= 5.0
        ? QStringLiteral("color: %1;").arg(theme::danger.name())
        : (dropPct >= 1.0 ? QStringLiteral("color: %1;").arg(theme::warn.name())
                          : QString()));
    m_skipped->setText(QString::number(stats.framesSkipped));

    if (stats.recording)
        m_recorded->setText(humanBytes(stats.recordedBytes));
    else if (stats.recordedBytes > 0)
        m_recorded->setText(QStringLiteral("%1 (stopped)").arg(humanBytes(stats.recordedBytes)));
    else
        m_recorded->setText(QStringLiteral("—"));
}

void StatusPanel::setChannelInfo(const sdr::Config& cfg)
{
    m_chCount->setText(QString::number(cfg.src.streamChannels));
    m_chRate->setText(QStringLiteral("%1 MSPS").arg(cfg.acq.sampleRateMsps, 0, 'f', 2));
    m_chCenter->setText(engFreq(cfg.acq.centerFreqHz()));
    m_chSpan->setText(engFreq(cfg.acq.displayRateHz()));
    const double rbw = cfg.disp.fftSize > 0
        ? cfg.acq.displayRateHz() / cfg.disp.fftSize : 0.0;
    m_chRbw->setText(engFreq(rbw));
    m_chFormat->setText(formatName(cfg.src.format));
}

} // namespace sdr::ui
