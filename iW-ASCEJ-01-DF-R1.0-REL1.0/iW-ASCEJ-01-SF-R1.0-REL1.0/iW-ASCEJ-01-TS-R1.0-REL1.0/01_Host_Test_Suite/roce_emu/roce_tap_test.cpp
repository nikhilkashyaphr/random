// ---------------------------------------------------------------------------
// TS-11  RoCEv2 extractor tap (RTAP) through the real RoceShmSource.
//
// The tap is the second RoCEv2 producer and its ABI differs from IQRING in the
// two ways that matter for correctness:
//
//   1. The header carries the sample FORMAT, channel count, rate, centre and
//      full scale. The GUI must ADOPT them, not assume int16 I/Q -- plotting
//      the wrong decode looks like a signal problem and costs far more time
//      than an error message.
//   2. Each slot is published under a SEQLOCK: meta.seq is odd while the
//      payload is being copied. A slot overwritten mid-read must be discarded,
//      because a torn frame plots as a believable signal artefact.
//
// Both are checked here against the shipping reader, with no NIC.
// ---------------------------------------------------------------------------
#include "RoceShmSource.h"
#include "SampleCodec.h"
#include "fake_ring.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <cstdio>
#include <string>
#include <vector>

using namespace sdr;

static int g_fail = 0;
#define CHECK(c, ...) do { std::printf((c) ? "  PASS  " : "  FAIL  "); std::printf(__VA_ARGS__); \
                           std::printf("\n"); if (!(c)) ++g_fail; } while (0)

