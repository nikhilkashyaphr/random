#include "HardwareDb.h"

#include <algorithm>

namespace sdr::hw {

QString provenanceName(Provenance p)
{
    switch (p) {
    case Provenance::Verified:      return QStringLiteral("verified");
    case Provenance::Derived:       return QStringLiteral("derived");
    case Provenance::Unverified:    return QStringLiteral("UNVERIFIED");
    case Provenance::NotApplicable: return QStringLiteral("n/a");
    }
    return QStringLiteral("unknown");
}

bool BoardSpec::hasUnverifiedSpecs() const
{
    auto bad = [](const ConverterSpec& c) {
        return c.present && (!c.maxSampleRateGsps.known());
    };
    return bad(adc) || bad(dac);
}

int BoardSpec::maxChannelsFor(bool useAdc, bool useDac) const
{
    int n = 0;
    if (useAdc && adc.present) n = adc.channels;
    if (useDac && dac.present) n = (n == 0) ? dac.channels : std::min(n, dac.channels);
    if (n == 0) n = std::max(adc.channels, dac.channels);
    // The host path can be narrower than the converter array; the tighter of
    // the two is what the user can actually stream.
    if (maxStreamChannels > 0) n = std::min(n, maxStreamChannels);
    return n;
}

double BoardSpec::maxRateGspsFor(bool useAdc, bool useDac) const
{
    double r = 0.0;
    if (useAdc && adc.present && adc.maxSampleRateGsps.known())
        r = adc.maxSampleRateGsps.value;
    if (useDac && dac.present && dac.maxSampleRateGsps.known())
        r = (r == 0.0) ? dac.maxSampleRateGsps.value
                       : std::min(r, dac.maxSampleRateGsps.value);
    return r;
}

namespace {

Spec verified(double v, const QString& src, const QString& note = QString())
{ return {v, Provenance::Verified, src, note}; }

Spec unverified(const QString& note)
{ return {0.0, Provenance::Unverified, QString(), note}; }

QVector<BoardSpec> buildDb()
{
    QVector<BoardSpec> db;

    // ---------------------------------------------------------------- 47DR
    {
        BoardSpec b;
        b.id          = QStringLiteral("iw_rfsoc_zu47dr");
        b.displayName = QStringLiteral("iWave RFSoC ZU47DR");
        b.family      = QStringLiteral("Zynq UltraScale+ RFSoC Gen3");
        b.device      = QStringLiteral("XCZU47DR");
        b.vendor      = QStringLiteral("iWave / AMD");

        b.adc.present        = true;
        b.adc.channels       = 8;
        b.adc.resolutionBits = 14;
        b.adc.maxSampleRateGsps = verified(5.0,
            QStringLiteral("AMD XCZU47DR device data (Gen3): 8x 14-bit RF-ADC"),
            QStringLiteral("One secondary source quotes 2.5 GSPS for this part; "
                           "the majority of sources and the Gen3 device data give "
                           "5.0 GSPS. Confirm against the board's own datasheet "
                           "before relying on the top of the range."));
        b.adc.minSampleRateGsps = verified(0.5,
            QStringLiteral("RF-ADC tile minimum, Gen3"));

        b.dac.present        = true;
        b.dac.channels       = 8;
        b.dac.resolutionBits = 14;
        b.dac.maxSampleRateGsps = verified(9.85,
            QStringLiteral("AMD XCZU47DR device data (Gen3): 8x 14-bit RF-DAC"),
            QStringLiteral("Quoted as 9.85 GSPS; 10 GSPS operation exists but "
                           "requires vendor sign-off. Some modules derate to "
                           "8.92 GSPS depending on clocking."));
        b.dac.minSampleRateGsps = verified(0.5,
            QStringLiteral("RF-DAC tile minimum, Gen3"));

        b.rfChannels = 8;
        b.interfaces << QStringLiteral("PCIe") << QStringLiteral("Ethernet");
        b.maxStreamChannels = 8;
        b.notes << QStringLiteral(
            "RF-ADCs may be configured individually for real data or in pairs "
            "for I/Q; an I/Q pair consumes two converter channels.");
        b.notes << QStringLiteral(
            "Board-level RF front-end conditioning may restrict the usable band "
            "below the converter's Nyquist limit.");
        db.push_back(b);
    }

    // ---------------------------------------------------------------- 28DR
    {
        BoardSpec b;
        b.id          = QStringLiteral("iw_rfsoc_zu28dr");
        b.displayName = QStringLiteral("iWave RFSoC ZU28DR");
        b.family      = QStringLiteral("Zynq UltraScale+ RFSoC Gen1");
        b.device      = QStringLiteral("XCZU28DR");
        b.vendor      = QStringLiteral("iWave / AMD");

        b.adc.present        = true;
        b.adc.channels       = 8;
        b.adc.resolutionBits = 12;
        b.adc.maxSampleRateGsps = verified(2.0,
            QStringLiteral("AMD UG1556: ZU28DR has eight 2 GSPS RFADC channels"));
        b.adc.minSampleRateGsps = verified(0.5, QStringLiteral("RF-ADC tile minimum"));

        b.dac.present        = true;
        b.dac.channels       = 8;
        b.dac.resolutionBits = 14;
        b.dac.maxSampleRateGsps = verified(6.4,
            QStringLiteral("AMD UG1556: 14-bit RFDACs support up to 6.4 GSPS"));
        b.dac.minSampleRateGsps = verified(0.5, QStringLiteral("RF-DAC tile minimum"));

        b.rfChannels = 8;
        b.interfaces << QStringLiteral("PCIe") << QStringLiteral("Ethernet");
        b.maxStreamChannels = 8;
        b.notes << QStringLiteral(
            "Gen1 part: 12-bit ADCs and a lower sample rate than Gen3. "
            "Do not assume Gen3 figures apply.");
        db.push_back(b);
    }

    // -------------------------------------------------------------- Agilex 9
    {
        BoardSpec b;
        b.id          = QStringLiteral("iw_agilex9");
        b.displayName = QStringLiteral("iWave Agilex 9 Direct RF");
        b.family      = QStringLiteral("Intel/Altera Agilex 9");
        b.device      = QStringLiteral("Agilex 9 Direct RF-Series");
        b.vendor      = QStringLiteral("iWave / Altera");

        // The Agilex 9 Direct RF family exists and integrates data converters,
        // but the CHANNEL COUNTS AND RATES OF THE iWAVE BOARD could not be
        // confirmed from a primary source. They are therefore left unknown and
        // the UI refuses to validate against invented limits.
        b.adc.present        = true;
        b.adc.channels       = 0;
        b.adc.resolutionBits = 0;
        b.adc.maxSampleRateGsps = unverified(
            QStringLiteral("iWave Agilex 9 board-level ADC rate not confirmed "
                           "from a primary source"));
        b.adc.note = QStringLiteral("channel count unconfirmed");

        b.dac.present        = true;
        b.dac.channels       = 0;
        b.dac.resolutionBits = 0;
        b.dac.maxSampleRateGsps = unverified(
            QStringLiteral("iWave Agilex 9 board-level DAC rate not confirmed "
                           "from a primary source"));
        b.dac.note = QStringLiteral("channel count unconfirmed");

        b.rfChannels = 0;
        b.interfaces << QStringLiteral("PCIe") << QStringLiteral("Ethernet");
        b.maxStreamChannels = 0;
        b.notes << QStringLiteral(
            "SPECIFICATIONS NOT VERIFIED. Populate this entry from the iWave "
            "datasheet before using it to validate a configuration — limits are "
            "not enforced while the figures are unknown.");
        db.push_back(b);
    }

    // ------------------------------------------------- catalogue additions
    //
    // Boards visible in the iWave product catalogue (SoM and COTS lines).
    // Converter figures come from the AMD/Altera device data where the part
    // is unambiguous; where the catalogue groups several parts under one
    // entry (e.g. "ZU49/ZU39/ZU29DR"), the SUPERSET part is modelled and the
    // ambiguity recorded, because validating against the wrong member of a
    // group would be worse than warning.

    {   // ZU49DR / ZU39DR / ZU29DR — Gen3 16-channel class
        BoardSpec b;
        b.id          = QStringLiteral("iw_rfsoc_zu49dr");
        b.displayName = QStringLiteral("iWave RFSoC ZU49/ZU39/ZU29DR");
        b.family      = QStringLiteral("Zynq UltraScale+ RFSoC Gen3");
        b.device      = QStringLiteral("XCZU49DR / ZU39DR / ZU29DR");
        b.vendor      = QStringLiteral("iWave / AMD");
        b.adc.present = true; b.adc.channels = 16; b.adc.resolutionBits = 14;
        b.adc.maxSampleRateGsps = verified(2.5,
            QStringLiteral("Gen3 16-channel RF-ADC class"),
            QStringLiteral("Catalogue groups ZU49/ZU39/ZU29DR; the 16-channel "
                           "Gen3 parts run 2.5 GSPS ADCs. Confirm the exact "
                           "device before relying on the limit."));
        b.adc.minSampleRateGsps = verified(0.5, QStringLiteral("RF-ADC tile minimum"));
        b.dac.present = true; b.dac.channels = 16; b.dac.resolutionBits = 14;
        b.dac.maxSampleRateGsps = verified(9.85,
            QStringLiteral("Gen3 RF-DAC"),
            QStringLiteral("Grouped catalogue entry — verify per device."));
        b.dac.minSampleRateGsps = verified(0.5, QStringLiteral("RF-DAC tile minimum"));
        b.rfChannels = 16; b.maxStreamChannels = 16;
        b.interfaces << QStringLiteral("PCIe") << QStringLiteral("Ethernet");
        b.notes << QStringLiteral(
            "Catalogue entry covers three devices; channel counts and rates "
            "differ between them. Select the exact part before final sign-off.");
        db.push_back(b);
    }

    {   // ZU27DR / ZU25DR — Gen1 8-channel class, same tile spec as ZU28DR
        BoardSpec b;
        b.id          = QStringLiteral("iw_rfsoc_zu27dr");
        b.displayName = QStringLiteral("iWave RFSoC ZU27/ZU25DR");
        b.family      = QStringLiteral("Zynq UltraScale+ RFSoC Gen1");
        b.device      = QStringLiteral("XCZU27DR / ZU25DR");
        b.vendor      = QStringLiteral("iWave / AMD");
        b.adc.present = true; b.adc.channels = 8; b.adc.resolutionBits = 12;
        b.adc.maxSampleRateGsps = verified(2.0,
            QStringLiteral("Gen1 RF-ADC, 12-bit"),
            QStringLiteral("ZU25DR has fewer converters than ZU27DR; the "
                           "grouped entry models the superset."));
        b.adc.minSampleRateGsps = verified(0.5, QStringLiteral("RF-ADC tile minimum"));
        b.dac.present = true; b.dac.channels = 8; b.dac.resolutionBits = 14;
        b.dac.maxSampleRateGsps = verified(6.4, QStringLiteral("Gen1 RF-DAC"));
        b.dac.minSampleRateGsps = verified(0.5, QStringLiteral("RF-DAC tile minimum"));
        b.rfChannels = 8; b.maxStreamChannels = 8;
        b.interfaces << QStringLiteral("PCIe") << QStringLiteral("Ethernet");
        b.notes << QStringLiteral(
            "Gen1 class: 12-bit ADCs at 2.0 GSPS. Do not apply Gen3 figures.");
        db.push_back(b);
    }

    {   // ZU67/ZU65/ZU64/ZU63DR — Gen3 DFE class
        BoardSpec b;
        b.id          = QStringLiteral("iw_rfsoc_zu67dr");
        b.displayName = QStringLiteral("iWave RFSoC ZU67/ZU65/ZU64/ZU63DR");
        b.family      = QStringLiteral("Zynq UltraScale+ RFSoC DFE");
        b.device      = QStringLiteral("XCZU67DR / ZU65DR / ZU64DR / ZU63DR");
        b.vendor      = QStringLiteral("iWave / AMD");
        b.adc.present = true; b.adc.channels = 0; b.adc.resolutionBits = 0;
        b.adc.maxSampleRateGsps = unverified(
            QStringLiteral("RFSoC DFE converter counts vary widely across this "
                           "group and were not confirmed per device"));
        b.dac.present = true; b.dac.channels = 0; b.dac.resolutionBits = 0;
        b.dac.maxSampleRateGsps = unverified(
            QStringLiteral("RFSoC DFE DAC configuration not confirmed"));
        b.rfChannels = 0; b.maxStreamChannels = 0;
        b.interfaces << QStringLiteral("PCIe") << QStringLiteral("Ethernet");
        b.notes << QStringLiteral(
            "SPECIFICATIONS NOT VERIFIED for this group. Populate from the "
            "device datasheet; limits are not enforced meanwhile.");
        db.push_back(b);
    }

    {   // Versal RF
        BoardSpec b;
        b.id          = QStringLiteral("iw_versal_rf");
        b.displayName = QStringLiteral("iWave Versal RF VR1902/VR1652/VR1602");
        b.family      = QStringLiteral("AMD Versal RF");
        b.device      = QStringLiteral("VR1952 / VR1902 / VR1652 / VR1602");
        b.vendor      = QStringLiteral("iWave / AMD");
        b.adc.present = true; b.adc.channels = 0; b.adc.resolutionBits = 0;
        b.adc.maxSampleRateGsps = unverified(
            QStringLiteral("Versal RF converter counts/rates not confirmed"));
        b.dac.present = true; b.dac.channels = 0; b.dac.resolutionBits = 0;
        b.dac.maxSampleRateGsps = unverified(
            QStringLiteral("Versal RF DAC configuration not confirmed"));
        b.rfChannels = 0; b.maxStreamChannels = 0;
        b.interfaces << QStringLiteral("PCIe") << QStringLiteral("Ethernet");
        b.notes << QStringLiteral(
            "SPECIFICATIONS NOT VERIFIED. Versal RF is a different architecture "
            "from Zynq RFSoC; do not carry RFSoC figures across.");
        db.push_back(b);
    }

    {   // Agilex 9 wide-band / mid-band variants
        BoardSpec b;
        b.id          = QStringLiteral("iw_agilex9_wideband");
        b.displayName = QStringLiteral("iWave Agilex 9 Wide-band");
        b.family      = QStringLiteral("Intel/Altera Agilex 9 Direct RF");
        b.device      = QStringLiteral("Agilex 9 Direct RF-Series");
        b.vendor      = QStringLiteral("iWave / Altera");
        b.adc.present = true; b.adc.channels = 0; b.adc.resolutionBits = 0;
        b.adc.maxSampleRateGsps = unverified(
            QStringLiteral("Agilex 9 wide-band board-level ADC not confirmed"));
        b.dac.present = true; b.dac.channels = 0; b.dac.resolutionBits = 0;
        b.dac.maxSampleRateGsps = unverified(
            QStringLiteral("Agilex 9 wide-band board-level DAC not confirmed"));
        b.rfChannels = 0; b.maxStreamChannels = 0;
        b.interfaces << QStringLiteral("PCIe") << QStringLiteral("Ethernet");
        b.notes << QStringLiteral(
            "SPECIFICATIONS NOT VERIFIED. Wide-band and mid-band variants "
            "differ; populate each from its own datasheet.");
        db.push_back(b);
    }

    // ------------------------------------------------------- generic / other
    {
        BoardSpec b;
        b.id          = QStringLiteral("generic");
        b.displayName = QStringLiteral("Generic / unlisted board");
        b.family      = QStringLiteral("—");
        b.device      = QStringLiteral("—");
        b.vendor      = QStringLiteral("—");
        b.adc.present  = true;  b.adc.channels = 16; b.adc.resolutionBits = 16;
        b.adc.maxSampleRateGsps = unverified(QStringLiteral("no board selected"));
        b.dac.present  = true;  b.dac.channels = 16; b.dac.resolutionBits = 16;
        b.dac.maxSampleRateGsps = unverified(QStringLiteral("no board selected"));
        b.rfChannels = 16;
        b.maxStreamChannels = 16;
        b.interfaces << QStringLiteral("PCIe") << QStringLiteral("Ethernet");
        b.notes << QStringLiteral(
            "No hardware limits are enforced. Select the actual development kit "
            "to enable validation.");
        db.push_back(b);
    }

    return db;
}

} // namespace

const QVector<BoardSpec>& HardwareDb::boards()
{
    static const QVector<BoardSpec> db = buildDb();
    return db;
}

const BoardSpec* HardwareDb::byId(const QString& id)
{
    for (const BoardSpec& b : boards()) if (b.id == id) return &b;
    return nullptr;
}

QStringList HardwareDb::displayNames()
{
    QStringList n;
    for (const BoardSpec& b : boards()) n << b.displayName;
    return n;
}

const BoardSpec* HardwareDb::byDisplayName(const QString& name)
{
    for (const BoardSpec& b : boards()) if (b.displayName == name) return &b;
    return nullptr;
}

} // namespace sdr::hw
