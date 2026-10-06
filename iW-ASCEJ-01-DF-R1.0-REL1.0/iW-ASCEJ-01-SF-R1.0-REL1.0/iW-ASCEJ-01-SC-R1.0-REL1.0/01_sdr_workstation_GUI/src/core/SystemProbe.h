#pragma once
// ---------------------------------------------------------------------------
// SystemProbe — runtime environment discovery for the launcher.
//
// Everything here reads sysfs/procfs or runs a short external query; nothing
// requires elevated privileges. Results are plain structs so the UI can
// present them however it likes (status labels, the PCIe discovery splash,
// or error dialogs). All probes are best-effort: a missing file yields an
// informative "not found" rather than a crash, because this code runs on
// customer machines whose configuration we cannot assume.
// ---------------------------------------------------------------------------

#include <QString>
#include <QStringList>
#include <QVector>

namespace sdr::probe {

/// One display row of a probe result: label, value, and a traffic-light state.
struct Item {
    enum State { Ok, Warn, Fail, Info };
    QString label;
    QString value;
    State   state = Info;
};

// ------------------------------------------------------------------ GPU ----

struct GpuInfo {
    bool    present       = false;   ///< an NVIDIA GPU responded
    bool    driverLoaded  = false;   ///< kernel driver is loaded
    bool    cudaAvailable = false;   ///< CUDA runtime/library found
    QString model;
    QString memory;                  ///< e.g. "16384 MiB"
    QString driverVersion;
    QString computeCap;              ///< e.g. "8.6"
    QString detail;                  ///< human-readable summary / error
};

/// Query GPU availability. Prefers `nvidia-smi`; falls back to
/// /proc/driver/nvidia/version for the driver check. Synchronous with a
/// short timeout — call it from the launcher, not the streaming path.
GpuInfo detectGpu();

// ----------------------------------------------------------------- PCIe ----

struct PcieDevice {
    bool    found = false;
    QString bdf;              ///< bus:device.function, e.g. "0000:01:00.0"
    QString vendorId;         ///< e.g. "0x10ee" (Xilinx)
    QString deviceId;
    QString linkSpeed;        ///< current_link_speed
    QString linkWidth;        ///< current_link_width
    QString maxLinkSpeed;
    QString maxLinkWidth;
    QString driver;           ///< bound kernel driver name, if any
    QStringList bars;         ///< non-empty BAR regions, formatted
};

/// Locate the FPGA card. A device qualifies if it is bound to a driver whose
/// name contains `driverHint` (default "iwfg"), or failing that, if its
/// vendor ID matches `vendorHint` (default Xilinx 0x10ee).
PcieDevice findPcieDevice(const QString& driverHint = QStringLiteral("iwfg"),
                          const QString& vendorHint = QStringLiteral("0x10ee"));

// --------------------------------------------------------------- driver ----

struct DriverStatus {
    bool    moduleLoaded = false;    ///< present in /proc/modules
    QString moduleName;
    QString version;                 ///< /sys/module/<n>/version, if exposed
    bool    nodePresent  = false;    ///< device node exists
    bool    nodeReadable = false;    ///< open(O_RDONLY|O_NONBLOCK) succeeded
    QString nodePath;
    QString detail;
};

/// Check the iwfg kernel module and the configured device node.
DriverStatus checkDriver(const QString& devicePath,
                         const QString& moduleName = QStringLiteral("iwfg"));

// ------------------------------------------------------------- summary ----

/// Full PCIe pre-flight: device + driver + node, flattened into display rows
/// for the discovery splash. `ok` is true when acquisition can proceed.
struct PcieReport {
    bool ok = false;
    QVector<Item> items;
    QString failureSummary;          ///< one-line reason when !ok
    PcieDevice   device;
    DriverStatus driver;
};

PcieReport pciePreflight(const QString& devicePath);

// --------------------------------------------------------- ethernet ------

struct EthInterface {
    bool    present      = false;    ///< a Mellanox NIC was found
    QString name;                    ///< kernel ifname, e.g. "enp1s0f0"
    QString vendor;                  ///< PCI vendor string / id
    QString model;                   ///< device description if available
    QString macAddress;
    bool    carrier      = false;    ///< link detected
    QString operState;               ///< up / down / unknown
    int     mtu          = 0;        ///< current MTU
    QString ipAddress;               ///< current IPv4, if any
    QString detail;
};

/// Detect the first Mellanox Ethernet interface (PCI vendor 0x15b3). Read-only:
/// enumerates /sys/class/net and matches on the bound device's vendor. Returns
/// present=false with a friendly detail line on systems without one.
EthInterface detectMellanox();

struct EthConfigResult {
    bool    ok = false;
    QString log;                     ///< transcript of the ip commands
    QString failure;
    EthInterface iface;              ///< re-probed state after configuration
};

/// Bring the interface up with a fixed lab configuration: IPv4 `ip`, `mtu`,
/// state up. Uses `ip` via pkexec (the commands are exec'd directly, never
/// through a shell). Idempotent: re-applying the same address is not an error.
EthConfigResult configureMellanox(const QString& ifname,
                                   const QString& ip  = QStringLiteral("192.168.1.1"),
                                   int mtu = 9000);

/// Rows describing the interface for the status surface.
QVector<Item> describeEth(const EthInterface& e);

} // namespace sdr::probe