namespace {

std::string tapPath(const char* tag)
{
    return "/dev/shm/ts11tap_" + std::string(tag) + "_" + std::to_string(::getpid());
}

void pump(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

struct Harness {
    RoceShmSource src;
    std::vector<SampleBlock> blocks;
    QStringList errors, statuses;

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
};

/// A slot of interleaved int16 I/Q, 14-bit MSB-aligned.
std::vector<std::int16_t> cint16Slot(std::size_t samples)
{
    std::vector<std::int16_t> v(samples * 2u);
    for (std::size_t i = 0; i < samples; ++i) {
        v[2 * i    ] = std::int16_t((std::int16_t(i % 4000) - 2000) << 2);
        v[2 * i + 1] = std::int16_t((std::int16_t((i + 7) % 4000) - 2000) << 2);
    }
    return v;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::printf("RoCEv2 extractor tap (RTAP) -> RoceShmSource\n\n");

    // -----------------------------------------------------------------
    std::printf("The tap is detected by its magic, not by an operator setting\n");
    // -----------------------------------------------------------------
    {
        // RTAP's magic is a 32-bit "RTAP" at offset 0, so a 64-bit read there
        // matches neither IQRING constant. The reader must test the 32-bit one
        // first or the tap looks like a corrupt IQRING.
        t::FakeTapRing tap;
        const std::string p = tapPath("detect");
        const auto slot = cint16Slot(2048);
        const std::uint32_t slotBytes = std::uint32_t(slot.size() * 2u);
        CHECK(tap.create(p, 8u, slotBytes, 1u, t::TapCInt16, 61.44e6, 2.4e9, 1.0),
              "create an RTAP ring, cint16, 61.44 MSPS, 2.4 GHz");
        Harness h(p);
        h.src.start();
        pump(20);
        // After start(): mapRoceTap() baselines `produced`, deliberately, so
        // attaching to a running extractor does not replay its history.
        for (int i = 0; i < 10; ++i) { tap.publish(slot.data(), slotBytes); pump(4); }
        CHECK(h.errors.isEmpty(), "the tap is accepted%s",
              h.errors.isEmpty() ? "" : qPrintable(": " + h.errors.join("; ")));
        CHECK(h.src.name().contains(QLatin1String("extractor tap")),
              "the source identifies itself as the tap: %s", qPrintable(h.src.name()));
        CHECK(!h.blocks.empty(), "samples arrive (%zu blocks)", h.blocks.size());
        if (!h.blocks.empty()) {
            const auto& b = h.blocks.back();
            CHECK(b.rawFormat == SampleFormat::Cs16,
                  "dtype TAP_DT_CINT16 maps to SampleFormat::Cs16");
            CHECK(b.rawChannels == 1, "channel count adopted from the header");
            CHECK(std::uint32_t(b.raw.size()) == slotBytes,
                  "the whole declared slot length is delivered (%u B)", slotBytes);
        }
        h.src.stop();
        tap.destroy();
    }

    // -----------------------------------------------------------------
    std::printf("\nRate, centre, channels and full scale come from the header\n");
    // -----------------------------------------------------------------
    {
        t::FakeTapRing tap;
        const std::string p = tapPath("hdr");
        const auto slot = cint16Slot(1024);
        const std::uint32_t slotBytes = std::uint32_t(slot.size() * 2u);
        tap.create(p, 4u, slotBytes, 2u, t::TapCInt16, 122.88e6, 5.8e9, 8191.0);

        double rateMsps = 0.0, centerGHz = 0.0;
        const bool ok = RoceShmSource::peekRingInfo(QString::fromStdString(p),
                                                    &rateMsps, &centerGHz);
        CHECK(ok, "peekRingInfo understands the RTAP header too");
        CHECK(std::fabs(rateMsps - 122.88) < 1e-6,
              "rate %.4f MSPS adopted (expected 122.8800)", rateMsps);
        CHECK(std::fabs(centerGHz - 5.8) < 1e-9,
              "centre %.4f GHz adopted (expected 5.8000)", centerGHz);

        Harness h(p);
        h.src.start();
        pump(20);
        for (int i = 0; i < 10; ++i) { tap.publish(slot.data(), slotBytes); pump(4); }
        if (!h.blocks.empty()) {
            const auto& b = h.blocks.back();
            CHECK(b.rawChannels == 2, "2 channels adopted from the header, got %d",
                  b.rawChannels);
            CHECK(std::fabs(b.rawFullScale - 8191.0) < 1e-9,
                  "full scale %.1f adopted from the header (not defaulted to 1.0)",
                  b.rawFullScale);
        } else {
            CHECK(false, "a block arrived to inspect");
        }
        h.src.stop();
        tap.destroy();
    }

    // -----------------------------------------------------------------
    std::printf("\nEvery declared dtype maps to the matching decode\n");
    // -----------------------------------------------------------------
    {
        struct Case { std::uint32_t dtype; SampleFormat fmt; const char* name; };
        const Case cases[] = {
            { t::TapCInt16,  SampleFormat::Cs16, "TAP_DT_CINT16  -> Cs16" },
            { t::TapInt16,   SampleFormat::Rs16, "TAP_DT_INT16   -> Rs16" },
            { t::TapInt8,    SampleFormat::Rs8,  "TAP_DT_INT8    -> Rs8"  },
            { t::TapInt32,   SampleFormat::Rs32, "TAP_DT_INT32   -> Rs32" },
            { t::TapFloat32, SampleFormat::Rf32, "TAP_DT_FLOAT32 -> Rf32" },
        };
        for (const auto& c : cases) {
            t::FakeTapRing tap;
            const std::string p = tapPath("dtype");
            const std::uint32_t slotBytes = 4096u;
            tap.create(p, 4u, slotBytes, 1u, c.dtype, 100e6, 0.0, 1.0);
            std::vector<char> payload(slotBytes, 0x11);

            Harness h(p);
            h.src.start();
            pump(20);
            for (int k = 0; k < 6; ++k) { tap.publish(payload.data(), slotBytes); pump(4); }
            CHECK(!h.blocks.empty() && h.blocks.back().rawFormat == c.fmt,
                  "%s", c.name);
            h.src.stop();
            tap.destroy();
        }
    }
    {
        // An unknown dtype must be refused. Defaulting it would plot a wrong
        // decode that looks like a hardware fault.
        t::FakeTapRing tap;
        const std::string p = tapPath("baddtype");
        tap.create(p, 4u, 4096u, 1u, 99u, 100e6, 0.0, 1.0);
        Harness h(p);
        h.src.start();
        pump(40);
        CHECK(!h.errors.isEmpty(), "an unrecognised dtype is refused");
        if (!h.errors.isEmpty())
            CHECK(h.errors.first().contains(QLatin1String("dtype 99")),
                  "the refusal names the dtype: %s", qPrintable(h.errors.first().left(72)));
        CHECK(h.blocks.empty(), "and nothing is decoded");
        h.src.stop();
        tap.destroy();
    }

    // -----------------------------------------------------------------
    std::printf("\nSeqlock: a slot overwritten mid-read is discarded\n");
    // -----------------------------------------------------------------
    {
        t::FakeTapRing tap;
        const std::string p = tapPath("torn");
        const auto slot = cint16Slot(1024);
        const std::uint32_t slotBytes = std::uint32_t(slot.size() * 2u);
        tap.create(p, 4u, slotBytes, 1u, t::TapCInt16, 100e6, 0.0, 1.0);

        Harness h(p);
        h.src.start();
        pump(20);

        // Published with the seqlock left ODD -- the producer is still
        // copying. The reader must not take it.
        tap.publishTorn(slot.data(), slotBytes);
        pump(40);
        CHECK(h.blocks.empty(),
              "a slot whose seqlock is odd (mid-write) is skipped (%zu blocks)",
              h.blocks.size());

        // A clean publish afterwards must still be delivered, proving the
        // reader skipped rather than wedged.
        tap.publish(slot.data(), slotBytes);
        pump(40);
        CHECK(!h.blocks.empty(), "the next complete slot is delivered normally");
        h.src.stop();
        tap.destroy();
    }

    // -----------------------------------------------------------------
    std::printf("\nMalformed tap headers are refused\n");
    // -----------------------------------------------------------------
    {
        // nslots must be a power of two: the producer masks with nslots-1, so
        // anything else aliases slots onto each other.
        t::FakeTapRing tap;
        const std::string p = tapPath("npow2");
        tap.create(p, 6u, 4096u, 1u, t::TapCInt16, 100e6, 0.0, 1.0);
        Harness h(p);
        h.src.start();
        pump(40);
        CHECK(!h.errors.isEmpty(), "a non-power-of-two nslots is refused");
        if (!h.errors.isEmpty())
            CHECK(h.errors.first().contains(QLatin1String("power of two")),
                  "the refusal explains the masking: %s",
                  qPrintable(h.errors.first().left(72)));
        h.src.stop();
        tap.destroy();
    }
    {
        // A version the build does not understand must be refused, not parsed
        // hopefully with offsets that may have moved.
        t::FakeTapRing tap;
        const std::string p = tapPath("ver");
        tap.create(p, 4u, 4096u, 1u, t::TapCInt16, 100e6, 0.0, 1.0, /*version=*/7u);
        Harness h(p);
        h.src.start();
        pump(40);
        CHECK(!h.errors.isEmpty(), "an unknown RTAP version is refused");
        if (!h.errors.isEmpty())
            CHECK(h.errors.first().contains(QLatin1String("version 7")),
                  "the refusal names the version: %s",
                  qPrintable(h.errors.first().left(72)));
        h.src.stop();
        tap.destroy();
    }
    {
        // A slot claiming more bytes than slot_bytes must be rejected, or the
        // reader copies past the end of the slot.
        t::FakeTapRing tap;
        const std::string p = tapPath("overlong");
        const std::uint32_t slotBytes = 4096u;
        tap.create(p, 4u, slotBytes, 1u, t::TapCInt16, 100e6, 0.0, 1.0);
        Harness h(p);
        h.src.start();
        pump(20);
        tap.publishOverlongLen(slotBytes * 4u);
        pump(40);
        CHECK(h.blocks.empty(),
              "a slot declaring %u B in a %u B slot is rejected", slotBytes * 4u, slotBytes);
        h.src.stop();
        tap.destroy();
    }

    // -----------------------------------------------------------------
    std::printf("\nProducer exit is surfaced, not shown as a hang\n");
    // -----------------------------------------------------------------
    {
        t::FakeTapRing tap;
        const std::string p = tapPath("exit");
        const auto slot = cint16Slot(512);
        const std::uint32_t slotBytes = std::uint32_t(slot.size() * 2u);
        tap.create(p, 4u, slotBytes, 1u, t::TapCInt16, 100e6, 0.0, 1.0);
        tap.publish(slot.data(), slotBytes);

        Harness h(p);
        h.src.start();
        pump(40);
        tap.setRunning(0u);                 // roce-extractor has exited
        pump(40);
        CHECK(h.statuses.join(QLatin1Char('|')).contains(QLatin1String("stopped publishing")),
              "the GUI is told the extractor stopped publishing");
        h.src.stop();
        tap.destroy();
    }

    std::printf("\nRESULT: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
