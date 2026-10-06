#include "ConfigValidator.h"

namespace sdr::hw {

QString dataFlowName(DataFlowMode m)
{
    switch (m) {
    case DataFlowMode::AdcOnly:   return QStringLiteral("ADC Only");
    case DataFlowMode::DacOnly:   return QStringLiteral("DAC Only");
    case DataFlowMode::AdcAndDac: return QStringLiteral("ADC + DAC");
    }
    return QStringLiteral("unknown");
}

QString dataFlowDirections(DataFlowMode m)
{
    switch (m) {
    case DataFlowMode::AdcOnly:   return QStringLiteral("C2H");
    case DataFlowMode::DacOnly:   return QStringLiteral("H2C");
    case DataFlowMode::AdcAndDac: return QStringLiteral("C2H + H2C");
    }
    return QStringLiteral("—");
}

bool usesAdc(DataFlowMode m) { return m != DataFlowMode::DacOnly; }
bool usesDac(DataFlowMode m) { return m != DataFlowMode::AdcOnly; }

bool ValidationResult::hasErrors() const
{
    for (const Finding& f : findings) if (f.severity == Finding::Error) return true;
    return false;
}
bool ValidationResult::hasWarnings() const
{
    for (const Finding& f : findings) if (f.severity == Finding::Warning) return true;
    return false;
}
QVector<Finding> ValidationResult::of(Finding::Severity s) const
{
    QVector<Finding> out;
    for (const Finding& f : findings) if (f.severity == s) out.push_back(f);
    return out;
}

namespace {

/// Above this fraction of a limit we warn: the configuration is legal but
/// leaves no margin for clocking tolerance or board-level derating.
constexpr double kWarnFraction = 0.90;

void checkConverter(const BoardSpec& b, const ConverterSpec& c,
                    const QString& label, int reqChannels, double reqRateGsps,
                    ValidationResult& r)
{
    if (!c.present) {
        r.findings.push_back({Finding::Error,
            QStringLiteral("%1 not available").arg(label),
            QStringLiteral("%1 has no %2.").arg(b.displayName, label),
            QStringLiteral("Choose a data-flow mode that does not use the %1, "
                           "or select a board that provides one.").arg(label)});
        return;
    }

    // ---- channel count ----
    if (c.channels <= 0) {
        r.findings.push_back({Finding::Warning,
            QStringLiteral("%1 channel count unknown").arg(label),
            QStringLiteral("The %1 channel count for %2 is not in the hardware "
                           "database, so it cannot be checked.")
                .arg(label, b.displayName),
            QStringLiteral("Populate this board's entry from its datasheet to "
                           "enable validation.")});
    } else if (reqChannels > c.channels) {
        r.findings.push_back({Finding::Error,
            QStringLiteral("Too many %1 channels").arg(label),
            QStringLiteral("Maximum %1 channels supported: %2\nSelected: %3")
                .arg(label).arg(c.channels).arg(reqChannels),
            QStringLiteral("Reduce the channel count to %1 or fewer, or select a "
                           "development kit that supports %2 channels.")
                .arg(c.channels).arg(reqChannels)});
    } else if (reqChannels <= 0) {
        r.findings.push_back({Finding::Error,
            QStringLiteral("No %1 channels selected").arg(label),
            QStringLiteral("At least one %1 channel is required for this mode.")
                .arg(label),
            QStringLiteral("Select one or more %1 channels.").arg(label)});
    }

    // ---- sample rate ----
    const Spec& mx = c.maxSampleRateGsps;
    if (!mx.known()) {
        r.findings.push_back({Finding::Warning,
            QStringLiteral("%1 rate limit unverified").arg(label),
            QStringLiteral("The maximum %1 sample rate for %2 is not verified, "
                           "so the requested %3 GSPS cannot be checked.%4")
                .arg(label, b.displayName)
                .arg(reqRateGsps, 0, 'f', 3)
                .arg(mx.note.isEmpty() ? QString()
                                       : QStringLiteral("\n%1").arg(mx.note)),
            QStringLiteral("Confirm the limit against the board datasheet before "
                           "running at this rate.")});
    } else if (reqRateGsps > mx.value) {
        r.findings.push_back({Finding::Error,
            QStringLiteral("%1 sample rate too high").arg(label),
            QStringLiteral("Requested %1 rate: %2 GSPS\nMaximum supported: %3 GSPS")
                .arg(label).arg(reqRateGsps, 0, 'f', 3).arg(mx.value, 0, 'f', 3),
            QStringLiteral("Reduce the rate to %1 GSPS or below, or select a "
                           "development kit that supports it.")
                .arg(mx.value, 0, 'f', 3)});
    } else if (reqRateGsps > mx.value * kWarnFraction) {
        r.findings.push_back({Finding::Warning,
            QStringLiteral("%1 rate near the limit").arg(label),
            // Report the actual utilisation, not the threshold: "97% of the
            // 5.00 GSPS maximum" is actionable, "within 9%" was both wrong
            // (kWarnFraction is 0.90, so the band is 10%) and unclear.
            QStringLiteral("Requested %1 rate %2 GSPS is %3% of this board's "
                           "%4 GSPS maximum.")
                .arg(label).arg(reqRateGsps, 0, 'f', 3)
                .arg(100.0 * reqRateGsps / mx.value, 0, 'f', 1)
                .arg(mx.value, 0, 'f', 3),
            QStringLiteral("Leave margin for clocking tolerance and board-level "
                           "derating if the link proves unstable.")});
    }
    // The converter tile minimum is a WARNING, never an error.
    //
    // The rate the user enters here is the HOST streaming rate, and a rate
    // below the converter minimum is the normal case: the fabric decimates
    // before the data reaches the host, so 122.88 MSPS off a 5 GSPS ADC is
    // entirely valid. Treating this as a blocking error made the default
    // configuration unlaunchable — the converter minimum constrains the tile
    // clock, not what the host asks for.
    if (mx.known() && c.minSampleRateGsps.known()
        && reqRateGsps > 0.0 && reqRateGsps < c.minSampleRateGsps.value) {
        r.findings.push_back({Finding::Info,
            QStringLiteral("%1 rate is below the tile minimum").arg(label),
            QStringLiteral("Host rate %1 GSPS is below the %2 minimum tile rate "
                           "of %3 GSPS, which implies decimation in the fabric.")
                .arg(reqRateGsps, 0, 'f', 3).arg(label)
                .arg(c.minSampleRateGsps.value, 0, 'f', 3),
            QString()});
    }
}

} // namespace

ValidationResult validate(const RequestedConfig& req)
{
    ValidationResult r;
    const BoardSpec* b = HardwareDb::byId(req.boardId);
    if (!b) {
        r.findings.push_back({Finding::Error,
            QStringLiteral("No development kit selected"),
            QStringLiteral("A development kit must be selected before the "
                           "configuration can be validated."),
            QStringLiteral("Choose a kit from the Platform list.")});
        return r;
    }

    // Capability statement — always present, so limits sit next to the controls.
    r.findings.push_back({Finding::Info,
        QStringLiteral("%1 — %2").arg(b->displayName, b->family),
        QStringLiteral("ADC: %1  |  DAC: %2  |  data path: %3")
            .arg(b->adc.present
                     ? QStringLiteral("%1 ch, %2-bit, max %3")
                           .arg(b->adc.channels).arg(b->adc.resolutionBits)
                           .arg(b->adc.maxSampleRateGsps.known()
                                    ? QStringLiteral("%1 GSPS")
                                          .arg(b->adc.maxSampleRateGsps.value, 0, 'f', 2)
                                    : QStringLiteral("unverified"))
                     : QStringLiteral("none"),
                 b->dac.present
                     ? QStringLiteral("%1 ch, %2-bit, max %3")
                           .arg(b->dac.channels).arg(b->dac.resolutionBits)
                           .arg(b->dac.maxSampleRateGsps.known()
                                    ? QStringLiteral("%1 GSPS")
                                          .arg(b->dac.maxSampleRateGsps.value, 0, 'f', 2)
                                    : QStringLiteral("unverified"))
                     : QStringLiteral("none"),
                 dataFlowDirections(req.mode)),
        QString()});

    if (usesAdc(req.mode))
        checkConverter(*b, b->adc, QStringLiteral("ADC"),
                       req.adcChannels, req.adcRateGsps, r);
    if (usesDac(req.mode))
        checkConverter(*b, b->dac, QStringLiteral("DAC"),
                       req.dacChannels, req.dacRateGsps, r);

    // ---- combined ADC + DAC coexistence ----
    if (req.mode == DataFlowMode::AdcAndDac) {
        const int total = req.adcChannels + req.dacChannels;
        if (b->maxStreamChannels > 0 && total > b->maxStreamChannels * 2) {
            r.findings.push_back({Finding::Error,
                QStringLiteral("Combined channel count exceeds the data path"),
                QStringLiteral("ADC %1 + DAC %2 = %3 streams requested; the host "
                               "path supports %4 in each direction.")
                    .arg(req.adcChannels).arg(req.dacChannels).arg(total)
                    .arg(b->maxStreamChannels),
                QStringLiteral("Reduce the channel counts, or run ADC and DAC in "
                               "separate sessions.")});
        }
        if (b->adc.maxSampleRateGsps.known() && b->dac.maxSampleRateGsps.known()
            && req.adcRateGsps > 0.0 && req.dacRateGsps > 0.0) {
            // Duplex operation shares the clocking tree; wildly different rates
            // are legal but usually indicate a mistake worth surfacing.
            const double hi = std::max(req.adcRateGsps, req.dacRateGsps);
            const double lo = std::min(req.adcRateGsps, req.dacRateGsps);
            if (lo > 0.0 && hi / lo > 8.0) {
                r.findings.push_back({Finding::Warning,
                    QStringLiteral("ADC and DAC rates differ widely"),
                    QStringLiteral("ADC %1 GSPS vs DAC %2 GSPS. Both converters "
                                   "share a clocking tree; a ratio this large may "
                                   "not be achievable with a single reference.")
                        .arg(req.adcRateGsps, 0, 'f', 3)
                        .arg(req.dacRateGsps, 0, 'f', 3),
                    QStringLiteral("Check that both rates are derivable from the "
                                   "board's reference clock.")});
            }
        }
    }

    // ---- board-level caveats, carried as data ----
    for (const QString& n : b->notes) {
        r.findings.push_back({b->hasUnverifiedSpecs() ? Finding::Warning
                                                      : Finding::Info,
            QStringLiteral("%1 note").arg(b->displayName), n, QString()});
    }
    return r;
}

} // namespace sdr::hw
