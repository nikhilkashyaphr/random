#pragma once
#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>
#include <complex>
#include <vector>
#include <memory>
#include <cstdint>
#include <algorithm>
#include <QAtomicInteger>

namespace sdr {

using cf32    = std::complex<float>;
using IQBlock = std::vector<cf32>;

// ---------------------------------------------------------------- enums

/// Roce = live RoCEv2 via the receiver's shared-memory ring (see
/// RoceShmSource). Kept distinct from Dma so the PCIe pre-flight, device
/// paths and status text stay correct for each transport.
enum class SourceMode   { Simulated, File, Dma, Roce };
enum class RunState     { Stopped, Running, Paused, Error };
/// Wire formats. Complex (interleaved I/Q) and real-valued variants, named
/// after the GNU Radio stream types they correspond to. Real formats are
/// lifted into the complex pipeline with Q = 0.
enum class SampleFormat {
    Cs8, Cu8, Cs12Packed, Cs16, Cs32, Cf32, Cf64,   // complex
    Rs8, Ru8, Rs16, Rs32, Rf32, Rf64                // real
};

/// True when the format carries only a real part.
inline bool isRealFormat(SampleFormat f)
{
    switch (f) {
    case SampleFormat::Rs8:  case SampleFormat::Ru8:  case SampleFormat::Rs16:
    case SampleFormat::Rs32: case SampleFormat::Rf32: case SampleFormat::Rf64:
        return true;
    default:
        return false;
    }
}
enum class WindowType   { Rectangular, Hann, Hamming, Blackman, BlackmanHarris, FlatTop, Kaiser };
enum class AverageMode  { None, Exponential, LinearN, MaxHold, MinHold };
enum class Modulation   { QPSK, QAM16, QAM64, NoiseOnly };
enum class ChannelView  { Active, Overlay };

/// Bytes occupied by one complex sample in the given wire format.
inline int bytesPerSample(SampleFormat f)
{
    switch (f) {
    case SampleFormat::Cs8:
    case SampleFormat::Cu8:        return 2;
    case SampleFormat::Cs12Packed: return 3;   // 12-bit I + 12-bit Q
    case SampleFormat::Cs16:       return 4;
    case SampleFormat::Cs32:       return 8;
    case SampleFormat::Cf32:       return 8;
    case SampleFormat::Cf64:       return 16;
    case SampleFormat::Rs8:
    case SampleFormat::Ru8:        return 1;
    case SampleFormat::Rs16:       return 2;
    case SampleFormat::Rs32:       return 4;
    case SampleFormat::Rf32:       return 4;
    case SampleFormat::Rf64:       return 8;
    }
    return 8;
}

inline QString formatName(SampleFormat f)
{
    switch (f) {
    case SampleFormat::Cf32:       return QStringLiteral("Complex Float 32");
    case SampleFormat::Cf64:       return QStringLiteral("Complex Float 64");
    case SampleFormat::Cs32:       return QStringLiteral("Complex Int 32");
    case SampleFormat::Cs16:       return QStringLiteral("Complex Int 16");
    case SampleFormat::Cs12Packed: return QStringLiteral("Complex Int 12 (packed)");
    case SampleFormat::Cs8:        return QStringLiteral("Complex Int 8");
    case SampleFormat::Cu8:        return QStringLiteral("Complex Byte (uint8)");
    case SampleFormat::Rf32:       return QStringLiteral("Float 32");
    case SampleFormat::Rf64:       return QStringLiteral("Float 64");
    case SampleFormat::Rs32:       return QStringLiteral("Int 32");
    case SampleFormat::Rs16:       return QStringLiteral("Short 16");
    case SampleFormat::Rs8:        return QStringLiteral("Byte (int8)");
    case SampleFormat::Ru8:        return QStringLiteral("Byte (uint8)");
    }
    return QStringLiteral("unknown");
}

/// Short GNU Radio-style tag, handy for status lines and metadata sidecars.
inline QString formatTag(SampleFormat f)
{
    switch (f) {
    case SampleFormat::Cf32:       return QStringLiteral("fc32");
    case SampleFormat::Cf64:       return QStringLiteral("fc64");
    case SampleFormat::Cs32:       return QStringLiteral("sc32");
    case SampleFormat::Cs16:       return QStringLiteral("sc16");
    case SampleFormat::Cs12Packed: return QStringLiteral("sc12");
    case SampleFormat::Cs8:        return QStringLiteral("sc8");
    case SampleFormat::Cu8:        return QStringLiteral("uc8");
    case SampleFormat::Rf32:       return QStringLiteral("f32");
    case SampleFormat::Rf64:       return QStringLiteral("f64");
    case SampleFormat::Rs32:       return QStringLiteral("s32");
    case SampleFormat::Rs16:       return QStringLiteral("s16");
    case SampleFormat::Rs8:        return QStringLiteral("s8");
    case SampleFormat::Ru8:        return QStringLiteral("u8");
    }
    return QStringLiteral("?");
}

/// Presentation order for the format combos: complex group first (the common
/// case for an SDR front end), then the real-valued group. Combos bind the
/// enum as item data rather than relying on this order, so it can change
/// freely without silently reinterpreting a saved configuration.
inline QVector<SampleFormat> formatOrder()
{
    return {SampleFormat::Cf32, SampleFormat::Cf64, SampleFormat::Cs32,
            SampleFormat::Cs16, SampleFormat::Cs12Packed, SampleFormat::Cs8,
            SampleFormat::Cu8,
            SampleFormat::Rf32, SampleFormat::Rf64, SampleFormat::Rs32,
            SampleFormat::Rs16, SampleFormat::Rs8,  SampleFormat::Ru8};
}

/// Index of the group separator, i.e. where the real-valued formats begin.
inline int formatComplexCount() { return 7; }

inline QStringList formatNames()
{
    QStringList out;
    for (SampleFormat f : formatOrder()) out << formatName(f);
    return out;
}

inline QStringList windowNames()
{
    return {QStringLiteral("Rectangular"), QStringLiteral("Hann"),
            QStringLiteral("Hamming"),     QStringLiteral("Blackman"),
            QStringLiteral("Blackman-Harris"), QStringLiteral("Flat top"),
            QStringLiteral("Kaiser β=8.6")};
}

inline QStringList averageNames()
{
    return {QStringLiteral("Off"), QStringLiteral("Exponential"),
            QStringLiteral("Linear (N)"), QStringLiteral("Max hold"),
            QStringLiteral("Min hold")};
}

inline QString runStateName(RunState s)
{
    switch (s) {
    case RunState::Stopped: return QStringLiteral("STOPPED");
    case RunState::Running: return QStringLiteral("RUNNING");
    case RunState::Paused:  return QStringLiteral("PAUSED");
    case RunState::Error:   return QStringLiteral("ERROR");
    }
    return QStringLiteral("—");
}

// --------------------------------------------------------------- configs

/// Where samples come from. Changing anything here restarts acquisition.
struct SourceConfig {
    SourceMode   mode         = SourceMode::Simulated;
    /// Use the GPUDirect path when the ring supports it (magic IQRINGG1):
    /// the NIC DMAs payloads straight into VRAM and the spectrum is computed
    /// there, so sample data never reaches system RAM.
    ///
    /// Previously the "GPU offload" toggle existed in the dialog but had no
    /// field to write to, so selecting it changed nothing. This is that field.
    bool         useGpu       = false;
    QString      devicePath   = QStringLiteral("/dev/iwfg0");
    QString      filePath;
    SampleFormat format       = SampleFormat::Cf32;
    int          streamChannels = 2;      ///< channels physically interleaved on the wire
    /// Which of the streamChannels to process and display, as a bitmask
    /// (bit c = channel c). Lets the operator pick any single channel, any
    /// pair, or any arbitrary subset up to all 16 without changing the wire
    /// framing the FPGA produces. 0 is treated as "all present channels" so
    /// an unset mask never yields an empty display.
    quint32      channelMask  = 0xFFFFFFFFu;
    int          blockSamples = 16384;    ///< samples per channel per DMA block
    bool         loopFile     = true;
    bool         pacedReplay  = true;     ///< replay a file at its nominal rate
    double       replaySpeed  = 1.0;
    qint64       headerBytes  = 0;        ///< skip this many bytes of file header
    double       fullScale    = 1.0;      ///< ADC full-scale for integer formats
};

/// Indices selected by a channel mask, clamped to the channels actually
/// present. An empty or all-zero mask means "every present channel".
inline QVector<int> selectedChannels(quint32 mask, int present)
{
    QVector<int> out;
    for (int c = 0; c < present; ++c)
        if (mask == 0u || (mask & (1u << c))) out.append(c);
    if (out.isEmpty())
        for (int c = 0; c < present; ++c) out.append(c);
    return out;
}

/// Compact human label for a mask, e.g. "CH0-3", "CH1,5,10", "all".
inline QString channelMaskLabel(quint32 mask, int present)
{
    const QVector<int> sel = selectedChannels(mask, present);
    if (sel.size() == present) return QStringLiteral("all %1").arg(present);
    QStringList parts;
    int runStart = -1, prev = -2;
    auto flush = [&](int end) {
        if (runStart < 0) return;
        parts << (runStart == end ? QStringLiteral("%1").arg(runStart)
                                  : QStringLiteral("%1-%2").arg(runStart).arg(end));
    };
    for (int c : sel) {
        if (c != prev + 1) { flush(prev); runStart = c; }
        prev = c;
    }
    flush(prev);
    return QStringLiteral("CH") + parts.join(QLatin1Char(','));
}

/// Signal-chain settings: rate, down-conversion, tuning, and the parameters of
/// the built-in simulator.
struct AcqConfig {
    double     sampleRateMsps  = 122.88;
    int        decimation      = 1;
    int        interpolation   = 1;
    // Default 0: no digital down-conversion.
    //
    // -1.2288 MHz was a simulator-era default that exactly cancelled the
    // simulator's +1.2288 MHz Tx offset. On real hardware the FIFO already
    // carries baseband samples, so any non-zero NCO translates the spectrum
    // away from what the reference (GNU Radio, which applies no DDC) shows.
    // The translation is compensated in the frequency axis, so a non-zero NCO
    // still reads correctly -- but defaulting to 0 means the GUI performs the
    // same arithmetic as the reference chain rather than a shifted variant.
    double     ncoFreqMHz      = 0.0;
    // Default 0 Hz: report BASEBAND frequency, which is what the samples
    // actually represent.
    //
    // This data path is ADC -> DMA -> FIFO; there is no RF local oscillator
    // in it, so a non-zero centre is an assertion about hardware that is not
    // present. The previous 2.450 GHz default made the readout arithmetically
    // correct but physically fictional: a tone at -10.177 MHz baseband was
    // reported as 2439.823 MHz, which cannot be checked against a signal
    // generator and, read against the plot's left edge (2350 MHz), looks like
    // ~90 MHz. GNU Radio reports -10.177 MHz because its axis is baseband.
    //
    // With 0 the GUI reports the same number as the reference. Operators with
    // a real front end set their LO here and the display becomes
    // LO + baseband, which is correct by construction rather than by default.
    double     centerFreqGHz   = 0.0;
    int        bufferMiB       = 16;

