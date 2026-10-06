#include "SystemProbe.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

namespace sdr::probe {

namespace {

QString readSys(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(f.readAll()).trimmed();
}

bool haveExecutable(const QString& name)
{
    const QStringList dirs = QString::fromLocal8Bit(qgetenv("PATH"))
                                 .split(QLatin1Char(':'), Qt::SkipEmptyParts);
    for (const QString& d : dirs)
        if (QFileInfo::exists(d + QLatin1Char('/') + name)) return true;
    return false;
}

/// Run a program with an explicit argument vector (never a shell string) and
/// capture merged stdout/stderr. Elevated commands go through pkexec, which is
/// added as argv[0] here — the arguments are still separate tokens, so there
/// is no interpolation to inject through.
QString runArgv(const QString& prog, const QStringList& args, int timeoutMs, bool* ok)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(prog, args);
    if (!p.waitForStarted(3000)) { if (ok) *ok = false; return QStringLiteral("failed to start %1").arg(prog); }
    p.waitForFinished(timeoutMs);
    const QString out = QString::fromLocal8Bit(p.readAll());
    if (ok) *ok = (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0);
    return out;
}

} // namespace

// ---------------------------------------------------------------- GPU -----

GpuInfo detectGpu()
{
    GpuInfo g;

    // Driver presence is visible without any tools.
    const QString procVer = readSys(QStringLiteral("/proc/driver/nvidia/version"));
    g.driverLoaded = !procVer.isEmpty();

    if (!haveExecutable(QStringLiteral("nvidia-smi"))) {
        g.detail = g.driverLoaded
            ? QStringLiteral("NVIDIA driver loaded but nvidia-smi not found in PATH")
            : QStringLiteral("No NVIDIA driver detected (no /proc/driver/nvidia, no nvidia-smi)");
        return g;
    }

    QProcess smi;
    smi.start(QStringLiteral("nvidia-smi"),
              {QStringLiteral("--query-gpu=name,memory.total,driver_version,compute_cap"),
               QStringLiteral("--format=csv,noheader")});
    // The query answers in tens of milliseconds when healthy; a hung driver
    // must not hang the launcher, hence the short hard timeout.
    if (!smi.waitForFinished(2500)) {
        smi.kill();
        g.detail = QStringLiteral("nvidia-smi did not respond (driver hung?)");
        return g;
    }
    if (smi.exitCode() != 0) {
        g.detail = QStringLiteral("nvidia-smi failed: %1")
                       .arg(QString::fromUtf8(smi.readAllStandardError()).trimmed());
        return g;
    }

    const QString out = QString::fromUtf8(smi.readAllStandardOutput()).trimmed();
    const QStringList fields = out.section(QLatin1Char('\n'), 0, 0)
                                  .split(QLatin1Char(','));
    if (fields.size() >= 4) {
        g.present       = true;
        g.driverLoaded  = true;
        g.model         = fields[0].trimmed();
        g.memory        = fields[1].trimmed();
        g.driverVersion = fields[2].trimmed();
        g.computeCap    = fields[3].trimmed();
    } else {
        g.detail = QStringLiteral("Unexpected nvidia-smi output: %1").arg(out);
        return g;
    }

    // CUDA runtime: the toolkit compiler or the runtime library will do.
    g.cudaAvailable = haveExecutable(QStringLiteral("nvcc"))
        || QFileInfo::exists(QStringLiteral("/usr/local/cuda/lib64/libcudart.so"))
        || QFileInfo::exists(QStringLiteral("/usr/lib/x86_64-linux-gnu/libcudart.so"));

    g.detail = QStringLiteral("%1 · %2 · driver %3 · compute %4 · CUDA %5")
                   .arg(g.model, g.memory, g.driverVersion, g.computeCap,
                        g.cudaAvailable ? QStringLiteral("available")
                                        : QStringLiteral("runtime not found"));
    return g;
}

// --------------------------------------------------------------- PCIe -----

