#pragma once
// ---------------------------------------------------------------------------
// H2cControl — live control of the transmit (H2C / DAC) path.
//
// This does NOT invent a control surface. It writes the `struct iq_ctl` block
// that the supplied `rdma_tx` already polls at 1 Hz, exactly as the reference
// `viz/control_panel.py` does. The controllable parameters are therefore the
// three the transmitter actually honours, and no more:
//
//     pace_gbps       transmit pacing, 0 = unlimited
//     sample_rate_hz  transmitter sample rate
//     center_freq_hz  transmitter centre frequency
//
// ABI (rdma_common.h, guarded by _Static_assert on the producer side):
//
//     /dev/shm/iqctl, 4096 bytes
//       off  0  uint64  magic "IQCTL001"
//       off  8  uint32  version
//       off 12  uint32  seq             <- bumped LAST; tx applies on change
//       off 16  double  pace_gbps
//       off 24  uint64  sample_rate_hz
//       off 32  int64   center_freq_hz
//
// Write ordering matters: fields first, then `seq`. The transmitter treats a
// change of `seq` as "the other fields are now valid", so bumping it first
// would let tx latch a half-written parameter set.
//
// No UI code here — the window binds widgets to this, and the same class can
// be driven from a test or a script.
// ---------------------------------------------------------------------------

#include <QObject>
#include <QString>

namespace sdr {

class H2cControl : public QObject
{
    Q_OBJECT
public:
    explicit H2cControl(QObject* parent = nullptr);
    ~H2cControl() override;

    static constexpr quint64 kMagic     = 0x495143544c303031ULL;  // "IQCTL001"
    static constexpr int     kCtlBytes  = 4096;

    /// Map /dev/shm/iqctl. Creates and seeds it when absent, so the control
    /// window is usable before the transmitter has started; rdma_tx picks the
    /// values up when it comes up. Returns false with a reason on failure.
    bool attach(const QString& path = QStringLiteral("/dev/shm/iqctl"));
    void detach();
    bool attached() const { return m_base != nullptr; }
    QString lastError() const { return m_error; }
    QString path() const { return m_path; }

    // Current values as read back from the block.
    double  paceGbps() const;
    quint64 sampleRateHz() const;
    qint64  centerFreqHz() const;
    quint32 sequence() const;

public slots:
    /// Write all three parameters and bump `seq`. Returns false if not attached.
    bool apply(double paceGbps, quint64 sampleRateHz, qint64 centerFreqHz);

signals:
    void applied(double paceGbps, quint64 sampleRateHz, qint64 centerFreqHz);
    void statusMessage(const QString& line);
    void errorMessage(const QString& line);

private:
    QString m_path;
    QString m_error;
    int     m_fd   = -1;
    void*   m_base = nullptr;
};

} // namespace sdr
