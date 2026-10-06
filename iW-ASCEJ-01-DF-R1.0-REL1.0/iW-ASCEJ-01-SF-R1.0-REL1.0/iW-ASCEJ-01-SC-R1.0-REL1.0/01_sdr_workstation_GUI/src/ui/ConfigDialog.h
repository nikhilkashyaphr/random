#pragma once
#include "../core/Types.h"
#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QSpinBox;
class QCheckBox;
class QPushButton;
class QStackedWidget;

namespace sdr::ui {

/// Pre-launch screen: pick the device, the transport, the compute target and
/// how the session should start (simulated, offline capture, or live DMA).
class ConfigDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ConfigDialog(QWidget* parent = nullptr);

    sdr::SystemConfig config() const { return m_cfg; }

private:
    QPushButton* buildToggle(const QString& text, bool checked);
    void         refreshChannelChoices();
    bool         udpSelected() const;
    void         syncSessionForTransport();
    void         repopulateDevices();
    void         refreshState();
    void         commit();

    sdr::SystemConfig m_cfg;

    QComboBox*      m_device   = nullptr;
    QComboBox*      m_ethTransport = nullptr;
    /// Which RDMA NIC carries the RoCEv2 traffic. Separate from Device:
    /// the development kit is the same board on either transport, whereas
    /// the NIC is a property of the host, not of the kit.
    /// Ask which encapsulation, then dispatch to the right extractor.
    QString extractCapture(const QString& pcap);
    /// RoCEv2: shell out to roce-extractor.
    QString extractPcap(const QString& pcap);
    /// Plain UDP: strip Ethernet/IP/UDP in-process.
    QString extractUdp(const QString& pcap);
    /// Pick the wire format by measuring the file, not by its name.
    void    detectAndApplyFormat(const QString& path);

    QComboBox*      m_rdmaNic  = nullptr;
    QLabel*         m_rdmaNicLabel = nullptr;
    QSpinBox*       m_udpPort  = nullptr;
    QCheckBox*      m_udpRaw   = nullptr;
    QPushButton*    m_pcie     = nullptr;
    QPushButton*    m_eth      = nullptr;
    QPushButton*    m_cpu      = nullptr;
    QPushButton*    m_gpu      = nullptr;
    QLabel*         m_gpuStatus= nullptr;
    QLabel*         m_ethStatus= nullptr;

    QComboBox*      m_mode     = nullptr;
    QStackedWidget* m_stack    = nullptr;
    QLineEdit*      m_device_  = nullptr;
    QLineEdit*      m_file     = nullptr;
    QComboBox*      m_format   = nullptr;
    QRadioButton*   m_flowAdc   = nullptr;
    QRadioButton*   m_flowDac   = nullptr;
    QRadioButton*   m_flowBoth  = nullptr;
    QLabel*         m_capability = nullptr;
    QLabel*         m_findings   = nullptr;
    QComboBox*      m_channels = nullptr;
    QComboBox*      m_rate     = nullptr;

    QLabel*         m_summary  = nullptr;
    QPushButton*    m_launch   = nullptr;
};

} // namespace sdr::ui
