// ---------------------------------------------------------------------------
// RfdcControl — see RfdcControl.h for the protocol and why it looks like this.
// ---------------------------------------------------------------------------

#include "RfdcControl.h"
#include "pcie_regs.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QThread>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>

namespace sdr {

RfdcControl::RfdcControl(QObject* parent) : QObject(parent) {}

RfdcControl::~RfdcControl() { detach(); }

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------
QStringList RfdcControl::discover(int vendorId, int deviceId)
{
    QStringList out;
    QDir dir(QStringLiteral("/sys/bus/pci/devices"));
    const auto entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QString& bdf : entries) {
        auto readId = [&](const char* leaf, int* val) -> bool {
            QFile f(dir.filePath(bdf) + QLatin1Char('/') + QLatin1String(leaf));
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
            bool ok = false;
            *val = f.readAll().trimmed().toInt(&ok, 16);
            return ok;
        };
        int ven = 0, dev = 0;
        if (!readId("vendor", &ven) || !readId("device", &dev)) continue;
        if (vendorId >= 0 && ven != vendorId) continue;
        if (deviceId >= 0 && dev != deviceId) continue;
        out << bdf;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Attach / detach
// ---------------------------------------------------------------------------
bool RfdcControl::attachSysfs(const QString& bdf, int bar, quint64 regBase)
{
    detach();
    m_regBase = regBase;
    m_path = QStringLiteral("/sys/bus/pci/devices/%1/resource%2").arg(bdf).arg(bar);

    QFileInfo fi(m_path);
    if (!fi.exists()) {
        m_error = tr("No such BAR: %1 — check the BDF with `lspci -D`.").arg(m_path);
        emit errorMessage(m_error);
        return false;
    }

    m_fd = ::open(m_path.toLocal8Bit().constData(), O_RDWR | O_SYNC);
    if (m_fd < 0) {
        const int e = errno;
        m_error = tr("Cannot open %1: %2").arg(m_path, QString::fromLocal8Bit(strerror(e)));
        if (e == EACCES)
            m_error += tr("  (needs root or CAP_SYS_RAWIO)");
        else if (e == EBUSY)
            m_error += tr("  (a driver holds this BAR exclusively — use the "
                          "character-device backend instead)");
        emit errorMessage(m_error);
        return false;
    }

    m_mapLen = static_cast<size_t>(fi.size());
    m_map = ::mmap(nullptr, m_mapLen, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
    if (m_map == MAP_FAILED) {
        m_error = tr("mmap of %1 bytes failed: %2")
                      .arg(m_mapLen).arg(QString::fromLocal8Bit(strerror(errno)));
        ::close(m_fd);
        m_fd = -1;
        m_map = nullptr;
        emit errorMessage(m_error);
        return false;
    }

    m_useMmap = true;
    m_attachedFlag.storeRelease(1);
    emit attachedChanged(true);
    emit statusMessage(tr("RFDC control attached: %1 (%2 bytes), base offset 0x%3")
                           .arg(m_path).arg(m_mapLen).arg(m_regBase, 0, 16));
    return true;
}

bool RfdcControl::attachCharDev(const QString& path, quint64 regBase)
{
    detach();
    m_regBase = regBase;
    m_path = path;
    m_fd = ::open(path.toLocal8Bit().constData(), O_RDWR | O_SYNC);
    if (m_fd < 0) {
        m_error = tr("Cannot open %1: %2").arg(path, QString::fromLocal8Bit(strerror(errno)));
        emit errorMessage(m_error);
        return false;
    }
    m_useMmap = false;
    m_attachedFlag.storeRelease(1);
    emit attachedChanged(true);
    emit statusMessage(tr("RFDC control attached: %1").arg(path));
    return true;
}

void RfdcControl::detach()
{
    const bool was = m_attachedFlag.loadAcquire() != 0;
    m_attachedFlag.storeRelease(0);
    if (was) emit attachedChanged(false);
    if (m_map && m_map != MAP_FAILED) { ::munmap(m_map, m_mapLen); m_map = nullptr; }
    if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }
    m_mapLen = 0;
}

// ---------------------------------------------------------------------------
// Register access
//
// volatile so the compiler cannot merge, reorder or elide MMIO — essential for
// a polled handshake.
// ---------------------------------------------------------------------------
bool RfdcControl::readRegister(quint32 offset, quint32* value)
{
    if (m_fd < 0 || !value) { m_error = tr("Not attached."); return false; }
    const quint64 off = m_regBase + offset;

    if (m_useMmap) {
        if (off + 4 > m_mapLen) {
            m_error = tr("Read 0x%1 is beyond the BAR (%2 bytes).")
                          .arg(off, 0, 16).arg(m_mapLen);
            return false;
        }
        *value = *reinterpret_cast<volatile quint32*>(
            static_cast<volatile quint8*>(m_map) + off);
        return true;
    }
    return ::pread(m_fd, value, 4, static_cast<off_t>(off)) == 4;
}

bool RfdcControl::writeRegister(quint32 offset, quint32 value)
{
    if (m_fd < 0) { m_error = tr("Not attached."); return false; }
    const quint64 off = m_regBase + offset;

    if (m_useMmap) {
        if (off + 4 > m_mapLen) {
            m_error = tr("Write 0x%1 is beyond the BAR (%2 bytes).")
                          .arg(off, 0, 16).arg(m_mapLen);
            return false;
        }
        auto* p = reinterpret_cast<volatile quint32*>(
            static_cast<volatile quint8*>(m_map) + off);
        *p = value;
        (void)*p;   // read back: force the posted write out before we continue,
                    // so payload really does land before the command word
        return true;
    }
    return ::pwrite(m_fd, &value, 4, static_cast<off_t>(off)) == 4;
}

bool RfdcControl::snapshot(quint32* ring, quint32* cfg)
{
    return readRegister(PCIE_REG_RING, ring) && readRegister(PCIE_REG_CFG, cfg);
}

// ---------------------------------------------------------------------------
// Transaction
// ---------------------------------------------------------------------------
const char* RfdcControl::errorText(unsigned code)
{
    switch (code) {
    case 0x0: return "no error";
    case 0x1: return "invalid TARGET field";
    case 0x2: return "invalid TILE field";
    case 0x3: return "invalid CHANNEL field";
    case 0x4: return "unknown EVENT code";
    case 0x5: return "payload out of range";
    case 0x6: return "EVENT not valid for this TARGET";
    case 0x7: return "tile/block not enabled in this design";
    case 0x8: return "driver rejected the setting";
    case 0x9: return "back-to-back tile encoding unsupported";
    case 0xA: return "manager not initialised";
    case 0xB: return "known code, not wired up";
    default:  return "unknown error";
    }
}

bool RfdcControl::sendCommand(const char* name, quint32 target, quint32 tile,
                              quint32 channel, quint32 event, quint32 payload)
{
    m_abort.storeRelease(0);
    if (m_busyDepth.fetchAndAddOrdered(1) == 0) emit busyChanged(true);
    struct BusyGuard {
        RfdcControl* c;
        ~BusyGuard() {
            if (c->m_busyDepth.fetchAndAddOrdered(-1) == 1) emit c->busyChanged(false);
        }
    } guard{this};

    if (m_fd < 0) {
        m_error = tr("Not attached to the PCIe control interface.");
        emit errorMessage(m_error);
        emit commandResult(QString::fromLatin1(name), false, m_error);
        return false;
    }

    // 1. the previous command must have been consumed
    quint32 cur = 0;
    if (!readRegister(PCIE_REG_RING, &cur)) {
        emit errorMessage(m_error);
        emit commandResult(QString::fromLatin1(name), false, m_error);
        return false;
    }
    if (cur & RING_NEWCMD_MASK) {
        m_error = tr("Device still has NEW_CMD set (ring=0x%1). The previous "
                     "command has not been processed, or the firmware is not "
                     "polling — check it is not sitting in a UART sub-menu.")
                      .arg(cur, 8, 16, QLatin1Char('0'));
        emit errorMessage(m_error);
        emit commandResult(QString::fromLatin1(name), false, m_error);
        return false;
    }

    const quint32 ring = RING_SET(TARGET, target)
                       | RING_SET(TILE, tile)
                       | RING_LASTTILE_MASK
                       | RING_SET(CHANNEL, channel)
                       | RING_SET(EVENT, event);

    // 2. payload BEFORE the command word
    if (!writeRegister(PCIE_REG_CFG, payload) ||
        !writeRegister(PCIE_REG_RING, ring | RING_NEWCMD_MASK)) {
        emit errorMessage(m_error);
        emit commandResult(QString::fromLatin1(name), false, m_error);
        return false;
    }

    // 3. wait for NEW_CMD to clear
    QElapsedTimer t;
    t.start();
    for (;;) {
        quint32 v = 0;
        if (!readRegister(PCIE_REG_RING, &v)) {
            emit errorMessage(m_error);
            emit commandResult(QString::fromLatin1(name), false, m_error);
            return false;
        }
        if ((v & RING_NEWCMD_MASK) == 0) {
            if (RING_ACK_VALID(v)) {
                const unsigned e = RING_ACK_ERR(v);

                // 0x4 = "unknown EVENT code". For a readback that is a
                // capability answer, not a transient error: the firmware does
                // not implement 0x0F1. Latch it so the remaining twelve reads
                // are skipped instead of spamming the console with rejections.
                if (e == 0x4 && event == PCIE_EVT_READBACK) {
                    m_readbackOk = false;
                    m_error = tr("This firmware does not implement register "
                                 "readback (event 0x0F1). pcie_cfg.c is older "
                                 "than main.c — reflash BOTH.");
                    emit errorMessage(m_error);
                    emit commandResult(QString::fromLatin1(name), false, m_error);
                    return false;
                }
                if (e != 0) {
                    m_error = tr("%1 rejected by device: %2 (0x%3)")
                                  .arg(QString::fromLatin1(name),
                                       QString::fromLatin1(errorText(e)))
                                  .arg(e, 0, 16);
                    emit errorMessage(m_error);
                    emit commandResult(QString::fromLatin1(name), false, m_error);
                    return false;
                }
            }
            emit statusMessage(tr("%1 applied.").arg(QString::fromLatin1(name)));
            emit commandResult(QString::fromLatin1(name), true, QString());
            return true;
        }
        if (m_abort.loadAcquire()) {
            m_error = tr("%1 abandoned at the operator's request after %2 ms. "
                         "The device may still complete it.")
                          .arg(QString::fromLatin1(name)).arg(t.elapsed());
            emit errorMessage(m_error);
            emit commandResult(QString::fromLatin1(name), false, m_error);
            return false;
        }
        if (t.elapsed() > m_timeoutMs) {
            m_error = tr("%1 timed out after %2 ms — the firmware did not clear "
                         "NEW_CMD. PLL changes need a longer timeout.")
                          .arg(QString::fromLatin1(name)).arg(m_timeoutMs);
            emit errorMessage(m_error);
            emit commandResult(QString::fromLatin1(name), false, m_error);
            return false;
        }
        QThread::usleep(200);
    }
}

// ---------------------------------------------------------------------------
// Documented events
// ---------------------------------------------------------------------------
bool RfdcControl::setNcoFrequency(int target, quint32 tile, quint32 channel, double mhz)
{
    const bool neg = mhz < 0.0;
    const quint32 mag = quint32((neg ? -mhz : mhz) * 1000.0 + 0.5) & PCIE_PAYLOAD_MAG_MASK;
    return sendCommand("NCO frequency", quint32(target), tile, channel,
                       PCIE_EVT_NCO_FREQ, neg ? (mag | PCIE_PAYLOAD_SIGN_MASK) : mag);
}

bool RfdcControl::setNcoPhase(int target, quint32 tile, quint32 channel, int degrees)
{
    if (degrees < -PCIE_PHASE_MAX_DEG || degrees > PCIE_PHASE_MAX_DEG) {
        m_error = tr("Phase must be -180..180 degrees.");
        emit errorMessage(m_error);
        return false;
    }
    const bool neg = degrees < 0;
    const quint32 mag = quint32(neg ? -degrees : degrees);
    return sendCommand("NCO phase", quint32(target), tile, channel,
                       PCIE_EVT_NCO_PHASE, neg ? (mag | PCIE_PAYLOAD_SIGN_MASK) : mag);
}

bool RfdcControl::setDecimation(quint32 tile, quint32 channel, quint32 factor)
{
    return sendCommand("Decimation", Adc, tile, channel, PCIE_EVT_DECIMATION, factor);
}

bool RfdcControl::setInterpolation(quint32 tile, quint32 channel, quint32 factor)
{
    return sendCommand("Interpolation", Dac, tile, channel, PCIE_EVT_INTERPOLATION, factor);
}

bool RfdcControl::setDdsFrequency(double mhz)
{
    return sendCommand("DDS frequency", Dds, 0, 0, PCIE_EVT_DDS_PHASE_INC,
                       quint32(mhz * 1e6 + 0.5));
}

// ---------------------------------------------------------------------------
// Extended events
// ---------------------------------------------------------------------------
bool RfdcControl::setSamplingRate(int target, quint32 mhz)
{
    // Reprograms the PLL, re-aligns MTS and restores NCOs. Seconds, not ms.
    const int saved = m_timeoutMs;
    if (m_timeoutMs < 15000) m_timeoutMs = 15000;
    const bool ok = sendCommand("Sampling rate", quint32(target), AllTiles,
                                BothCh, PCIE_EVT_SAMPLING_RATE, mhz);
    m_timeoutMs = saved;
    return ok;
}

bool RfdcControl::setNyquistZone(int target, quint32 zone)
{
    return sendCommand("Nyquist zone", quint32(target), AllTiles, BothCh,
                       PCIE_EVT_NYQUIST_ZONE, zone);
}

bool RfdcControl::setAdcDsa(quint32 tile, quint32 channel, double dB)
{
    return sendCommand("ADC DSA", Adc, tile, channel, PCIE_EVT_ADC_DSA,
                       quint32(dB * 10.0 + 0.5));
}

bool RfdcControl::setQmcGain(int target, quint32 tile, quint32 channel, double gain)
{
    return sendCommand("QMC gain", quint32(target), tile, channel,
                       PCIE_EVT_QMC_GAIN, quint32(gain * 10000.0 + 0.5));
}

bool RfdcControl::setDacVop(quint32 tile, quint32 channel, double milliAmps)
{
    return sendCommand("DAC VOP", Dac, tile, channel, PCIE_EVT_DAC_VOP,
                       quint32(milliAmps * 1000.0 + 0.5));
}

bool RfdcControl::setAxisRouting(int c0, int c1, int c2, int c3)
{
    const quint32 p = quint32(c0 & 0xF) | (quint32(c1 & 0xF) << 4)
                    | (quint32(c2 & 0xF) << 8) | (quint32(c3 & 0xF) << 12);
    return sendCommand("AXIS routing", Adc, AllTiles, BothCh,
                       PCIE_EVT_AXIS_ROUTING, p);
}

bool RfdcControl::setPlGpioRouting(int mode)
{
    return sendCommand("PL GPIO routing", Adc, AllTiles, BothCh,
                       PCIE_EVT_PL_GPIO_ROUTING, quint32(mode));
}

bool RfdcControl::setDacSource(int source)
{
    // `source` is logical. Old firmware reads the payload as a switch port,
    // and its ports are the other way round, so it is inverted for it.
    quint32 wire = (source == int(PCIE_DAC_SRC_HOST)) ? PCIE_DAC_SRC_HOST
                                                      : PCIE_DAC_SRC_DDS;
    const quint32 logical = wire;
    if (m_dacSrcLegacySwap)
        wire = (wire == PCIE_DAC_SRC_HOST) ? PCIE_DAC_SRC_DDS : PCIE_DAC_SRC_HOST;
    if (!sendCommand("DAC input source", Dac, AllTiles, BothCh,
                     PCIE_EVT_DAC_SOURCE, wire))
        return false;

    // Write -> read back -> verify. 1.2.1 firmware answers from the switch
    // register itself, so this is the hardware's word, not an echo.
    quint32 v = 0;
    if (!m_readbackOk || !readParameter(PCIE_RB_DAC_SOURCE, 0, 0, &v))
        return true;                     // accepted; cannot be confirmed here
    if (m_dacSrcLegacySwap && v <= 1U) v = 1U - v;
    emit dacSourceConfirmed(int(v));
    if (v != logical) {
        m_error = tr("DAC input source: requested %1, but the device reports %2.")
                      .arg(logical == PCIE_DAC_SRC_HOST ? tr("host stream") : tr("DDS"))
                      .arg(v == PCIE_DAC_SRC_HOST ? tr("host stream")
                           : v == PCIE_DAC_SRC_DDS ? tr("DDS") : tr("nothing routed"));
        emit errorMessage(m_error);
        return false;
    }
    return true;
}

bool RfdcControl::realignMts()
{
    return sendCommand("MTS re-align", Adc, AllTiles, BothCh, PCIE_EVT_MTS_REALIGN, 0);
}

bool RfdcControl::start()
{
    return sendCommand("Start", AdcAndDac, AllTiles, BothCh, PCIE_EVT_START, 0);
}

bool RfdcControl::stop()
{
    return sendCommand("Stop", AdcAndDac, AllTiles, BothCh, PCIE_EVT_STOP, 0);
}

bool RfdcControl::softReset()
{
    const int saved = m_timeoutMs;
    if (m_timeoutMs < 15000) m_timeoutMs = 15000;
    const bool ok = sendCommand("Soft reset", AdcAndDac, AllTiles, BothCh,
                                PCIE_EVT_RESET, 0);
    m_timeoutMs = saved;
    return ok;
}

void RfdcControl::attachSysfsAsync(const QString& bdf, int bar, quint64 regBase)
{
    const bool ok = attachSysfs(bdf, bar, regBase);
    emit attachResult(ok, ok ? m_path : m_error);
    if (ok) pollRegisters();
}

void RfdcControl::attachCharDevAsync(const QString& path, quint64 regBase)
{
    const bool ok = attachCharDev(path, regBase);
    emit attachResult(ok, ok ? m_path : m_error);
    if (ok) pollRegisters();
}

void RfdcControl::detachAsync()
{
    detach();
}

void RfdcControl::pollRegisters()
{
    quint32 ring = 0, cfg = 0;
    if (m_fd < 0 || !readRegister(PCIE_REG_RING, &ring)
                 || !readRegister(PCIE_REG_CFG, &cfg)) {
        emit registersRead(0, 0);
        return;
    }
    emit registersRead(ring, cfg);
}

// ---------------------------------------------------------------------------
// Single synchronisation flow:  device -> software -> UI
//
// Attach, read EVERYTHING, then let the UI display it. Nothing is shown and
// nothing is writable until this completes, so stale widget defaults can never
// be pushed over live device settings.
// ---------------------------------------------------------------------------
void RfdcControl::attachAndSync(const QString& bdf, int bar, quint64 regBase,
                                quint32 tile, quint32 channel, bool adc)
{
    emit syncingChanged(true);

    const bool ok = attachSysfs(bdf, bar, regBase);
    emit attachResult(ok, ok ? m_path : m_error);
    if (!ok) {
        emit syncFinished(false, m_error);
        emit syncingChanged(false);
        return;
    }
    syncAttached(tile, channel, adc);
}

void RfdcControl::syncAttached(quint32 tile, quint32 channel, bool adc)
{
    emit syncingChanged(true);

    // Prove the link before trusting anything we read over it.
    if (!ping()) {
        const QString why = tr("Attached, but the device did not answer a ping: %1")
                                .arg(m_error);
        emit syncFinished(false, why);
        emit syncingChanged(false);
        return;
    }

    // Ask the firmware what it is, BEFORE concluding anything from a failure.
    // Reading this is itself version-safe: firmware too old to know the id
    // rejects it with "unknown EVENT code", and that rejection IS the answer.
    quint32 abi = 0, caps = 0;
    const bool haveAbi  = readParameter(PCIE_RB_ABI, tile, channel, &abi);
    const bool haveCaps = haveAbi && readParameter(PCIE_RB_CAPS, tile, channel, &caps);

    if (haveAbi && abi < PCIE_ABI_REQUIRED) {
        emit syncFinished(false, tr(
            "Firmware ABI %1, but this application needs ABI %2.\n\n"
            "The board is running an older build. Reflash ALL firmware files "
            "together — flashing main.c alone leaves pcie_cfg.c stale, which "
            "looks identical from the outside.\n\n"
            "Confirm on the serial console after flashing:\n"
            "  [pcie] pcie_cfg ABI %2, ... READBACK(0x0F1) DEFAULTS(0x023)")
            .arg(abi).arg(PCIE_ABI_REQUIRED));
        emit syncingChanged(false);
        return;
    }
    if (!haveAbi) {
        // Could not even ask: the firmware predates the identity readback,
        // which puts it below ABI 4 by definition.
        emit syncFinished(false, tr(
            "This firmware does not support the identity query, so it predates "
            "ABI %1. Readback and restore-defaults will not work.\n\n"
            "Reflash ALL firmware files together, then confirm the ABI line on "
            "the serial console.").arg(PCIE_ABI_REQUIRED));
        emit syncingChanged(false);
        return;
    }

    emit statusMessage(tr("Firmware ABI %1, capabilities 0x%2 — compatible.")
                           .arg(abi).arg(caps, 0, 16));

    // Only a capability word that was actually READ can prove the old swap;
    // an unreadable one is not evidence, and current firmware is assumed.
    m_dacSrcLegacySwap = haveCaps && !(caps & PCIE_CAP_DAC_SOURCE_MAPPED);
    if (m_dacSrcLegacySwap)
        emit statusMessage(tr(
            "NOTE: this firmware predates 1.2.1 and has DDS and host swapped at "
            "the DAC input switch. The GUI is compensating, so its DAC input "
            "selection is correct — but serial menu 16 and rfdc_ctl dac-source "
            "are still swapped until the 1.2.1 firmware is flashed."));

    readSnapshot(tile, channel, adc);

    if (m_lastSnapshotCount == 0) {
        // A REJECTED readback (old firmware answering "unknown EVENT code") is
        // not the same code path as the unsupported-tag check, so m_readbackOk
        // stayed true and sync used to report success falsely. Judge on how
        // many values actually came back instead.
        emit syncFinished(false,
            tr("Device is reachable, but NOT ONE register could be read back. "
               "The values shown are software defaults and must not be applied. "
               "Flash a firmware that supports event 0x0F1."));
        emit syncingChanged(false);
        return;
    }

    if (!m_readbackOk) {
        emit syncFinished(false,
            tr("Device is reachable but this firmware build does not support "
               "register readback. Displayed values are NOT confirmed against "
               "hardware — flash a firmware with event 0x0F1."));
    } else {
        emit syncFinished(true, tr("Synchronised with device."));
    }
    emit syncingChanged(false);
}

// ---------------------------------------------------------------------------
// Write -> read back -> verify.
//
// `tolerance` exists because the RFDC PLL synthesises RefClk x FBDIV / OutDiv
// and cannot reach every requested rate: 2000 MHz becomes 2006 MHz. Demanding
// an exact match would report a failure where the hardware behaved correctly.
// Outside the tolerance it IS a failure and is reported as one.
// ---------------------------------------------------------------------------
void RfdcControl::setSamplingRateVerified(int target, quint32 mhz,
                                          quint32 tolerance,
                                          quint32 tile, quint32 channel)
{
    emit syncingChanged(true);
    struct Guard { RfdcControl* c; ~Guard(){ emit c->syncingChanged(false); } } g{this};

    if (!setSamplingRate(target, mhz)) {
        emit verifyResult(tr("Sampling rate"), false, mhz, 0,
                          tr("Write failed: %1").arg(m_error));
        return;
    }

    quint32 actual = 0;
    const quint32 id = (target == Dac) ? PCIE_RB_DAC_FS_MHZ : PCIE_RB_ADC_FS_MHZ;
    if (!readParameter(id, tile, channel, &actual)) {
        emit verifyResult(tr("Sampling rate"), false, mhz, 0,
                          m_readbackOk
                              ? tr("Read-back failed: %1").arg(m_error)
                              : tr("Firmware does not support read-back, so the "
                                   "write could NOT be verified."));
        return;
    }

    const quint32 diff = (actual > mhz) ? (actual - mhz) : (mhz - actual);
    if (diff > tolerance) {
        emit verifyResult(tr("Sampling rate"), false, mhz, actual,
                          tr("Device reports %1 MHz, which is more than %2 MHz "
                             "from the %3 MHz requested.")
                              .arg(actual).arg(tolerance).arg(mhz));
        return;
    }

    emit verifyResult(tr("Sampling rate"), true, mhz, actual,
                      (diff == 0)
                          ? tr("Confirmed %1 MHz.").arg(actual)
                          : tr("Confirmed %1 MHz (PLL quantised the %2 MHz "
                               "request by %3 MHz).").arg(actual).arg(mhz).arg(diff));
}

// ---------------------------------------------------------------------------
// Complete reset: stop -> defaults -> wait ready -> re-read -> report.
// Success is reported only if every stage succeeded.
// ---------------------------------------------------------------------------
void RfdcControl::completeReset(quint32 tile, quint32 channel, bool adc)
{
    emit syncingChanged(true);
    struct Guard { RfdcControl* c; ~Guard(){ emit c->syncingChanged(false); } } g{this};

    emit statusMessage(tr("Complete reset: stopping the datapath..."));
    if (!stop()) {
        emit resetFinished(false, tr("Could not stop the datapath: %1").arg(m_error));
        return;
    }

    emit statusMessage(tr("Complete reset: restoring defaults..."));
    if (!restoreDefaults()) {
        // Leaving the converters stopped after a failed reset would be worse
        // than the reset failing, so put the datapath back first.
        start();
        emit resetFinished(false, tr("Restore defaults failed: %1").arg(m_error));
        return;
    }

    // Wait for the device to answer again. restoreDefaults() may reprogram the
    // PLL and re-run MTS, so it can be unresponsive briefly.
    emit statusMessage(tr("Complete reset: waiting for the device..."));
    bool ready = false;
    for (int i = 0; i < 20 && !m_abort.loadAcquire(); ++i) {
        if (ping()) { ready = true; break; }
        QThread::msleep(100);
    }
    if (!ready) {
        emit resetFinished(false, tr("Device did not become ready after reset."));
        return;
    }

    if (!start()) {
        emit resetFinished(false, tr("Device reset, but the datapath could not "
                                     "be restarted: %1").arg(m_error));
        return;
    }

    emit statusMessage(tr("Complete reset: re-reading registers..."));
    readSnapshot(tile, channel, adc);

    if (m_lastSnapshotCount == 0 || !m_readbackOk) {
        emit resetFinished(false,
            tr("Defaults were applied, but this firmware cannot read registers "
               "back, so the displayed values are not confirmed."));
        return;
    }
    emit resetFinished(true, tr("Reset complete. Configuration synchronised "
                                "with the device."));
}

void RfdcControl::readSnapshot(quint32 tile, quint32 channel, bool adc)
{
    // One busy period for the whole batch, not one per parameter.
    if (m_busyDepth.fetchAndAddOrdered(1) == 0) emit busyChanged(true);
    struct Guard {
        RfdcControl* c;
        ~Guard() { if (c->m_busyDepth.fetchAndAddOrdered(-1) == 1) emit c->busyChanged(false); }
    } g{this};

    QVariantMap m;
    quint32 v = 0;
    int got = 0;

    auto rd = [&](const char* key, quint32 id) {
        if (readParameter(id, tile, channel, &v)) { m.insert(QLatin1String(key), v); ++got; }
    };

    rd("fs_mhz",        adc ? PCIE_RB_ADC_FS_MHZ     : PCIE_RB_DAC_FS_MHZ);
    rd("decimation",    PCIE_RB_DECIMATION);
    rd("interpolation", PCIE_RB_INTERPOLATION);
    rd("nco_khz",       adc ? PCIE_RB_ADC_NCO_KHZ    : PCIE_RB_DAC_NCO_KHZ);
    rd("dds_hz",        PCIE_RB_DDS_HZ);
    rd("dsa_db10",      PCIE_RB_ADC_DSA_DB10);
    rd("vop_ua",        PCIE_RB_DAC_VOP_UA);
    rd("qmc_10000",     PCIE_RB_QMC_GAIN_10000);
    rd("dac_source",    PCIE_RB_DAC_SOURCE);
    if (m_dacSrcLegacySwap && m.contains(QStringLiteral("dac_source"))) {
        const quint32 raw = m.value(QStringLiteral("dac_source")).toUInt();
        if (raw <= 1U) m.insert(QStringLiteral("dac_source"), 1U - raw);
    }
    rd("pl_gpio",       PCIE_RB_PL_GPIO_MODE);
    rd("axis",          PCIE_RB_AXIS_ROUTING);
    rd("nyquist",       adc ? PCIE_RB_NYQUIST_ADC    : PCIE_RB_NYQUIST_DAC);
    rd("stream_khz",    adc ? PCIE_RB_ADC_STREAM_KHZ : PCIE_RB_DAC_STREAM_KHZ);
    // The host sample rate is the DAC stream clock whatever the panel's target
    // is. Deriving it from "stream_khz" gave the ADC clock with the (default)
    // ADC target, which only matched the DAC at the boot configuration.
    rd("dac_stream_khz", PCIE_RB_DAC_STREAM_KHZ);
    // Both converters regardless of the panel's target, so the transmit chain
    // (DAC side) and the receive chain (ADC side) can be shown together while
    // H2C runs — the loopback is only understood with both in view.
    rd("adc_stream_khz", PCIE_RB_ADC_STREAM_KHZ);
    rd("adc_fs_mhz",     PCIE_RB_ADC_FS_MHZ);
    rd("dac_fs_mhz",     PCIE_RB_DAC_FS_MHZ);
    rd("adc_nco_khz",    PCIE_RB_ADC_NCO_KHZ);
    rd("dac_nco_khz",    PCIE_RB_DAC_NCO_KHZ);

    m_lastSnapshotCount = got;
    m.insert(QStringLiteral("count"), got);
    pollRegisters();
    m.insert(QStringLiteral("supported"), m_readbackOk);
    emit snapshotReady(m);
}

bool RfdcControl::ping()
{
    return sendCommand("Ping", Adc, AllTiles, BothCh, PCIE_EVT_PING, 0);
}

bool RfdcControl::restoreDefaults()
{
    const int saved = m_timeoutMs;
    if (m_timeoutMs < 20000) m_timeoutMs = 20000;
    const bool ok = sendCommand("Restore defaults", AdcAndDac, AllTiles, BothCh,
                                PCIE_EVT_DEFAULTS, 0);
    m_timeoutMs = saved;
    return ok;
}

bool RfdcControl::readParameter(quint32 id, quint32 tile, quint32 channel,
                                quint32* value)
{
    if (!value) return false;
    *value = 0;

    // Once the hardware has shown it cannot answer, stop paying for a full
    // transaction per parameter — thirteen of them per refresh otherwise.
    if (!m_readbackOk) return false;

    // The firmware overwrites the config register with the answer. We send a
    // tagged request so that an unchanged read means "this PL build cannot
    // write that register", rather than a value we would otherwise believe.
    if (!sendCommand("Readback", Adc, tile, channel,
                     PCIE_EVT_READBACK, PCIE_RB_REQUEST(id)))
        return false;

    quint32 v = 0;
    if (!readRegister(PCIE_REG_CFG, &v)) return false;

    if (PCIE_RB_IS_UNANSWERED(v)) {
        m_readbackOk = false;
        m_error = tr("Readback unsupported: the firmware could not write the "
                     "config register (request tag came back unchanged).");
        emit errorMessage(m_error);
        return false;
    }

    m_readbackOk = true;
    *value = v;
    return true;
}

} // namespace sdr
