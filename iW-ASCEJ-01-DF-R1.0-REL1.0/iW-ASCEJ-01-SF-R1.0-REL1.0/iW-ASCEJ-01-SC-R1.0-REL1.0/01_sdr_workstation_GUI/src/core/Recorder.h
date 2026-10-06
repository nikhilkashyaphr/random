#pragma once
#include "Types.h"
#include <QObject>
#include <QString>

class QFile;

namespace sdr {

/// Writes acquisition blocks straight to disk on its own thread.
///
/// It is fed from the source's `blockReady` signal directly rather than from
/// the DSP stage, so recording captures every block even while the display is
/// throttled — a recording must not inherit the GUI's frame rate.
class Recorder : public QObject
{
    Q_OBJECT
public:
    explicit Recorder(QObject* parent = nullptr);
    ~Recorder() override;

    bool isRecording() const { return m_file != nullptr; }

public slots:
    void startRecording(const QString& path, const sdr::Config& cfg);
    void stopRecording();
    void writeBlock(const sdr::SampleBlock& block);

signals:
    void recordingStarted(const QString& path);
    void recordingStopped(const QString& path, quint64 bytes, quint64 samples);
    void recordingProgress(quint64 bytes, quint64 samples);
    void recorderError(const QString& text);

private:
    void writeSidecar(const QString& path, const sdr::Config& cfg) const;

    QFile*            m_file = nullptr;
    QString           m_path;
    Config            m_cfg;
    std::vector<char> m_scratch;
    quint64           m_bytes   = 0;
    quint64           m_samples = 0;
    quint64           m_sinceReport = 0;
};

} // namespace sdr
