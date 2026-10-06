#pragma once
// ---------------------------------------------------------------------------
// ResourceMonitor — CPU, RAM, GPU and disk telemetry.
//
// PLATFORM NOTE. The specification is written against Windows performance
// counters. This application runs on Ubuntu, so the equivalent primary
// sources are used and named explicitly below. The *formulas* are the same;
// only the source APIs differ. Nothing is estimated when a real source is
// unavailable — the metric reports `valid == false` and the UI shows "n/a".
//
// Sources and formulas
// --------------------
// CPU   /proc/stat, aggregate "cpu" line. Utilisation is computed from the
//       DELTA between two samples, never from cumulative totals:
//
//           busy  = (total - idleAll) - (totalPrev - idleAllPrev)
//           total = sum(all fields) - sum(all fields at previous sample)
//           cpu%  = 100 * busy / total
//
//       where idleAll = idle + iowait. This is already normalised across
//       logical processors because the kernel's aggregate line sums jiffies
//       over every CPU and the divisor grows with core count — so summing
//       per-core percentages (the classic error) cannot occur here.
//
// RAM   /proc/meminfo. Used is derived from MemAvailable, not MemFree:
//
//           used% = 100 * (MemTotal - MemAvailable) / MemTotal
//
//       MemFree excludes reclaimable page cache and buffers, so using it
//       overstates consumption badly on a normal system. MemAvailable is the
//       kernel's own estimate of what a new allocation could obtain, and is
//       what `free` and most system monitors report. Used + Available ==
//       Total holds by construction with this definition.
//
// GPU   nvidia-smi --query-gpu=... . Per adapter: utilisation and memory.
//       When no NVIDIA driver is present the metric is simply invalid; no
//       value is inferred from CPU or process activity.
//
// DISK  statvfs() on the RECORDING DIRECTORY specifically, not the root
//       filesystem, so the figure describes the volume actually being
//       written to:
//
//           total = f_blocks * f_frsize
//           free  = f_bavail * f_frsize      (unprivileged-available)
//
//       f_bavail is used rather than f_bfree because the difference is the
//       root reserve, which a recording process cannot use.
//
// Sampling cost: one small read of /proc/stat and /proc/meminfo per tick, and
// one statvfs. nvidia-smi is a process spawn, so it is polled at a slower
// cadence than CPU/RAM and never on the GUI thread's critical path.
// ---------------------------------------------------------------------------

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QVector>

class QProcess;

class QTimer;

namespace sdr {

/// A metric that may legitimately be unavailable. `valid == false` means "not
/// measurable here", never "zero".
struct Metric {
    bool    valid = false;
    double  value = 0.0;
    QString detail;
};

struct CpuSample {
    bool   valid = false;
    double usagePct = 0.0;      ///< 0..100, normalised over all logical CPUs
    int    logicalCpus = 0;
    QString detail;
};

struct RamSample {
    bool    valid = false;
    quint64 totalBytes = 0;
    quint64 availableBytes = 0;
    quint64 usedBytes = 0;      ///< total - available, by definition
    double  usedPct = 0.0;
    QString detail;
};

struct GpuSample {
    bool    valid = false;
    int     index = 0;
    QString name;
    double  utilPct = 0.0;
    quint64 memUsedBytes = 0;
    quint64 memTotalBytes = 0;
    QString detail;
};

struct DiskSample {
    bool    valid = false;
    QString path;               ///< the directory actually measured
    QString mountPoint;
    quint64 totalBytes = 0;
    quint64 freeBytes = 0;      ///< available to an unprivileged writer
    quint64 usedBytes = 0;
    double  usedPct = 0.0;
    QString detail;
};

struct ProcessSample {
    int     pid = 0;
    QString name;
    quint64 rssBytes = 0;       ///< resident set — the Linux analogue of
                                ///< Working Set. NOT virtual or committed.
    quint64 vmBytes = 0;        ///< virtual size, reported separately and
                                ///< never conflated with RSS
    double  cpuPct = 0.0;
};

/// Severity of a metric against its thresholds.
enum class Level { Normal, Warning, Critical };

/// Thresholds are data, documented and adjustable, not magic numbers spread
/// through the UI. Percentages, 0..100.
struct ResourceThresholds {
    double cpuWarn  = 85.0, cpuCrit  = 95.0;
    double ramWarn  = 85.0, ramCrit  = 95.0;
    double gpuWarn  = 90.0, gpuCrit  = 98.0;
    double diskWarn = 85.0, diskCrit = 95.0;

