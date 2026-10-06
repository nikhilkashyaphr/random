#include "DriverManager.h"
#include "SystemProbe.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

namespace sdr {

namespace {

QString runCapture(const QString& prog, const QStringList& args,
                   const QString& workDir, int timeoutMs, bool* ok)
{
    QProcess p;
    if (!workDir.isEmpty()) {
        p.setWorkingDirectory(workDir);
        // setWorkingDirectory changes the child's cwd but NOT the inherited
        // $PWD env var. A Makefile that keys its object list off $(PWD) — as
        // the driver's original did — would then read a stale path and build
        // nothing. We keep the corrected $(src)-based Makefile, but also make
        // $PWD match reality here so any make invoked by the app behaves
        // exactly as it does in an interactive shell.
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("PWD"), workDir);
        p.setProcessEnvironment(env);
    }
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(prog, args);
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        if (ok) *ok = false;
        return QStringLiteral("%1 timed out after %2 s")
            .arg(prog).arg(timeoutMs / 1000);
    }
    if (ok) *ok = (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0);
    return QString::fromUtf8(p.readAll()).trimmed();
}

QString kernelRelease()
{
    bool ok = false;
    const QString r = runCapture(QStringLiteral("uname"),
                                 {QStringLiteral("-r")}, {}, 3000, &ok);
    return ok ? r : QString();
}

bool inPath(const QString& name)
{
    const QStringList dirs = QString::fromLocal8Bit(qgetenv("PATH"))
                                 .split(QLatin1Char(':'), Qt::SkipEmptyParts);
    for (const QString& d : dirs)
        if (QFileInfo::exists(d + QLatin1Char('/') + name)) return true;
    return false;
}

} // namespace

DriverManager::DriverManager(QObject* parent) : QObject(parent) {}

QString DriverManager::driversDir()
{
    const QString app = QCoreApplication::applicationDirPath();
    // Deployed: app sits beside drivers/. Development: binary is in build/,
    // drivers/ one level up. Both are checked so behaviour matches the
    // documented package layout without a special dev flag.
    for (const QString& c : {app + QStringLiteral("/drivers"),
                             app + QStringLiteral("/../drivers"),
                             QDir::currentPath() + QStringLiteral("/drivers")}) {
        QDir d(c);
        if (d.exists() && d.exists(QStringLiteral("Makefile")))
            return d.absolutePath();
    }
    return {};
}

QString DriverManager::compiledModulePath()
{
    const QString dir = driversDir();
    if (dir.isEmpty()) return {};
    // Prefer the operator-supplied iwfg.ko (the validated module named in the
    // requirements); fall back to any *.ko a source build produced.
    const QDir d(dir);
    if (d.exists(QStringLiteral("iwfg.ko")))
        return dir + QStringLiteral("/iwfg.ko");
    const QStringList kos = d.entryList({QStringLiteral("*.ko")}, QDir::Files);
    return kos.isEmpty() ? QString() : dir + QLatin1Char('/') + kos.first();
}

QVector<DriverManager::StepResult> DriverManager::checkBuildDeps()
{
    QVector<StepResult> out;
    auto dep = [&out](const QString& t, bool ok, const QString& d) {
        out.push_back({ok, t, d});
    };

    dep(QStringLiteral("make"), inPath(QStringLiteral("make")),
        inPath(QStringLiteral("make")) ? QStringLiteral("found")
            : QStringLiteral("install with: sudo apt install build-essential"));
    dep(QStringLiteral("compiler"),
        inPath(QStringLiteral("cc")) || inPath(QStringLiteral("gcc")),
        (inPath(QStringLiteral("cc")) || inPath(QStringLiteral("gcc")))
            ? QStringLiteral("found")
            : QStringLiteral("install with: sudo apt install build-essential"));

    const QString kr = kernelRelease();
    const QString hdrs = QStringLiteral("/lib/modules/%1/build").arg(kr);
    dep(QStringLiteral("kernel headers"),
        !kr.isEmpty() && QFileInfo::exists(hdrs),
        QFileInfo::exists(hdrs)
            ? hdrs
            : QStringLiteral("missing %1 — install with: sudo apt install linux-headers-%2")
                  .arg(hdrs, kr));
    return out;
}

DriverManager::StepResult DriverManager::buildModule()
{
    StepResult r;
    r.title = QStringLiteral("Build driver");

    const QString dir = driversDir();
    if (dir.isEmpty()) {
        r.detail = QStringLiteral(
            "drivers/ directory not found next to the application. "
            "Copy the iwfg driver sources (with their Makefile) into it.");
        return r;
    }

    // If a module is already compiled, do not rebuild — the operator's
    // validated iwfg.ko is authoritative, and rebuilding could diverge from
    // it. Only build when nothing is there to load.
    if (!compiledModulePath().isEmpty()) {
        r.ok = true;
        r.detail = QStringLiteral("already built: %1").arg(compiledModulePath());
        return r;
    }

    emit progress(QStringLiteral("make -C %1").arg(dir));
    bool ok = false;
    // Run `make` in the driver directory exactly as the operator would on the
    // command line, so the module produced matches a manual build. The
    // driver's own Makefile (if it shipped one) is used as-is; the bundled
    // fallback only applies when the sources arrived without one. Kernel
    // builds are minutes, not seconds, hence the generous timeout.
    const QString out = runCapture(QStringLiteral("make"),
                                   {QStringLiteral("-j2")}, dir, 300000, &ok);
    r.ok = ok && !compiledModulePath().isEmpty();
    r.detail = r.ok ? QStringLiteral("built %1").arg(compiledModulePath())
                    : (out.isEmpty() ? QStringLiteral("make produced no output")
                                     : out.right(4000));   // tail holds the error
    return r;
}

