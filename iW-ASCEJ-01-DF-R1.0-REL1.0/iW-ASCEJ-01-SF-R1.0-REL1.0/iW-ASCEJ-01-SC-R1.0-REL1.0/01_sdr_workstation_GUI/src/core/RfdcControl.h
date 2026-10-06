#pragma once
// ---------------------------------------------------------------------------
// RfdcControl — live control of the RF Data Converter on the ZU47DR, over PCIe.
//
// This is a SECOND, independent control surface. It does not replace
// H2cControl:
//
//   H2cControl   /dev/shm/iqctl   -> the HOST transmitter (rdma_tx)
//                                    pacing, sample rate, centre frequency
//
//   RfdcControl  PCIe BAR regs    -> the BARE-METAL APPLICATION on the A53
//                                    NCO, decimation, interpolation, DDS,
//                                    DSA, QMC, VOP, routing, MTS, start/stop
//
// The two are complementary: one paces the host-side stream, the other
// configures the silicon it lands in.
//
// Register surface (src/core/pcie_regs.h, shared byte-for-byte with the
// bare-metal firmware and the rfdc_ctl CLI — one definition, no drift):
//
//     +0xA8  ring_register       command word, bit31 = NEW_CMD
//     +0xAC  RFDC_configuration  payload
//
// Handshake, exactly as the firmware implements it:
//
//     1. verify NEW_CMD is clear          (previous command consumed)
//     2. write payload                    (before the command word, so the
//                                          target cannot latch a half-written
//                                          parameter set)
//     3. write command word with NEW_CMD
//     4. poll until NEW_CMD clears        <- this IS the acknowledgement
//     5. if the ACK carries the status marker, decode the result code
//
// Blocking is bounded by ackTimeoutMs(). Commands that reprogram the PLL take
// seconds, so the timeout is caller-settable rather than fixed.
// ---------------------------------------------------------------------------

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QAtomicInt>
#include <cstdint>

namespace sdr {

class RfdcControl : public QObject
{
    Q_OBJECT
public:
    explicit RfdcControl(QObject* parent = nullptr);
    ~RfdcControl() override;

    // ---- connection -------------------------------------------------------

    /// Map a BAR through sysfs. `bdf` is e.g. "0000:3c:00.0".
    /// `regBase` is added to every access, for when the control block sits at
    /// an offset inside a larger BAR. Returns false with lastError() set.
    bool attachSysfs(const QString& bdf, int bar = 2, quint64 regBase = 0);

    /// Worker-thread entry points. open()/mmap() and every register access must
    /// happen on the thread that owns the descriptor, so the UI queues these
    /// rather than calling attachSysfs() across threads.
    Q_INVOKABLE void attachSysfsAsync(const QString& bdf, int bar, quint64 regBase);
    Q_INVOKABLE void attachCharDevAsync(const QString& path, quint64 regBase);
    Q_INVOKABLE void detachAsync();

    /// Read ring/config once and report them. Replaces GUI-thread snapshot().
    Q_INVOKABLE void pollRegisters();

    /// Attach, then immediately read every register and report them together.
    /// This is the single synchronisation entry point: the UI stays disabled
    /// until it completes, so default widget values can never be written back
    /// over live device settings.
    Q_INVOKABLE void attachAndSync(const QString& bdf, int bar, quint64 regBase,
                                   quint32 tile, quint32 channel, bool adc);
    /// The synchronisation half of attachAndSync(), for a device that is
    /// already attached (any backend): ping, identify the firmware, read every
    /// register. Emits the same signals.
    Q_INVOKABLE void syncAttached(quint32 tile, quint32 channel, bool adc);

    /// Write, read back, and VERIFY. `tolerance` allows for values the device
    /// legitimately quantises — the RFDC PLL cannot hit every requested rate,
    /// so an exact-match test would report a false failure.
    /// Emits verifyResult() with what was requested and what the device holds.
    Q_INVOKABLE void setSamplingRateVerified(int target, quint32 mhz,
                                             quint32 tolerance,
                                             quint32 tile, quint32 channel);

    /// Full reset: stop the datapath, restore defaults, wait for the device to
    /// answer again, then re-read everything. Reports success only if every
    /// stage succeeded.
    Q_INVOKABLE void completeReset(quint32 tile, quint32 channel, bool adc);

    /// Open a character device that supports pread/pwrite at BAR offsets,
    /// for stacks that expose the BAR as a file instead.
    bool attachCharDev(const QString& path, quint64 regBase = 0);

    void detach();
    /// Safe to read from any thread: this object normally lives on a worker
    /// thread (see ControlWindow3D), so the GUI must not touch m_fd directly.
    bool attached() const { return m_attachedFlag.loadAcquire() != 0; }

    /// Request that an in-flight transaction give up early. Lets the GUI stay
    /// responsive during a 15 s PLL reprogram instead of waiting it out.
    void requestAbort() { m_abort.storeRelease(1); }
    QString lastError() const { return m_error; }
    QString path() const { return m_path; }

    /// Endpoints found under /sys/bus/pci/devices. Pass -1 to wildcard.
    /// Xilinx is vendor 0x10ee.
    static QStringList discover(int vendorId = 0x10ee, int deviceId = -1);

