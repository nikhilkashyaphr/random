#include "ResourceMonitor.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

#ifdef Q_OS_UNIX
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace sdr {

namespace {

/// nvidia-smi is a process spawn; polling it every second would cost more
/// than everything else combined. Every 5th tick is ample for a utilisation
/// read-out and keeps the monitor's own footprint negligible.
constexpr int kGpuEveryNTicks = 5;

QStringList readLines(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
}

} // namespace

ResourceMonitor::ResourceMonitor(QObject* parent) : QObject(parent)
{
    m_diskPath = QDir::homePath();
}

void ResourceMonitor::start(int intervalMs)
{
    if (!m_timer) {
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, &ResourceMonitor::poll);
    }
    // Guard the interval: a caller passing 0 would spin the event loop.
    m_timer->start(std::max(250, intervalMs));
    poll();
}

void ResourceMonitor::stop() { if (m_timer) m_timer->stop(); }

void ResourceMonitor::setDiskPath(const QString& dir)
{
    // Measure the directory itself when it exists, otherwise the nearest
    // existing ancestor — a path that has not been created yet still lives on
    // a filesystem whose free space is the meaningful number.
    QString p = dir;
    while (!p.isEmpty() && !QFileInfo::exists(p)) {
        const QString up = QFileInfo(p).absolutePath();
        if (up == p) break;
        p = up;
    }
    m_diskPath = p.isEmpty() ? QDir::rootPath() : p;
    readDisk();
}

// ------------------------------------------------------------------- CPU

bool ResourceMonitor::readCpu()
{
    const QStringList lines = readLines(QStringLiteral("/proc/stat"));
    if (lines.isEmpty()) {
        m_cpu.valid = false;
        m_cpu.detail = QStringLiteral("/proc/stat unavailable");
        return false;
    }

    int cpus = 0;
    for (const QString& l : lines)
        if (l.startsWith(QLatin1String("cpu")) && l.size() > 3 && l[3].isDigit()) ++cpus;
    m_cpu.logicalCpus = cpus;

    for (const QString& l : lines) {
        if (!l.startsWith(QLatin1String("cpu "))) continue;
        const QStringList f = l.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        // user nice system idle iowait irq softirq steal guest guest_nice
        if (f.size() < 8) break;

        quint64 total = 0;
        for (int i = 1; i < f.size(); ++i) total += f[i].toULongLong();
        const quint64 idleAll = f[4].toULongLong() + f[5].toULongLong(); // idle+iowait

        if (!m_havePrev) {
            m_prevTotal = total; m_prevIdle = idleAll; m_havePrev = true;
            m_cpu.valid = false;
            m_cpu.detail = QStringLiteral("priming");
            return false;   // first sample establishes the baseline only
        }

        const quint64 dTotal = total   - m_prevTotal;
        const quint64 dIdle  = idleAll - m_prevIdle;
        m_prevTotal = total; m_prevIdle = idleAll;

        if (dTotal == 0) {          // division-by-zero guard: no jiffies elapsed
            m_cpu.detail = QStringLiteral("no delta");
            return m_cpu.valid;     // keep the previous reading rather than 0
        }
        // dIdle can exceed dTotal only if the counters wrapped; clamp instead
        // of producing a negative percentage.
        const double busy = double(dTotal - std::min(dIdle, dTotal));
        m_cpu.usagePct = std::clamp(100.0 * busy / double(dTotal), 0.0, 100.0);
        m_cpu.valid = true;
        m_cpu.detail.clear();
        return true;
    }
    m_cpu.valid = false;
    return false;
}

// ------------------------------------------------------------------- RAM

bool ResourceMonitor::readRam()
{
    const QStringList lines = readLines(QStringLiteral("/proc/meminfo"));
    if (lines.isEmpty()) {
        m_ram.valid = false;
        m_ram.detail = QStringLiteral("/proc/meminfo unavailable");
        return false;
    }
    quint64 total = 0, avail = 0;
    bool haveAvail = false;
    for (const QString& l : lines) {
        const QStringList f = l.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (f.size() < 2) continue;
        if (f[0] == QLatin1String("MemTotal:"))      total = f[1].toULongLong() * 1024ULL;
        else if (f[0] == QLatin1String("MemAvailable:")) {
            avail = f[1].toULongLong() * 1024ULL; haveAvail = true;
        }
    }
    if (total == 0 || !haveAvail) {
        m_ram.valid = false;
        m_ram.detail = QStringLiteral("MemTotal/MemAvailable missing");
        return false;
    }
    // Clamp so Used + Available == Total always holds and the percentage
    // always corresponds to the displayed byte values.
    avail = std::min(avail, total);
    m_ram.totalBytes     = total;
    m_ram.availableBytes = avail;
    m_ram.usedBytes      = total - avail;
    m_ram.usedPct        = 100.0 * double(m_ram.usedBytes) / double(total);
    m_ram.valid = true;
    m_ram.detail.clear();
    return true;
}

// ------------------------------------------------------------------ DISK