DriverManager::StepResult DriverManager::insertModule()
{
    StepResult r;
    r.title = QStringLiteral("Insert kernel module");

    const QString ko = compiledModulePath();
    if (ko.isEmpty()) {
        r.detail = QStringLiteral("no compiled module (*.ko) found");
        return r;
    }

    if (inPath(QStringLiteral("pkexec"))) {
        emit progress(QStringLiteral("pkexec insmod %1").arg(ko));
        bool ok = false;
        // insmod is exec'd directly — never through `sh -c` with the path
        // interpolated into the command string. That form executes whatever
        // the install path contains as root: a directory named with a space
        // or shell metacharacter (or a hostile name in a shared deploy
        // location) becomes root command injection.
        const QString out = runCapture(QStringLiteral("pkexec"),
            {QStringLiteral("insmod"), ko}, {}, 60000, &ok);
        r.ok = ok;
        r.detail = ok ? QStringLiteral("module inserted")
                      : QStringLiteral("pkexec failed: %1").arg(out);
    } else {
        r.detail = QStringLiteral(
            "Root privileges required and no polkit agent available.\n"
            "Run manually:  sudo insmod %1").arg(ko);
    }
    return r;
}

DriverManager::StepResult DriverManager::removeModule()
{
    StepResult r;
    r.title = QStringLiteral("Unload kernel module");

    // Idempotent: nothing loaded is a clean state, not a failure.
    if (!probe::checkDriver(QString(), QString::fromLatin1(kModuleName)).moduleLoaded) {
        r.ok = true;
        r.detail = QStringLiteral("module %1 not loaded").arg(QString::fromLatin1(kModuleName));
        return r;
    }

    if (inPath(QStringLiteral("pkexec"))) {
        emit progress(QStringLiteral("pkexec rmmod %1").arg(QString::fromLatin1(kModuleName)));
        bool ok = false;
        // rmmod by module name, exec'd directly (no shell). The name is a
        // compile-time constant, so there is nothing here to inject through.
        const QString out = runCapture(QStringLiteral("pkexec"),
            {QStringLiteral("rmmod"), QString::fromLatin1(kModuleName)}, {}, 30000, &ok);
        r.ok = ok;
        r.detail = ok ? QStringLiteral("module unloaded")
                      : QStringLiteral("pkexec rmmod failed: %1").arg(out);
    } else {
        r.detail = QStringLiteral(
            "Root privileges required and no polkit agent available.\n"
            "Run manually:  sudo rmmod %1").arg(QString::fromLatin1(kModuleName));
    }
    return r;
}

bool DriverManager::ensureDriver(const QString& devicePath,
                                 QVector<StepResult>& log)
{
    // Already loaded and the node is there — nothing to do.
    auto st = probe::checkDriver(devicePath);
    if (st.moduleLoaded && st.nodePresent) {
        log.push_back({true, QStringLiteral("Driver check"),
                       QStringLiteral("module already loaded (%1), node %2 present")
                           .arg(st.version, devicePath)});
        return true;
    }

    if (!st.moduleLoaded) {
        if (compiledModulePath().isEmpty()) {
            const auto deps = checkBuildDeps();
            bool depsOk = true;
            for (const auto& d : deps) {
                log.push_back(d);
                depsOk = depsOk && d.ok;
            }
            if (!depsOk) return false;

            const auto b = buildModule();
            log.push_back(b);
            if (!b.ok) return false;
        } else {
            log.push_back({true, QStringLiteral("Build driver"),
                           QStringLiteral("already compiled: %1")
                               .arg(compiledModulePath())});
        }

        const auto ins = insertModule();
        log.push_back(ins);
        if (!ins.ok) return false;
        m_loadedByUs = ins.ok;
    }

    // Node creation is the driver/udev's job; report rather than mknod
    // blindly, because the correct major/minor is only known to the driver.
    st = probe::checkDriver(devicePath);
    log.push_back({st.nodePresent,
                   QStringLiteral("Device node"),
                   st.nodePresent
                       ? QStringLiteral("%1 present").arg(devicePath)
                       : QStringLiteral(
                             "%1 still missing after module load — check the "
                             "driver's udev rules or dmesg for the assigned "
                             "device numbers").arg(devicePath)});
    return st.moduleLoaded && st.nodePresent;
}

} // namespace sdr
