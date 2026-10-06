// ---------------------------------------------------------------------------
// Simulation: the simulated source must show what the transmit settings would
// put through a DAC -> ADC loopback, exactly as the board does.
//
//   DDS selected                        -> DDS tone
//   Host selected, generator running    -> the generator's waveform (same
//                                          samples the FIFO would carry)
//   Host selected, nothing transmitting -> noise only
//   loopback off                        -> the original modem demo, unchanged
// ---------------------------------------------------------------------------
#include "Sources.h"
#include "IqGenerator.h"
#include "pcie_regs.h"
#include "spectrum.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>

using namespace sdr;

static int g_fail = 0;
#define CHECK(c, ...) do { std::printf((c) ? "  PASS  " : "  FAIL  "); std::printf(__VA_ARGS__); \
                           std::printf("\n"); if (!(c)) ++g_fail; } while (0)

// Run the simulator until `n` contiguous samples of channel 0 are collected.
static std::vector<cf32> capture(SimulatedSource& src, std::size_t n)
{
    std::vector<cf32> out;
    QEventLoop loop;
    auto c = QObject::connect(&src, &ISignalSource::blockReady, &loop,
        [&](const SampleBlock& b) {
            const auto& ch = b.channels.front();
            out.insert(out.end(), ch.begin(), ch.end());
            if (out.size() >= n) loop.quit();
        });
    QTimer::singleShot(20000, &loop, &QEventLoop::quit);
    src.start();
    loop.exec();
    src.stop();
    QObject::disconnect(c);
    out.resize(std::min(out.size(), n));
    return out;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::printf("Simulated RF loopback (SimulatedSource + TxState)\n\n");

    Config cfg;                       // defaults: 122.88 MSPS display, SNR 25 dB
    cfg.src.streamChannels = 1;
    const double fs = cfg.acq.displayRateHz();
    const std::size_t N = 65536;
    const double bin = fs / double(N);

    TxState tx;
    tx.loopback = true;
    tx.dacStreamSps = 200e6;
    tx.loopPairs = 4096;

    {   // DDS
        SimulatedSource src; src.applyConfig(cfg);
        tx.dacSource = int(PCIE_DAC_SRC_DDS); tx.ddsHz = 10e6; tx.hostRunning = false;
        src.setTxState(tx);
        auto s = t::analyse(capture(src, N), fs);
        CHECK(std::abs(s.peakHz - 10e6) < 2 * bin, "DDS selected: tone at %+.4f MHz (DDS 10 MHz, positive frequency)", s.peakHz / 1e6);
        CHECK(s.peakDb - s.medianDb > 40.0, "DDS selected: tone %.1f dB above the floor", s.peakDb - s.medianDb);
    }
    {   // DDS selected while the generator runs: the DAC still plays the DDS
        SimulatedSource src; src.applyConfig(cfg);
        tx.dacSource = int(PCIE_DAC_SRC_DDS); tx.ddsHz = 15e6; tx.hostRunning = true; tx.hostToneHz = 5e6;
        src.setTxState(tx);
        auto s = t::analyse(capture(src, N), fs);
        CHECK(std::abs(s.peakHz - 15e6) < 2 * bin, "DDS selected, generator running: DDS wins (%+.4f MHz)", s.peakHz / 1e6);
    }
    {   // Host, generator running, sine
        IqGenerator::Config g; g.sampleRateSps = 200e6; g.frequencyHz = 7e6; g.chunkBytes = 16384;
        const double act = IqGenerator::transmittedHz(g);
        SimulatedSource src; src.applyConfig(cfg);
        tx.dacSource = int(PCIE_DAC_SRC_HOST); tx.hostRunning = true;
        tx.waveform = 0; tx.hostToneHz = act; tx.amplitude = 0.9; tx.coherent = true;
        src.setTxState(tx);
        auto s = t::analyse(capture(src, N), fs);
        CHECK(std::abs(s.peakHz - act) < 2 * bin, "Host + generator: tone at %+.4f MHz (generator transmits %.6f MHz)", s.peakHz / 1e6, act / 1e6);
        CHECK(s.peakDb - s.medianDb > 40.0, "Host + generator: tone %.1f dB above the floor", s.peakDb - s.medianDb);
    }
    {   // Host, generator running, the 16 MiB loop (4 M samples) — no table
        IqGenerator::Config g; g.sampleRateSps = 200e6; g.frequencyHz = 10e6; g.chunkBytes = 16 << 20;
        const double act = IqGenerator::transmittedHz(g);
        SimulatedSource src; src.applyConfig(cfg);
        TxState big = tx;
        big.dacSource = int(PCIE_DAC_SRC_HOST); big.hostRunning = true; big.waveform = 0;
        big.hostToneHz = act; big.loopPairs = (16 << 20) / 4;
        src.setTxState(big);
        auto s = t::analyse(capture(src, N), fs);
        CHECK(std::abs(s.peakHz - act) < 2 * bin, "Host + generator, 16 MiB loop: tone at %+.4f MHz", s.peakHz / 1e6);
    }
    {   // Host, generator running, negative frequency keeps its sign
        SimulatedSource src; src.applyConfig(cfg);
        tx.dacSource = int(PCIE_DAC_SRC_HOST); tx.hostRunning = true; tx.waveform = 0;
        tx.hostToneHz = -4.98046875e6;   // coherent at 200 MSPS / 4096
        src.setTxState(tx);
        auto s = t::analyse(capture(src, N), fs);
        CHECK(std::abs(s.peakHz - tx.hostToneHz) < 2 * bin, "Host + generator, negative tone: %+.4f MHz", s.peakHz / 1e6);
    }
    {   // Host, square wave: fundamental plus odd harmonics
        SimulatedSource src; src.applyConfig(cfg);
        tx.dacSource = int(PCIE_DAC_SRC_HOST); tx.hostRunning = true;
        tx.waveform = 2; tx.hostToneHz = 4.00390625e6;   // 82 cycles per loop
        src.setTxState(tx);
        auto x = capture(src, N);
        auto s = t::analyse(x, fs);
        CHECK(std::abs(s.peakHz - tx.hostToneHz) < 2 * bin, "Host + square wave: fundamental at %+.4f MHz", s.peakHz / 1e6);
        CHECK(s.sfdrDb < 20.0, "Host + square wave: harmonics present (SFDR %.1f dB, a sine would be >40)", s.sfdrDb);
    }
    {   // Host selected, nothing transmitting
        SimulatedSource src; src.applyConfig(cfg);
        tx.dacSource = int(PCIE_DAC_SRC_HOST); tx.hostRunning = false; tx.waveform = 0;
        src.setTxState(tx);
        auto s = t::analyse(capture(src, N), fs);
        // Only the receiver's small DC offset remains; no carrier anywhere.
        CHECK(std::abs(s.peakHz) < 4 * bin || s.peakDb - s.medianDb < 15.0,
              "Host, nothing transmitting: no carrier (peak %.1f dB over floor at %+.3f MHz — DC offset only)",
              s.peakDb - s.medianDb, s.peakHz / 1e6);
    }
    {   // Out of the displayed band: not shown (the DDC would filter it)
        SimulatedSource src; src.applyConfig(cfg);
        tx.dacSource = int(PCIE_DAC_SRC_DDS); tx.ddsHz = 70e6;   // > 61.44 MHz
        QString note;
        QObject::connect(&src, &ISignalSource::statusMessage, [&](const QString& m){
            if (m.contains(QStringLiteral("loopback"))) note = m; });
        src.setTxState(tx);
        auto s = t::analyse(capture(src, N), fs);
        CHECK(std::abs(s.peakHz - 70e6 + fs) > 4 * bin, "DDS outside the displayed band: not aliased into view");
        CHECK(note.contains(QStringLiteral("outside")), "  ...and the status bar says why: \"%s\"", qPrintable(note));
    }
    {   // Loopback off: the original simulator, untouched
        SimulatedSource src; src.applyConfig(cfg);
        TxState off; off.loopback = false;
        src.setTxState(off);
        auto x = capture(src, N);
        double p = 0; for (auto& v : x) p += std::norm(v); p /= double(x.size());
        CHECK(x.size() == N && p > 0.01, "loopback off: modem demo still produced (mean power %.3f)", p);
    }

    std::printf("\nRESULT: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
