#pragma once
// ---------------------------------------------------------------------------
// Waveform — the one definition of the host transmit signal.
//
// Used by IqGenerator (the samples written to the H2C FIFO) AND by the
// simulator's loopback model (what the plots show with no hardware). Keeping
// both on the same code is what makes Simulation show exactly the waveform the
// DAC would receive, rather than a look-alike.
//
// WHY "LOOP-COHERENT"
// -------------------
// iwfg_h2c does not stream the FIFO sample by sample. It REPLAYS the most
// recent chunk back to back at DMA speed until the next one arrives (the card
// consumes ~800 MB/s; no host producer keeps up). A chunk is therefore a loop,
// and a loop is seamless only if it holds a whole number of cycles. A 10 MHz
// tone in a 4096-sample chunk at 200 MSPS holds 204.8 cycles: every replay
// boundary is a phase jump, which the spectrum shows as a comb of spurs spaced
// fs/N (48.8 kHz) either side of the tone — the GNU Radio chain had exactly
// this artefact.
//
// The cure is to transmit the nearest frequency that fits the loop exactly,
// k * fs / N. Every chunk is then identical, so replaying one, two or ten
// times is indistinguishable from true streaming, and the tone is clean. The
// cost is resolution: fs/N (48.8 kHz at 200 MSPS with the default 16 KiB
// chunk). The frequency actually transmitted is always reported to the user.
// ---------------------------------------------------------------------------

#include <cmath>
#include <cstdint>

namespace sdr::wave {

/// Numbering matches iq_source_v9 and IqGenerator::Waveform.
enum Kind { Sine = 0, Cosine = 1, Square = 2, Saw = 3, Triangle = 4, Noise = 5 };

constexpr double kTwoPi = 6.283185307179586476925286766559;

/// One period of the chosen shape, normalised to +/-1, at phase `phi`.
/// Identical shapes to iq_source_v9. Noise has no phase and returns 0 here.
inline double shape(int kind, double phi)
{
    switch (kind) {
    case Sine:     return std::sin(phi);
    case Cosine:   return std::cos(phi);
    case Square:   return (std::sin(phi) >= 0.0) ? 1.0 : -1.0;
    case Saw: {
        double pn = std::fmod(phi / kTwoPi, 1.0);
        if (pn < 0.0) pn += 1.0;
        return 2.0 * pn - 1.0;
    }
    case Triangle: {
        double pn = std::fmod(phi / kTwoPi, 1.0);
        if (pn < 0.0) pn += 1.0;
        return 2.0 * std::fabs(2.0 * pn - 1.0) - 1.0;
    }
    default:
        return 0.0;
    }
}

/// Frequency resolution of an N-sample replay loop at `fs`.
inline double loopResolutionHz(double fs, int loopPairs)
{
    return (fs > 0.0 && loopPairs > 0) ? fs / double(loopPairs) : 0.0;
}

/// Whole cycles per loop for the nearest loop-coherent frequency.
inline long long loopCycles(double hz, double fs, int loopPairs)
{
    const double bin = loopResolutionHz(fs, loopPairs);
    return bin > 0.0 ? static_cast<long long>(std::llround(hz / bin)) : 0;
}

/// The nearest frequency that loops seamlessly: k * fs / N.
inline double coherentHz(double hz, double fs, int loopPairs)
{
    const double bin = loopResolutionHz(fs, loopPairs);
    return bin > 0.0 ? double(loopCycles(hz, fs, loopPairs)) * bin : hz;
}

/// Phase of sample n in a loop of N samples holding k cycles, computed from
/// the integer product so it is exact — no accumulator drift, and sample N
/// lands on precisely the phase of sample 0.
inline double loopPhase(long long k, int n, int loopPairs)
{
    long long m = (k * static_cast<long long>(n)) % loopPairs;
    if (m < 0) m += loopPairs;
    return kTwoPi * double(m) / double(loopPairs);
}

} // namespace sdr::wave
