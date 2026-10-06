#pragma once
// ---------------------------------------------------------------------------
// PcieDiscoveryDialog — the initialization screen shown after PCIe mode is
// selected and before the workstation opens.
//
// It reveals the pre-flight results row by row over ~1.2 s: link status,
// speed/width, enumeration, IDs, BARs, module, node and DMA state. On success
// it auto-accepts and the main window opens; on failure it stays up with the
// failure reason and offers the integrated driver build/insert path.
// ---------------------------------------------------------------------------

#include "../core/SystemProbe.h"

#include <QDialog>
#include <QVector>

class QGridLayout;
class QLabel;
class QPushButton;
class QTimer;

namespace sdr::ui {

class PcieDiscoveryDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PcieDiscoveryDialog(const QString& devicePath,
                                 QWidget* parent = nullptr);

    /// True when acquisition may proceed (accepted, or the operator chose to
    /// continue with a FIFO-only warning state).
    bool passed() const { return m_passed; }

    /// True when the operator's "Build & load driver" action inserted the
    /// module during this dialog, so the caller can arrange to unload it on
    /// exit and leave the system clean.
    bool driverLoadedHere() const { return m_driverLoadedHere; }

private slots:
    void revealNext();
    void onFixDriver();

private:
    void finish(bool ok);
    void addRow(const sdr::probe::Item& item, bool visible);

    QString              m_devicePath;
    sdr::probe::PcieReport m_report;
    QGridLayout*         m_grid    = nullptr;
    QLabel*              m_summary = nullptr;
    QPushButton*         m_fixBtn  = nullptr;
    QPushButton*         m_closeBtn= nullptr;
    QTimer*              m_timer   = nullptr;
    QVector<QLabel*>     m_values;
    int                  m_shown   = 0;
    bool                 m_passed  = false;
    bool                 m_driverLoadedHere = false;
};

} // namespace sdr::ui
