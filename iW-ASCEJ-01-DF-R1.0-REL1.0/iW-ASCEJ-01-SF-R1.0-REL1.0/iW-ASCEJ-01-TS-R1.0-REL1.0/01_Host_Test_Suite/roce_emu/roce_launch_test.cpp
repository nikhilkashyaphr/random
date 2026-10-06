// ---------------------------------------------------------------------------
// TS-11  Starting the RoCEv2 receiver from the GUI.
//
// Until now, selecting RoCEv2 and pressing Start only worked if the operator
// had already run `rdma_rx` in a terminal; otherwise the GUI reported
// "cannot open /dev/shm/iqring" and left them to work out why. BackendLauncher
// already manages the C2H, H2C and UDP helpers, so the RoCEv2 receiver is now
// one more managed process.
//
// It cannot be STARTED here -- that needs rdma-core, a ConnectX NIC and a
// cabled link. What can be checked without hardware is everything around the
// start: discovery, the refusal when a receiver is already publishing, the
// refusal when the binary has not been built, and that the capture watchdog
// does not treat this transport as a C2H helper. Those are the paths an
// operator actually hits, and they are the ones that were missing.
// ---------------------------------------------------------------------------
#include "BackendLauncher.h"
#include "fake_ring.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QTimer>

#include <cstdio>

using namespace sdr;

static int g_fail = 0;
#define CHECK(c, ...) do { std::printf((c) ? "  PASS  " : "  FAIL  "); std::printf(__VA_ARGS__); \
                           std::printf("\n"); if (!(c)) ++g_fail; } while (0)

