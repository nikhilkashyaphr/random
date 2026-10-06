#pragma once
// ---------------------------------------------------------------------------
// DriverManager — makes the application self-contained for PCIe mode.
//
// The application package carries the iwfg driver sources in `drivers/`.
// When PCIe mode is selected and the module is not already loaded, this class
// walks the chain: dependency check → out-of-tree build → module insert →
// node validation, reporting each step so a failure is diagnosable rather
// than a mystery.
//
// Module insertion needs root. We do not silently escalate: `insmod` is run
// through pkexec when a polkit agent is available, and otherwise the exact
// command is reported for the operator to run. An application that quietly
// sudo's kernel modules into a customer machine is not "polished", it is a
// liability.
// ---------------------------------------------------------------------------

#include <QObject>
#include <QString>
#include <QStringList>

namespace sdr {

class DriverManager : public QObject
{
    Q_OBJECT
public:
    struct StepResult {
        bool    ok = false;
        QString title;    ///< e.g. "Build driver"
        QString detail;   ///< command output / explanation
    };

    explicit DriverManager(QObject* parent = nullptr);

    static constexpr const char* kModuleName = "iwfg";

    /// Directory holding the driver sources: `drivers/` beside the
    /// executable, or beside the project root during development.
    /// Empty when not found.
    static QString driversDir();

    /// True if a compiled module (*.ko) exists in the drivers directory.
    static QString compiledModulePath();

    /// Build-time dependencies: make, cc, and kernel headers for the running
    /// kernel. Returns a result per dependency.
    static QVector<StepResult> checkBuildDeps();

    /// Run `make` in the drivers directory. Long-running; emits progress().
    StepResult buildModule();

    /// Insert the module. Prefers the operator-supplied `iwfg.ko` in the
    /// drivers directory and inserts it directly (`insmod iwfg.ko`); only
    /// builds from source when no prebuilt module is present. Uses pkexec
    /// when available, otherwise returns ok=false with the exact command.
    StepResult insertModule();

    /// Remove the module (`rmmod iwfg`). Best-effort and idempotent: a module
    /// that is not loaded is reported as success, so exit-time teardown never
    /// blocks on an already-clean state.
    StepResult removeModule();

    /// True when this manager's ensureDriver() actually inserted the module,
    /// i.e. it was not already loaded. Exit teardown only unloads what the
    /// application itself loaded, so a module the operator inserted by hand
    /// (or one left by another tool) is never yanked out from under them.
    bool loadedByUs() const { return m_loadedByUs; }

    /// Full best-effort chain. Stops at the first hard failure. Steps are
    /// appended to `log`. Returns true when the module ends up loaded and
    /// the node exists.
    bool ensureDriver(const QString& devicePath, QVector<StepResult>& log);

signals:
    void progress(const QString& line);

private:
    bool m_loadedByUs = false;
};

} // namespace sdr
