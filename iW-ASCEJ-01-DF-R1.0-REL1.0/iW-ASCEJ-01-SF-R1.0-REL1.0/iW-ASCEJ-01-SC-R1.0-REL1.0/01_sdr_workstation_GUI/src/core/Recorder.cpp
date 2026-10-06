#include "Recorder.h"
#include "SampleCodec.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

namespace sdr {

namespace {
constexpr quint64 kReportEveryBytes = 1u << 20;   // report roughly per MiB
}

Recorder::Recorder(QObject* parent) : QObject(parent) {}

Recorder::~Recorder() { stopRecording(); }

void Recorder::startRecording(const QString& path, const sdr::Config& cfg)
{
    stopRecording();
    if (path.isEmpty()) {
        emit recorderError(QStringLiteral("No output path given"));
        return;
    }

    m_file = new QFile(path, this);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString why = m_file->errorString();
        delete m_file;
        m_file = nullptr;
        emit recorderError(QStringLiteral("Cannot write %1 — %2").arg(path, why));
        return;
    }

    m_path        = path;
    m_cfg         = cfg;
    m_bytes       = 0;
    m_samples     = 0;
    m_sinceReport = 0;

    writeSidecar(path, cfg);
    emit recordingStarted(path);
}

void Recorder::stopRecording()
{
    if (!m_file) return;
    m_file->flush();
    m_file->close();
    delete m_file;
    m_file = nullptr;
    emit recordingStopped(m_path, m_bytes, m_samples);
}

void Recorder::writeBlock(const sdr::SampleBlock& block)
{
    if (!m_file) return;

    // Blocks from the file and DMA sources carry the undecoded wire bytes, so
    // the recording is a byte-identical copy of the stream at memcpy cost.
    // Only the simulator's synthesised blocks need encoding.
    qint64 written = 0;
    if (!block.raw.isEmpty()) {
        written = m_file->write(block.raw);
    } else if (!block.channels.empty()) {
        const std::size_t frames = codec::encode(block.channels, m_cfg.src.format,
                                                 m_cfg.src.fullScale, m_scratch);
        if (frames == 0) return;
        written = m_file->write(m_scratch.data(),
                                static_cast<qint64>(m_scratch.size()));
    } else {
        return;
    }
    if (written < 0) {
        emit recorderError(QStringLiteral("Write failed on %1 — %2")
                               .arg(m_path, m_file->errorString()));
        stopRecording();
        return;
    }

    m_bytes       += static_cast<quint64>(written);
    m_samples     += block.samplesPerChannel();
    m_sinceReport += static_cast<quint64>(written);

    if (m_sinceReport >= kReportEveryBytes) {
        m_sinceReport = 0;
        emit recordingProgress(m_bytes, m_samples);
    }
}

void Recorder::writeSidecar(const QString& path, const sdr::Config& cfg) const
{
    // A capture is useless without its rate and format. The sidecar is plain
    // key=value so the loader can parse it and a human can read it.
    QFile meta(path + QStringLiteral(".meta"));
    if (!meta.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;

    QTextStream ts(&meta);
    ts << "# SDR workstation capture metadata\n";
    ts << "created="      << QDateTime::currentDateTime().toString(Qt::ISODate) << '\n';
    ts << "data_file="    << QFileInfo(path).fileName()   << '\n';
    ts << "format="       << formatName(cfg.src.format)   << '\n';
    ts << "channels="     << cfg.src.streamChannels       << '\n';
    ts << "channel_mask=0x" << QString::number(cfg.src.channelMask, 16) << '\n';
    ts << "sample_rate_msps=" << QString::number(cfg.acq.sampleRateMsps, 'f', 6) << '\n';
    ts << "center_freq_ghz="  << QString::number(cfg.acq.centerFreqGHz, 'f', 9) << '\n';
    ts << "nco_freq_mhz="     << QString::number(cfg.acq.ncoFreqMHz, 'f', 6)    << '\n';
    ts << "decimation="   << cfg.acq.decimation           << '\n';
    ts << "full_scale="   << QString::number(cfg.src.fullScale, 'f', 6) << '\n';
    ts << "header_bytes=0\n";
    ts << "layout=interleaved_iq_then_channel\n";
}

} // namespace sdr
