#include "RecordingGuard.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <cmath>

namespace sdr {

RecordingGuard::RecordingGuard(QObject* parent) : QObject(parent) {}

quint64 RecordingGuard::safeCapacity(const DiskSample& d)
{
    if (!d.valid) return 0;
    // Reserve the greater of a fixed floor and a proportion of the volume, so
    // the rule behaves sensibly on both a 32 GiB stick and a 16 TiB array.
    const quint64 pct = quint64(double(d.totalBytes) * kSafetyReserveFrac);
    const quint64 reserve = std::max(kSafetyReserveBytes, pct);
    return d.freeBytes > reserve ? d.freeBytes - reserve : 0;
}

QVector<RecWarning> RecordingGuard::validateLimit(double limitMiB,
                                                  const QString& outputDir,
                                                  const DiskSample& disk)
{
    QVector<RecWarning> out;

    if (!(limitMiB > 0.0) || std::isnan(limitMiB)) {
        out.push_back({QStringLiteral("limit.nonpositive"), RecWarning::Critical,
            QStringLiteral("Invalid recording storage limit"),
            QStringLiteral("The limit is %1 MB.").arg(limitMiB, 0, 'f', 0),
            QStringLiteral("A recording cannot be bounded by a zero or negative "
                           "size, so there would be nothing preventing it from "
                           "filling the disk."),
            QStringLiteral("Enter a positive limit, for example 5000 MB.")});
        return out;   // nothing else is meaningful once the limit is invalid
    }

    const QFileInfo fi(outputDir);
    if (outputDir.isEmpty() || !fi.exists() || !fi.isDir()) {
        out.push_back({QStringLiteral("dir.missing"), RecWarning::Critical,
            QStringLiteral("Recording directory unavailable"),
            QStringLiteral("'%1' does not exist or is not a directory.").arg(outputDir),
            QStringLiteral("Recording cannot start without a valid destination, "
                           "and free space cannot be verified."),
            QStringLiteral("Choose an existing directory on a mounted volume.")});
        return out;
    }
    if (!fi.isWritable()) {
        out.push_back({QStringLiteral("dir.readonly"), RecWarning::Critical,
            QStringLiteral("Recording directory is not writable"),
            QStringLiteral("'%1' cannot be written to by this user.").arg(outputDir),
            QStringLiteral("Recording would fail on the first write."),
            QStringLiteral("Choose a writable location or adjust permissions.")});
        return out;
    }
    if (!disk.valid) {
        out.push_back({QStringLiteral("disk.unknown"), RecWarning::Warning,
            QStringLiteral("Free space could not be determined"),
            QStringLiteral("statvfs did not return usable figures for '%1'.").arg(outputDir),
            QStringLiteral("The limit cannot be checked against real capacity, "
                           "so disk exhaustion cannot be ruled out."),
            QStringLiteral("Verify the volume is mounted and readable.")});
        return out;
    }

    const quint64 requested = quint64(limitMiB * 1024.0 * 1024.0);
    const quint64 safe = safeCapacity(disk);
    if (requested > safe) {
        out.push_back({QStringLiteral("limit.exceeds_safe"), RecWarning::Critical,
            QStringLiteral("Insufficient recording storage"),
            QStringLiteral("The selected recording limit is %1, but only %2 of "
                           "safely usable space is available on %3.")
                .arg(ResourceMonitor::formatBytes(requested),
                     ResourceMonitor::formatBytes(safe), disk.path),
            QStringLiteral("Recording with this configuration may exhaust the "
                           "disk. A reserve of %1 is held back so the volume "
                           "does not reach zero free space.")
                .arg(ResourceMonitor::formatBytes(
                         std::max(kSafetyReserveBytes,
                                  quint64(double(disk.totalBytes) * kSafetyReserveFrac)))),
            QStringLiteral("Set the limit below %1, or select another storage "
                           "location.").arg(ResourceMonitor::formatBytes(safe))});
    } else if (double(requested) > double(safe) * kWarnFraction) {
        out.push_back({QStringLiteral("limit.near_safe"), RecWarning::Warning,
            QStringLiteral("Recording limit close to available capacity"),
            QStringLiteral("The limit (%1) is %2%% of the %3 safely available.")
                .arg(ResourceMonitor::formatBytes(requested))
                .arg(100.0 * double(requested) / double(safe), 0, 'f', 0)
                .arg(ResourceMonitor::formatBytes(safe)),
            QStringLiteral("Other activity on the volume during a long capture "
                           "could still exhaust it."),
            QStringLiteral("Consider a smaller limit or a dedicated volume.")});
    }
    return out;
}

void RecordingGuard::beginSession(quint64 limitBytes)
{
    m_active = true;
    m_status = RecStatus{};
    m_status.state = RecStatus::Normal;
    m_status.limitBytes = limitBytes;
    m_clock.start();
    m_lastBytes = 0;
    m_lastMs = 0;
    m_smoothedRate = 0.0;
    m_growthSamples = 0;
    m_limitSignalled = false;
    for (const auto& w : m_activeWarnings) emit warningCleared(w.first);
    m_activeWarnings.clear();
}

void RecordingGuard::endSession()
{
    m_active = false;
    m_status.state = RecStatus::Idle;
    for (const auto& w : m_activeWarnings) emit warningCleared(w.first);
    m_activeWarnings.clear();
}

RecStatus RecordingGuard::update(quint64 sessionBytes, const DiskSample& disk)
{
    if (!m_active) { m_status.state = RecStatus::Idle; return m_status; }

    m_status.sessionBytes = sessionBytes;
    m_status.remainingBytes = m_status.limitBytes > sessionBytes
                                  ? m_status.limitBytes - sessionBytes : 0;

    // ---- growth rate: delta bytes over delta time -----------------------
    const qint64 nowMs = m_clock.elapsed();
    const qint64 dtMs  = nowMs - m_lastMs;
    if (dtMs > 0 && sessionBytes >= m_lastBytes) {
        const double inst = double(sessionBytes - m_lastBytes) * 1000.0 / double(dtMs);
        // Exponential smoothing: a variable-bitrate stream otherwise makes the
        // ETA jump around uselessly between polls.
        m_smoothedRate = (m_smoothedRate <= 0.0) ? inst
                                                 : 0.7 * m_smoothedRate + 0.3 * inst;
    }
    m_lastBytes = sessionBytes;
    m_lastMs    = nowMs;
    m_status.growthBytesPerSec = m_smoothedRate;

    // An estimate needs enough SAMPLES for the smoothing to settle, plus a
    // real rate. Gating on wall-clock age alone was wrong: with a 400 ms poll
    // the rate is well established long before an arbitrary 3 s, and a slow
    // first write could also make a long-running session look unreliable.
    // A paused recording still fails the rate test, so it cannot produce an
    // "infinite time remaining".
    if (dtMs > 0) ++m_growthSamples;
    m_status.growthReliable = (m_growthSamples >= kMinGrowthSamples)
                           && (m_smoothedRate > kMinGrowthBytesPerSec);

    if (!m_status.growthReliable) {
        m_status.etaReliable = false;
        m_status.secondsRemaining = 0.0;
        m_status.etaText = (m_growthSamples < kMinGrowthSamples)
                               ? QStringLiteral("Estimating…")
                               : QStringLiteral("Insufficient data");
    } else {
        // Bound by whichever runs out first: the configured limit, or the
        // safe capacity of the volume.
        const quint64 safe = safeCapacity(disk);
        const quint64 headroom = std::min(m_status.remainingBytes,
                                          safe > 0 ? safe : m_status.remainingBytes);
        m_status.secondsRemaining = double(headroom) / m_smoothedRate;
        m_status.etaReliable = true;
        const double s = m_status.secondsRemaining;
        m_status.etaText = s >= 3600.0
            ? QStringLiteral("%1 h %2 m").arg(int(s / 3600)).arg(int(std::fmod(s, 3600) / 60))
            : s >= 60.0 ? QStringLiteral("%1 m %2 s").arg(int(s / 60)).arg(int(std::fmod(s, 60)))
            // One decimal below a minute: int() truncation displayed "0 s"
            // for anything under a second, which reads as "already over".
                        : QStringLiteral("%1 s").arg(s, 0, 'f', 1);
    }

    // ---- classification --------------------------------------------------
    const double usedFrac = m_status.limitBytes
                                ? double(sessionBytes) / double(m_status.limitBytes) : 0.0;
    const quint64 safe = safeCapacity(disk);

    if (m_status.limitBytes && sessionBytes >= m_status.limitBytes) {
        m_status.state = RecStatus::LimitReached;
        raise({QStringLiteral("rec.limit_reached"), RecWarning::Critical,
            QStringLiteral("Recording limit reached"),
            QStringLiteral("The session has written %1, reaching the configured "
                           "limit of %2.")
                .arg(ResourceMonitor::formatBytes(sessionBytes),
                     ResourceMonitor::formatBytes(m_status.limitBytes)),
            QStringLiteral("Continuing would exceed the limit you set to protect "
                           "the volume."),
            QStringLiteral("Stop the recording, or raise the limit if the volume "
                           "has room.")});
        if (!m_limitSignalled) {
            m_limitSignalled = true;
            emit limitReached(sessionBytes, m_status.limitBytes);
        }
    } else if (disk.valid && safe == 0) {
        m_status.state = RecStatus::Critical;
        raise({QStringLiteral("disk.exhausted"), RecWarning::Critical,
            QStringLiteral("Disk space critically low"),
            QStringLiteral("%1 has %2 free, at or below the safety reserve.")
                .arg(disk.path, ResourceMonitor::formatBytes(disk.freeBytes)),
            QStringLiteral("Further writing risks filling the volume completely, "
                           "which can destabilise the system."),
            QStringLiteral("Stop recording and free space on %1.").arg(disk.path)});
    } else if (usedFrac >= kCriticalFraction
               || (m_status.etaReliable && m_status.secondsRemaining < 60.0)) {
        m_status.state = RecStatus::Critical;
        raise({QStringLiteral("rec.near_limit"), RecWarning::Critical,
            QStringLiteral("Recording capacity nearly exhausted"),
            QStringLiteral("%1 of %2 written (%3%%); about %4 remaining.")
                .arg(ResourceMonitor::formatBytes(sessionBytes),
                     ResourceMonitor::formatBytes(m_status.limitBytes))
                .arg(100.0 * usedFrac, 0, 'f', 0).arg(m_status.etaText),
            QStringLiteral("The recording will stop at the limit shortly."),
            QStringLiteral("Stop when convenient, or raise the limit if the "
                           "volume has capacity.")});
    } else if (usedFrac >= kWarnFraction) {
        m_status.state = RecStatus::Warning;
        raise({QStringLiteral("rec.near_limit"), RecWarning::Warning,
            QStringLiteral("Approaching the recording limit"),
            QStringLiteral("%1 of %2 written (%3%%); about %4 remaining.")
                .arg(ResourceMonitor::formatBytes(sessionBytes),
                     ResourceMonitor::formatBytes(m_status.limitBytes))
                .arg(100.0 * usedFrac, 0, 'f', 0).arg(m_status.etaText),
            QStringLiteral("Recording will stop once the limit is reached."),
            QStringLiteral("Plan to stop, or raise the limit now.")});
    } else {
        m_status.state = RecStatus::Normal;
        clear(QStringLiteral("rec.near_limit"));
        clear(QStringLiteral("disk.exhausted"));
    }
    return m_status;
}

void RecordingGuard::raise(const RecWarning& w)
{
    // Stateful: re-emit only on first appearance or a severity change, so a
    // 1 Hz poll cannot spam the same warning.
    for (auto& e : m_activeWarnings) {
        if (e.first != w.id) continue;
        if (e.second != int(w.severity)) { e.second = int(w.severity); emit warningRaised(w); }
        return;
    }
    m_activeWarnings.push_back({w.id, int(w.severity)});
    emit warningRaised(w);
}

void RecordingGuard::clear(const QString& id)
{
    for (int i = 0; i < m_activeWarnings.size(); ++i) {
        if (m_activeWarnings[i].first != id) continue;
        m_activeWarnings.remove(i);
        emit warningCleared(id);
        return;
    }
}

} // namespace sdr