    // ---- raw access (diagnostics) -----------------------------------------
    bool readRegister(quint32 offset, quint32* value);
    bool writeRegister(quint32 offset, quint32 value);

    /// Read both control registers without sending anything.
    bool snapshot(quint32* ring, quint32* cfg);

    int  ackTimeoutMs() const { return m_timeoutMs; }
    void setAckTimeoutMs(int ms) { m_timeoutMs = ms; }

    // ---- targets ----------------------------------------------------------
    enum Target { Adc = 0, Dac = 1, Dds = 2, AdcAndDac = 4 };
    enum Channel { Ch0 = 0, Ch1 = 1, BothCh = 2 };
    static constexpr quint32 AllTiles = 7;

public slots:
    // Documented events (Register_mapping.docx)
    bool setNcoFrequency(int target, quint32 tile, quint32 channel, double mhz);
    bool setNcoPhase(int target, quint32 tile, quint32 channel, int degrees);
    bool setDecimation(quint32 tile, quint32 channel, quint32 factor);
    bool setInterpolation(quint32 tile, quint32 channel, quint32 factor);
    bool setDdsFrequency(double mhz);

    // Extended events — see the EXTENSION note in pcie_regs.h
    bool setSamplingRate(int target, quint32 mhz);
    bool setNyquistZone(int target, quint32 zone);
    bool setAdcDsa(quint32 tile, quint32 channel, double dB);
    bool setQmcGain(int target, quint32 tile, quint32 channel, double gain);
    bool setDacVop(quint32 tile, quint32 channel, double milliAmps);
    bool setAxisRouting(int c0, int c1, int c2, int c3);
    bool setPlGpioRouting(int mode);
    bool setDacSource(int source);          // PCIE_DAC_SRC_DDS / _HOST (logical)
    bool realignMts();
    bool start();
    bool stop();
    bool softReset();
    bool ping();

    /// Restore the firmware's boot configuration. Reprograms the PLL only if
    /// the rate has drifted, so it is fast in the common case. Seconds worst
    /// case, so the timeout is raised internally.
    bool restoreDefaults();

    /// Read one live parameter back from the firmware.
    /// `id` is a PCIE_RB_* constant; tile/channel select the block where the
    /// parameter is per-block. Returns false if readback is unsupported by the
    /// hardware (see supportsReadback()) or the transaction failed.
    bool readParameter(quint32 id, quint32 tile, quint32 channel, quint32* value);

    /// False once a readback attempt has shown the firmware could not write
    /// the answer back — i.e. this PL build does not support it.
    bool supportsReadback() const { return m_readbackOk; }

    /// Read every displayable parameter in ONE worker-thread pass and report
    /// them together. Thirteen separate round trips from the GUI thread was
    /// the single worst source of UI stalls.
    Q_INVOKABLE void readSnapshot(quint32 tile, quint32 channel, bool adc);

signals:
    void snapshotReady(const QVariantMap& values);
    void registersRead(quint32 ring, quint32 cfg);
    void attachResult(bool ok, const QString& detail);
    /// Emitted once a full register sync has finished (or failed).
    void syncFinished(bool ok, const QString& detail);
    /// requested vs. actual after a verified write.
    void verifyResult(const QString& name, bool ok,
                      quint32 requested, quint32 actual, const QString& detail);
    void resetFinished(bool ok, const QString& detail);
    /// True while a sync or reset owns the device; the UI must stay locked.
    void syncingChanged(bool syncing);
    /// Emitted around every transaction so the UI can show a busy state and
    /// disable controls without guessing.
    void busyChanged(bool busy);
    void attachedChanged(bool attached);
    void statusMessage(const QString& line);
    void errorMessage(const QString& line);
    /// Emitted after every completed transaction, success or failure.
    void commandResult(const QString& name, bool ok, const QString& detail);
    /// The DAC input source the DEVICE reports after a change (logical
    /// PCIE_DAC_SRC_* value, already corrected for old firmware). Emitted only
    /// when it could actually be read back.
    void dacSourceConfirmed(int source);

private:
    bool sendCommand(const char* name, quint32 target, quint32 tile,
                     quint32 channel, quint32 event, quint32 payload);
    static const char* errorText(unsigned code);

    QString  m_path;
    QString  m_error;
    int      m_fd        = -1;
    void*    m_map       = nullptr;
    size_t   m_mapLen    = 0;
    quint64  m_regBase   = 0;
    bool     m_useMmap   = false;
    int      m_timeoutMs = 2000;
    bool     m_readbackOk = true;   // until proven otherwise
    // Firmware before 1.2.1 treats the DAC_SOURCE value as a raw switch port,
    // which swaps DDS and host (see pcie_regs.h PCIE_CAP_DAC_SOURCE_MAPPED).
    // Detected from the capability word at attach; while set, the value is
    // inverted in both directions so the GUI still routes what it says.
    bool     m_dacSrcLegacySwap = false;
    int      m_lastSnapshotCount = 0;  // values actually read back
    QAtomicInt m_attachedFlag { 0 };
    /* busy is refcounted: readSnapshot() runs 13 transactions and the UI
     * must see one busy period, not thirteen. */
    QAtomicInt m_busyDepth    { 0 };
    QAtomicInt m_abort        { 0 };
};

} // namespace sdr
