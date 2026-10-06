#pragma once
// ---------------------------------------------------------------------------
// RecordingGuard — storage-limit validation and predictive disk warnings.
//
// Structured states, not scattered UI conditions:
//
//     metrics -> validation -> safety rules -> classification -> notification
//
// Every warning carries an id, a severity, what is wrong, why it matters and
// how to fix it. Warnings are STATEFUL: a given id is emitted once when it
// becomes active and again only if its severity changes, so a 1 Hz poll
// cannot produce warning spam.
//
// Thresholds are named constants here, documented, and not scattered as
// magic numbers through the UI.
// ---------------------------------------------------------------------------

#include "ResourceMonitor.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QVector>

namespace sdr {

struct RecWarning {
    enum Severity { Info, Warning, Critical };
    QString  id;              ///< stable identifier, for de-duplication
    Severity severity = Info;
    QString  title;
    QString  what;            ///< what is wrong
    QString  why;             ///< why it is wrong / the impact
    QString  fix;             ///< what the user should do
};

/// Live recording state, recomputed each poll.
struct RecStatus {
    enum State { Idle, Normal, Warning, Critical, LimitReached };
    State   state = Idle;
    quint64 sessionBytes = 0;      ///< bytes written this session
    quint64 limitBytes   = 0;      ///< configured limit, 0 = none
    quint64 remainingBytes = 0;    ///< limit - session, floored at 0
    double  growthBytesPerSec = 0.0;
    bool    growthReliable = false;///< false -> show "Estimating…"
    double  secondsRemaining = 0.0;
    bool    etaReliable = false;
    QString etaText;               ///< preformatted, incl. "Estimating…"
};

class RecordingGuard : public QObject
{
    Q_OBJECT
public:
    explicit RecordingGuard(QObject* parent = nullptr);

    // --- documented thresholds ------------------------------------------
    /// Never plan to fill a volume completely: filesystems degrade and other
    /// processes need room. The larger of these two is reserved.
    static constexpr quint64 kSafetyReserveBytes = 2ULL * 1024 * 1024 * 1024; // 2 GiB
    static constexpr double  kSafetyReserveFrac  = 0.05;                       // 5 %
    /// Warn once the session passes this fraction of its configured limit.
    static constexpr double  kWarnFraction       = 0.80;
    static constexpr double  kCriticalFraction   = 0.95;
    /// A growth estimate needs this many samples before the exponential
    /// smoothing has settled enough to trust.
    static constexpr int     kMinGrowthSamples   = 3;
    static constexpr double  kMinGrowthBytesPerSec = 1024.0;

    /// Bytes on `path`'s filesystem that a recording may safely consume.
    static quint64 safeCapacity(const DiskSample& d);

    /// Validate a proposed limit BEFORE recording starts. Empty result = ok.
    static QVector<RecWarning> validateLimit(double limitMiB,
                                             const QString& outputDir,
                                             const DiskSample& disk);

    void beginSession(quint64 limitBytes);
    void endSession();
    bool active() const { return m_active; }

    /// Called as bytes are written. Returns the recomputed status.
    RecStatus update(quint64 sessionBytes, const DiskSample& disk);
    RecStatus status() const { return m_status; }

signals:
    /// Emitted only when a warning becomes active or changes severity.
    void warningRaised(const sdr::RecWarning& w);
    void warningCleared(const QString& id);
    /// The configured limit has been reached; the caller must stop writing.
    void limitReached(quint64 sessionBytes, quint64 limitBytes);

private:
    void raise(const RecWarning& w);
    void clear(const QString& id);

    bool      m_active = false;
    RecStatus m_status;
    QElapsedTimer m_clock;
    quint64   m_lastBytes = 0;
    qint64    m_lastMs = 0;
    double    m_smoothedRate = 0.0;
    int       m_growthSamples = 0;
    bool      m_limitSignalled = false;
    QVector<QPair<QString, int>> m_activeWarnings;   ///< id -> severity
};

} // namespace sdr
