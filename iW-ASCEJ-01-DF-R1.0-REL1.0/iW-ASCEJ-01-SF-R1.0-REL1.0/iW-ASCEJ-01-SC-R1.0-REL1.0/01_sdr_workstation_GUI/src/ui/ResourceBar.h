#pragma once
// ---------------------------------------------------------------------------
// ResourceBar — compact CPU / RAM / GPU read-out for the menu bar, which
// switches to storage detail while recording.
//
// Uses only the existing palette (Theme.h) and the existing hintLabel /
// valueLabel typography; no new colours or shapes are introduced.
// ---------------------------------------------------------------------------

#include "../core/RecordingGuard.h"
#include "../core/ResourceMonitor.h"

#include <QWidget>

class QLabel;

namespace sdr::ui {

class ResourceBar : public QWidget
{
    Q_OBJECT
public:
    explicit ResourceBar(QWidget* parent = nullptr);

    /// Share the application's monitor rather than starting a second poller.
    void setMonitor(sdr::ResourceMonitor* m);

    /// Enter/leave recording presentation. While recording the bar shows
    /// storage rather than CPU/RAM/GPU, because that is the metric that
    /// decides whether the capture survives.
    void setRecording(bool on);
    void setRecordingStatus(const sdr::RecStatus& st);

private slots:
    void refresh();

private:
    void applyState(int severity);   ///< 0 normal, 1 warning, 2 critical
    static void paint(QLabel* l, sdr::Level lv);

    sdr::ResourceMonitor* m_mon = nullptr;
    QLabel* m_cpu  = nullptr;
    QLabel* m_ram  = nullptr;
    QLabel* m_gpu  = nullptr;
    QLabel* m_disk = nullptr;
    bool    m_recording = false;
    sdr::RecStatus m_rec;
};

} // namespace sdr::ui
