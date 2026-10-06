// ---------------------------------------------------------------------------
// End-to-end PCIe control test, no hardware: the GUI's real RfdcControl talks
// to the firmware's real pcie_cfg.c through a shared file standing in for
// BAR2 (ring +0xA8, cfg +0xAC). The DAC input switch is modelled at register
// level with the bitstream's wiring, so every assertion is about what the DAC
// would actually play.
//
// Built against current firmware (emu_test) and against the 1.2.0 firmware
// (emu_test_legacy), which proves the GUI routes correctly on a board that
// has not been reflashed yet.
// ---------------------------------------------------------------------------
#include "RfdcControl.h"
#include "pcie_regs.h"

#include <QCoreApplication>
#include <QVariantMap>
#include <atomic>
#include <cstdio>
#include <thread>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
int  PcieCfg_Init(void* rfdc);
int  PcieCfg_Poll(void);
void PcieCfg_SetBase(uintptr_t base);
extern char RFdcInst;             // address only
void emu_board_boot(void);
int  emu_dac_plays(void);         // 0 host, 1 DDS, -1 nothing
}

static int g_fail = 0;
#define CHECK(c, ...) do { std::printf((c) ? "  PASS  " : "  FAIL  "); std::printf(__VA_ARGS__); \
                           std::printf("\n"); if (!(c)) ++g_fail; } while (0)

static const char* plays(int p) { return p == 1 ? "DDS" : p == 0 ? "host stream" : "nothing"; }

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
#ifdef LEGACY
    std::printf("PCIe end to end — GUI 1.2.1 against FIRMWARE 1.2.0 (not yet reflashed)\n\n");
#else
    std::printf("PCIe end to end — GUI 1.2.1 against firmware 1.2.1\n\n");
#endif

    // "BAR2": a 4 KiB file, mapped by the firmware side, pread/pwrite by the GUI.
    char path[] = "/tmp/pcie_emu_barXXXXXX";
    const int fd = ::mkstemp(path);
    if (fd < 0 || ::ftruncate(fd, 4096) != 0) { std::perror("bar file"); return 2; }
    void* bar = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    emu_board_boot();
#ifdef LEGACY
    CHECK(emu_dac_plays() == 0, "1.2.0 boot: the DAC plays the %s (the old bug)", plays(emu_dac_plays()));
#else
    CHECK(emu_dac_plays() == 1, "boot: the DAC plays the %s", plays(emu_dac_plays()));
#endif

    PcieCfg_SetBase(reinterpret_cast<uintptr_t>(bar));
    PcieCfg_Init(&RFdcInst);
    std::atomic<bool> stop{false};
    std::thread fw([&]{ while (!stop) { PcieCfg_Poll(); std::this_thread::sleep_for(std::chrono::microseconds(50)); } });

    sdr::RfdcControl rc;
    int confirmed = -2;
    QObject::connect(&rc, &sdr::RfdcControl::dacSourceConfirmed, [&](int s){ confirmed = s; });
    QString notes;
    QObject::connect(&rc, &sdr::RfdcControl::statusMessage, [&](const QString& m){ notes += m + '\n'; });
    bool synced = false;
    QObject::connect(&rc, &sdr::RfdcControl::syncFinished, [&](bool ok, const QString&){ synced = ok; });
    QVariantMap snap;
    QObject::connect(&rc, &sdr::RfdcControl::snapshotReady, [&](const QVariantMap& m){ snap = m; });

    CHECK(rc.attachCharDev(QString::fromLatin1(path), 0), "GUI attached to the emulated BAR");
    rc.syncAttached(0, 0, true);
    CHECK(synced, "full sync: ping, identity, every register read back");
    CHECK(snap.value(QStringLiteral("count")).toInt() >= 14, "snapshot returned %d values", snap.value(QStringLiteral("count")).toInt());
    CHECK(snap.value(QStringLiteral("dac_stream_khz")).toUInt() == 200000U,
          "DAC stream clock read back: %u kHz (the generator's sample rate)",
          snap.value(QStringLiteral("dac_stream_khz")).toUInt());
#ifdef LEGACY
    CHECK(notes.contains(QStringLiteral("predates 1.2.1")), "old firmware detected from its capability word");
    CHECK(snap.value(QStringLiteral("dac_source")).toInt() == int(PCIE_DAC_SRC_HOST),
          "readback shows the truth: the DAC is on the host stream (firmware itself claims DDS)");
#else
    CHECK(!notes.contains(QStringLiteral("predates")), "current firmware: no compensation");
    CHECK(snap.value(QStringLiteral("dac_source")).toInt() == int(PCIE_DAC_SRC_DDS), "readback: DDS");
#endif

    CHECK(rc.setDacSource(int(PCIE_DAC_SRC_HOST)), "GUI selects Host / GNU Radio stream");
    CHECK(emu_dac_plays() == 0, "  -> the DAC plays the %s", plays(emu_dac_plays()));
    CHECK(confirmed == int(PCIE_DAC_SRC_HOST), "  -> the device confirms host");

    CHECK(rc.setDacSource(int(PCIE_DAC_SRC_DDS)), "GUI selects DDS compiler");
    CHECK(emu_dac_plays() == 1, "  -> the DAC plays the %s", plays(emu_dac_plays()));
    CHECK(confirmed == int(PCIE_DAC_SRC_DDS), "  -> the device confirms DDS");

    rc.setDacSource(int(PCIE_DAC_SRC_HOST));
    CHECK(rc.restoreDefaults(), "restore defaults");
#ifdef LEGACY
    CHECK(emu_dac_plays() == 0, "  -> 1.2.0 defaults put the %s on the DAC (its bug), and the GUI will show that", plays(emu_dac_plays()));
#else
    CHECK(emu_dac_plays() == 1, "  -> defaults put the %s on the DAC", plays(emu_dac_plays()));
#endif
    rc.readSnapshot(0, 0, true);
    const int shown = snap.value(QStringLiteral("dac_source")).toInt();
    CHECK((shown == int(PCIE_DAC_SRC_DDS)) == (emu_dac_plays() == 1),
          "after defaults the panel shows what the DAC really plays (%s)", plays(emu_dac_plays()));

    // Raw protocol: an invalid source is rejected with an error in the ACK.
    quint32 rej = 0;
    {
        // Drive the registers directly, like rfdc_ctl does.
        const quint32 ring = RING_SET(TARGET, PCIE_TARGET_DAC) | RING_SET(TILE, PCIE_TILE_ALL)
                           | RING_LASTTILE_MASK | RING_SET(CHANNEL, PCIE_CHAN_BOTH)
                           | RING_SET(EVENT, PCIE_EVT_DAC_SOURCE);
        rc.writeRegister(PCIE_REG_CFG, 7);
        rc.writeRegister(PCIE_REG_RING, ring | RING_NEWCMD_MASK);
        for (int i = 0; i < 2000; ++i) { rc.readRegister(PCIE_REG_RING, &rej); if (!(rej & RING_NEWCMD_MASK)) break; ::usleep(500); }
    }
    CHECK(RING_ACK_VALID(rej) && RING_ACK_ERR(rej) == PCIE_ERR_BAD_PAYLOAD, "DAC source 7 rejected: ACK error 0x%x", RING_ACK_ERR(rej));

    stop = true; fw.join();
    ::munmap(bar, 4096); ::close(fd); ::unlink(path);
    std::printf("\nRESULT: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