    Modulation modulation      = Modulation::QAM16;
    double     signalOffsetMHz = 1.2288;
    double     snrDb           = 25.0;

    double sampleRateHz()  const { return sampleRateMsps * 1e6; }
    /// Rate the display pipeline actually sees after the resampler.
    double displayRateHz() const
    {
        const double dec = decimation    > 0 ? decimation    : 1;
        const double itp = interpolation > 0 ? interpolation : 1;
        return sampleRateHz() * itp / dec;
    }
    double centerFreqHz()  const { return centerFreqGHz * 1e9; }
};

/// Everything that affects how a frame is turned into pixels.
struct DisplayConfig {
    int         fftSize        = 2048;
    WindowType  window         = WindowType::BlackmanHarris;
    AverageMode average        = AverageMode::Exponential;
    int         averageCount   = 8;
    double      averageAlpha   = 0.35;
    int         refreshFps     = 30;

    double      floorDb        = -130.0;
    double      ceilDb         = 10.0;

    bool        peakSearch     = true;
    int         peakCount      = 5;
    double      peakThreshDb   = 10.0;   ///< above the measured noise floor
    double      peakExcursionPct = 4.0;  ///< min separation, % of span

    int         colorMap       = 0;
    int         waterfallRows  = 320;
    bool        persistence    = true;
    double      persistDecay   = 0.90;

