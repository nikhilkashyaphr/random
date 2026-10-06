// ---------------------------------------------------------------------------
// TS-11  RoCEv2 host ring (IQRING01) through the real RoceShmSource.
//
// The GUI's RoCEv2 ingest had no automated coverage: the only way to exercise
// it was to cable a ConnectX port to itself, run the RDMA stack and look at
// the screen. This drives the SHIPPING RoceShmSource against a byte-compatible
// synthetic ring, so the path is checked on any host with no NIC, no RDMA and
// no GPU.
//
// What is deliberately real here: RoceShmSource (unmodified), the ring ABI
// offsets, SampleCodec's Cs16 decode, and the FFT from the GUI's own Fft.h.
// Only the producer is synthetic -- and it is built from the ABI constants
// independently of rdma_common.h, so a layout change breaks a test and names
// the field instead of silently mis-parsing.
// ---------------------------------------------------------------------------
#include "RoceShmSource.h"
#include "SampleCodec.h"
#include "spectrum.h"
#include "fake_ring.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <cstdio>
#include <string>

using namespace sdr;

static int g_fail = 0;
#define CHECK(c, ...) do { std::printf((c) ? "  PASS  " : "  FAIL  "); std::printf(__VA_ARGS__); \
                           std::printf("\n"); if (!(c)) ++g_fail; } while (0)

