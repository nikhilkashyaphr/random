#pragma once
// ---------------------------------------------------------------------------
// ChannelDialog — modal selector for any combination of channels 1..16.
//
// Layout is entirely layout-managed (no fixed geometry, no hardcoded pixel
// sizes) so it renders correctly at any font size or display-scaling factor.
// The grid column count adapts to the channel count, and every cell is sized
// from font metrics rather than a literal width, which is what keeps the
// labels from being clipped on high-DPI screens.
// ---------------------------------------------------------------------------

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace sdr::ui {

class ChannelDialog : public QDialog
{
    Q_OBJECT
public:
    /// `available` is how many channels the wire actually carries (1..16);
    /// `mask` is the current selection (bit c = channel index c).
    ChannelDialog(int available, quint32 mask, QWidget* parent = nullptr)
        : QDialog(parent), m_available(std::clamp(available, 1, 16))
    {
        setWindowTitle(tr("Select Channels"));
        setObjectName(QStringLiteral("channelDialog"));
        setModal(true);

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(16, 16, 16, 16);
        root->setSpacing(12);

        auto* heading = new QLabel(tr("Choose which channels to acquire and display."));
        heading->setObjectName(QStringLiteral("hintLabel"));
        heading->setWordWrap(true);
        root->addWidget(heading);

        auto* box = new QGroupBox(tr("Channels"));
        auto* grid = new QGridLayout(box);
        grid->setContentsMargins(12, 14, 12, 12);
        grid->setHorizontalSpacing(18);
        grid->setVerticalSpacing(8);

        // Four columns keeps 16 channels in a square block that fits without
        // a scroll area at any reasonable font size.
        constexpr int kCols = 4;
        for (int c = 0; c < m_available; ++c) {
            auto* cb = new QCheckBox(tr("Channel %1").arg(c + 1));
            cb->setChecked(mask == 0u || (mask & (1u << c)));
            // Minimum width from the widest label the grid will hold, so no
            // cell can clip its text when the font is scaled up.
            const QFontMetrics fm(cb->font());
            cb->setMinimumWidth(fm.horizontalAdvance(tr("Channel 16")) + 34);
            connect(cb, &QCheckBox::toggled, this, &ChannelDialog::refreshSummary);
            m_boxes[c] = cb;
            grid->addWidget(cb, c / kCols, c % kCols);
        }
        for (int col = 0; col < kCols; ++col) grid->setColumnStretch(col, 1);
        root->addWidget(box);

        auto* quick = new QHBoxLayout;
        auto* selAll = new QPushButton(tr("Select All"));
        auto* clrAll = new QPushButton(tr("Clear All"));
        connect(selAll, &QPushButton::clicked, this, [this] { setAll(true); });
        connect(clrAll, &QPushButton::clicked, this, [this] { setAll(false); });
        quick->addWidget(selAll);
        quick->addWidget(clrAll);
        quick->addStretch(1);
        m_summary = new QLabel;
        m_summary->setObjectName(QStringLiteral("valueLabel"));
        quick->addWidget(m_summary);
        root->addLayout(quick);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        m_okButton = buttons->button(QDialogButtonBox::Ok);
        root->addWidget(buttons);

        refreshSummary();
        // Size to the layout's own idea of what fits, rather than a fixed
        // geometry that would clip once the font grows.
        adjustSize();
        setMinimumSize(sizeHint());
    }

    /// Selection bitmask. Never returns 0 from an accepted dialog: OK is
    /// disabled while nothing is ticked, because an empty acquisition is
    /// always a mistake rather than an intent.
    quint32 mask() const
    {
        quint32 m = 0;
        for (int c = 0; c < m_available; ++c)
            if (m_boxes[c] && m_boxes[c]->isChecked()) m |= (1u << c);
        return m;
    }

    /// Compact label for the main window, e.g. "1,2,5,8" or "All (16)".
    static QString label(quint32 mask, int available)
    {
        available = std::clamp(available, 1, 16);
        QStringList on;
        for (int c = 0; c < available; ++c)
            if (mask == 0u || (mask & (1u << c))) on << QString::number(c + 1);
        if (on.size() == available) return tr("All (%1)").arg(available);
        if (on.isEmpty())           return tr("None");
        return on.join(QLatin1Char(','));
    }

private slots:
    void refreshSummary()
    {
        const int n = static_cast<int>(qPopulationCount(mask()));
        m_summary->setText(tr("%1 of %2 selected").arg(n).arg(m_available));
        if (m_okButton) m_okButton->setEnabled(n > 0);
    }

private:
    void setAll(bool on)
    {
        for (int c = 0; c < m_available; ++c)
            if (m_boxes[c]) m_boxes[c]->setChecked(on);
    }

    int          m_available = 1;
    QCheckBox*   m_boxes[16] {};
    QLabel*      m_summary  = nullptr;
    QPushButton* m_okButton = nullptr;
};

} // namespace sdr::ui