    int         activeChannel  = 0;
    ChannelView channelView    = ChannelView::Active;
};

/// What the transmit side is putting on the DAC, as configured in the H2C /
/// RFDC control window. Published by the window whenever any of it changes.
///
/// Live and file sources ignore it: there the receiver shows whatever really
/// arrives. The SIMULATED source renders it as an RF loopback (DAC -> ADC), so
/// DDS vs host selection, the DDS frequency and the IQ generator's waveform
/// are all visible — and behave the same way — with no hardware attached.
struct TxState {
    /// Simulator only: render the loopback instead of the modem demo burst.
    bool   loopback      = false;
    int    dacSource     = 0;        ///< PCIE_DAC_SRC_DDS (0) / _HOST (1)
    double ddsHz         = 10e6;     ///< DDS compiler output frequency
    double dacStreamSps  = 200e6;    ///< DAC stream clock = host sample rate

    // Host / GNU Radio stream (the IQ generator)
    bool   hostRunning   = false;    ///< generator producing samples
    int    waveform      = 0;        ///< sdr::wave::Kind
    double hostToneHz    = 10e6;     ///< ACTUALLY transmitted (after snapping)
    double amplitude     = 0.9;      ///< 0..1 of DAC full scale
    double phaseOffset   = 0.0;      ///< radians
    double iqPhaseDiff   = -1.5707963267948966;
    int    loopPairs     = 4096;     ///< H2C replay loop length
    bool   coherent      = true;
};

/// The single object every worker receives. One struct, one slot, one
/// metatype — the alternative is three parallel plumbing paths that drift.
struct Config {
    SourceConfig  src;
    AcqConfig     acq;
    DisplayConfig disp;
};

/// Chosen in the pre-launch dialog, fixed for the session.
struct SystemConfig {
    QString device        = QStringLiteral("QDMA FPGA");
    QString computeDevice = QStringLiteral("CPU (native)");
    QString interfaceName = QStringLiteral("PCIe");
    Config  initial;

