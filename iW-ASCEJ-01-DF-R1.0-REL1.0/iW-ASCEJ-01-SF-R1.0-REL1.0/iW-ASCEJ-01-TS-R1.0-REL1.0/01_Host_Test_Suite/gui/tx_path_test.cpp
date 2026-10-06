// ---------------------------------------------------------------------------
// Host transmit path: IqGenerator -> real FIFO -> a consumer that behaves like
// iwfg_h2c (reads whole chunks, and REPLAYS the latest one until the next
// arrives) -> the sample stream the DAC would play -> spectrum.
//
// Checks: DAC packing, the frequency actually transmitted, a clean spectrum in
// coherent mode, the spur comb the replay causes without it (i.e. that the
// test can see the defect it guards against), live parameter updates, stop /
// restart, and recovery after a FIFO error.
// ---------------------------------------------------------------------------
#include "IqGenerator.h"
#include "Waveform.h"
#include "spectrum.h"

#include <QCoreApplication>
#include <QThread>
#include <QElapsedTimer>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using sdr::IqGenerator;
using sdr::cf32;

static int g_fail = 0;
#define CHECK(c, ...) do { std::printf((c) ? "  PASS  " : "  FAIL  "); std::printf(__VA_ARGS__); \
                           std::printf("\n"); if (!(c)) ++g_fail; } while (0)

static constexpr int kChunkBytes = 16384;            // iwfg_h2c default
static constexpr int kPairs = kChunkBytes / 4;

// ---- a consumer with iwfg_h2c's semantics --------------------------------
struct ReplayConsumer {
    std::string path;
    std::atomic<bool> stop{false};
    std::mutex mtx;
    std::vector<qint16> latest;          // most recent complete chunk
    std::atomic<unsigned> chunks{0};
    std::thread th;