bool ResourceMonitor::readDisk()
{
#ifndef Q_OS_UNIX
    m_disk.valid = false;
    m_disk.detail = QStringLiteral("statvfs unavailable on this platform");
    return false;
#else
    m_disk.path = m_diskPath;
    struct statvfs vfs {};
    if (::statvfs(m_diskPath.toLocal8Bit().constData(), &vfs) != 0) {
        m_disk.valid = false;
        m_disk.detail = QStringLiteral("statvfs failed for %1").arg(m_diskPath);
        return false;
    }
    const quint64 frsize = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    m_disk.totalBytes = quint64(vfs.f_blocks) * frsize;
    // f_bavail, not f_bfree: the difference is the root reserve, which a
    // recording process running as a normal user cannot write into.
    m_disk.freeBytes  = quint64(vfs.f_bavail) * frsize;
    m_disk.usedBytes  = m_disk.totalBytes > m_disk.freeBytes
                            ? m_disk.totalBytes - m_disk.freeBytes : 0;
    m_disk.usedPct    = m_disk.totalBytes
                            ? 100.0 * double(m_disk.usedBytes) / double(m_disk.totalBytes)
                            : 0.0;
    m_disk.valid = m_disk.totalBytes > 0;
    m_disk.detail.clear();
    return m_disk.valid;
#endif
}

// ------------------------------------------------------------------- GPU

void ResourceMonitor::readGpus()
{
    if (m_gpuProc) {
        // Previous query still running. A wedged nvidia-smi is killed; the
        // object is reaped when it actually exits, never destroyed running.
        if (m_gpuClock.elapsed() > 5000) m_gpuProc->kill();
        return;
    }
    if (m_gpuToolState == 0)
        m_gpuToolState = QStandardPaths::findExecutable(QStringLiteral("nvidia-smi")).isEmpty() ? -1 : 1;
    if (m_gpuToolState < 0) {
        // No driver / not installed. Report unavailable — never synthesise a
        // number from unrelated activity.
        m_gpus.clear();
        return;
    }

    auto* p = new QProcess(this);
    m_gpuProc = p;
    m_gpuClock.start();
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, p](int, QProcess::ExitStatus st) {
        if (m_gpuProc == p) m_gpuProc = nullptr;
        p->deleteLater();
        if (st != QProcess::NormalExit) return;
        const QString out = QString::fromLocal8Bit(p->readAllStandardOutput());
        QVector<GpuSample> found;
        for (const QString& line : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const QStringList f = line.split(QLatin1Char(','));
            if (f.size() < 5) continue;
            GpuSample g;
            g.index         = f[0].trimmed().toInt();
            g.name          = f[1].trimmed();
            g.utilPct       = f[2].trimmed().toDouble();
            g.memUsedBytes  = quint64(f[3].trimmed().toDouble() * 1024.0 * 1024.0);
            g.memTotalBytes = quint64(f[4].trimmed().toDouble() * 1024.0 * 1024.0);
            g.valid = true;
            found.push_back(g);
        }
        m_gpus = found;
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        if (m_gpuProc == p) m_gpuProc = nullptr;
        m_gpus.clear();
        p->deleteLater();
    });
    p->start(QStringLiteral("nvidia-smi"),
             {QStringLiteral("--query-gpu=index,name,utilization.gpu,"
                             "memory.used,memory.total"),
              QStringLiteral("--format=csv,noheader,nounits")});
}

ResourceMonitor::~ResourceMonitor()
{
    if (m_gpuProc && m_gpuProc->state() != QProcess::NotRunning) {
        m_gpuProc->disconnect(this);
        m_gpuProc->kill();
        if (!m_gpuProc->waitForFinished(300))
            m_gpuProc->setParent(nullptr);   // leaked rather than block the exit
    }
}

Level ResourceMonitor::classify(double pct, double warn, double crit,
                                Level current) const
{
    // Rising edges use the plain threshold; falling edges must clear it by
    // `releaseMargin` first. This is what stops a metric sitting on the
    // boundary from flapping and emitting an alert every poll.
    const double m = m_thr.releaseMargin;
    switch (current) {
    case Level::Critical:
        if (pct < crit - m) return (pct >= warn) ? Level::Warning : Level::Normal;
        return Level::Critical;
    case Level::Warning:
        if (pct >= crit) return Level::Critical;
        if (pct < warn - m) return Level::Normal;
        return Level::Warning;
    case Level::Normal:
    default:
        if (pct >= crit) return Level::Critical;
        if (pct >= warn) return Level::Warning;
        return Level::Normal;
    }
}

