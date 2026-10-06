#pragma once
// ---------------------------------------------------------------------------
// BackendModeDialog — the one popup this integration adds: pick C2H or
// H2C_C2H before live acquisition begins.
//
// Deliberately minimal. It reuses the existing theme object names (no new
// styling, no palette of its own) so it inherits the finished look of the
// rest of the application rather than introducing a second visual language.
// ---------------------------------------------------------------------------

#include "../core/BackendLauncher.h"

#include <QButtonGroup>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QLabel>
#include <QRadioButton>
#include <QVBoxLayout>

namespace sdr::ui {

class BackendModeDialog : public QDialog
{
    Q_OBJECT
public:
    explicit BackendModeDialog(sdr::BackendMode current = sdr::BackendMode::C2H,
                               QWidget* parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle(tr("Start Live Acquisition"));
        setObjectName(QStringLiteral("channelDialog"));   // reuse existing styling
        setModal(true);

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(18, 18, 18, 18);
        root->setSpacing(12);

        auto* heading = new QLabel(tr("Select the acquisition pipeline to launch."));
        heading->setObjectName(QStringLiteral("hintLabel"));
        heading->setWordWrap(true);
        root->addWidget(heading);

        m_c2h = new QRadioButton(tr("C2H  —  capture from card"));
        m_c2h->setToolTip(tr("Runs the C2H capture application; the workstation "
                             "reads its FIFO."));
        m_both = new QRadioButton(tr("H2C_C2H  —  playback and capture (loopback)"));
        m_both->setToolTip(tr("Runs H2C playback into the card and C2H capture "
                              "back out of it."));

        auto* group = new QButtonGroup(this);
        group->addButton(m_c2h);
        group->addButton(m_both);
        (current == sdr::BackendMode::H2C_C2H ? m_both : m_c2h)->setChecked(true);

        root->addWidget(m_c2h);
        root->addWidget(m_both);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText(tr("Start"));
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        root->addWidget(buttons);

        adjustSize();
        setMinimumWidth(sizeHint().width());
    }

    sdr::BackendMode mode() const
    {
        return m_both->isChecked() ? sdr::BackendMode::H2C_C2H
                                   : sdr::BackendMode::C2H;
    }

private:
    QRadioButton* m_c2h  = nullptr;
    QRadioButton* m_both = nullptr;
};

} // namespace sdr::ui
