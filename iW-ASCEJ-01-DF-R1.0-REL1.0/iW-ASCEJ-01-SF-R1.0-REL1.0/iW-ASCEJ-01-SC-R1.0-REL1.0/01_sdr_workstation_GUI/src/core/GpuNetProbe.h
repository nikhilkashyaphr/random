#pragma once
// ---------------------------------------------------------------------------
// GpuNetProbe — read-only capability discovery for the Ethernet/RoCEv2 → GPU
// data path.
//
// Everything here is inspection, never configuration. It answers the questions
// that must be settled *before* a transport is chosen:
//
//   * Which RDMA device exists, and which Linux netdev does it belong to?
//     (mlx5_0 is an RDMA device name, not an interface name — the mapping is
//     read from /sys/class/infiniband/<dev>/device/net/.)
//   * Is the port link layer Ethernet (RoCE) or InfiniBand?
//   * Does the port advertise a RoCEv2 GID?
//   * Is a GPUDirect RDMA peer-memory path present (nvidia_peermem, or
//     dma-buf), without which "NIC → GPU memory" is not achievable?
//   * Are Rivermax and Holoscan actually installed, and at what version?
//
// Implementation note: this deliberately uses sysfs and already-present
// command-line tools rather than linking libibverbs/CUDA. Detection must run
// on machines that do not yet have those SDKs — that is the entire point —
// and adding link-time dependencies to answer "is it installed?" would be
// self-defeating.
// ---------------------------------------------------------------------------

#include <QString>
#include <QStringList>
#include <QVector>

namespace sdr::probe {

/// One port of one RDMA device.
struct RdmaPort {
    QString device;          ///< e.g. "mlx5_0" (RDMA name, NOT the netdev)
    int     port = 1;
    QString netdev;          ///< the actual Linux interface, e.g. "enp1s0f0"
    QString pciBdf;          ///< e.g. "0000:01:00.0"
    QString linkLayer;       ///< "Ethernet" (RoCE) or "InfiniBand"
    QString state;           ///< ACTIVE / DOWN / …
    QString physState;
    QString rate;            ///< e.g. "100 Gb/sec"
    QString fwVersion;
    QStringList gidTypes;    ///< e.g. {"IB/RoCE v1", "RoCE v2"}
    bool    roceV2 = false;  ///< a RoCEv2 GID is present on this port
    QString gidV2;           ///< first RoCEv2 GID, if any
};

/// Enumerate RDMA devices/ports from /sys/class/infiniband.
QVector<RdmaPort> detectRdmaPorts();

/// GPUDirect RDMA peer-memory capability. Without one of these, a NIC cannot
/// DMA directly into CUDA device memory and the "zero-copy to GPU" claim is
/// not achievable on this machine.
struct GpuDirectStatus {
    bool    nvidiaPeermem = false;  ///< nvidia_peermem / nv_peer_mem loaded
    bool    dmabufCapable = false;  ///< kernel new enough for dma-buf MRs
    QString kernelRelease;
    QString detail;
    bool available() const { return nvidiaPeermem || dmabufCapable; }
};
GpuDirectStatus detectGpuDirect();

/// Presence/version of an optional SDK.
struct SdkStatus {
    bool    present = false;
    QString version;
    QString path;
    QString detail;
};

SdkStatus detectRivermax();     ///< libmedia / rivermax runtime
SdkStatus detectHoloscan();     ///< Holoscan SDK (C++ lib or python module)
SdkStatus detectCuda();         ///< CUDA runtime/driver version
SdkStatus detectPythonModule(const QString& moduleName);   ///< numpy, scipy, cupy

/// One row of the dependency matrix.
struct DepRow {
    QString component;
    QString version;
    bool    installed = false;
    QString requirement;     ///< "Yes" / "If applicable" / …
    QString note;
};

/// Verdict for one gate. Never invented: UNKNOWN and NOT_INSTALLED exist
/// precisely so the report never has to guess a PASS.
enum class Verdict { Pass, Fail, Unknown, NotInstalled, NotApplicable };
QString verdictName(Verdict v);

/// Verified association between a Linux netdev and an RDMA device.
///
/// The mapping is DISCOVERED, never assumed: sysfs
/// (/sys/class/infiniband/<dev>/device/net/) is authoritative, and the node
/// GUID is then cross-checked against the interface MAC as independent
/// confirmation. Mellanox derives the GUID by inserting 0x0300 between the
/// OUI and the NIC-specific bytes, so
///     MAC 04:3f:72:a4:11:bc  ->  GUID 043f7203 00a411bc
/// Both must agree before the mapping is reported as verified.
struct NetRdmaMapping {
    // netdev side
    QString netdev;
    QString mac;
    QString ipv4;
    int     mtu       = 0;
    bool    carrier   = false;      ///< RUNNING / link detected
    QString driver;
    QString pciBdf;

