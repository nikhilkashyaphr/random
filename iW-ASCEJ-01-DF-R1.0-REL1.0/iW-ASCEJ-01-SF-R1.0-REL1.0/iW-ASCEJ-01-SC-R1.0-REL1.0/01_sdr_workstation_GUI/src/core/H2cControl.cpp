#include "H2cControl.h"

#include <QFileInfo>

#include <cstring>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sdr {

namespace {
constexpr int kOffMagic   = 0;
constexpr int kOffVersion = 8;
constexpr int kOffSeq     = 12;
constexpr int kOffPace    = 16;
constexpr int kOffRate    = 24;
constexpr int kOffCentre  = 32;

template <typename T> T peek(const void* b, int off)
{ T v{}; std::memcpy(&v, static_cast<const char*>(b) + off, sizeof(T)); return v; }

template <typename T> void poke(void* b, int off, T v)
{ std::memcpy(static_cast<char*>(b) + off, &v, sizeof(T)); }
} // namespace

H2cControl::H2cControl(QObject* parent) : QObject(parent) {}
H2cControl::~H2cControl() { detach(); }

bool H2cControl::attach(const QString& path)
{
#ifndef Q_OS_UNIX
    m_error = QStringLiteral("shared-memory control requires POSIX");
    return false;
#else
    detach();
    m_path = path;
    const QByteArray p = path.toLocal8Bit();

    // O_CREAT: the control block may not exist yet if the transmitter has not
    // started. Seeding it here means the operator can set parameters first and
    // rdma_tx adopts them when it comes up, rather than the window being dead
    // until something else creates the file.
    const bool existed = QFileInfo::exists(path);
    m_fd = ::open(p.constData(), O_RDWR | O_CREAT, 0666);
    if (m_fd < 0) {
        m_error = QStringLiteral("cannot open %1: %2")
                      .arg(path, QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    if (::ftruncate(m_fd, kCtlBytes) != 0) {
        m_error = QStringLiteral("cannot size %1: %2")
                      .arg(path, QString::fromLocal8Bit(strerror(errno)));
        ::close(m_fd); m_fd = -1;
        return false;
    }
    m_base = ::mmap(nullptr, kCtlBytes, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
    if (m_base == MAP_FAILED) {
        m_base = nullptr;
        m_error = QStringLiteral("mmap failed: %1")
                      .arg(QString::fromLocal8Bit(strerror(errno)));
        ::close(m_fd); m_fd = -1;
        return false;
    }

    if (!existed || peek<quint64>(m_base, kOffMagic) != kMagic) {
        // Seed with the reference defaults (control_panel.py): 10 Gb/s pace,
        // 100 MSPS, 2.45 GHz. Magic written last so a concurrent reader never
        // sees a valid magic over uninitialised fields.
        poke<quint32>(m_base, kOffVersion, 1);
        poke<quint32>(m_base, kOffSeq, 0);
        poke<double>(m_base, kOffPace, 10.0);
        poke<quint64>(m_base, kOffRate, 100000000ULL);
        poke<qint64>(m_base, kOffCentre, 2450000000LL);
        __sync_synchronize();
        poke<quint64>(m_base, kOffMagic, kMagic);
        emit statusMessage(QStringLiteral("Created and seeded %1").arg(path));
    } else {
        emit statusMessage(QStringLiteral("Attached to %1 (seq %2)")
                               .arg(path).arg(sequence()));
    }
    return true;
#endif
}

void H2cControl::detach()
{
#ifdef Q_OS_UNIX
    if (m_base) { ::munmap(m_base, kCtlBytes); m_base = nullptr; }
    if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }
#endif
}

double  H2cControl::paceGbps() const
{ return m_base ? peek<double>(m_base, kOffPace) : 0.0; }
quint64 H2cControl::sampleRateHz() const
{ return m_base ? peek<quint64>(m_base, kOffRate) : 0; }
qint64  H2cControl::centerFreqHz() const
{ return m_base ? peek<qint64>(m_base, kOffCentre) : 0; }
quint32 H2cControl::sequence() const
{ return m_base ? peek<quint32>(m_base, kOffSeq) : 0; }

bool H2cControl::apply(double paceGbps, quint64 sampleRateHz, qint64 centerFreqHz)
{
    if (!m_base) {
        emit errorMessage(QStringLiteral("Not attached to a control block."));
        return false;
    }
    poke<double>(m_base, kOffPace, paceGbps);
    poke<quint64>(m_base, kOffRate, sampleRateHz);
    poke<qint64>(m_base, kOffCentre, centerFreqHz);

    // Barrier, then bump seq. The transmitter applies on a seq change, so the
    // fields must be visible before it observes the new value.
    __sync_synchronize();
    poke<quint32>(m_base, kOffSeq, peek<quint32>(m_base, kOffSeq) + 1);

    emit applied(paceGbps, sampleRateHz, centerFreqHz);
    emit statusMessage(QStringLiteral("Applied — pace %1, rate %2 MSPS, "
                                      "centre %3 MHz (seq %4)")
        .arg(paceGbps <= 0.0 ? QStringLiteral("unlimited")
                             : QStringLiteral("%1 Gb/s").arg(paceGbps, 0, 'f', 2))
        .arg(sampleRateHz / 1e6, 0, 'f', 3)
        .arg(centerFreqHz / 1e6, 0, 'f', 3)
        .arg(sequence()));
    return true;
}

} // namespace sdr
