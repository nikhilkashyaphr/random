#include "PcieDiscoveryDialog.h"
#include "../core/DriverManager.h"
#include "Theme.h"

#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace sdr::ui {

using probe::Item;

PcieDiscoveryDialog::PcieDiscoveryDialog(const QString& devicePath,
                                         QWidget* parent)
    : QDialog(parent), m_devicePath(devicePath)
{
    setObjectName(QStringLiteral("configDialog"));
    setWindowTitle(QStringLiteral("PCIe Link Discovery"));
    setModal(true);
    setMinimumWidth(520);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(26, 22, 26, 20);
    root->setSpacing(10);

    auto* head = new QLabel(QStringLiteral("PCIe INITIALIZATION"));
    head->setObjectName(QStringLiteral("dialogHeading"));
    auto* sub = new QLabel(QStringLiteral(
        "Validating the hardware and driver stack before acquisition"));
    sub->setObjectName(QStringLiteral("dialogSub"));
    root->addWidget(head);
    root->addWidget(sub);

    m_grid = new QGridLayout;
    m_grid->setHorizontalSpacing(18);
    m_grid->setVerticalSpacing(7);
    m_grid->setColumnStretch(1, 1);
    root->addLayout(m_grid);
    root->addStretch(1);

    m_summary = new QLabel;
    m_summary->setObjectName(QStringLiteral("summaryLabel"));
    m_summary->setWordWrap(true);
    m_summary->setText(QStringLiteral("Probing…"));
    root->addWidget(m_summary);

    auto* buttons = new QHBoxLayout;
    m_fixBtn = new QPushButton(QStringLiteral("Build && load driver"));
    m_fixBtn->hide();
    connect(m_fixBtn, &QPushButton::clicked, this, &PcieDiscoveryDialog::onFixDriver);
    m_closeBtn = new QPushButton(QStringLiteral("Close"));
    m_closeBtn->hide();
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addStretch(1);
    buttons->addWidget(m_fixBtn);
    buttons->addWidget(m_closeBtn);
    root->addLayout(buttons);

    // The probe itself is fast sysfs reads; the staged reveal is purely so
    // the operator can see what was checked. Real validation already ran.
    m_report = probe::pciePreflight(m_devicePath);
    for (const Item& it : m_report.items)
        addRow(it, false);

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &PcieDiscoveryDialog::revealNext);
    m_timer->start(std::max(60, 1100 / std::max(1, int(m_report.items.size()))));
}

void PcieDiscoveryDialog::addRow(const Item& item, bool visible)
{
    const int row = m_grid->rowCount();

    auto* name = new QLabel(item.label);
    auto* value = new QLabel(item.value);
    value->setObjectName(QStringLiteral("valueLabel"));
    value->setFont(theme::monoFont(11));

    QColor c = theme::textDim;
    switch (item.state) {
    case Item::Ok:   c = theme::ok;     break;
    case Item::Warn: c = theme::warn;   break;
    case Item::Fail: c = theme::danger; break;
    case Item::Info: c = theme::text;   break;
    }
    value->setStyleSheet(QStringLiteral("color:%1;").arg(c.name()));

    name->setVisible(visible);
    value->setVisible(visible);
    m_grid->addWidget(name, row, 0);
    m_grid->addWidget(value, row, 1, Qt::AlignRight);
    m_values.push_back(name);
    m_values.push_back(value);
}

void PcieDiscoveryDialog::revealNext()
{
    if (m_shown * 2 < m_values.size()) {
        m_values[m_shown * 2]->show();
        m_values[m_shown * 2 + 1]->show();
        ++m_shown;
        return;
    }
    m_timer->stop();
    finish(m_report.ok);
}

void PcieDiscoveryDialog::finish(bool ok)
{
    if (ok) {
        m_passed = true;
        m_summary->setText(QStringLiteral("All checks passed — starting acquisition"));
        // A short beat so the final state is readable before the transition.
        QTimer::singleShot(450, this, &QDialog::accept);
        return;
    }

    m_summary->setText(m_report.failureSummary);
    m_closeBtn->show();
    // Offer the integrated path only when it could actually help: the module
    // is missing and either a prebuilt iwfg.ko or the driver sources are
    // present. The label reflects what the click will really do, so the
    // operator is not told "build" when the module is already compiled.
    if (!m_report.driver.moduleLoaded && !DriverManager::driversDir().isEmpty()) {
        const bool prebuilt = !DriverManager::compiledModulePath().isEmpty();
        m_fixBtn->setText(prebuilt ? QStringLiteral("Load driver")
                                   : QStringLiteral("Build && load driver"));
        m_fixBtn->show();
    }
}

void PcieDiscoveryDialog::onFixDriver()
{
    m_fixBtn->setEnabled(false);
    m_summary->setText(QStringLiteral("Building and loading the driver…"));

    DriverManager mgr;
    QVector<DriverManager::StepResult> log;
    const bool ok = mgr.ensureDriver(m_devicePath, log);
    if (ok && mgr.loadedByUs()) m_driverLoadedHere = true;

    QString text;
    for (const auto& step : log)
        text += QStringLiteral("%1 %2 — %3\n")
                    .arg(step.ok ? QStringLiteral("[ok]") : QStringLiteral("[FAILED]"),
                         step.title, step.detail);

    // Show the full transcript regardless of outcome. A silent success is
    // fine, but a silent failure is exactly what leaves an operator dropping
    // to a terminal to run make by hand — which is the situation this whole
    // path exists to prevent.
    if (ok) {
        m_report = probe::pciePreflight(m_devicePath);
        m_summary->setText(QStringLiteral("Driver loaded — all checks passed"));
        m_passed = true;
        QTimer::singleShot(900, this, &QDialog::accept);
    } else {
        m_fixBtn->setEnabled(true);
        m_summary->setText(QStringLiteral(
            "Driver setup did not complete — see the log. You can also run "
            "`make && sudo insmod drivers/iwfg.ko` manually and reopen."));
    }
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Driver setup log"));
    box.setIcon(ok ? QMessageBox::Information : QMessageBox::Warning);
    box.setText(ok ? QStringLiteral("Driver loaded successfully.")
                   : QStringLiteral("Driver setup did not complete."));
    box.setDetailedText(text);          // full make/insmod transcript
    box.exec();
}

} // namespace sdr::ui