namespace {

/// A unique ring path per test process, so a run never touches a real
/// receiver's /dev/shm/iqring and parallel runs cannot collide.
std::string ringPath(const char* tag)
{
    return "/dev/shm/ts11_" + std::string(tag) + "_"
         + std::to_string(::getpid());
}

/// Pump the Qt event loop for `ms`, letting the source's 2 ms poll timer run.
void pump(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

/// Start a source on a given ring path and collect the blocks it publishes.
struct Harness {
    RoceShmSource src;
    std::vector<SampleBlock> blocks;
    QStringList  errors;
    QStringList  statuses;

    explicit Harness(const std::string& path)
    {
        Config cfg;
        cfg.src.mode       = SourceMode::Roce;
        cfg.src.devicePath = QString::fromStdString(path);
        src.applyConfig(cfg);
        QObject::connect(&src, &ISignalSource::blockReady,
                         [this](const SampleBlock& b) { blocks.push_back(b); });
        QObject::connect(&src, &ISignalSource::sourceError,
                         [this](const QString& e) { errors << e; });
        QObject::connect(&src, &ISignalSource::statusMessage,
                         [this](const QString& s) { statuses << s; });
    }

    /// Total samples across every collected block, decoded from wire bytes.
    std::vector<cf32> decodeAll() const
    {
        std::vector<cf32> all;
        for (const auto& b : blocks) {
            std::vector<IQBlock> ch;
            codec::decode(b.raw.constData(), std::size_t(b.raw.size()),
                          b.rawFormat, b.rawChannels, b.rawFullScale, ch);
            if (!ch.empty()) all.insert(all.end(), ch[0].begin(), ch[0].end());
        }
        return all;
    }
};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::printf("RoCEv2 host ring (IQRING01) -> RoceShmSource -> decode\n\n");

    // -----------------------------------------------------------------
    std::printf("Ring geometry and header adoption\n");
    // -----------------------------------------------------------------
    {
        // The real producer's geometry, asserted explicitly: 32 slots of
        // 262,144 samples gives a 1 MiB payload and a 1,052,672 B stride.
        t::FakeIqRing ring;
        const std::string p = ringPath("geom");
        CHECK(ring.create(p, 32u, 256u * 1024u, 200.25e6, 3.1e9),
              "create a 32-slot ring with the production frame size");
        CHECK(ring.stride() == 1052672u,
              "slot stride is 1052672 B (64 B header + 1 MiB, 4 KiB-rounded), got %u",
              ring.stride());

        // peekRingInfo must read rate and centre WITHOUT starting acquisition:
        // main.cpp relies on this to fill the axes before the source exists.
        double rateMsps = 0.0, centerGHz = 0.0;
        const bool ok = RoceShmSource::peekRingInfo(QString::fromStdString(p),
                                                    &rateMsps, &centerGHz);
        CHECK(ok, "peekRingInfo reads the control block");
        CHECK(std::fabs(rateMsps - 200.25) < 1e-6,
              "sample rate adopted from the ring: %.4f MSPS (expected 200.2500)", rateMsps);
        CHECK(std::fabs(centerGHz - 3.1) < 1e-9,
              "centre frequency adopted from the ring: %.4f GHz (expected 3.1000)", centerGHz);
        ring.destroy();
    }

    // -----------------------------------------------------------------
    std::printf("\nThe configured ring path is honoured\n");
    // -----------------------------------------------------------------
    {
        // Regression: RoceShmSource used to ignore cfg.src.devicePath and
        // always open /dev/shm/iqring, so `--roce <path>` read its rate from
        // the named ring and then streamed from a different one.
        t::FakeIqRing ring;
        const std::string p = ringPath("path");
        ring.create(p, 8u, 4096u, 100e6, 1.0e9);

        Harness h(p);
        h.src.start();
        pump(20);
        // Publish AFTER start: mapRing() baselines write_count to whatever is
        // already in the ring, deliberately, so that attaching to a
        // long-running receiver does not report its whole history as drops.
        for (int i = 0; i < 10; ++i) { ring.publishTone(5e6); pump(4); }
        CHECK(h.errors.isEmpty(), "opening the configured path reports no error%s",
              h.errors.isEmpty() ? "" : qPrintable(": " + h.errors.join("; ")));
        CHECK(h.src.name().contains(QString::fromStdString(p)),
              "the source names the configured ring: %s", qPrintable(h.src.name()));
        CHECK(!h.blocks.empty(), "samples arrive from the configured ring (%zu blocks)",
              h.blocks.size());
        h.src.stop();
        ring.destroy();
    }

    // -----------------------------------------------------------------
    std::printf("\nA known tone survives the ring and the decode\n");
    // -----------------------------------------------------------------
    {
        // End-to-end arithmetic check: a CW tone written as 14-bit
        // MSB-aligned int16 must come back out at the same frequency after
        // the reader and SampleCodec have both handled it.
        const double fs   = 100e6;
        const double tone = -12.5e6;          // negative, to catch I/Q swaps
        const std::uint32_t N = 16384;

        t::FakeIqRing ring;
        const std::string p = ringPath("tone");
        ring.create(p, 8u, N, fs, 0.0);

        Harness h(p);
        h.src.start();
        // Publish enough frames that the 2 ms poll certainly catches some.
        for (int i = 0; i < 40; ++i) { ring.publishTone(tone); pump(3); }
        h.src.stop();

        CHECK(!h.blocks.empty(), "blocks received from the ring: %zu", h.blocks.size());
        if (!h.blocks.empty()) {
            const auto& b = h.blocks.back();
            CHECK(b.rawFormat == SampleFormat::Cs16,
                  "wire format reported as Cs16 (interleaved int16 I/Q)");
            CHECK(b.rawChannels == 1, "single channel, as the ring ABI defines");
            CHECK(std::size_t(b.raw.size()) == std::size_t(N) * 4u,
                  "payload is %u samples x 4 B = %u B, got %d",
                  N, N * 4u, b.raw.size());

            std::vector<IQBlock> ch;
            codec::decode(b.raw.constData(), std::size_t(b.raw.size()),
                          b.rawFormat, b.rawChannels, b.rawFullScale, ch);
            CHECK(!ch.empty() && ch[0].size() == N,
                  "decode yields %u samples", N);
            if (!ch.empty() && ch[0].size() == N) {
                const t::Spectrum s = t::analyse(ch[0], fs);
                const double errHz = std::fabs(s.peakHz - tone);
                const double binHz = fs / double(N);
                CHECK(errHz < binHz,
                      "measured tone %.4f MHz, expected %.4f MHz (error %.3f kHz, bin %.3f kHz)",
                      s.peakHz / 1e6, tone / 1e6, errHz / 1e3, binHz / 1e3);
                CHECK(s.sfdrDb > 40.0,
                      "spur-free dynamic range %.1f dB (> 40 dB means a clean single tone)",
                      s.sfdrDb);
            }
        }
        ring.destroy();
    }

    // -----------------------------------------------------------------
    std::printf("\nOverwriting ring: newest frame wins, skips are counted\n");
    // -----------------------------------------------------------------
    {
        // The producer never back-pressures, so a slow display must take the
        // newest slot and ACCOUNT for what it missed rather than hiding it.
        t::FakeIqRing ring;
        const std::string p = ringPath("drop");
        ring.create(p, 8u, 4096u, 100e6, 0.0);

        Harness h(p);
        h.src.start();
        pump(20);

        ring.publishTone(1e6);
        pump(20);
        const std::size_t afterFirst = h.blocks.size();
        CHECK(afterFirst >= 1, "first frame is delivered");

        // Five frames appear between polls; only the newest may be published.
        ring.skipFrames(5);
        ring.publishTone(1e6);
        pump(40);

        CHECK(h.blocks.size() == afterFirst + 1,
              "6 frames published between polls -> exactly 1 more block delivered "
              "(%zu -> %zu)", afterFirst, h.blocks.size());
        h.src.stop();
        ring.destroy();
    }

    // -----------------------------------------------------------------
    std::printf("\nMalformed and half-built rings are refused, not displayed\n");
    // -----------------------------------------------------------------
    {
        // A ring whose magic has not been written yet. The producer writes it
        // LAST, so this is what a reader sees if it maps during setup.
        t::FakeIqRing ring;
        const std::string p = ringPath("nomagic");
        ring.create(p, 8u, 4096u, 100e6, 0.0);
        ring.clearMagic();

        Harness h(p);
        h.src.start();
        pump(30);
        CHECK(!h.errors.isEmpty(), "a ring with no magic is refused with a reason");
        if (!h.errors.isEmpty())
            CHECK(h.errors.first().contains(QLatin1String("IQRING01")),
                  "the refusal names the expected magic: %s",
                  qPrintable(h.errors.first().left(72)));
        CHECK(h.blocks.empty(), "nothing is published from an invalid ring");
        h.src.stop();
        ring.destroy();
    }
    {
        // Geometry that does not fit the mapping must be caught before any
        // slot pointer is formed, or the reader walks off the end.
        t::FakeIqRing ring;
        const std::string p = ringPath("geomfit");
        ring.create(p, 8u, 4096u, 100e6, 0.0);
        ring.setNumSlots(4096u);              // far more than the file holds

        Harness h(p);
        h.src.start();
        pump(30);
        CHECK(!h.errors.isEmpty() && h.blocks.empty(),
              "geometry larger than the mapping is refused");
        if (!h.errors.isEmpty())
            CHECK(h.errors.first().contains(QLatin1String("exceeds the mapping")),
                  "the refusal explains why: %s", qPrintable(h.errors.first().left(72)));
        h.src.stop();
        ring.destroy();
    }
    {
        // A torn frame: write_count advanced but the frame magic is wrong.
        // It must be skipped silently, not plotted as a glitch.
        t::FakeIqRing ring;
        const std::string p = ringPath("torn");
        ring.create(p, 8u, 4096u, 100e6, 0.0);

        Harness h(p);
        h.src.start();
        pump(20);
        ring.publishTone(1e6);
        ring.corruptNewestFrameMagic();
        pump(40);
        CHECK(h.blocks.empty(),
              "a frame with a bad header magic is discarded (%zu blocks)",
              h.blocks.size());
        CHECK(h.errors.isEmpty(), "and it is not reported as a source error");
        h.src.stop();
        ring.destroy();
    }
    {
        // A missing ring is the commonest operator situation: no receiver is
        // running. The message must say so and name the path.
        Harness h("/dev/shm/ts11_definitely_absent");
        h.src.start();
        pump(20);
        CHECK(!h.errors.isEmpty(), "a missing ring is refused");
        if (!h.errors.isEmpty())
            CHECK(h.errors.first().contains(QLatin1String("rdma_rx")),
                  "the message points at the receiver: %s",
                  qPrintable(h.errors.first().left(72)));
        h.src.stop();
    }

    // -----------------------------------------------------------------
    std::printf("\nGPUDirect ring (IQRINGG1)\n");
    // -----------------------------------------------------------------
    {
        // A CPU-only build must DECLINE a VRAM ring with the reason, never
        // display zeros and never silently fall back from GPU to CPU.
        t::FakeGpuRing gpu;
        const std::string p = ringPath("gpuring");
        CHECK(gpu.create(p), "create an IQRINGG1 control block");

        Harness h(p);
        h.src.start();
        pump(30);
#ifdef SDR_ENABLE_CUDA
        // With CUDA compiled in there is still no GPU here, so the IPC open
        // must fail with an explanation rather than crash or publish.
        CHECK(!h.errors.isEmpty(), "CUDA build with no device declines the VRAM ring");
        CHECK(h.blocks.empty(), "and publishes nothing");
#else
        CHECK(!h.errors.isEmpty(), "non-CUDA build declines the VRAM ring");
        if (!h.errors.isEmpty()) {
            const QString e = h.errors.first();
            CHECK(e.contains(QLatin1String("GPUDirect")),
                  "the refusal says the payload is in VRAM");
            CHECK(e.contains(QLatin1String("SDR_ENABLE_CUDA")),
                  "and names the build flag that enables it: %s", qPrintable(e.left(72)));
        }
        CHECK(h.blocks.empty(), "nothing is published -- no zeros, no silent fallback");
#endif
        h.src.stop();
        gpu.destroy();
    }

    // -----------------------------------------------------------------
    std::printf("\nStop releases the ring\n");
    // -----------------------------------------------------------------
    {
        t::FakeIqRing ring;
        const std::string p = ringPath("stop");
        ring.create(p, 8u, 4096u, 100e6, 0.0);
        Harness h(p);
        h.src.start();
        pump(20);
        ring.publishTone(1e6);
        pump(20);
        const std::size_t before = h.blocks.size();
        h.src.stop();
        // After stop the poll timer must be dead: new frames go unread.
        for (int i = 0; i < 5; ++i) { ring.publishTone(1e6); pump(5); }
        CHECK(h.blocks.size() == before,
              "no blocks arrive after stop() (%zu -> %zu)", before, h.blocks.size());
        CHECK(h.statuses.join(QLatin1Char('|')).contains(QLatin1String("released")),
              "stop() reports the ring released");
        ring.destroy();
    }

    std::printf("\nRESULT: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