    /// Hysteresis band. A metric must fall this far BELOW a threshold before
    /// the level is allowed to drop again. Without it a value hovering on the
    /// boundary flaps between states and emits a warning every poll — the
    /// classic cause of alert spam.
    double releaseMargin = 3.0;
};

/// One threshold crossing, reported once per transition.
struct ResourceAlert {
    QString id;            ///< "cpu", "ram", "gpu0", "disk"
    Level   level = Level::Normal;
    QString title;
    QString detail;
    QString fix;
};

/// Polls the system and publishes samples. Lives on the GUI thread but does
/// only cheap reads there; the GPU probe is rate-limited separately.
class ResourceMonitor : public QObject
{
    Q_OBJECT
public:
    explicit ResourceMonitor(QObject* parent = nullptr);
    ~ResourceMonitor() override;

    /// Poll cadence for CPU/RAM/disk. Default 1000 ms: fast enough to be
    /// live, slow enough that the monitor is not itself a load.
    void start(int intervalMs = 1000);
    void stop();

    /// Directory whose filesystem the disk metric describes. Set this to the
    /// recording destination so the figure is the one that matters.
    void setDiskPath(const QString& dir);
    QString diskPath() const { return m_diskPath; }

    CpuSample  cpu()  const { return m_cpu; }
    RamSample  ram()  const { return m_ram; }
    DiskSample disk() const { return m_disk; }
    QVector<GpuSample> gpus() const { return m_gpus; }

    /// Top processes by resident memory. Sampled on demand only — walking
    /// /proc is far more expensive than the headline metrics and must not run
    /// on the periodic tick.
    static QVector<ProcessSample> topProcesses(int limit = 15);

    /// Human-readable byte formatter shared by the UI.
    static QString formatBytes(quint64 b, int decimals = 1);

    void setThresholds(const ResourceThresholds& t) { m_thr = t; }
    ResourceThresholds thresholds() const { return m_thr; }

    /// Current level of each metric, for colouring.
    Level cpuLevel()  const { return m_lvlCpu; }
    Level ramLevel()  const { return m_lvlRam; }
    Level gpuLevel()  const { return m_lvlGpu; }
    Level diskLevel() const { return m_lvlDisk; }

signals:
    void updated();
    /// Emitted only on a level TRANSITION, never every poll.
    void alert(const sdr::ResourceAlert& a);

private slots:
    void poll();

private:
    bool readCpu();
    bool readRam();
    bool readDisk();
    void readGpus();

    QString m_diskPath;
    QTimer* m_timer = nullptr;

    // Previous /proc/stat totals, for the delta computation.
    quint64 m_prevTotal = 0, m_prevIdle = 0;
    bool    m_havePrev = false;

    CpuSample  m_cpu;
    RamSample  m_ram;
    DiskSample m_disk;
    QVector<GpuSample> m_gpus;

    int m_gpuTickCounter = 0;   ///< nvidia-smi is spawned every Nth tick
    /// The running nvidia-smi query, if any. Asynchronous: 1.2.3 ran it with
    /// waitForFinished() on the GUI thread every five seconds, and a slow or
    /// hung nvidia-smi (busy GPU, driver trouble) froze the window for up to
    /// 2.3 s — and then up to 30 s more in QProcess's destructor.
    QProcess*       m_gpuProc = nullptr;
    QElapsedTimer   m_gpuClock;
    int             m_gpuToolState = 0;   ///< 0 unknown, 1 present, -1 absent

    ResourceThresholds m_thr;
    Level m_lvlCpu = Level::Normal, m_lvlRam = Level::Normal;
    Level m_lvlGpu = Level::Normal, m_lvlDisk = Level::Normal;

    /// Apply thresholds with hysteresis and emit on transition.
    Level classify(double pct, double warn, double crit, Level current) const;
    void  evaluate();
};

} // namespace sdr