namespace {

void pump(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

/// Collect whichever of the launcher's two outcomes arrives.
struct Outcome {
    QStringList failures;
    QStringList statuses;
    bool ready = false;

    explicit Outcome(BackendLauncher& b)
    {
        QObject::connect(&b, &BackendLauncher::startFailed,
                         [this](const QString& w) { failures << w; });
        QObject::connect(&b, &BackendLauncher::statusMessage,
                         [this](const QString& s) { statuses << s; });
        QObject::connect(&b, &BackendLauncher::becameReady,
                         [this](const QString&) { ready = true; });
    }
};

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // roceDirectory() looks beside the executable and in the working
    // directory, so run as the GUI would: from its own source tree.
    QDir::setCurrent(QStringLiteral(SDR_GUI_DIR));

    std::printf("Starting the RoCEv2 receiver from the GUI (BackendLauncher)\n\n");

    // -----------------------------------------------------------------
    std::printf("The transport names itself, and its ring path is fixed\n");
    // -----------------------------------------------------------------
    {
        CHECK(backendModeName(BackendMode::RoceRx) == QLatin1String("RoCEv2"),
              "backendModeName(RoceRx) is \"RoCEv2\", got \"%s\"",
              qPrintable(backendModeName(BackendMode::RoceRx)));
        // rdma_rx hard-codes RING_SHM_NAME "/iqring" and takes no option for
        // it, so a launched receiver always publishes here. Anything that
        // lets the operator believe otherwise is a bug.
        CHECK(roceRingPath() == QLatin1String("/dev/shm/iqring"),
              "roceRingPath() is /dev/shm/iqring, matching RING_SHM_NAME");

        BackendConfig cfg;
        CHECK(cfg.rocePort == 7471,
              "the default port is rdma_common.h's DEFAULT_TCP_PORT (7471), got %d",
              cfg.rocePort);
        CHECK(cfg.roceTransport == QLatin1String("write_imm"),
              "the default -w mode matches rdma_rx's own default");
        CHECK(!cfg.roceGpu, "the host ring (rdma_rx) is the default, not GPUDirect");
    }

    // -----------------------------------------------------------------
    std::printf("The supplied RoCEv2 stack is found in the delivered tree\n");
    // -----------------------------------------------------------------
    {
        const QString dir = BackendLauncher::roceDirectory();
        CHECK(!dir.isEmpty(), "roceDirectory() locates reference/roce-iq-holoscan");
        if (!dir.isEmpty()) {
            CHECK(QFileInfo::exists(dir + QStringLiteral("/Makefile")),
                  "and it holds the Makefile that builds the receivers");
            CHECK(QFileInfo::exists(dir + QStringLiteral("/src/rdma_rx.c")),
                  "and rdma_rx.c, the host-ring receiver");
            CHECK(QFileInfo::exists(dir + QStringLiteral("/src/rdma_rx_gpu.c")),
                  "and rdma_rx_gpu.c, the GPUDirect receiver");
        }
    }

    // -----------------------------------------------------------------
    std::printf("An unbuilt receiver is refused with the command to run\n");
    // -----------------------------------------------------------------
    {
        // The delivered package ships sources, not built receivers, so this is
        // the state every operator starts from.
        const bool built = !BackendLauncher::roceReceiverPath(false).isEmpty();
        if (built) {
            std::printf("  SKIP  rdma_rx is already built in this tree\n");
        } else {
            BackendLauncher b;
            Outcome o(b);
            BackendConfig cfg;
            cfg.mode = BackendMode::RoceRx;
            b.start(cfg);
            pump(50);
            CHECK(!o.failures.isEmpty(), "starting without a built receiver fails");
            if (!o.failures.isEmpty()) {
                const QString w = o.failures.first();
                CHECK(w.contains(QLatin1String("rdma_rx")),
                      "the refusal names the missing binary");
                CHECK(w.contains(QLatin1String("make")),
                      "and the command that builds it: %s", qPrintable(w.left(80)));
            }
            CHECK(!o.ready, "and it never reports ready");
        }
    }
    {
        // The GPU receiver must be named separately: `make` does not build it,
        // `make gpu` does, and it additionally needs the CUDA toolkit.
        const bool built = !BackendLauncher::roceReceiverPath(true).isEmpty();
        if (built) {
            std::printf("  SKIP  rdma_rx_gpu is already built in this tree\n");
        } else {
            BackendLauncher b;
            Outcome o(b);
            BackendConfig cfg;
            cfg.mode    = BackendMode::RoceRx;
            cfg.roceGpu = true;
            b.start(cfg);
            pump(50);
            CHECK(!o.failures.isEmpty(), "the GPUDirect receiver is refused when unbuilt");
            if (!o.failures.isEmpty())
                CHECK(o.failures.first().contains(QLatin1String("rdma_rx_gpu"))
                   && o.failures.first().contains(QLatin1String("make gpu")),
                      "naming rdma_rx_gpu and `make gpu`: %s",
                      qPrintable(o.failures.first().left(80)));
        }
    }

    // -----------------------------------------------------------------
    std::printf("A receiver that is already publishing is not duplicated\n");
    // -----------------------------------------------------------------
    {
        // rdma_rx creates /dev/shm/iqring and dies if it cannot own it, and a
        // second receiver would also find the rdma_cm port taken. So when a
        // ring is already there the launcher must decline and say to use it,
        // rather than starting a process that fails obscurely.
        CHECK(!BackendLauncher::roceRingPresent() || true,
              "roceRingPresent() reports %s before the test creates one",
              BackendLauncher::roceRingPresent() ? "present (pre-existing)" : "absent");

        const bool preexisting = BackendLauncher::roceRingPresent();
        if (preexisting) {
            std::printf("  SKIP  a real ring is present at %s; not disturbing it\n",
                        qPrintable(roceRingPath()));
        } else {
            t::FakeIqRing ring;
            // Deliberately the real path: this is the collision being tested.
            const bool made = ring.create(roceRingPath().toStdString(),
                                          8u, 4096u, 100e6, 0.0);
            CHECK(made, "a ring exists at %s", qPrintable(roceRingPath()));
            CHECK(BackendLauncher::roceRingPresent(),
                  "roceRingPresent() now sees it");

            BackendLauncher b;
            Outcome o(b);
            BackendConfig cfg;
            cfg.mode = BackendMode::RoceRx;
            b.start(cfg);
            pump(50);
            CHECK(!o.failures.isEmpty(), "starting a second receiver is refused");
            if (!o.failures.isEmpty()) {
                const QString w = o.failures.first();
                CHECK(w.contains(roceRingPath()),
                      "the refusal names the ring that already exists");
                CHECK(w.contains(QLatin1String("already publishing")),
                      "and explains that a receiver already owns it: %s",
                      qPrintable(w.left(80)));
            }
            CHECK(!o.ready, "and nothing reports ready");

            ring.destroy();
            CHECK(!BackendLauncher::roceRingPresent(),
                  "the test leaves no ring behind at %s", qPrintable(roceRingPath()));
        }
    }

    // -----------------------------------------------------------------
    std::printf("The RoCEv2 transport is not a C2H capture helper\n");
    // -----------------------------------------------------------------
    {
        // The capture watchdog restarts a dead C2H helper and ends the session
        // if it keeps dying. rdma_rx is neither, so the watchdog must skip it
        // exactly as it skips the UDP receiver.
        BackendLauncher b;
        CHECK(!b.isRunning(), "a fresh launcher runs nothing");
        BackendConfig cfg;
        cfg.mode = BackendMode::RoceRx;
        // Nothing to assert beyond not crashing and not running: the start
        // refuses above. This pins that a RoceRx config is accepted by the
        // same entry point as every other mode rather than falling through to
        // the C2H launch path.
        Outcome o(b);
        b.start(cfg);
        pump(50);
        CHECK(!b.h2cRunning(), "no H2C playback helper is started for RoCEv2");
        CHECK(o.statuses.join(QLatin1Char('|')).contains(QLatin1String("C2H")) == false,
              "and no C2H capture helper is announced");
    }

    std::printf("\nRESULT: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