PcieDevice findPcieDevice(const QString& driverHint, const QString& vendorHint)
{
    PcieDevice d;
    QDir bus(QStringLiteral("/sys/bus/pci/devices"));
    if (!bus.exists()) return d;

    QString fallbackBdf;   // vendor match without the expected driver

    const QStringList entries = bus.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& bdf : entries) {
        const QString base = bus.filePath(bdf);
        const QString drv  = QFileInfo(base + QStringLiteral("/driver"))
                                 .symLinkTarget().section(QLatin1Char('/'), -1);
        const QString ven  = readSys(base + QStringLiteral("/vendor"));

        if (!drv.isEmpty() && drv.contains(driverHint, Qt::CaseInsensitive)) {
            d.bdf = bdf;
            d.driver = drv;
            break;
        }
        if (fallbackBdf.isEmpty() && ven.compare(vendorHint, Qt::CaseInsensitive) == 0)
            fallbackBdf = bdf;
    }
    if (d.bdf.isEmpty()) d.bdf = fallbackBdf;
    if (d.bdf.isEmpty()) return d;

    const QString base = bus.filePath(d.bdf);
    d.found        = true;
    d.vendorId     = readSys(base + QStringLiteral("/vendor"));
    d.deviceId     = readSys(base + QStringLiteral("/device"));
    d.linkSpeed    = readSys(base + QStringLiteral("/current_link_speed"));
    d.linkWidth    = readSys(base + QStringLiteral("/current_link_width"));
    d.maxLinkSpeed = readSys(base + QStringLiteral("/max_link_speed"));
    d.maxLinkWidth = readSys(base + QStringLiteral("/max_link_width"));
    if (d.driver.isEmpty())
        d.driver = QFileInfo(base + QStringLiteral("/driver"))
                       .symLinkTarget().section(QLatin1Char('/'), -1);

    // BARs: /sys/.../resource is one "start end flags" line per region.
    const QString res = readSys(base + QStringLiteral("/resource"));
    int barIdx = 0;
    for (const QString& line : res.split(QLatin1Char('\n'))) {
        const QStringList f = line.simplified().split(QLatin1Char(' '));
        if (f.size() >= 2) {
            bool ok0 = false, ok1 = false;
            const qulonglong a = f[0].toULongLong(&ok0, 16);
            const qulonglong b = f[1].toULongLong(&ok1, 16);
            if (ok0 && ok1 && b > a) {
                const qulonglong sz = b - a + 1;
                d.bars << QStringLiteral("BAR%1  0x%2  %3 KiB")
                              .arg(barIdx)
                              .arg(a, 0, 16)
                              .arg(sz / 1024);
            }
        }
        ++barIdx;
    }
    return d;
}

// ------------------------------------------------------------- driver -----

DriverStatus checkDriver(const QString& devicePath, const QString& moduleName)
{
    DriverStatus st;
    st.moduleName = moduleName;
    st.nodePath   = devicePath;

    const QString mods = readSys(QStringLiteral("/proc/modules"));
    for (const QString& line : mods.split(QLatin1Char('\n')))
        if (line.startsWith(moduleName + QLatin1Char(' '))) { st.moduleLoaded = true; break; }

    st.version = readSys(QStringLiteral("/sys/module/%1/version").arg(moduleName));
    if (st.version.isEmpty() && st.moduleLoaded)
        st.version = QStringLiteral("(not exported)");

    st.nodePresent = QFileInfo::exists(devicePath);
#ifdef Q_OS_UNIX
    if (st.nodePresent) {
        const int fd = ::open(devicePath.toLocal8Bit().constData(),
                              O_RDONLY | O_NONBLOCK);
        st.nodeReadable = fd >= 0;
        if (fd >= 0) ::close(fd);
    }
#endif
    return st;
}

// ------------------------------------------------------------ summary -----