void ResourceMonitor::evaluate()
{
    auto fire = [this](const QString& id, Level was, Level now,
                       const QString& title, const QString& detail,
                       const QString& fix) {
        if (now == was) return;               // transitions only
        if (now == Level::Normal) {
            emit alert({id, now, QStringLiteral("%1 back to normal").arg(title),
                        detail, QString()});
        } else {
            emit alert({id, now, title, detail, fix});
        }
    };

    if (m_cpu.valid) {
        const Level was = m_lvlCpu;
        m_lvlCpu = classify(m_cpu.usagePct, m_thr.cpuWarn, m_thr.cpuCrit, was);
        fire(QStringLiteral("cpu"), was, m_lvlCpu, QStringLiteral("High CPU load"),
             QStringLiteral("CPU utilisation is %1% across %2 logical processors "
                            "(threshold %3%).")
                 .arg(m_cpu.usagePct, 0, 'f', 0).arg(m_cpu.logicalCpus)
                 .arg(m_lvlCpu == Level::Critical ? m_thr.cpuCrit : m_thr.cpuWarn, 0, 'f', 0),
             QStringLiteral("Reduce FFT size or refresh rate, or select fewer "
                            "channels, if acquisition is affected."));
    }
    if (m_ram.valid) {
        const Level was = m_lvlRam;
        m_lvlRam = classify(m_ram.usedPct, m_thr.ramWarn, m_thr.ramCrit, was);
        fire(QStringLiteral("ram"), was, m_lvlRam, QStringLiteral("High memory use"),
             QStringLiteral("%1 of %2 physical memory in use (%3%).")
                 .arg(formatBytes(m_ram.usedBytes), formatBytes(m_ram.totalBytes))
                 .arg(m_ram.usedPct, 0, 'f', 0),
             QStringLiteral("Reduce the DMA buffer or waterfall history, or close "
                            "other applications."));
    }
    if (!m_gpus.isEmpty()) {
        const Level was = m_lvlGpu;
        double worst = 0.0;
        for (const auto& g : m_gpus) worst = std::max(worst, g.utilPct);
        m_lvlGpu = classify(worst, m_thr.gpuWarn, m_thr.gpuCrit, was);
        fire(QStringLiteral("gpu"), was, m_lvlGpu, QStringLiteral("High GPU load"),
             QStringLiteral("Peak GPU utilisation across %1 adapter(s) is %2%.")
                 .arg(m_gpus.size()).arg(worst, 0, 'f', 0),
             QStringLiteral("Move processing to CPU, or reduce the display rate."));
    } else {
        m_lvlGpu = Level::Normal;      // no telemetry is not an alarm
    }
    if (m_disk.valid) {
        const Level was = m_lvlDisk;
        m_lvlDisk = classify(m_disk.usedPct, m_thr.diskWarn, m_thr.diskCrit, was);
        fire(QStringLiteral("disk"), was, m_lvlDisk, QStringLiteral("Low disk space"),
             QStringLiteral("%1 has %2 free of %3 (%4% used).")
                 .arg(m_disk.path, formatBytes(m_disk.freeBytes),
                      formatBytes(m_disk.totalBytes))
                 .arg(m_disk.usedPct, 0, 'f', 0),
             QStringLiteral("Free space on %1 before recording.").arg(m_disk.path));
    }
}

void ResourceMonitor::poll()
{
    readCpu();
    readRam();
    readDisk();
    if (m_gpuTickCounter-- <= 0) {
        m_gpuTickCounter = kGpuEveryNTicks - 1;
        readGpus();
    }
    evaluate();
    emit updated();
}

// -------------------------------------------------------------- processes

QVector<ProcessSample> ResourceMonitor::topProcesses(int limit)
{
    QVector<ProcessSample> out;
#ifdef Q_OS_UNIX
    const long pageSize = ::sysconf(_SC_PAGESIZE);
    QDir proc(QStringLiteral("/proc"));
    for (const QString& e : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool isPid = false;
        const int pid = e.toInt(&isPid);
        if (!isPid) continue;

        QFile f(QStringLiteral("/proc/%1/statm").arg(pid));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QStringList m = QString::fromUtf8(f.readLine())
                                  .split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (m.size() < 2) continue;

        ProcessSample s;
        s.pid = pid;
        // statm: size (virtual) and resident, both in PAGES. These are
        // distinct concepts and are reported separately, never summed or
        // interchanged.
        s.vmBytes  = m[0].toULongLong() * quint64(pageSize);
        s.rssBytes = m[1].toULongLong() * quint64(pageSize);

        QFile c(QStringLiteral("/proc/%1/comm").arg(pid));
        if (c.open(QIODevice::ReadOnly | QIODevice::Text))
            s.name = QString::fromUtf8(c.readLine()).trimmed();
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(),
              [](const ProcessSample& a, const ProcessSample& b) {
                  return a.rssBytes > b.rssBytes;
              });
    if (out.size() > limit) out.resize(limit);
#else
    Q_UNUSED(limit);
#endif
    return out;
}

QString ResourceMonitor::formatBytes(quint64 b, int decimals)
{
    // Binary units, consistently: 1 GiB = 1024^3. Mixing decimal and binary
    // prefixes is a common source of figures that do not add up.
    static const char* unit[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = double(b);
    int i = 0;
    while (v >= 1024.0 && i < 5) { v /= 1024.0; ++i; }
    return QStringLiteral("%1 %2").arg(v, 0, 'f', i == 0 ? 0 : decimals)
                                  .arg(QLatin1String(unit[i]));
}

} // namespace sdr
