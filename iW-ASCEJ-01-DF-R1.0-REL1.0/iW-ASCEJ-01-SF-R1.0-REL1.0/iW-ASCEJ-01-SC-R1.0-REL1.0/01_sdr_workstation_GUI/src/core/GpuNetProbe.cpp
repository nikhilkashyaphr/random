#include "GpuNetProbe.h"
#include "SystemProbe.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

namespace sdr::probe {

namespace {

QString readFirstLine(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(f.readLine()).trimmed();
}

QString runCmd(const QString& prog, const QStringList& args, int ms = 4000)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(prog, args);
    if (!p.waitForStarted(1500)) return {};
    p.waitForFinished(ms);
    return QString::fromLocal8Bit(p.readAll()).trimmed();
}

bool inPath(const QString& exe)
{
    const QStringList dirs = QString::fromLocal8Bit(qgetenv("PATH"))
                                 .split(QLatin1Char(':'), Qt::SkipEmptyParts);
    for (const QString& d : dirs)
        if (QFileInfo::exists(d + QLatin1Char('/') + exe)) return true;
    return false;
}

bool moduleLoaded(const QString& name)
{
    QFile f(QStringLiteral("/proc/modules"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    const QString all = QString::fromUtf8(f.readAll());
    // Module names normalise '-' to '_' in /proc/modules.
    QString n = name; n.replace(QLatin1Char('-'), QLatin1Char('_'));
    for (const QString& line : all.split(QLatin1Char('\n')))
        if (line.section(QLatin1Char(' '), 0, 0) == n) return true;
    return false;
}

} // namespace

QString verdictName(Verdict v)
{
    switch (v) {
    case Verdict::Pass:          return QStringLiteral("PASS");
    case Verdict::Fail:          return QStringLiteral("FAIL");
    case Verdict::Unknown:       return QStringLiteral("UNKNOWN");
    case Verdict::NotInstalled:  return QStringLiteral("NOT INSTALLED");
    case Verdict::NotApplicable: return QStringLiteral("NOT APPLICABLE");
    }
    return QStringLiteral("UNKNOWN");
}

GpuDirectEligibility classifyGpuForGpuDirect(const QString& model)
{
    GpuDirectEligibility e;
    const QString m = model.toUpper();
    if (m.isEmpty()) {
        e.gpuClass = QStringLiteral("unknown");
        e.reason   = QStringLiteral("no GPU model reported");
        return e;
    }
    // Professional workstation parts FIRST. Substring matching against the
    // data-centre list would otherwise claim "RTX A4000" as an "A40".
    if (m.contains(QStringLiteral("QUADRO"))
        || QRegularExpression(QStringLiteral("RTX\\s*A\\d{3,4}")).match(m).hasMatch()
        || m.contains(QStringLiteral("ADA GENERATION"))) {
        e.modelEligible = true;
        e.gpuClass = QStringLiteral("Professional (RTX A / Quadro)");
        e.reason = QStringLiteral(
            "professional workstation part — GPUDirect RDMA supported "
            "(the feature is enabled on Quadro/RTX-A and data-centre lines)");
        return e;
    }
    // Data-centre parts, anchored so a longer model number cannot alias onto
    // a shorter key (A40 vs A4000, L4 vs L40).
    static const char* dc[] = {"TESLA","A100","A800","H100","H800","H200","A30","A40","A16","A10",
                               "L40S","L40","L4","V100","P100","B100","B200","GH200"};
    for (const char* k : dc) {
        const QString pat = QStringLiteral("\\b%1\\b").arg(QLatin1String(k));
        if (QRegularExpression(pat).match(m).hasMatch()) {
            e.modelEligible = true;
            e.gpuClass = QStringLiteral("Data centre");
            e.reason = QStringLiteral("data-centre part — GPUDirect RDMA supported");
            return e;
        }
    }
    if (m.contains(QStringLiteral("GEFORCE")) || m.contains(QStringLiteral("GTX"))
        || QRegularExpression(QStringLiteral("RTX\\s*[2-5]0\\d0")).match(m).hasMatch()) {
        e.modelEligible = false;
        e.gpuClass = QStringLiteral("GeForce");
        e.reason = QStringLiteral(
            "GeForce class — NVIDIA does NOT enable GPUDirect RDMA on this "
            "product line. No driver or peermem setting changes that.");
        return e;
    }
    e.gpuClass = QStringLiteral("unrecognised");
    e.reason = QStringLiteral("model not in the known lists — verify against "
                              "NVIDIA's GPUDirect RDMA support matrix");
    return e;
}

GpuTopology detectGpuTopology()
{
    GpuTopology t;
    const QString q = runCmd(QStringLiteral("nvidia-smi"),
        {QStringLiteral("--query-gpu=pci.bus_id,pcie.link.gen.current,"
                        "pcie.link.width.current,persistence_mode,compute_cap,"
                        "display_active"),
         QStringLiteral("--format=csv,noheader")});
    if (q.isEmpty() || q.contains(QStringLiteral("not found"))) {
        t.detail = QStringLiteral("nvidia-smi unavailable");
        return t;
    }
    const QStringList f = q.section(QLatin1Char('\n'), 0, 0).split(QLatin1Char(','));
    auto at = [&f](int i) { return i < f.size() ? f[i].trimmed() : QString(); };
    t.pciBdf          = at(0);
    t.linkGen         = at(1);
    t.linkWidth       = at(2);
    t.persistenceMode = at(3);
    t.computeCap      = at(4);
    t.displayAttached = at(5).compare(QStringLiteral("Enabled"), Qt::CaseInsensitive) == 0;
    return t;
}

HostInfo detectHost()
{
    HostInfo h;
    QFile f(QStringLiteral("/etc/os-release"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        for (const QString& l : QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'))) {
            const QString k = l.section(QLatin1Char('='), 0, 0);
            QString v = l.section(QLatin1Char('='), 1);
            v.remove(QLatin1Char('"'));
            if (k == QLatin1String("PRETTY_NAME")) h.osPretty = v;
            if (k == QLatin1String("VERSION_ID"))  h.osVersionId = v;
        }
    }
    h.kernel = runCmd(QStringLiteral("uname"), {QStringLiteral("-r")});
    h.arch   = runCmd(QStringLiteral("uname"), {QStringLiteral("-m")});
    const QString g = runCmd(QStringLiteral("gcc"), {QStringLiteral("-dumpfullversion")});
    h.gcc = g.isEmpty() ? QStringLiteral("(not found)") : g;
    h.python = runCmd(QStringLiteral("python3"), {QStringLiteral("--version")});
    h.cmake  = runCmd(QStringLiteral("cmake"), {QStringLiteral("--version")})
                   .section(QLatin1Char('\n'), 0, 0);
    return h;
}

// ------------------------------------------------------------------ RDMA

QVector<RdmaPort> detectRdmaPorts()
{
    QVector<RdmaPort> out;
    const QString root = qEnvironmentVariable("SDR_SYSFS_IB", QStringLiteral("/sys/class/infiniband"));
    QDir d(root);
    if (!d.exists()) return out;

    for (const QString& dev : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString base = root + QLatin1Char('/') + dev;

        // RDMA device -> Linux netdev. This is the mapping that stops anyone
        // assuming "mlx5_0" is an interface name.
        QString netdev;
        QDir netDir(base + QStringLiteral("/device/net"));
        const QStringList nets = netDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        if (!nets.isEmpty()) netdev = nets.first();

        const QString pci =
            QFileInfo(base + QStringLiteral("/device")).canonicalFilePath()
                .section(QLatin1Char('/'), -1);
        const QString fw = readFirstLine(base + QStringLiteral("/fw_ver"));

        QDir portsDir(base + QStringLiteral("/ports"));
        for (const QString& pnum : portsDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString pbase = portsDir.absolutePath() + QLatin1Char('/') + pnum;
            RdmaPort p;
            p.device     = dev;
            p.port       = pnum.toInt();
            p.netdev     = netdev;
            p.pciBdf     = pci;
            p.fwVersion  = fw;
            p.linkLayer  = readFirstLine(pbase + QStringLiteral("/link_layer"));
            p.state      = readFirstLine(pbase + QStringLiteral("/state"));
            p.physState  = readFirstLine(pbase + QStringLiteral("/phys_state"));
            p.rate       = readFirstLine(pbase + QStringLiteral("/rate"));

            // GID types decide RoCE v1 vs v2. RoCEv2 is the routable,
            // UDP/IPv4-encapsulated variant (dport 4791).
            QDir gt(pbase + QStringLiteral("/gid_attrs/types"));
            const QStringList idx = gt.entryList(QDir::Files);
            for (const QString& i : idx) {
                const QString t = readFirstLine(gt.absolutePath() + QLatin1Char('/') + i);
                if (t.isEmpty()) continue;
                if (!p.gidTypes.contains(t)) p.gidTypes << t;
                if (t.contains(QStringLiteral("v2"), Qt::CaseInsensitive) && p.gidV2.isEmpty()) {
                    p.roceV2 = true;
                    p.gidV2 = readFirstLine(pbase + QStringLiteral("/gids/") + i);
                }
            }
            out.push_back(p);
        }
    }
    return out;
}


// ------------------------------------------------- netdev <-> RDMA mapping

QString guidFromMac(const QString& mac)
{
    QString h = mac.toLower();
    h.remove(QLatin1Char(':')).remove(QLatin1Char('-'));
    if (h.size() != 12) return {};
    for (QChar c : h) if (!isxdigit(c.toLatin1())) return {};
    // Mellanox: OUI(3B) + 0x0300 + NIC-specific(3B). Standard EUI-64 would
    // insert fffe here; ConnectX does not, which is why this is derived from
    // observed hardware rather than the generic rule.
    return h.left(6) + QStringLiteral("0300") + h.right(6);
}

QVector<NetRdmaMapping> mapNetdevsToRdma()
{
    QVector<NetRdmaMapping> out;
    const QVector<RdmaPort> ports = detectRdmaPorts();

    for (const RdmaPort& p : ports) {
        NetRdmaMapping m;
        m.rdmaDevice = p.device;
        m.rdmaPort   = p.port;
        m.portState  = p.state;
        m.rate       = p.rate;
        m.linkLayer  = p.linkLayer;
        m.roceV2     = p.roceV2;
        m.gidV2      = p.gidV2;
        m.pciBdf     = p.pciBdf;
        m.netdev     = p.netdev;
        m.sysfsMapped = !p.netdev.isEmpty();

        const QString ibase = qEnvironmentVariable("SDR_SYSFS_IB", QStringLiteral("/sys/class/infiniband")) + QStringLiteral("/") + p.device;
        m.nodeGuid = readFirstLine(ibase + QStringLiteral("/node_guid"))
                         .remove(QLatin1Char(':'));
        m.activeMtu = readFirstLine(
            QStringLiteral("%1/ports/%2/rate").arg(ibase).arg(p.port));

        if (!m.netdev.isEmpty()) {
            const QString nbase = qEnvironmentVariable("SDR_SYSFS_NET", QStringLiteral("/sys/class/net")) + QStringLiteral("/") + m.netdev;
            m.mac     = readFirstLine(nbase + QStringLiteral("/address"));
            m.mtu     = readFirstLine(nbase + QStringLiteral("/mtu")).toInt();
            m.carrier = readFirstLine(nbase + QStringLiteral("/carrier"))
                            == QLatin1String("1");
            m.driver  = QFileInfo(nbase + QStringLiteral("/device/driver"))
                            .canonicalFilePath().section(QLatin1Char('/'), -1);

            if (inPath(QStringLiteral("ip"))) {
                const QString o = runCmd(QStringLiteral("ip"),
                    {QStringLiteral("-o"), QStringLiteral("-4"),
                     QStringLiteral("addr"), QStringLiteral("show"), m.netdev},
                    3000);
                const int i = o.indexOf(QStringLiteral("inet "));
                if (i >= 0) m.ipv4 = o.mid(i + 5).trimmed()
                                        .section(QLatin1Char('/'), 0, 0);
            }

            // Independent cross-check of the sysfs association.
            const QString derived = guidFromMac(m.mac);
            m.guidMatchesMac = !derived.isEmpty()
                            && !m.nodeGuid.isEmpty()
                            && derived.compare(m.nodeGuid, Qt::CaseInsensitive) == 0;
            m.mappingNote = m.guidMatchesMac
                ? QStringLiteral("sysfs + node GUID %1 matches MAC %2")
                      .arg(m.nodeGuid, m.mac)
                : QStringLiteral("sysfs says %1, but GUID %2 does not derive "
                                 "from MAC %3 (expected %4) — treat with caution")
                      .arg(m.netdev, m.nodeGuid, m.mac, derived);
        } else {
            m.mappingNote = QStringLiteral(
                "no netdev under /sys/class/infiniband/%1/device/net").arg(p.device);
        }

        // RoCEv2 GID index, needed by the receiver's -x argument.
        QDir gt(QStringLiteral("%1/ports/%2/gid_attrs/types").arg(ibase).arg(p.port));
        const QStringList idx = gt.entryList(QDir::Files);
        for (const QString& i : idx) {
            const QString t = readFirstLine(gt.absoluteFilePath(i));
            if (t.contains(QStringLiteral("v2"), Qt::CaseInsensitive)) {
                m.gidIndexV2 = i.toInt();
                break;
            }
        }
        out.push_back(m);
    }
    return out;
}

NetRdmaMapping selectRoceMapping(const QVector<NetRdmaMapping>& all, QString* reason)
{
    if (all.isEmpty()) {
        if (reason) *reason = QStringLiteral(
            "no RDMA devices found — is rdma-core / MLNX_OFED loaded?");
        return {};
    }
    // Strictly ranked, so the choice is explainable rather than incidental.
    for (const NetRdmaMapping& m : all)
        if (m.verified() && m.carrier && m.roceV2 && !m.ipv4.isEmpty()
            && m.portState.contains(QStringLiteral("ACTIVE"))) {
            if (reason) *reason = QStringLiteral(
                "%1 <-> %2: verified, link up, RoCEv2 GID %3, IPv4 %4")
                .arg(m.netdev, m.rdmaDevice).arg(m.gidIndexV2).arg(m.ipv4);
            return m;
        }

    // Nothing fully qualifies — say precisely what is missing on the best
    // candidate rather than returning a bare failure.
    const NetRdmaMapping& b = all.first();
    QStringList missing;
    if (!b.sysfsMapped)      missing << QStringLiteral("no netdev association");
    if (!b.guidMatchesMac)   missing << QStringLiteral("GUID/MAC mismatch");
    if (!b.carrier)          missing << QStringLiteral("NO CARRIER (cable/link down)");
    if (!b.roceV2)           missing << QStringLiteral("no RoCEv2 GID");
    if (b.ipv4.isEmpty())    missing << QStringLiteral("no IPv4 address");
    if (!b.portState.contains(QStringLiteral("ACTIVE")))
        missing << QStringLiteral("port state %1").arg(b.portState);
    if (reason) *reason = QStringLiteral("%1 <-> %2 not usable: %3")
        .arg(b.netdev.isEmpty() ? QStringLiteral("(none)") : b.netdev,
             b.rdmaDevice, missing.join(QStringLiteral("; ")));
    return {};
}

// ------------------------------------------------------------- peermem

PeermemStatus checkPeermem(bool attemptLoad)
{
    PeermemStatus s;
    const QString mi = runCmd(QStringLiteral("modinfo"),
                              {QStringLiteral("nvidia_peermem")});
    s.installed = !mi.isEmpty() && !mi.contains(QStringLiteral("not found"),
                                                Qt::CaseInsensitive);
    for (const QString& l : mi.split(QLatin1Char('\n')))
        if (l.startsWith(QStringLiteral("version:")))
            s.version = l.section(QLatin1Char(':'), 1).trimmed();

    s.loaded = moduleLoaded(QStringLiteral("nvidia_peermem"))
            || moduleLoaded(QStringLiteral("nv_peer_mem"));

    if (!s.loaded && attemptLoad && s.installed) {
        s.loadAttempted = true;
        QString out;
        if (inPath(QStringLiteral("pkexec")))
            out = runCmd(QStringLiteral("pkexec"),
                         {QStringLiteral("modprobe"),
                          QStringLiteral("nvidia_peermem")}, 15000);
        else
            out = runCmd(QStringLiteral("modprobe"),
                         {QStringLiteral("nvidia_peermem")}, 15000);

        // A zero exit from modprobe is NOT proof. Re-read /proc/modules.
        s.loaded = moduleLoaded(QStringLiteral("nvidia_peermem"))
                || moduleLoaded(QStringLiteral("nv_peer_mem"));
        if (!s.loaded) {
            s.error = out.trimmed();
            if (s.error.isEmpty())
                s.error = QStringLiteral(
                    "modprobe produced no output but nvidia_peermem is still "
                    "absent from /proc/modules");
        }
    }
    if (!s.installed && s.error.isEmpty())
        s.error = QStringLiteral("nvidia_peermem not installed (ships with the "
                                 "NVIDIA driver; check driver packaging)");
    return s;
}

// ------------------------------------------------------------- GPUDirect

GpuDirectStatus detectGpuDirect()
{
    GpuDirectStatus s;
    s.kernelRelease = runCmd(QStringLiteral("uname"), {QStringLiteral("-r")});

    // Either mechanism can back an ibv_reg_mr on CUDA device memory:
    //   nvidia_peermem (formerly nv_peer_mem) — the classic peer-memory path
    //   dma-buf MRs    — kernel >= 5.12 with a recent mlx5 + CUDA >= 11.7
    s.nvidiaPeermem = moduleLoaded(QStringLiteral("nvidia_peermem"))
                   || moduleLoaded(QStringLiteral("nv_peer_mem"));

    const QRegularExpression re(QStringLiteral("^(\\d+)\\.(\\d+)"));
    const auto m = re.match(s.kernelRelease);
    if (m.hasMatch()) {
        const int maj = m.captured(1).toInt(), min = m.captured(2).toInt();
        s.dmabufCapable = (maj > 5) || (maj == 5 && min >= 12);
    }

    if (s.nvidiaPeermem)
        s.detail = QStringLiteral("nvidia_peermem loaded — GPUDirect RDMA available");
    else if (s.dmabufCapable)
        s.detail = QStringLiteral(
            "nvidia_peermem NOT loaded; kernel %1 supports dma-buf MRs, so "
            "GPUDirect may work via dma-buf if the CUDA and MLNX_OFED versions "
            "support it. Verify before relying on it.").arg(s.kernelRelease);
    else
        s.detail = QStringLiteral(
            "No peer-memory path: nvidia_peermem not loaded and kernel %1 "
            "predates dma-buf MR support. NIC cannot DMA into GPU memory.")
            .arg(s.kernelRelease);
    return s;
}

// ------------------------------------------------------------------ SDKs

SdkStatus detectCuda()
{
    SdkStatus s;
    if (inPath(QStringLiteral("nvcc"))) {
        const QString out = runCmd(QStringLiteral("nvcc"), {QStringLiteral("--version")});
        const auto m = QRegularExpression(QStringLiteral("release ([0-9.]+)")).match(out);
        if (m.hasMatch()) { s.present = true; s.version = m.captured(1); }
    }
    if (!s.present && inPath(QStringLiteral("nvidia-smi"))) {
        const QString out = runCmd(QStringLiteral("nvidia-smi"),
            {QStringLiteral("--query-gpu=driver_version"),
             QStringLiteral("--format=csv,noheader")});
        if (!out.isEmpty() && !out.contains(QStringLiteral("not found"))) {
            s.present = true;
            s.version = QStringLiteral("driver ") + out.section(QLatin1Char('\n'), 0, 0);
            s.detail  = QStringLiteral("driver present; nvcc not found (runtime only)");
        }
    }
    if (!s.present) s.detail = QStringLiteral("no nvcc and no nvidia-smi");
    return s;
}

SdkStatus detectRivermax()
{
    SdkStatus s;
    const QStringList libs = {
        QStringLiteral("/usr/lib/x86_64-linux-gnu/librivermax.so"),
        QStringLiteral("/usr/lib/librivermax.so"),
        QStringLiteral("/opt/mellanox/rivermax/lib/librivermax.so"),
    };
    for (const QString& l : libs) {
        // Match the versioned soname too.
        QFileInfo fi(l);
        QDir dir(fi.absolutePath());
        const QStringList hits =
            dir.entryList({fi.fileName() + QStringLiteral("*")}, QDir::Files);
        if (!hits.isEmpty()) {
            s.present = true;
            s.path = dir.absoluteFilePath(hits.first());
            s.version = hits.first().section(QStringLiteral(".so."), 1);
            break;
        }
    }
    if (!s.present)
        s.detail = QStringLiteral("librivermax not found in standard locations");
    return s;
}

SdkStatus detectHoloscan()
{
    SdkStatus s;
    // Python package first — that is how Holoscan is most often consumed.
    const QString py = runCmd(QStringLiteral("python3"),
        {QStringLiteral("-c"),
         QStringLiteral("import holoscan,sys;sys.stdout.write(holoscan.__version__)")});
    if (!py.isEmpty() && !py.contains(QStringLiteral("Traceback"))
                     && !py.contains(QStringLiteral("Error"))) {
        s.present = true; s.version = py; s.path = QStringLiteral("python module");
        return s;
    }
    for (const QString& d : {QStringLiteral("/opt/nvidia/holoscan"),
                             QStringLiteral("/usr/lib/holoscan")}) {
        if (QFileInfo::exists(d)) {
            s.present = true; s.path = d;
            s.version = readFirstLine(d + QStringLiteral("/VERSION"));
            return s;
        }
    }
    s.detail = QStringLiteral("no holoscan python module and no /opt/nvidia/holoscan");
    return s;
}

SdkStatus detectPythonModule(const QString& moduleName)
{
    SdkStatus s;
    const QString out = runCmd(QStringLiteral("python3"),
        {QStringLiteral("-c"),
         QStringLiteral("import %1,sys;sys.stdout.write(getattr(%1,'__version__',''))")
             .arg(moduleName)});
    if (!out.isEmpty() && !out.contains(QStringLiteral("Traceback"))
                       && !out.contains(QStringLiteral("Error"))) {
        s.present = true; s.version = out;
    } else {
        s.detail = QStringLiteral("import %1 failed").arg(moduleName);
    }
    return s;
}

// ------------------------------------------------------------- assessment

GpuNetReport runGpuNetPreflight()
{
    GpuNetReport r;
    r.rdma      = detectRdmaPorts();
    r.gpuDirect = detectGpuDirect();
    r.cuda      = detectCuda();
    r.rivermax  = detectRivermax();
    r.holoscan  = detectHoloscan();
    r.numpy     = detectPythonModule(QStringLiteral("numpy"));
    r.scipy     = detectPythonModule(QStringLiteral("scipy"));
    r.cupy      = detectPythonModule(QStringLiteral("cupy"));

    const GpuInfo gpu = detectGpu();
    const EthInterface eth = detectMellanox();

    auto row = [&r](const QString& c, const QString& v, bool ok,
                    const QString& req, const QString& note = QString()) {
        r.matrix.push_back({c, v.isEmpty() ? QStringLiteral("—") : v, ok, req, note});
    };

    row(QStringLiteral("NVIDIA GPU"), gpu.model, gpu.present, QStringLiteral("Yes"),
        gpu.present ? QString() : gpu.detail);
    row(QStringLiteral("NVIDIA driver"), gpu.driverVersion, gpu.driverLoaded,
        QStringLiteral("Yes"));
    row(QStringLiteral("CUDA"), r.cuda.version, r.cuda.present, QStringLiteral("Yes"),
        r.cuda.detail);
    row(QStringLiteral("Mellanox NIC"), eth.vendor, eth.present, QStringLiteral("Yes"),
        eth.present ? QStringLiteral("netdev %1").arg(eth.name) : eth.detail);
    row(QStringLiteral("RDMA device"),
        r.rdma.isEmpty() ? QString() : r.rdma.first().device,
        !r.rdma.isEmpty(), QStringLiteral("Yes"),
        r.rdma.isEmpty() ? QStringLiteral("/sys/class/infiniband empty — "
                                          "rdma-core / MLNX_OFED not loaded")
                         : QString());
    bool roce = false;
    for (const RdmaPort& p : r.rdma) if (p.roceV2) roce = true;
    row(QStringLiteral("RoCEv2 GID"), roce ? QStringLiteral("present") : QString(),
        roce, QStringLiteral("Yes, for RDMA transport"));
    row(QStringLiteral("GPUDirect RDMA"),
        r.gpuDirect.nvidiaPeermem ? QStringLiteral("nvidia_peermem")
                                  : (r.gpuDirect.dmabufCapable ? QStringLiteral("dma-buf?")
                                                               : QString()),
        r.gpuDirect.available(), QStringLiteral("Yes, for NIC→GPU"),
        r.gpuDirect.detail);
    row(QStringLiteral("Rivermax"), r.rivermax.version, r.rivermax.present,
        QStringLiteral("Only if media/UDP"), r.rivermax.detail);
    row(QStringLiteral("Holoscan"), r.holoscan.version, r.holoscan.present,
        QStringLiteral("Optional"), r.holoscan.detail);
    row(QStringLiteral("NumPy"), r.numpy.version, r.numpy.present,
        QStringLiteral("If Python DSP"));
    row(QStringLiteral("SciPy"), r.scipy.version, r.scipy.present,
        QStringLiteral("If required"));
    row(QStringLiteral("CuPy"), r.cupy.version, r.cupy.present,
        QStringLiteral("Optional (GPU NumPy)"));

    r.host = detectHost();
    r.gpuClass = classifyGpuForGpuDirect(gpu.model);
    r.gpuTopo  = detectGpuTopology();

    // ---- per-gate verdicts, evidence only -----------------------------
    r.vRdma = r.rdma.isEmpty() ? Verdict::NotInstalled : Verdict::Pass;
    r.vRoce = r.rdma.isEmpty() ? Verdict::Unknown
                               : (roce ? Verdict::Pass : Verdict::Fail);
    r.vCuda = r.cuda.present ? Verdict::Pass : Verdict::NotInstalled;
    r.vHoloscan = r.holoscan.present ? Verdict::Pass : Verdict::NotInstalled;
    r.vRivermax = r.rivermax.present ? Verdict::Pass : Verdict::NotApplicable;
    // GPUDirect is only ever PASS after a functional data-path test; sysfs
    // can show capability, never proof. UNKNOWN is the honest ceiling here.
    if (gpu.present && !r.gpuClass.modelEligible
        && r.gpuClass.gpuClass == QLatin1String("GeForce")) {
        // Hard product-line block: no amount of software fixes this.
        r.vGpuDirect = Verdict::Fail;
    }
    else if (!gpu.present || r.rdma.isEmpty()) r.vGpuDirect = Verdict::Fail;
    else if (r.gpuDirect.nvidiaPeermem)        r.vGpuDirect = Verdict::Unknown;
    else if (r.gpuDirect.dmabufCapable)        r.vGpuDirect = Verdict::Unknown;
    else                                       r.vGpuDirect = Verdict::Fail;

    // ---- transport decision, from evidence ----------------------------
    if (roce && r.gpuDirect.available() && gpu.present) {
        r.recommendedTransport = QStringLiteral("RDMA verbs + GPUDirect RDMA");
        r.rationale = QStringLiteral(
            "The port advertises a RoCEv2 GID and a peer-memory path exists, so "
            "the NIC can place payload directly into CUDA device memory via an "
            "ibv_reg_mr over a cudaMalloc region. This is the lowest-copy path "
            "for RDMA traffic. Rivermax is NOT the transport for RoCEv2.");
        r.gpuPathViable = true;
    } else if (roce && gpu.present) {
        r.recommendedTransport = QStringLiteral("RDMA verbs → host memory (GPU path blocked)");
        r.rationale = QStringLiteral(
            "RoCEv2 is available but no peer-memory path is present, so the NIC "
            "cannot DMA into GPU memory. Receiving to host memory and copying to "
            "the GPU is possible but is NOT zero-copy and must not be described "
            "as GPUDirect.");
        r.blockers << QStringLiteral("Load nvidia_peermem (or enable dma-buf MR support)");
    } else if (!roce && !r.rdma.isEmpty()) {
        r.recommendedTransport = QStringLiteral("Undetermined — no RoCEv2 GID");
        r.rationale = QStringLiteral(
            "An RDMA device exists but no RoCEv2 GID was found on any port. "
            "Confirm the traffic really is RoCEv2 before choosing a transport; "
            "if it is plain UDP media, Rivermax becomes the candidate instead.");
        r.blockers << QStringLiteral("Confirm RoCE mode / GID index on the port");
    } else {
        r.recommendedTransport = QStringLiteral("Not determinable on this machine");
        r.rationale = QStringLiteral(
            "No RDMA device is exposed. Either rdma-core/MLNX_OFED is not "
            "installed, or this host has no ConnectX adapter. Transport cannot "
            "be chosen from evidence yet.");
        r.blockers << QStringLiteral("Install rdma-core / MLNX_OFED and re-run");
    }

    if (!gpu.present)  r.blockers << QStringLiteral("No usable NVIDIA GPU detected");
    if (!r.cuda.present) r.blockers << QStringLiteral("CUDA toolkit/runtime not detected");
    return r;
}

QString formatGpuNetReport(const GpuNetReport& r)
{
    QString o;
    auto line = [&o](const QString& s = QString()) { o += s + QLatin1Char('\n'); };

    line(QStringLiteral("========================================"));
    line(QStringLiteral("GPU/RDMA ENVIRONMENT PREFLIGHT"));
    line(QStringLiteral("========================================"));
    line();
    line(QStringLiteral("OS      : %1").arg(r.host.osPretty));
    line(QStringLiteral("Kernel  : %1  (%2)").arg(r.host.kernel, r.host.arch));
    line(QStringLiteral("GCC     : %1").arg(r.host.gcc));
    line(QStringLiteral("Python  : %1").arg(r.host.python));
    line(QStringLiteral("CMake   : %1").arg(r.host.cmake));
    line();

    line(QStringLiteral("-- RDMA devices ------------------------------------------"));
    if (r.rdma.isEmpty()) {
        line(QStringLiteral("  none (/sys/class/infiniband is empty or absent)"));
    } else {
        for (const RdmaPort& p : r.rdma) {
            line(QStringLiteral("  %1 port %2").arg(p.device).arg(p.port));
            line(QStringLiteral("    netdev      : %1")
                     .arg(p.netdev.isEmpty() ? QStringLiteral("(none)") : p.netdev));
            line(QStringLiteral("    pci         : %1").arg(p.pciBdf));
            line(QStringLiteral("    link layer  : %1").arg(p.linkLayer));
            line(QStringLiteral("    state       : %1  phys: %2").arg(p.state, p.physState));
            line(QStringLiteral("    rate        : %1").arg(p.rate));
            line(QStringLiteral("    firmware    : %1").arg(p.fwVersion));
            line(QStringLiteral("    GID types   : %1")
                     .arg(p.gidTypes.isEmpty() ? QStringLiteral("(none)")
                                               : p.gidTypes.join(QStringLiteral(", "))));
            line(QStringLiteral("    RoCEv2      : %1")
                     .arg(p.roceV2 ? QStringLiteral("YES  gid=") + p.gidV2
                                   : QStringLiteral("no")));
        }
    }
    line();

    line(QStringLiteral("-- netdev <-> RDMA mapping (discovered) ------------------"));
    {
        const auto maps = mapNetdevsToRdma();
        if (maps.isEmpty()) {
            line(QStringLiteral("  none"));
        } else {
            for (const NetRdmaMapping& m : maps) {
                line(QStringLiteral("  %1  <->  %2 port %3   [%4]")
                         .arg(m.netdev.isEmpty() ? QStringLiteral("(no netdev)") : m.netdev,
                              m.rdmaDevice).arg(m.rdmaPort)
                         .arg(m.verified() ? QStringLiteral("VERIFIED")
                                           : QStringLiteral("UNVERIFIED")));
                line(QStringLiteral("      mac %1   node_guid %2   guid<->mac %3")
                         .arg(m.mac, m.nodeGuid,
                              m.guidMatchesMac ? QStringLiteral("match")
                                               : QStringLiteral("MISMATCH")));
                line(QStringLiteral("      ipv4 %1   mtu %2   carrier %3   port %4   rate %5")
                         .arg(m.ipv4.isEmpty() ? QStringLiteral("(none)") : m.ipv4)
                         .arg(m.mtu)
                         .arg(m.carrier ? QStringLiteral("UP") : QStringLiteral("DOWN"))
                         .arg(m.portState, m.rate));
                line(QStringLiteral("      link_layer %1   RoCEv2 %2   gid_index %3")
                         .arg(m.linkLayer,
                              m.roceV2 ? QStringLiteral("yes") : QStringLiteral("no"))
                         .arg(m.gidIndexV2));
            }
            QString why;
            const NetRdmaMapping sel = selectRoceMapping(maps, &why);
            line(QStringLiteral("  selected for RoCEv2 : %1")
                     .arg(sel.rdmaDevice.isEmpty() ? QStringLiteral("NONE")
                                                   : sel.rdmaDevice));
            line(QStringLiteral("  %1").arg(why));
        }
    }
    line();

    line(QStringLiteral("-- GPU ---------------------------------------------------"));
    line(QStringLiteral("  class           : %1").arg(r.gpuClass.gpuClass));
    line(QStringLiteral("  GPUDirect elig. : %1")
             .arg(r.gpuClass.modelEligible ? QStringLiteral("YES") : QStringLiteral("NO")));
    line(QStringLiteral("  %1").arg(r.gpuClass.reason));
    line(QStringLiteral("  pci bdf         : %1").arg(r.gpuTopo.pciBdf));
    line(QStringLiteral("  pcie link       : gen %1 x%2")
             .arg(r.gpuTopo.linkGen, r.gpuTopo.linkWidth));
    line(QStringLiteral("  compute cap     : %1").arg(r.gpuTopo.computeCap));
    line(QStringLiteral("  persistence     : %1").arg(r.gpuTopo.persistenceMode));
    line(QStringLiteral("  display attached: %1")
             .arg(r.gpuTopo.displayAttached
                      ? QStringLiteral("YES - desktop shares this GPU")
                      : QStringLiteral("no")));
    line();

    line(QStringLiteral("-- GPUDirect ---------------------------------------------"));
    line(QStringLiteral("  kernel          : %1").arg(r.gpuDirect.kernelRelease));
    line(QStringLiteral("  nvidia_peermem  : %1")
             .arg(r.gpuDirect.nvidiaPeermem ? QStringLiteral("loaded")
                                            : QStringLiteral("not loaded")));
    line(QStringLiteral("  dma-buf capable : %1")
             .arg(r.gpuDirect.dmabufCapable ? QStringLiteral("kernel OK")
                                            : QStringLiteral("no")));
    line(QStringLiteral("  %1").arg(r.gpuDirect.detail));
    line();

    line(QStringLiteral("-- Dependency matrix -------------------------------------"));
    line(QStringLiteral("  %1 %2 %3 %4")
             .arg(QStringLiteral("Component"), -18)
             .arg(QStringLiteral("Version"), -22)
             .arg(QStringLiteral("Inst"), -6)
             .arg(QStringLiteral("Required")));
    for (const DepRow& d : r.matrix) {
        line(QStringLiteral("  %1 %2 %3 %4")
                 .arg(d.component, -18)
                 .arg(d.version.left(22), -22)
                 .arg(d.installed ? QStringLiteral("yes") : QStringLiteral("NO"), -6)
                 .arg(d.requirement));
        if (!d.note.isEmpty())
            line(QStringLiteral("      note: %1").arg(d.note));
    }
    line();

    line(QStringLiteral("========================================"));
    line(QStringLiteral("ARCHITECTURE DECISION"));
    line(QStringLiteral("========================================"));
    line(QStringLiteral("RoCEv2          : %1").arg(verdictName(r.vRoce)));
    line(QStringLiteral("RDMA            : %1").arg(verdictName(r.vRdma)));
    line(QStringLiteral("CUDA            : %1").arg(verdictName(r.vCuda)));
    line(QStringLiteral("GPUDirect RDMA  : %1%2").arg(verdictName(r.vGpuDirect),
         r.vGpuDirect == Verdict::Unknown
             ? QStringLiteral("  (capability seen; needs functional test)")
             : QString()));
    line(QStringLiteral("Holoscan        : %1").arg(verdictName(r.vHoloscan)));
    line(QStringLiteral("Rivermax        : %1").arg(verdictName(r.vRivermax)));
    line();
    line(QStringLiteral("-- Decision ----------------------------------------------"));
    line(QStringLiteral("  transport : %1").arg(r.recommendedTransport));
    line(QStringLiteral("  rationale : %1").arg(r.rationale));
    line(QStringLiteral("  GPU path viable : %1")
             .arg(r.gpuPathViable ? QStringLiteral("YES") : QStringLiteral("NO")));
    if (!r.blockers.isEmpty()) {
        line(QStringLiteral("  blockers  :"));
        for (const QString& b : r.blockers) line(QStringLiteral("    - %1").arg(b));
    }
    line(QStringLiteral("Overall         : %1")
             .arg(r.gpuPathViable ? QStringLiteral("READY")
                                  : QStringLiteral("NOT READY")));
    line(QStringLiteral("========================================"));
    return o;
}

} // namespace sdr::probe
