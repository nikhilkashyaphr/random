#pragma once
#include "Types.h"
#include <cstring>
#include <cmath>

namespace sdr::codec {

/// Wire layout assumed throughout: complex samples interleaved I,Q and
/// channels interleaved sample-by-sample —
///     ch0.I ch0.Q ch1.I ch1.Q ch0.I ch0.Q ch1.I ch1.Q ...
/// This is what a QDMA C2H stream with per-channel packing looks like once the
/// descriptor header is stripped. Change decodeOne() if your fabric packs
/// channels in blocks instead.

inline float readScalar(const unsigned char* p, SampleFormat fmt, int half, double fullScale)
{
    // `half` selects I (0) or Q (1) within one complex sample.
    switch (fmt) {
    case SampleFormat::Cs8: {
        const auto v = static_cast<int8_t>(p[half]);
        return static_cast<float>(v / 128.0 * fullScale);
    }
    case SampleFormat::Cu8: {
        const int v = static_cast<int>(p[half]) - 128;
        return static_cast<float>(v / 128.0 * fullScale);
    }
    case SampleFormat::Cs16: {
        int16_t v;
        std::memcpy(&v, p + half * 2, 2);
        return static_cast<float>(v / 32768.0 * fullScale);
    }
    case SampleFormat::Cs32: {
        int32_t v;
        std::memcpy(&v, p + half * 4, 4);
        return static_cast<float>(v / 2147483648.0 * fullScale);
    }
    case SampleFormat::Cf32: {
        float v;
        std::memcpy(&v, p + half * 4, 4);
        return static_cast<float>(v * fullScale);
    }
    case SampleFormat::Cf64: {
        double v;
        std::memcpy(&v, p + half * 8, 8);
        return static_cast<float>(v * fullScale);
    }
    case SampleFormat::Cs12Packed: {
        // 3 bytes hold two signed 12-bit words, little-endian nibble order:
        //   b0 = I[7:0], b1 = Q[3:0] | I[11:8], b2 = Q[11:4]
        int raw = (half == 0) ? (p[0] | ((p[1] & 0x0F) << 8))
                              : ((p[1] >> 4) | (p[2] << 4));
        if (raw & 0x800) raw -= 0x1000;          // sign-extend 12 -> 32
        return static_cast<float>(raw / 2048.0 * fullScale);
    }

    // Real-valued formats: one scalar per sample, so the Q half is always 0.
    // The spectrum of such a stream is conjugate-symmetric; that is a property
    // of the data, not a bug in the display.
    case SampleFormat::Rs8: {
        if (half) return 0.0f;
        return static_cast<float>(static_cast<int8_t>(p[0]) / 128.0 * fullScale);
    }
    case SampleFormat::Ru8: {
        if (half) return 0.0f;
        return static_cast<float>((static_cast<int>(p[0]) - 128) / 128.0 * fullScale);
    }
    case SampleFormat::Rs16: {
        if (half) return 0.0f;
        int16_t v; std::memcpy(&v, p, 2);
        return static_cast<float>(v / 32768.0 * fullScale);
    }
    case SampleFormat::Rs32: {
        if (half) return 0.0f;
        int32_t v; std::memcpy(&v, p, 4);
        return static_cast<float>(v / 2147483648.0 * fullScale);
    }
    case SampleFormat::Rf32: {
        if (half) return 0.0f;
        float v; std::memcpy(&v, p, 4);
        return static_cast<float>(v * fullScale);
    }
    case SampleFormat::Rf64: {
        if (half) return 0.0f;
        double v; std::memcpy(&v, p, 8);
        return static_cast<float>(v * fullScale);
    }
    }
    return 0.0f;
}

/// Decode `bytes` of raw stream into `channels` de-interleaved IQ vectors.
/// Returns the number of complete per-channel samples produced.
inline std::size_t decode(const char* data, std::size_t bytes, SampleFormat fmt,
                          int channels, double fullScale, std::vector<IQBlock>& out)
{
    const int nch = std::max(1, channels);
    const int bps = bytesPerSample(fmt);
    const std::size_t frameBytes = static_cast<std::size_t>(bps) * static_cast<std::size_t>(nch);
    if (frameBytes == 0) return 0;

    const std::size_t frames = bytes / frameBytes;
    out.assign(static_cast<std::size_t>(nch), IQBlock(frames));

    const bool real = isRealFormat(fmt);
    const auto* base = reinterpret_cast<const unsigned char*>(data);

    // Channel-outer ordering: each inner loop then walks one output vector
    // linearly, which is far friendlier to the cache than writing a different
    // vector on every iteration.
    for (int c = 0; c < nch; ++c) {
        IQBlock& dst = out[static_cast<std::size_t>(c)];
        const unsigned char* p = base + static_cast<std::size_t>(c) * bps;
        if (real) {
            for (std::size_t f = 0; f < frames; ++f, p += frameBytes)
                dst[f] = cf32(readScalar(p, fmt, 0, fullScale), 0.0f);
        } else {
            for (std::size_t f = 0; f < frames; ++f, p += frameBytes)
                dst[f] = cf32(readScalar(p, fmt, 0, fullScale),
                              readScalar(p, fmt, 1, fullScale));
        }
    }
    return frames;
}

inline void writeScalar(unsigned char* p, SampleFormat fmt, int half, float v, double fullScale)
{
    const double s = fullScale > 0.0 ? v / fullScale : v;
    switch (fmt) {
    case SampleFormat::Cs8:
        p[half] = static_cast<unsigned char>(static_cast<int8_t>(std::lround(std::clamp(s, -1.0, 1.0) * 127.0)));
        break;
    case SampleFormat::Cu8:
        p[half] = static_cast<unsigned char>(std::clamp<int>(static_cast<int>(std::lround(std::clamp(s, -1.0, 1.0) * 127.0)) + 128, 0, 255));
        break;
    case SampleFormat::Cs16: {
        const auto q = static_cast<int16_t>(std::lround(std::clamp(s, -1.0, 1.0) * 32767.0));
        std::memcpy(p + half * 2, &q, 2);
        break;
    }
    case SampleFormat::Cs32: {
        const auto q = static_cast<int32_t>(std::llround(std::clamp(s, -1.0, 1.0) * 2147483647.0));
        std::memcpy(p + half * 4, &q, 4);
        break;
    }
    case SampleFormat::Cf32: {
        const float q = static_cast<float>(s);
        std::memcpy(p + half * 4, &q, 4);
        break;
    }
    case SampleFormat::Cf64: {
        const double q = s;
        std::memcpy(p + half * 8, &q, 8);
        break;
    }
    case SampleFormat::Rs8:
        if (half) break;
        p[0] = static_cast<unsigned char>(static_cast<int8_t>(std::lround(std::clamp(s, -1.0, 1.0) * 127.0)));
        break;
    case SampleFormat::Ru8:
        if (half) break;
        p[0] = static_cast<unsigned char>(std::clamp<int>(static_cast<int>(std::lround(std::clamp(s, -1.0, 1.0) * 127.0)) + 128, 0, 255));
        break;
    case SampleFormat::Rs16: {
        if (half) break;
        const auto q = static_cast<int16_t>(std::lround(std::clamp(s, -1.0, 1.0) * 32767.0));
        std::memcpy(p, &q, 2);
        break;
    }
    case SampleFormat::Rs32: {
        if (half) break;
        const auto q = static_cast<int32_t>(std::llround(std::clamp(s, -1.0, 1.0) * 2147483647.0));
        std::memcpy(p, &q, 4);
        break;
    }
    case SampleFormat::Rf32: {
        if (half) break;
        const float q = static_cast<float>(s);
        std::memcpy(p, &q, 4);
        break;
    }
    case SampleFormat::Rf64: {
        if (half) break;
        const double q = s;
        std::memcpy(p, &q, 8);
        break;
    }
    case SampleFormat::Cs12Packed: {
        int q = static_cast<int>(std::lround(std::clamp(s, -1.0, 1.0) * 2047.0)) & 0xFFF;
        if (half == 0) {
            p[0] = static_cast<unsigned char>(q & 0xFF);
            p[1] = static_cast<unsigned char>((p[1] & 0xF0) | ((q >> 8) & 0x0F));
        } else {
            p[1] = static_cast<unsigned char>((p[1] & 0x0F) | ((q & 0x0F) << 4));
            p[2] = static_cast<unsigned char>((q >> 4) & 0xFF);
        }
        break;
    }
    }
}

/// Re-interleave per-channel IQ into the wire format. Used by the recorder so
/// a recorded file replays byte-identically through the file source.
inline std::size_t encode(const std::vector<IQBlock>& in, SampleFormat fmt,
                          double fullScale, std::vector<char>& out)
{
    if (in.empty()) { out.clear(); return 0; }
    const int nch = static_cast<int>(in.size());
    const int bps = bytesPerSample(fmt);
    const std::size_t frames = in[0].size();
    const std::size_t frameBytes = static_cast<std::size_t>(bps) * static_cast<std::size_t>(nch);

    out.assign(frames * frameBytes, 0);
    auto* base = reinterpret_cast<unsigned char*>(out.data());

    for (std::size_t f = 0; f < frames; ++f) {
        unsigned char* row = base + f * frameBytes;
        for (int c = 0; c < nch; ++c) {
            unsigned char* p = row + static_cast<std::size_t>(c) * bps;
            const cf32& s = in[static_cast<std::size_t>(c)][f];
            writeScalar(p, fmt, 0, s.real(), fullScale);
            writeScalar(p, fmt, 1, s.imag(), fullScale);
        }
    }
    return frames;
}

} // namespace sdr::codec