    /// Data-flow mode chosen in the launcher, as an index matching
    /// hw::DataFlowMode (0 = ADC Only/C2H, 1 = DAC Only/H2C, 2 = both).
    /// Stored as an int so core/Types.h stays free of a dependency on the
    /// hardware-database headers.
    int     dataFlowMode = 0;

    // --- UDP live stream ------------------------------------------------
    /// Set when the Ethernet/UDP transport is chosen. The main window starts
    /// backend/udp/iwfg_c2h itself using these, so the operator never runs a
    /// command by hand.
    bool    udpTransport = false;
    int     udpPort      = 16384;
    QString udpInterface;            ///< non-empty selects --raw capture mode
    QString udpFifo      = QStringLiteral("/tmp/iwfg_c2h.fifo");

    /// True when the launcher's PCIe pre-flight inserted the iwfg module for
    /// this session. The main window unloads it on exit only when this is set,
    /// so a module the operator had already loaded is left untouched.
    bool    driverLoadedBySession = false;
};

// ----------------------------------------------------------------- stats

/// Shared between the source thread and the DSP thread. The source increments
/// `queued` as it emits and the DSP decrements it as it consumes, so the
/// difference is the real pipeline backlog rather than a guess.
struct BackPressure {
    QAtomicInteger<qint64> queued{0};
    qint64 capacity = 32;

    double utilisation() const
    {
        const qint64 q = queued.loadAcquire();
        return capacity > 0 ? std::min(1.0, static_cast<double>(q) / static_cast<double>(capacity)) : 0.0;
    }
};
using BackPressurePtr = std::shared_ptr<BackPressure>;

struct StreamStats {
    RunState state          = RunState::Stopped;
    double   dmaMBps        = 0.0;
    double   sampleRateSps  = 0.0;
    double   bufferPct      = 0.0;
    quint64  blocksIn       = 0;
    quint64  blocksDropped  = 0;
    quint64  samplesDropped = 0;
    quint64  framesOut      = 0;
    quint64  framesSkipped  = 0;

    /// Fraction of produced blocks discarded by back-pressure, as a
    /// percentage. The absolute sample count is a poor health indicator at
    /// these rates -- 0.87 % of a 200 MSPS stream is still half a billion
    /// samples after a few minutes, which reads as catastrophic when it is
    /// not. The ratio is the number that actually says whether the pipeline
    /// is keeping up.
    double dropRatePct() const
    {
        const quint64 total = blocksIn + blocksDropped;
        return total ? 100.0 * static_cast<double>(blocksDropped)
                              / static_cast<double>(total) : 0.0;
    }
    quint64  bytesTotal     = 0;
    qint64   elapsedMs      = 0;

    // File replay progress
    qint64   filePos        = 0;
    qint64   fileTotal      = 0;

    // Recorder
    bool     recording      = false;
    quint64  recordedBytes  = 0;
    QString  recordPath;

    QString  sourceName     = QStringLiteral("—");
    QString  statusText;