    // rdma side
    QString rdmaDevice;             ///< e.g. mlx5_0 — discovered, not hardcoded
    int     rdmaPort  = 1;
    QString nodeGuid;
    QString portState;              ///< ACTIVE / DOWN
    QString activeMtu;
    QString rate;
    QString linkLayer;
    bool    roceV2    = false;
    QString gidV2;
    int     gidIndexV2 = -1;

    // verification
    bool    sysfsMapped     = false; ///< found via /sys .../device/net
    bool    guidMatchesMac  = false; ///< arithmetic cross-check passed
    bool    verified() const { return sysfsMapped && guidMatchesMac; }
    QString mappingNote;
};

/// Mellanox node GUID derived from a MAC (lower-case hex, no separators).
/// Empty when the MAC is malformed.
QString guidFromMac(const QString& mac);

/// Discover and verify every netdev <-> RDMA association on this host.
QVector<NetRdmaMapping> mapNetdevsToRdma();

/// Pick the mapping to use for RoCEv2: prefers a verified, carrier-up,
/// RoCEv2-capable port that has an IPv4 address. Returns an invalid mapping
/// (empty rdmaDevice) plus a reason when none qualifies.
NetRdmaMapping selectRoceMapping(const QVector<NetRdmaMapping>& all,
                                 QString* reason);

/// nvidia_peermem state, with an explicit load attempt distinguished from
/// verification of the resulting state.
struct PeermemStatus {
    bool    installed     = false;   ///< modinfo finds it
    bool    loaded        = false;   ///< actually present in lsmod AFTER the attempt
    bool    loadAttempted = false;
    QString version;
    QString error;                   ///< real modprobe/kernel error when loading failed
};
/// Inspect, optionally attempt `modprobe nvidia_peermem`, then RE-VERIFY.
/// A successful modprobe exit code is never taken as proof on its own.
PeermemStatus checkPeermem(bool attemptLoad);

/// Whether the GPU model is in a class NVIDIA supports for GPUDirect RDMA.
/// This is a hard product-line restriction, not a driver setting: GeForce
/// cards do not support it regardless of driver, CUDA version or peermem.
struct GpuDirectEligibility {
    bool    modelEligible = false;
    QString gpuClass;        ///< "Data centre", "Professional (RTX A / Quadro)", "GeForce"
    QString reason;
};
GpuDirectEligibility classifyGpuForGpuDirect(const QString& model);

/// Extra GPU facts that matter for a GPUDirect data path.
struct GpuTopology {
    QString pciBdf;
    QString linkGen;         ///< current PCIe generation
    QString linkWidth;       ///< current PCIe width
    QString persistenceMode; ///< Enabled / Disabled
    QString computeCap;
    bool    displayAttached = false;  ///< X/Wayland running on this GPU
    QString detail;
};
GpuTopology detectGpuTopology();

/// Host/OS facts needed to pick exact install commands.
struct HostInfo {
    QString osPretty;        ///< /etc/os-release PRETTY_NAME
    QString osVersionId;     ///< "22.04"
    QString kernel;          ///< uname -r
    QString arch;
    QString gcc;
    QString python;
    QString cmake;
};
HostInfo detectHost();

/// Full readiness assessment for the Ethernet → GPU path.
struct GpuNetReport {
    QVector<RdmaPort> rdma;
    GpuDirectStatus   gpuDirect;
    SdkStatus         cuda, rivermax, holoscan, numpy, scipy, cupy;
    QVector<DepRow>   matrix;

    /// The architecture this machine can actually support, decided from the
    /// evidence above rather than from intent.
    HostInfo host;
    GpuDirectEligibility gpuClass;
    GpuTopology          gpuTopo;
    Verdict vRoce = Verdict::Unknown;
    Verdict vRdma = Verdict::Unknown;
    Verdict vCuda = Verdict::Unknown;
    Verdict vGpuDirect = Verdict::Unknown;
    Verdict vHoloscan = Verdict::Unknown;
    Verdict vRivermax = Verdict::NotApplicable;
    QString recommendedTransport;
    QString rationale;
    QStringList blockers;      ///< what must be installed/enabled
    bool gpuPathViable = false;
};

GpuNetReport runGpuNetPreflight();

/// Render the report as plain text for the console (`--preflight`).
QString formatGpuNetReport(const GpuNetReport& r);

} // namespace sdr::probe
