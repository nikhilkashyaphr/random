#include "ResourceBar.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QFontMetrics>
#include <QLabel>

namespace sdr::ui {

namespace {
QLabel* mk(const QString& tip)
{
    auto* l = new QLabel;
    l->setObjectName(QStringLiteral("resMetric"));
    l->setToolTip(tip);
    // The pill styling adds horizontal padding; without an explicit minimum
    // the label kept its unpadded size hint and clipped its own text.
    l->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    l->setMinimumWidth(0);
    return l;
}
} // namespace

ResourceBar::ResourceBar(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("resourceBar"));
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(0, 0, 8, 0);
    h->setSpacing(12);

    m_cpu  = mk(tr("CPU utilisation across all logical processors, from "
                   "/proc/stat deltas."));
    m_ram  = mk(tr("Physical memory in use = MemTotal − MemAvailable."));
    m_gpu  = mk(tr("GPU utilisation reported by the NVIDIA driver."));
    m_disk = mk(tr("Storage on the recording volume."));
    m_disk->hide();

    h->addWidget(m_cpu);
    h->addWidget(m_ram);
    h->addWidget(m_gpu);
    h->addWidget(m_disk);
}

void ResourceBar::setMonitor(sdr::ResourceMonitor* m)
{
    m_mon = m;
    if (m_mon) connect(m_mon, &sdr::ResourceMonitor::updated,
                       this, &ResourceBar::refresh);
    refresh();
}

void ResourceBar::setRecording(bool on)
{
    m_recording = on;
    // While recording, storage is what matters; the CPU/RAM/GPU trio would
    // otherwise crowd the same strip and bury it.
    m_cpu->setVisible(!on);
    m_ram->setVisible(!on);
    m_gpu->setVisible(!on);
    m_disk->setVisible(on);
    refresh();
}

void ResourceBar::setRecordingStatus(const sdr::RecStatus& st)
{
    m_rec = st;
    refresh();
}

void ResourceBar::applyState(int severity)
{
    // Retained for the recording view, which is a single combined state.
    paint(m_disk, severity == 2 ? sdr::Level::Critical
                : severity == 1 ? sdr::Level::Warning : sdr::Level::Normal);
}

void ResourceBar::paint(QLabel* l, sdr::Level lv)
{
    if (!l) return;
    // Existing palette only (Theme.h). Normal is the accent-adjacent 'ok'
    // green rather than the previous dim grey: the bar was too faint to read
    // at a glance, which was the point of adding it.
    QColor fg;
    QColor bg(0, 0, 0, 0);
    switch (lv) {
    case sdr::Level::Critical: fg = theme::danger; bg = QColor(248, 113, 113, 38); break;
    case sdr::Level::Warning:  fg = theme::warn;   bg = QColor(251, 191,  36, 34); break;
    default:                   fg = theme::ok;     bg = QColor( 51, 209, 122, 26); break;
    }
    l->setStyleSheet(QStringLiteral(
        "color:%1; background:rgba(%2,%3,%4,%5); border:1px solid rgba(%2,%3,%4,110);"
        " border-radius:4px; padding:1px 7px; font-weight:bold;")
        .arg(fg.name()).arg(bg.red()).arg(bg.green()).arg(bg.blue()).arg(bg.alpha()));
}

void ResourceBar::refresh()
{
    if (!m_mon) return;

    if (!m_recording) {
        const auto c = m_mon->cpu();
        m_cpu->setText(c.valid ? tr("CPU %1%").arg(c.usagePct, 0, 'f', 0)
                               : tr("CPU n/a"));

        const auto r = m_mon->ram();
        m_ram->setText(r.valid
            ? tr("RAM %1%  %2/%3").arg(r.usedPct, 0, 'f', 0)
                  .arg(sdr::ResourceMonitor::formatBytes(r.usedBytes, 1),
                       sdr::ResourceMonitor::formatBytes(r.totalBytes, 1))
            : tr("RAM n/a"));

        const auto g = m_mon->gpus();
        if (g.isEmpty()) {
            // No telemetry: say so. Never infer a figure from CPU activity.
            m_gpu->setText(tr("GPU n/a"));
            m_gpu->setToolTip(tr("No NVIDIA GPU telemetry available on this system."));
        } else {
            const auto& p = g.first();
            m_gpu->setText(g.size() == 1
                ? tr("GPU %1%").arg(p.utilPct, 0, 'f', 0)
                : tr("GPU0 %1%  (+%2)").arg(p.utilPct, 0, 'f', 0).arg(g.size() - 1));
            QString tip;
            for (const auto& x : g)
                tip += tr("[%1] %2 — %3%%, memory %4 / %5\n").arg(x.index).arg(x.name)
                           .arg(x.utilPct, 0, 'f', 0)
                           .arg(sdr::ResourceMonitor::formatBytes(x.memUsedBytes),
                                sdr::ResourceMonitor::formatBytes(x.memTotalBytes));
            m_gpu->setToolTip(tip.trimmed());
        }

        // Each metric is coloured by ITS OWN level, so a CPU spike does not
        // recolour memory and vice versa. Levels come from the monitor, which
        // applies the documented thresholds with hysteresis.
        for (QLabel* l : {m_cpu, m_ram, m_gpu})
            l->setMinimumWidth(QFontMetrics(l->font()).horizontalAdvance(l->text()) + 20);

        paint(m_cpu, c.valid ? m_mon->cpuLevel() : sdr::Level::Normal);
        paint(m_ram, r.valid ? m_mon->ramLevel() : sdr::Level::Normal);
        paint(m_gpu, g.isEmpty() ? sdr::Level::Normal : m_mon->gpuLevel());
        if (!c.valid) m_cpu->setStyleSheet(QStringLiteral("color:%1;").arg(theme::textDim.name()));
        if (!r.valid) m_ram->setStyleSheet(QStringLiteral("color:%1;").arg(theme::textDim.name()));
        if (g.isEmpty()) m_gpu->setStyleSheet(QStringLiteral("color:%1;").arg(theme::textDim.name()));
        return;
    }

    // ---- recording presentation -----------------------------------------
    const auto d = m_mon->disk();
    const quint64 safe = sdr::RecordingGuard::safeCapacity(d);
    m_disk->setText(tr("REC %1 / %2   ·   %3 free   ·   %4/s   ·   %5")
        .arg(sdr::ResourceMonitor::formatBytes(m_rec.sessionBytes, 1),
             m_rec.limitBytes ? sdr::ResourceMonitor::formatBytes(m_rec.limitBytes, 1)
                              : tr("no limit"),
             d.valid ? sdr::ResourceMonitor::formatBytes(d.freeBytes, 1) : tr("n/a"),
             m_rec.growthReliable
                 ? sdr::ResourceMonitor::formatBytes(quint64(m_rec.growthBytesPerSec), 1)
                 : tr("—"),
             m_rec.etaText));
    m_disk->setToolTip(tr("Recording to %1\nSafe capacity %2 (free minus reserve)\n"
                          "Session %3 of %4")
        .arg(d.valid ? d.path : tr("(unknown)"),
             sdr::ResourceMonitor::formatBytes(safe),
             sdr::ResourceMonitor::formatBytes(m_rec.sessionBytes),
             m_rec.limitBytes ? sdr::ResourceMonitor::formatBytes(m_rec.limitBytes)
                              : tr("no limit")));

    applyState(m_rec.state == sdr::RecStatus::LimitReached
                   || m_rec.state == sdr::RecStatus::Critical ? 2
               : m_rec.state == sdr::RecStatus::Warning ? 1 : 0);
}

} // namespace sdr::ui
