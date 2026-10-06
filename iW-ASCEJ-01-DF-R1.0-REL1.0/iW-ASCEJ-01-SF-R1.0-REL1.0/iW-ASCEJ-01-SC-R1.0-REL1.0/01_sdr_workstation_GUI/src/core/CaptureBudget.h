#pragma once
// ---------------------------------------------------------------------------
// Capture bandwidth planning (1.2.5).
//
// Why this matters for TRANSMIT: when the FPGA's C2H FIFO overflows because
// the host does not drain the capture fast enough, the iwfg driver recovers
// by soft-resetting the whole QDMA and rebuilding every queue — the H2C queue
// included, even while an H2C transfer is waiting on it (iwfg_dma.c,
// iwfg_rx_poll(), the usr_ovf branch). A capture that asks for more than the
// host can drain therefore keeps interrupting transmit, and can wedge the
// device. 4 channels x 200 MSPS x 4 B needs 3052 MiB/s; the bench host
// drained about 2400.
//
// These helpers are pure so they can be tested without a device.
// ---------------------------------------------------------------------------

#include <cmath>

namespace sdr {

/// What the capture may ask for while transmitting. 2 channels at 200 MSPS
/// (1526 MiB/s) fit with margin on the bench host; 4 do not.
constexpr double kCaptureBudgetMib = 1600.0;

/// MiB/s a capture of `channels` x `rateMsps` x `bytesPerSample` needs.
inline double captureNeedMib(int channels, double rateMsps, int bytesPerSample)
{
    return double(channels) * rateMsps * 1e6 * double(bytesPerSample) / 1048576.0;
}

/// PL GPIO routing mode (firmware menu 10) for a channel count:
/// 1 = single channel, 2 = two channels, 3 = four channels (the boot mode).
inline int plGpioModeForChannels(int channels)
{
    return channels >= 4 ? 3 : (channels >= 2 ? 2 : 1);
}

/// Channels to capture while transmitting: `channels` if it fits the
/// budget, otherwise the largest of 2 or 1 that does (1 at the least).
inline int captureChannelsWithinBudget(int channels, double rateMsps, int bytesPerSample,
                                       double budgetMib = kCaptureBudgetMib)
{
    if (captureNeedMib(channels, rateMsps, bytesPerSample) <= budgetMib) return channels;
    for (int n : {2, 1})
        if (n < channels && captureNeedMib(n, rateMsps, bytesPerSample) <= budgetMib)
            return n;
    return 1;
}

/// The channel count a measured capture rate corresponds to. True only when
/// the rate is within 15 % of a whole 1, 2, 4 or 8 channels' worth — an
/// overloaded capture (e.g. 3.2 channels' worth) is deliberately not read as
/// a channel count.
inline bool impliedCaptureChannels(double deliveredMib, double rateMsps, int bytesPerSample,
                                   int* channels)
{
    const double per = captureNeedMib(1, rateMsps, bytesPerSample);
    if (per <= 0.0 || deliveredMib <= 0.0) return false;
    const double x = deliveredMib / per;
    for (int c : {1, 2, 4, 8}) {
        if (std::abs(x - double(c)) <= 0.15 * double(c)) {
            if (channels) *channels = c;
            return true;
        }
    }
    return false;
}

} // namespace sdr
