// ---------------------------------------------------------------------------
// IqGenerator — see IqGenerator.h for the four contracts this honours.
// ---------------------------------------------------------------------------

#include "IqGenerator.h"
#include "Waveform.h"

#include <QElapsedTimer>
#include <QMutexLocker>
#include <QThread>
#include <QtMath>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <cerrno>
#include <cstring>
#include <vector>

namespace sdr {

// Quantise to a true 14-bit signed value FIRST, then MSB-align. Doing it in
// this order is what guarantees bits[1:0] == 00 for every emitted word.
// Scaling straight to +/-32764 and rounding does NOT: it leaves ~75% of
// samples with nonzero LSBs, violating the DAC packing contract.
static constexpr double kPeak14 = 8191.0;

static inline qint16 packDac(double v)
{
    const qint16 i14 = qint16(qBound(-kPeak14, qRound(v * kPeak14) * 1.0, kPeak14));
    return qint16(i14 << 2);
}

// Coherent mode writes the loop ONCE per configuration and then leaves the
// FIFO alone: iwfg_h2c replays the last chunk by itself. Rewriting it (1.2.1
// did so 50x/s) made iwfg_h2c memcpy each copy into its idle DMA buffer
// between transfers — harmless at 16 KiB, a millisecond hole in the DAC feed
// at 16 MiB. While idle the writer only watches for the reader going away.
static constexpr int kIdlePollMs = 50;
// Bound on writing the closing silence at stop.
static constexpr int kSilenceTimeoutMs = 2000;

static int sanitiseChunkBytes(int bytes)
{
    bytes -= bytes % 4;                              // iwfg_h2c: multiple of 4
    return qBound(1024, bytes, 64 * 1024 * 1024);
}

IqGenerator::IqGenerator(QObject* parent) : QObject(parent) {}

IqGenerator::~IqGenerator() { stop(); }

void IqGenerator::stop() { m_run.storeRelease(0); }

// Write `len` bytes, paced with select() so a frozen reader cannot wedge us.
// `honourStop`: give up (returning Stopped) as soon as stop() is requested.
// Tracks m_partial so a chunk cut short can be completed later.
IqGenerator::WriteResult IqGenerator::writeBytes(int fd, const char* p, qint64 len,
                                                 bool honourStop, int timeoutMs)
{
    QElapsedTimer t; t.start();
    const qint64 chunk = qint64(m_cfg.chunkBytes);
    while (len > 0) {
        if (honourStop && !m_run.loadAcquire()) return WriteResult::Stopped;
        if (timeoutMs >= 0 && t.elapsed() > timeoutMs) return WriteResult::Broken;

        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(fd, &wf);
        struct timeval tv { 0, 100000 };           // 100 ms
        const int r = ::select(fd + 1, nullptr, &wf, nullptr, &tv);
        if (r == 0) continue;                      // pipe full: reader busy
        if (r < 0) { if (errno == EINTR) continue; return WriteResult::Broken; }

        const ssize_t w = ::write(fd, p, size_t(len));
        if (w > 0) {
            p += w; len -= w;
            m_partial = (m_partial + qint64(w)) % chunk;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        return WriteResult::Broken;                // EPIPE: reader gone
    }
    return WriteResult::Done;
}

// Stop means silence. iwfg_h2c keeps replaying the last chunk it received,
// so without this the DAC went on transmitting the tone after "Stop". Any
// chunk cut short is completed with zeros first, so the reader's framing is
// kept and the final replayed chunk is all zeros.
void IqGenerator::writeSilence(int fd)
{
    const qint64 chunk = qint64(m_cfg.chunkBytes);
    const qint64 bytes = (chunk - m_partial) % chunk + chunk;
    std::vector<char> zeros(size_t(bytes), 0);
    if (writeBytes(fd, zeros.data(), bytes, /*honourStop=*/false, kSilenceTimeoutMs)
            == WriteResult::Done)
        emit silenced();
}

int IqGenerator::loopPairsFor(int chunkBytes)
{
    return sanitiseChunkBytes(chunkBytes) / 4;
}

double IqGenerator::transmittedHz(const Config& cfg)
{
    if (!cfg.coherent || cfg.waveform == Noise) return cfg.frequencyHz;
    return wave::coherentHz(cfg.frequencyHz, cfg.sampleRateSps,
                            loopPairsFor(cfg.chunkBytes));
}

void IqGenerator::requestUpdate(const Config& cfg)
{
    QMutexLocker lock(&m_pendingLock);
    m_pending = cfg;
    m_reload.storeRelease(1);
}

void IqGenerator::updateConfig(const Config& cfg) { requestUpdate(cfg); }

// ---------------------------------------------------------------------------
// Waveforms — numbering and shapes match iq_source_v9 exactly (Waveform.h).
// ---------------------------------------------------------------------------
double IqGenerator::waveAt(double phi) const
{
    return wave::shape(int(m_cfg.waveform), phi);
}

static void fillNoise(qint16* out, int pairs, double amp, quint32* rng)
{
    quint32 r = *rng;
    for (int i = 0; i < pairs; ++i) {
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        const double a = (double(qint32(r)) / 2147483648.0) * amp;
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        const double b = (double(qint32(r)) / 2147483648.0) * amp;
        out[2 * i]     = packDac(a);
        out[2 * i + 1] = packDac(b);
    }
    *rng = r;
}

// One seamless loop: k whole cycles in `pairs` samples, phase from the exact
// integer product so sample `pairs` would land precisely on sample 0.
void IqGenerator::renderLoop(const Config& cfg, qint16* out, int pairs, quint32* rngState)
{
    const double amp = qBound(0.0, cfg.amplitude, 1.0);
    if (cfg.waveform == Noise) {
        quint32 seed = 0x12345678u;
        fillNoise(out, pairs, amp, rngState ? rngState : &seed);
        return;
    }
    const long long k = wave::loopCycles(cfg.frequencyHz, cfg.sampleRateSps, pairs);
    const int w = int(cfg.waveform);
    for (int n = 0; n < pairs; ++n) {
        const double phi = cfg.phaseOffset + wave::loopPhase(k, n, pairs);
        out[2 * n]     = packDac(wave::shape(w, phi) * amp);
        out[2 * n + 1] = packDac(wave::shape(w, phi + cfg.iqPhaseDiff) * amp);
    }
}

// Non-coherent streaming path: phase-continuous across chunks, exactly as
// iq_source_v9 produces it. Used when coherent mode is switched off.
void IqGenerator::fillChunk(qint16* out, int pairs)
{
    constexpr double twoPi = 2.0 * M_PI;
    const double amp  = qBound(0.0, m_cfg.amplitude, 1.0);
    const double step = (m_cfg.sampleRateSps > 0.0)
                        ? twoPi * m_cfg.frequencyHz / m_cfg.sampleRateSps : 0.0;
    const double iqd  = m_cfg.iqPhaseDiff;

    if (m_cfg.waveform == Noise) {
        fillNoise(out, pairs, amp, &m_rng);
        return;
    }

    for (int i = 0; i < pairs; ++i) {
        out[2 * i]     = packDac(waveAt(m_phase)       * amp);
        out[2 * i + 1] = packDac(waveAt(m_phase + iqd) * amp);

        m_phase += step;
        if (m_phase >  twoPi) m_phase -= twoPi;
        if (m_phase < -twoPi) m_phase += twoPi;
    }
}

// Build the coherent loop into m_buf (or mark the streaming path active) and
// tell the UI what is really being transmitted.
void IqGenerator::rebuildLoop()
{
    const int pairs = m_buf.size() / 2;
    // Noise too: a random loop the size of the buffer (16 MiB = 21 ms at
    // 200 MSPS, so its spectral lines are 48 Hz apart) sent once, rather than
    // a fresh chunk each time that iwfg_h2c would have to copy in.
    if (m_cfg.coherent) {
        renderLoop(m_cfg, m_buf.data(), pairs, &m_rng);
        m_loopValid = true;
    } else {
        m_loopValid = false;
    }
    emit transmitting(m_cfg.frequencyHz, transmittedHz(m_cfg), pairs, m_cfg.coherent);
}

void IqGenerator::applyPending()
{
    Config next;
    {
        QMutexLocker lock(&m_pendingLock);
        next = m_pending;
    }
    // Fixed for the lifetime of a run: changing them needs a reopen / a new
    // iwfg_h2c chunk size, which is a restart, not an update.
    next.fifoPath   = m_cfg.fifoPath;
    next.chunkBytes = m_cfg.chunkBytes;

    // Same rule as iq_source_v9's set_phase_offset: shift, never reset.
    m_phase += next.phaseOffset - m_cfg.phaseOffset;
    m_cfg = next;
    rebuildLoop();
}

// ---------------------------------------------------------------------------
// FIFO handling
// ---------------------------------------------------------------------------
bool IqGenerator::ensureFifo()
{
    struct stat st;
    const QByteArray p = m_cfg.fifoPath.toLocal8Bit();

    if (::stat(p.constData(), &st) == 0) {
        if (!S_ISFIFO(st.st_mode)) {
            emit error(tr("%1 exists but is not a FIFO.").arg(m_cfg.fifoPath));
            return false;
        }
        return true;                      // reuse the live inode; never unlink
    }
    if (::mkfifo(p.constData(), 0666) == 0 || errno == EEXIST) return true;

    emit error(tr("Cannot create FIFO %1: %2")
                   .arg(m_cfg.fifoPath, QString::fromLocal8Bit(strerror(errno))));
    return false;
}

// Non-blocking, retrying, path-re-resolving open. A blocking open(O_WRONLY)
// resolves ONE inode and waits on it, so a consumer doing unlink+mkfifo would
// leave us waiting on a dead inode forever. Re-resolving on every retry makes
// an inode swap self-healing.
int IqGenerator::openFifo()
{
    bool warned = false;

    while (m_run.loadAcquire()) {
        if (!ensureFifo()) return -1;

        const int fd = ::open(m_cfg.fifoPath.toLocal8Bit().constData(),
                              O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            struct stat st;
            if (::fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode)) {
                return fd;                // stays NON-BLOCKING, deliberately
            }
            ::close(fd);
            QThread::msleep(200);
            continue;
        }

        if (errno == ENXIO || errno == ENOENT) {
            // ENXIO = FIFO exists but iwfg_h2c has not opened it yet.
            // Parameter updates are still taken, so the first chunk the
            // reader sees is already the current configuration.
            if (m_reload.fetchAndStoreOrdered(0)) applyPending();
            QThread::msleep(100);
            continue;
        }
        if (!warned) {
            emit error(tr("open(%1): %2 — retrying")
                           .arg(m_cfg.fifoPath, QString::fromLocal8Bit(strerror(errno))));
            warned = true;
        }
        QThread::msleep(500);
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Streaming
//
// Writes are paced with select() on a 0.1 s timeout, so a frozen reader can
// never wedge this thread: stop() is honoured within ~0.25 s in any state. A
// chunk interrupted by a broken connection is dropped WHOLE, so the next
// connection starts on a chunk boundary and I/Q pair framing is preserved.
// ---------------------------------------------------------------------------
IqGenerator::WriteResult IqGenerator::streamUntilBroken(int fd, quint64* total,
                                                        quint64* chunks)
{
    QElapsedTimer clk; clk.start();
    quint64 sinceReport = 0;
    bool loaded = false;        // coherent: the current loop has been sent

    while (m_run.loadAcquire()) {
        if (m_reload.fetchAndStoreOrdered(0)) { applyPending(); loaded = false; }

        const int pairs = m_buf.size() / 2;

        if (m_loopValid && loaded) {
            // The card is replaying the loop; there is nothing to send until a
            // parameter changes. Watch for the reader disappearing (a FIFO's
            // write end reports POLLERR once no reader holds it open).
            struct pollfd pfd { fd, 0, 0 };
            const int r = ::poll(&pfd, 1, kIdlePollMs);
            if (r > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
                return WriteResult::Broken;
            if (clk.elapsed() >= 1000) {
                emit progress(*total, 0.0, *chunks, m_reconnects);
                clk.restart();
            }
            continue;
        }

        if (!m_loopValid) fillChunk(m_buf.data(), pairs);   // streaming / noise

        const WriteResult w = writeBytes(fd, reinterpret_cast<const char*>(m_buf.constData()),
                                         qint64(m_buf.size()) * qint64(sizeof(qint16)),
                                         /*honourStop=*/true, /*timeoutMs=*/-1);
        if (w != WriteResult::Done) return w;
        loaded = true;

        *total  += quint64(pairs);
        *chunks += 1;
        sinceReport += quint64(pairs);
        if (m_loopValid) emit loopLoaded(*chunks);

        if (clk.elapsed() >= 1000) {
            emit progress(*total,
                          double(sinceReport) / double(clk.elapsed()) / 1000.0,
                          *chunks, m_reconnects);
            sinceReport = 0;
            clk.restart();
        }
    }
    return WriteResult::Stopped;
}

void IqGenerator::start(const Config& cfg)
{
    if (m_run.loadAcquire()) return;

    // A FIFO whose reader has gone raises SIGPIPE in the writing thread, and
    // the default action ends the whole application. Block it here so the
    // write fails with EPIPE and is handled as a reconnect.
    {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &set, nullptr);
    }

    m_cfg = cfg;
    m_cfg.chunkBytes = sanitiseChunkBytes(m_cfg.chunkBytes);
    m_partial = 0;
    {
        QMutexLocker lock(&m_pendingLock);
        m_pending = m_cfg;
    }
    m_reload.storeRelease(0);
    m_run.storeRelease(1);
    m_phase = m_cfg.phaseOffset;
    m_reconnects = 0;

    m_buf.resize(m_cfg.chunkBytes / 2);      // allocated once, reused
    rebuildLoop();

    emit started();

    quint64 total = 0, chunks = 0;

    // Reconnect loop: the consumer restarting is normal operation, not an
    // error. Keep producing until explicitly stopped.
    while (m_run.loadAcquire()) {
        const int fd = openFifo();
        if (fd < 0) break;

        emit connectedChanged(true);
        m_partial = 0;                        // a new reader starts a new chunk
        const WriteResult r = streamUntilBroken(fd, &total, &chunks);
        if (r == WriteResult::Stopped) writeSilence(fd);
        ::close(fd);
        emit connectedChanged(false);

        if (!m_run.loadAcquire()) break;

        if (r == WriteResult::Broken) {
            m_reconnects++;
            emit error(tr("Reader disconnected (iwfg_h2c stopped?) — "
                          "reconnecting. Reconnects: %1").arg(m_reconnects));
        }
        QThread::msleep(250);
    }

    m_run.storeRelease(0);
    emit stopped();
}

} // namespace sdr