PcieReport pciePreflight(const QString& devicePath)
{
    PcieReport r;
    r.device = findPcieDevice();
    r.driver = checkDriver(devicePath);

    auto add = [&r](const QString& l, const QString& v, Item::State s) {
        r.items.push_back({l, v, s});
    };

    const bool nodeIsFifo = !devicePath.startsWith(QStringLiteral("/dev/"));

    add(QStringLiteral("PCIe link status"),
        r.device.found ? QStringLiteral("UP") : QStringLiteral("NOT DETECTED"),
        r.device.found ? Item::Ok : (nodeIsFifo ? Item::Warn : Item::Fail));

    if (r.device.found) {
        add(QStringLiteral("Link speed"),
            r.device.maxLinkSpeed.isEmpty()
                ? r.device.linkSpeed
                : QStringLiteral("%1 (max %2)").arg(r.device.linkSpeed,
                                                    r.device.maxLinkSpeed),
            Item::Ok);
        add(QStringLiteral("Link width"),
            QStringLiteral("x%1").arg(r.device.linkWidth), Item::Ok);
        add(QStringLiteral("Device enumeration"),
            r.device.bdf, Item::Ok);
        add(QStringLiteral("Vendor / Device ID"),
            QStringLiteral("%1 / %2").arg(r.device.vendorId, r.device.deviceId),
            Item::Ok);
        for (const QString& bar : r.device.bars)
            add(QStringLiteral("BAR region"), bar, Item::Info);
    }

    add(QStringLiteral("Kernel module (%1)").arg(r.driver.moduleName),
        r.driver.moduleLoaded
            ? QStringLiteral("LOADED · %1").arg(r.driver.version)
            : QStringLiteral("NOT LOADED"),
        r.driver.moduleLoaded ? Item::Ok : (nodeIsFifo ? Item::Warn : Item::Fail));

    add(QStringLiteral("Device node"),
        r.driver.nodePresent
            ? QStringLiteral("%1 · %2").arg(r.driver.nodePath,
                  r.driver.nodeReadable ? QStringLiteral("ACCESSIBLE")
                                        : QStringLiteral("PERMISSION DENIED"))
            : QStringLiteral("%1 · MISSING").arg(r.driver.nodePath),
        (r.driver.nodePresent && r.driver.nodeReadable) ? Item::Ok : Item::Fail);

    add(QStringLiteral("DMA engine"),
        (r.driver.nodePresent && r.driver.nodeReadable)
            ? QStringLiteral("READY") : QStringLiteral("UNAVAILABLE"),
        (r.driver.nodePresent && r.driver.nodeReadable) ? Item::Ok : Item::Fail);

    // A named FIFO is a legitimate loopback/test source with no PCIe card or
    // module behind it, so only the node itself is a hard requirement there.
    r.ok = r.driver.nodePresent && r.driver.nodeReadable
        && (nodeIsFifo || (r.device.found && r.driver.moduleLoaded));

    if (!r.ok) {
        if (!r.driver.nodePresent)
            r.failureSummary = QStringLiteral(
                "Device node %1 does not exist. Load the iwfg driver or check udev rules.")
                .arg(devicePath);
        else if (!r.driver.nodeReadable)
            r.failureSummary = QStringLiteral(
                "Device node %1 exists but is not readable — check permissions (udev/group).")
                .arg(devicePath);
        else if (!r.device.found)
            r.failureSummary = QStringLiteral(
                "No PCIe device bound to the iwfg driver (and no Xilinx device found).");
        else
            r.failureSummary = QStringLiteral(
                "The iwfg kernel module is not loaded.");
    }
    return r;
}

// ============================================================= ethernet ====

// Mellanox / NVIDIA-Networking PCI vendor id.
static constexpr const char* kMellanoxVendor = "0x15b3";

EthInterface detectMellanox()
{
    EthInterface e;
    QDir netDir(QStringLiteral("/sys/class/net"));
    const QStringList ifaces = netDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QString& name : ifaces) {
        if (name == QLatin1String("lo")) continue;
        const QString base = QStringLiteral("/sys/class/net/") + name;
        // The NIC's PCI vendor lives under device/vendor for a real adapter;
        // virtual interfaces have no such link and are skipped.
        const QString vendor = readSys(base + QStringLiteral("/device/vendor"));
        if (vendor.compare(QLatin1String(kMellanoxVendor), Qt::CaseInsensitive) != 0)
            continue;

        e.present    = true;
        e.name       = name;
        e.vendor     = QStringLiteral("Mellanox (0x15b3)");
        e.macAddress = readSys(base + QStringLiteral("/address"));
        e.operState  = readSys(base + QStringLiteral("/operstate"));
        e.carrier    = readSys(base + QStringLiteral("/carrier")) == QLatin1String("1");
        e.mtu        = readSys(base + QStringLiteral("/mtu")).toInt();
        e.model      = readSys(base + QStringLiteral("/device/device"));

        // First IPv4, best-effort via `ip -o -4 addr show <name>`.
        if (haveExecutable(QStringLiteral("ip"))) {
            bool ok = false;
            const QString out = runArgv(QStringLiteral("ip"),
                {QStringLiteral("-o"), QStringLiteral("-4"), QStringLiteral("addr"),
                 QStringLiteral("show"), name}, 3000, &ok);
            const int inet = out.indexOf(QStringLiteral("inet "));
            if (inet >= 0) {
                const QString rest = out.mid(inet + 5).trimmed();
                e.ipAddress = rest.section(QLatin1Char('/'), 0, 0);
            }
        }
        e.detail = QStringLiteral("%1 — %2, MTU %3")
                       .arg(name, e.operState.isEmpty() ? QStringLiteral("unknown") : e.operState)
                       .arg(e.mtu);
        return e;                        // first match wins (requirement: first port)
    }

    e.detail = QStringLiteral("No Mellanox adapter found — Ethernet mode unavailable");
    return e;
}