    double filePosFrac() const
    {
        return fileTotal > 0 ? static_cast<double>(filePos) / static_cast<double>(fileTotal) : 0.0;
    }
};

// ---------------------------------------------------------------- results

struct Peak {
    double freqHz    = 0.0;
    double levelDbfs = 0.0;
};

struct Metrics {
    double peakDbfs       = 0.0;
    double peakFreqHz     = 0.0;
    double noiseFloorDbfs = 0.0;
    double snrDb          = 0.0;
    double occupiedBwHz   = 0.0;
    double channelPowerDb = 0.0;   ///< integrated power in the occupied band
    double evm            = 0.0;   ///< fraction (0.03 == 3 %)
    double rms            = 0.0;
    double peakAmplitude  = 0.0;
    double paprDb         = 0.0;
    double phaseErrDeg    = 0.0;
    double residualHz     = 0.0;   ///< carrier offset removed from the constellation
    bool   residualValid  = false; ///< false when the estimate has aliased
    // --- IEEE-1241 style converter metrics (Xilinx RF Analyzer set) ---
    double sfdrDbc        = 0.0;   ///< spurious-free dynamic range
    double thdDbc         = 0.0;   ///< total harmonic distortion
    double sinadDb        = 0.0;
    double enob           = 0.0;   ///< effective number of bits
    /// True only when a real carrier was found. SINAD and ENOB are ratios
    /// against a fundamental; with no carrier the "fundamental" is just the
    /// largest noise bin and both numbers are meaningless rather than low.
    /// Reporting a figure in that state sends people hunting for a converter
    /// fault that is not there.
    bool   toneValid      = false;
    /// Why the tone was rejected, for display when toneValid is false.
    QString toneReason;
    /// Peak-to-median-bin, in dB. This is a DISPLAY contrast figure, not the
    /// SNR that defines ENOB — it compares two single bins, not signal power
    /// against integrated noise power.
    double peakToFloorDb  = 0.0;
    double harmonicsDbc[5] {};     ///< H2..H6 relative to the fundamental
    double dcOffset       = 0.0;
    double iqImbalanceDb  = 0.0;
    std::vector<Peak> peaks;
};

/// One processed channel within a display frame.
struct ChannelFrame {
    int                channel = 0;
    IQBlock            iq;        ///< decimated time-domain slice
    std::vector<float> spectrum;  ///< dBFS, DC-centred (fftshifted)
    std::vector<cf32>  symbols;   ///< constellation points
    Metrics            metrics;
};

/// One fully processed display frame. Crosses a queued connection by value, so
/// it must stay copyable and self-contained.
struct FrameResult {
    std::vector<ChannelFrame> channels;
    StreamStats               stats;
    double                    displayRateHz = 0.0;
    double                    centerFreqHz  = 0.0;
    /// Wall-clock span of the acquisition block the frame was built from.
    /// The time plot needs this because `iq` is decimated for display and its
    /// length alone no longer implies a duration.
    double                    blockSpanSec  = 0.0;
    quint64                   sequence      = 0;

    const ChannelFrame* channel(int i) const
    {
        if (i < 0 || i >= static_cast<int>(channels.size())) return nullptr;
        return &channels[static_cast<std::size_t>(i)];
    }
};

/// Raw acquisition block, already de-interleaved per channel.
struct SampleBlock {
    std::vector<IQBlock> channels;         ///< decoded samples (simulator path)

    /// Undecoded wire bytes (file/DMA path). Decoding every block up front
    /// burned an entire core at high stream rates even though the display
    /// throttle discards ~97 % of blocks unrendered; carrying raw bytes lets
    /// the DSP stage decode only the blocks it will actually analyse, and the
    /// recorder write the stream back byte-identically with no re-encode.
    QByteArray           raw;
    SampleFormat         rawFormat      = SampleFormat::Cf32;
    int                  rawChannels    = 1;
    double               rawFullScale   = 1.0;

    quint64              sequence    = 0;
    quint64              startSample = 0;   ///< absolute per-channel sample index
    qint64               filePos     = 0;
    qint64               fileTotal   = 0;

    std::size_t samplesPerChannel() const
    {
        if (!channels.empty()) return channels[0].size();
        if (raw.isEmpty()) return 0;
        const int nch = rawChannels > 0 ? rawChannels : 1;
        const std::size_t frameBytes =
            static_cast<std::size_t>(bytesPerSample(rawFormat)) * static_cast<std::size_t>(nch);
        return frameBytes ? static_cast<std::size_t>(raw.size()) / frameBytes : 0;
    }
};

} // namespace sdr

Q_DECLARE_METATYPE(sdr::SampleBlock)
Q_DECLARE_METATYPE(sdr::FrameResult)
Q_DECLARE_METATYPE(sdr::Config)
Q_DECLARE_METATYPE(sdr::StreamStats)
Q_DECLARE_METATYPE(sdr::RunState)
Q_DECLARE_METATYPE(sdr::TxState)