    void run() {
        int fd = -1;
        while (!stop && fd < 0) {
            fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd < 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (fd < 0) return;
        ::fcntl(fd, F_SETFL, 0);       // blocking reads from here on
        std::vector<qint16> buf(kPairs * 2);
        while (!stop) {
            // read_all(): exactly one chunk, as iwfg_h2c does
            char* p = reinterpret_cast<char*>(buf.data());
            size_t got = 0;
            while (got < size_t(kChunkBytes) && !stop) {
                ssize_t r = ::read(fd, p + got, size_t(kChunkBytes) - got);
                if (r > 0) got += size_t(r);
                else if (r == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
                else if (errno != EINTR) break;
            }
            if (got == size_t(kChunkBytes)) {
                std::lock_guard<std::mutex> l(mtx);
                latest = buf;
                chunks++;
            }
        }
        ::close(fd);
    }
    void begin(const std::string& p) { path = p; th = std::thread([this]{ run(); }); }
    void end() { stop = true; if (th.joinable()) th.join(); }

    // What the DAC plays: each received chunk, replayed 1..6 times (the DMA
    // runs far faster than any host producer), for `n` samples.
    std::vector<cf32> dacStream(size_t n, unsigned seed) {
        std::mt19937 rng(seed);
        std::uniform_int_distribution<int> reps(1, 6);
        std::vector<cf32> out; out.reserve(n);
        unsigned seen = chunks.load();
        std::vector<qint16> cur;
        while (out.size() < n) {
            { std::lock_guard<std::mutex> l(mtx); cur = latest; }
            if (cur.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
            const int r = reps(rng);
            for (int k = 0; k < r && out.size() < n; ++k)
                for (int i = 0; i < kPairs && out.size() < n; ++i)
                    out.emplace_back(float(cur[size_t(2*i)]) / 32764.0f, float(cur[size_t(2*i+1)]) / 32764.0f);
            // move on to a newer chunk before replaying again
            QElapsedTimer t; t.start();
            while (chunks.load() == seen && t.elapsed() < 20)
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            seen = chunks.load();
        }
        return out;
    }
};

struct GenRig {
    QThread thread;
    IqGenerator* gen = new IqGenerator;
    std::atomic<int> started{0}, stopped{0};
    std::atomic<double> actualHz{0.0};
    GenRig() {
        gen->moveToThread(&thread);
        QObject::connect(gen, &IqGenerator::started, gen, [this]{ started++; }, Qt::DirectConnection);
        QObject::connect(gen, &IqGenerator::stopped, gen, [this]{ stopped++; }, Qt::DirectConnection);
        QObject::connect(gen, &IqGenerator::transmitting, gen,
                         [this](double, double a, int, bool){ actualHz = a; }, Qt::DirectConnection);
        thread.start();
    }
    void start(const IqGenerator::Config& c) {
        QMetaObject::invokeMethod(gen, [this, c]{ gen->start(c); }, Qt::QueuedConnection);
    }
    bool waitStopped(int before, int ms) {
        QElapsedTimer t; t.start();
        while (stopped.load() == before && t.elapsed() < ms) QThread::msleep(5);
        return stopped.load() != before;
    }
    ~GenRig() { gen->stop(); thread.quit(); thread.wait(3000); delete gen; }
};

static std::string tmpFifo(const char* tag)
{
    std::string p = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
                  + "/iqgen_test_" + tag + "_" + std::to_string(::getpid());
    ::unlink(p.c_str());
    return p;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::printf("Host transmit path (IqGenerator -> FIFO -> iwfg_h2c-style replay)\n\n");

    // 1. Packing contract, every waveform, both modes -------------------------
    {
        bool lsbOk = true, peakOk = true;
        for (int w = 0; w <= 5; ++w) for (int coh = 0; coh <= 1; ++coh) {
            IqGenerator::Config c; c.waveform = IqGenerator::Waveform(w); c.coherent = coh;
            c.amplitude = 1.0;
            std::vector<qint16> v(size_t(2 * kPairs));
            quint32 rng = 1u;
            IqGenerator::renderLoop(c, v.data(), kPairs, &rng);
            for (qint16 s : v) { if (s & 3) lsbOk = false; if (std::abs(int(s)) > 32764) peakOk = false; }
        }
        CHECK(lsbOk,  "DAC packing: bits[1:0] == 00 in every word (all waveforms)");
        CHECK(peakOk, "DAC packing: |sample| <= 8191<<2 = 32764");
    }

    // 2. Frequency actually transmitted ---------------------------------------
    {
        IqGenerator::Config c; c.sampleRateSps = 200e6; c.frequencyHz = 10e6; c.chunkBytes = kChunkBytes;
        const double a = IqGenerator::transmittedHz(c);
        CHECK(std::abs(a - 10.009765625e6) < 1e-3, "10 MHz @ 200 MSPS, 4096-sample loop -> %.6f MHz (205 cycles)", a / 1e6);
        c.frequencyHz = 12.5e6;
        CHECK(std::abs(IqGenerator::transmittedHz(c) - 12.5e6) < 1e-3, "12.5 MHz fits the loop exactly -> unchanged");
        c.frequencyHz = -7.3e6;
        const double n = IqGenerator::transmittedHz(c);
        CHECK(n < 0 && std::abs(n + 7.3e6) <= 200e6 / kPairs / 2 + 1, "negative frequencies keep their sign (%.6f MHz)", n / 1e6);
        c.coherent = false; c.frequencyHz = 10e6;
        CHECK(IqGenerator::transmittedHz(c) == 10e6, "coherence off -> exact request");
    }

    // 3. Loop seamlessness in isolation: the loop repeated is a pure tone ------
    {
        IqGenerator::Config c; c.frequencyHz = 10e6; c.sampleRateSps = 200e6; c.chunkBytes = kChunkBytes;
        std::vector<qint16> v(size_t(2 * kPairs)); quint32 rng = 1u;
        IqGenerator::renderLoop(c, v.data(), kPairs, &rng);
        std::vector<cf32> x; x.reserve(65536);
        while (x.size() < 65536) for (int i = 0; i < kPairs && x.size() < 65536; ++i)
            x.emplace_back(float(v[size_t(2*i)]) / 32764.0f, float(v[size_t(2*i+1)]) / 32764.0f);
        auto s = t::analyse(x, 200e6);
        CHECK(std::abs(s.peakHz - 10.009765625e6) < 200e6 / 65536.0, "looped chunk: tone at %.6f MHz", s.peakHz / 1e6);
        CHECK(s.sfdrDb > 80.0, "looped chunk: SFDR %.1f dB (14-bit quantisation limited)", s.sfdrDb);
    }

    // 4. End to end through a FIFO and a replaying consumer --------------------
    auto endToEnd = [](bool coherent, double hz, double* sfdr, double* peak) {
        const std::string fifo = tmpFifo(coherent ? "coh" : "raw");
        ::mkfifo(fifo.c_str(), 0666);
        ReplayConsumer rc; rc.begin(fifo);
        GenRig rig;
        IqGenerator::Config c; c.fifoPath = QString::fromStdString(fifo);
        c.sampleRateSps = 200e6; c.frequencyHz = hz; c.chunkBytes = kChunkBytes; c.coherent = coherent;
        rig.start(c);
        QElapsedTimer t; t.start();
        while (rc.chunks.load() < 1 && t.elapsed() < 5000) QThread::msleep(5);
        auto x = rc.dacStream(65536, 7);
        auto s = t::analyse(x, 200e6);
        *sfdr = s.sfdrDb; *peak = s.peakHz;
        const int before = rig.stopped.load();
        rig.gen->stop();
        rig.waitStopped(before, 2000);
        rc.end();
        ::unlink(fifo.c_str());
    };
    {
        double sfdrC = 0, pkC = 0, sfdrR = 0, pkR = 0;
        endToEnd(true, 10e6, &sfdrC, &pkC);
        CHECK(std::abs(pkC - 10.009765625e6) < 200e6 / 65536.0, "end to end, coherent: tone at %.6f MHz", pkC / 1e6);
        CHECK(sfdrC > 75.0, "end to end, coherent: SFDR %.1f dB through the replay", sfdrC);
        endToEnd(false, 10e6, &sfdrR, &pkR);
        CHECK(sfdrR < sfdrC - 20.0,
              "end to end, NON-coherent (old behaviour): SFDR only %.1f dB — the replay seams the test must detect", sfdrR);
    }

    // 5. Live update, stop honoured quickly, restart, recovery ----------------
    {
        const std::string fifo = tmpFifo("upd");
        ::mkfifo(fifo.c_str(), 0666);
        ReplayConsumer rc; rc.begin(fifo);
        GenRig rig;
        IqGenerator::Config c; c.fifoPath = QString::fromStdString(fifo);
        c.sampleRateSps = 200e6; c.frequencyHz = 10e6; c.chunkBytes = kChunkBytes;
        rig.start(c);
        QElapsedTimer t; t.start();
        while (rc.chunks.load() < 1 && t.elapsed() < 5000) QThread::msleep(5);
        c.frequencyHz = 20e6;
        rig.gen->requestUpdate(c);            // from a foreign thread, while running
        const unsigned at = rc.chunks.load();
        t.restart();
        while (rc.chunks.load() < at + 1 && t.elapsed() < 3000) QThread::msleep(5);
        auto s = t::analyse(rc.dacStream(65536, 3), 200e6);
        CHECK(std::abs(s.peakHz - 20.01953125e6) < 200e6 / 65536.0,
              "live update while running: now %.6f MHz (20 MHz requested)", s.peakHz / 1e6);
        CHECK(std::abs(rig.actualHz.load() - 20.01953125e6) < 1.0, "transmitting() reports the new frequency");

        int before = rig.stopped.load();
        QElapsedTimer st; st.start();
        rig.gen->stop();
        const bool stoppedFast = rig.waitStopped(before, 1000);
        CHECK(stoppedFast && st.elapsed() < 500, "stop honoured in %lld ms", (long long)st.elapsed());

        const int startsBefore = rig.started.load();
        rig.start(c);
        t.restart();
        while (rig.started.load() == startsBefore && t.elapsed() < 2000) QThread::msleep(5);
        CHECK(rig.started.load() > startsBefore, "restart without recreating the generator");
        before = rig.stopped.load();
        rig.gen->stop(); rig.waitStopped(before, 2000);
        rc.end();
        ::unlink(fifo.c_str());
    }
    {
        // A path that exists but is not a FIFO: start fails. It must be
        // possible to start again afterwards (the run flag used to stay set).
        const std::string bad = tmpFifo("bad");
        { FILE* f = std::fopen(bad.c_str(), "w"); if (f) std::fclose(f); }
        GenRig rig;
        IqGenerator::Config c; c.fifoPath = QString::fromStdString(bad);
        int before = rig.stopped.load();
        rig.start(c);
        CHECK(rig.waitStopped(before, 2000), "non-FIFO path: generator stops with an error");
        CHECK(!rig.gen->running(), "  ...and is not left marked as running");
        before = rig.stopped.load();
        rig.start(c);
        CHECK(rig.waitStopped(before, 2000), "  ...and a second start is accepted (not silently ignored)");
        ::unlink(bad.c_str());
    }

    // 6. Write-once and silence-on-stop -------------------------------------
    {
        const std::string fifo = tmpFifo("once");
        ::mkfifo(fifo.c_str(), 0666);
        ReplayConsumer rc; rc.begin(fifo);
        GenRig rig;
        IqGenerator::Config c; c.fifoPath = QString::fromStdString(fifo);
        c.sampleRateSps = 200e6; c.frequencyHz = 10e6; c.chunkBytes = kChunkBytes;
        rig.start(c);
        QElapsedTimer t; t.start();
        while (rc.chunks.load() < 1 && t.elapsed() < 3000) QThread::msleep(5);
        const unsigned first = rc.chunks.load();
        QThread::msleep(1000);
        CHECK(first == 1 && rc.chunks.load() == 1,
              "coherent loop written once (%u chunk(s) after 1 s) — nothing more for iwfg_h2c to copy",
              rc.chunks.load());
        const int before = rig.stopped.load();
        rig.gen->stop();
        rig.waitStopped(before, 3000);
        t.restart();
        while (rc.chunks.load() < 2 && t.elapsed() < 2000) QThread::msleep(5);
        bool silent = false;
        { std::lock_guard<std::mutex> l(rc.mtx);
          silent = !rc.latest.empty();
          for (qint16 v : rc.latest) if (v) { silent = false; break; } }
        CHECK(silent, "Stop sends a zero chunk: the replayed chunk is silence");
        rc.end();
        ::unlink(fifo.c_str());
    }

    std::printf("\nRESULT: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
