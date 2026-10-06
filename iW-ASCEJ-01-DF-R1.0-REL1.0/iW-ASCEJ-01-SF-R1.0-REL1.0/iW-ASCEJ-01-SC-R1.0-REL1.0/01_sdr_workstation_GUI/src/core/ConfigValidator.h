#pragma once
// ---------------------------------------------------------------------------
// ConfigValidator — checks a requested configuration against the selected
// board's capabilities.
//
// The engine is deliberately generic: it reads limits from HardwareDb and
// applies the same rules to every board. There is no board-specific branching
// here, which is the whole point — adding a kit must never mean adding an
// `if (board == ...)`.
//
// Three outcomes, and the distinction matters:
//   Error   — not supported by the hardware. Blocks launch.
//   Warning — supported, but close to a limit or otherwise notable. Informs.
//   Info    — capability statement, no action needed.
//
// A limit that is UNKNOWN never produces an Error. Refusing a configuration on
// the strength of a number we could not verify would be worse than allowing
// it: it would block valid work for an invented reason. Unknown limits raise a
// Warning saying validation is unavailable.
// ---------------------------------------------------------------------------

#include "HardwareDb.h"

#include <QString>
#include <QVector>

namespace sdr::hw {

/// User-facing data-flow mode. Maps to the driver's C2H/H2C internally.
enum class DataFlowMode {
    AdcOnly,     ///< C2H        — capture from the converters
    DacOnly,     ///< H2C        — playback to the converters
    AdcAndDac    ///< C2H + H2C  — loopback / duplex
};
QString dataFlowName(DataFlowMode m);
/// Internal transfer-direction label, for logs and the backend layer.
QString dataFlowDirections(DataFlowMode m);
bool    usesAdc(DataFlowMode m);
bool    usesDac(DataFlowMode m);

/// The configuration the user has requested. Kept separate from the widgets so
/// the engine, the UI and the backend all read one model.
struct RequestedConfig {
    QString      boardId;
    DataFlowMode mode = DataFlowMode::AdcOnly;

    int    adcChannels    = 0;
    double adcRateGsps    = 0.0;
    int    dacChannels    = 0;
    double dacRateGsps    = 0.0;
};

struct Finding {
    enum Severity { Info, Warning, Error };
    Severity severity = Info;
    QString  title;
    QString  detail;      ///< what is wrong
    QString  remedy;      ///< how to fix it — omitted only when obvious
};

struct ValidationResult {
    QVector<Finding> findings;

    bool hasErrors()   const;
    bool hasWarnings() const;
    /// Launch is permitted only when there are no Errors.
    bool ok() const { return !hasErrors(); }
    QVector<Finding> of(Finding::Severity s) const;
};

/// Apply every rule to `req`. Safe to call on every keystroke.
ValidationResult validate(const RequestedConfig& req);

} // namespace sdr::hw
