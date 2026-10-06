#pragma once
// ---------------------------------------------------------------------------
// HardwareDb — the single source of truth for development-kit capabilities.
//
// Design rules this file exists to enforce:
//
//   * No board specification appears anywhere else in the codebase. The GUI
//     and the validation engine both read from here, so adding a board is a
//     data change, not a code change, and there is never a
//     `if (board == "47DR") max_channels = 8;` anywhere.
//
//   * Every value carries its PROVENANCE. A number that could not be verified
//     is marked Unverified and is rendered differently in the UI — the brief
//     is explicit that an unavailable specification must be flagged, not
//     silently assumed, and a configuration tool that quietly invents a
//     converter limit is worse than one that admits it does not know.
//
//   * Limits are expressed as data (min/max/enumerations), so the validation
//     engine stays generic. Board-specific quirks go in `notes` and
//     `extraRules`, not in branching logic.
// ---------------------------------------------------------------------------

#include <QString>
#include <QStringList>
#include <QVector>

namespace sdr::hw {

/// How much confidence we have in a given figure.
enum class Provenance {
    Verified,      ///< confirmed against vendor/silicon documentation
    Derived,       ///< follows arithmetically from a verified figure
    Unverified,    ///< board-level figure we could not confirm — SHOWN AS SUCH
    NotApplicable
};
QString provenanceName(Provenance p);

/// A capability figure plus the evidence behind it.
struct Spec {
    double     value = 0.0;
    Provenance prov  = Provenance::Unverified;
    QString    source;          ///< where the number came from
    QString    note;            ///< caveats, ranges, conflicting sources

    bool known() const { return prov == Provenance::Verified
                             || prov == Provenance::Derived; }
};

/// Converter (ADC or DAC) capability set.
struct ConverterSpec {
    bool    present      = false;
    int     channels     = 0;
    int     resolutionBits = 0;
    Spec    maxSampleRateGsps;   ///< per channel, maximum
    Spec    minSampleRateGsps;
    QString note;
};

/// One development kit.
struct BoardSpec {
    QString id;                  ///< stable key, e.g. "iw_rfsoc_zu47dr"
    QString displayName;
    QString family;              ///< "Zynq UltraScale+ RFSoC Gen3", "Agilex 9"
    QString device;              ///< "XCZU47DR"
    QString vendor;

    ConverterSpec adc;
    ConverterSpec dac;

    int         rfChannels = 0;
    QStringList interfaces;      ///< "PCIe Gen3 x8", "100G Ethernet", ...
    QStringList notes;           ///< board-specific restrictions, caveats

    /// Channels the host data path can carry simultaneously. Often lower than
    /// the converter count — that is exactly the kind of limit users trip on.
    int maxStreamChannels = 0;

    /// True when any headline figure is unverified, so the UI can say so.
    bool hasUnverifiedSpecs() const;
    /// Highest converter channel count that applies to a given data-flow mode.
    int maxChannelsFor(bool useAdc, bool useDac) const;
    /// Lowest of the applicable converter maxima, in GSPS. 0 when unknown.
    double maxRateGspsFor(bool useAdc, bool useDac) const;
};

/// The database. Static data, but exposed through functions so a future
/// version can load from JSON without touching any caller.
class HardwareDb
{
public:
    static const QVector<BoardSpec>& boards();
    static const BoardSpec*          byId(const QString& id);
    static QStringList               displayNames();
    static const BoardSpec*          byDisplayName(const QString& name);
};

} // namespace sdr::hw