EthConfigResult configureMellanox(const QString& ifname, const QString& ip, int mtu)
{
    EthConfigResult r;
    if (ifname.isEmpty()) { r.failure = QStringLiteral("no interface"); return r; }

    if (!haveExecutable(QStringLiteral("ip"))) {
        r.failure = QStringLiteral("iproute2 (`ip`) not found");
        return r;
    }
    const bool needsPkexec = (geteuid() != 0);
    if (needsPkexec && !haveExecutable(QStringLiteral("pkexec"))) {
        r.failure = QStringLiteral(
            "Root privileges required and no polkit agent available.\n"
            "Run:  sudo ip addr add %1/24 dev %2 && "
            "sudo ip link set %2 mtu %3 up").arg(ip, ifname).arg(mtu);
        return r;
    }

    auto ipCmd = [&](const QStringList& args, bool* ok) {
        QStringList full;
        QString prog;
        if (needsPkexec) { prog = QStringLiteral("pkexec"); full << QStringLiteral("ip"); }
        else             { prog = QStringLiteral("ip"); }
        full += args;
        const QString out = runArgv(prog, full, 8000, ok);
        r.log += QStringLiteral("$ %1 %2\n%3\n").arg(prog, full.join(QLatin1Char(' ')), out);
        return out;
    };

    // add addr (ignore "exists"), set mtu + up.
    bool ok1 = false;
    ipCmd({QStringLiteral("addr"), QStringLiteral("add"),
           QStringLiteral("%1/24").arg(ip), QStringLiteral("dev"), ifname}, &ok1);
    // A second identical add returns non-zero ("File exists"); that is success
    // for our purposes, so we do not gate on ok1.

    bool ok2 = false;
    ipCmd({QStringLiteral("link"), QStringLiteral("set"), ifname,
           QStringLiteral("mtu"), QString::number(mtu), QStringLiteral("up")}, &ok2);

    r.iface = detectMellanox();
    r.ok = ok2 && r.iface.present && r.iface.mtu == mtu;
    if (!r.ok && r.failure.isEmpty())
        r.failure = QStringLiteral("configuration did not fully apply — see log");
    return r;
}

QVector<Item> describeEth(const EthInterface& e)
{
    QVector<Item> items;
    if (!e.present) {
        items.push_back({QStringLiteral("Mellanox adapter"), QStringLiteral("not found"), Item::Warn});
        return items;
    }
    items.push_back({QStringLiteral("Interface"), e.name, Item::Ok});
    items.push_back({QStringLiteral("Vendor"), e.vendor, Item::Info});
    items.push_back({QStringLiteral("MAC"), e.macAddress, Item::Info});
    items.push_back({QStringLiteral("Link"),
                     e.carrier ? QStringLiteral("up") : QStringLiteral("down"),
                     e.carrier ? Item::Ok : Item::Warn});
    items.push_back({QStringLiteral("MTU"), QString::number(e.mtu),
                     e.mtu >= 9000 ? Item::Ok : Item::Info});
    items.push_back({QStringLiteral("IPv4"),
                     e.ipAddress.isEmpty() ? QStringLiteral("(none)") : e.ipAddress,
                     e.ipAddress.isEmpty() ? Item::Warn : Item::Ok});
    return items;
}

} // namespace sdr::probe
