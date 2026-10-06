#pragma once
#include "../core/Types.h"
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTableWidget;

namespace sdr::ui {

class ChannelDialog;

/// Left dock: where samples come from and how the signal chain is tuned.
/// Live, offline and simulated settings live in a stack so only the fields
/// that apply to the current mode are on screen.
class SourcePanel : public QWidget
{
    Q_OBJECT
public:
    explicit SourcePanel(QWidget* parent = nullptr);

    sdr::SourceConfig sourceConfig() const;
    sdr::AcqConfig    acqConfig() const;
    void              applyTo(sdr::Config& cfg) const;
    void              loadFrom(const sdr::Config& cfg);

    sdr::SourceMode mode() const;
    void setRunState(sdr::RunState state);
    void setStats(const sdr::StreamStats& stats);

    /// Apply a capture's `.meta` sidecar, if present, so a file opened from
    /// anywhere — browse button, File menu, or command line — reopens with the
    /// rate, format and resampling it was recorded with. Without this a
    /// capture taken at a decimated rate is replayed and down-converted at
    /// whatever happens to be on screen, which smears the constellation.
    void applySidecar(const QString& path);

signals:
    void configChanged();
    /// Mode, path or wire format changed — acquisition has to be restarted.
    void sourceRestartRequired();
    void seekRequested(double fraction);

private slots:
    void openChannelDialog();
    void refreshChannelField();
    void browseForFile();
    void browseForDevice();
    void syncModeStack();

private:
    QComboBox*      m_mode        = nullptr;
    QStackedWidget* m_stack       = nullptr;

    // Live DMA
    QLineEdit*      m_devicePath  = nullptr;
    QPushButton*    m_deviceBrowse= nullptr;
    QComboBox*      m_liveFormat  = nullptr;
    QSpinBox*       m_liveChannels= nullptr;
    QPushButton*    m_channelField = nullptr;
    quint32         m_channelMask  = 0xFFFFFFFFu;
    sdr::SourceMode m_liveMode     = sdr::SourceMode::Dma;  ///< PCIe or RoCEv2
    QComboBox*      m_blockSize   = nullptr;

    // Offline file
    QLineEdit*      m_filePath    = nullptr;
    QPushButton*    m_fileBrowse  = nullptr;
    QComboBox*      m_fileFormat  = nullptr;
    QSpinBox*       m_fileChannels= nullptr;
    QSpinBox*       m_headerBytes = nullptr;
    QCheckBox*      m_loop        = nullptr;
    QCheckBox*      m_paced       = nullptr;
    QDoubleSpinBox* m_speed       = nullptr;
    QSlider*        m_seek        = nullptr;
    QLabel*         m_seekLabel   = nullptr;

    // Simulation
    QComboBox*      m_modulation  = nullptr;
    QSpinBox*       m_snr         = nullptr;
    QDoubleSpinBox* m_txOffset    = nullptr;
    QSpinBox*       m_simChannels = nullptr;

    // Signal chain
    QComboBox*      m_sampleRate  = nullptr;
    QSpinBox*       m_interpolation = nullptr;
    QSpinBox*       m_decimation  = nullptr;
    QDoubleSpinBox* m_ncoFreq     = nullptr;
    QDoubleSpinBox* m_centerFreq  = nullptr;
    QComboBox*      m_bufferSize  = nullptr;

    bool m_loading = false;
};

/// Right dock: everything that changes how a frame becomes pixels.
class DisplayPanel : public QWidget
{
    Q_OBJECT
public:
    explicit DisplayPanel(QWidget* parent = nullptr);

    sdr::DisplayConfig displayConfig() const;
    void loadFrom(const sdr::Config& cfg);
    void setChannelCount(int n);

signals:
    void configChanged();
    void resetAveragingRequested();

private:
    QComboBox*      m_fftSize     = nullptr;
    QComboBox*      m_window      = nullptr;
    QComboBox*      m_average     = nullptr;
    QSpinBox*       m_avgCount    = nullptr;
    QDoubleSpinBox* m_avgAlpha    = nullptr;
    QSpinBox*       m_refreshFps  = nullptr;

    QSpinBox*       m_floorDb     = nullptr;
    QSpinBox*       m_ceilDb      = nullptr;

    QComboBox*      m_colorMap    = nullptr;
    QSpinBox*       m_wfRows      = nullptr;
    QCheckBox*      m_persistence = nullptr;
    QDoubleSpinBox* m_decay       = nullptr;

    QCheckBox*      m_peakSearch  = nullptr;
    QSpinBox*       m_peakCount   = nullptr;
    QSpinBox*       m_peakThresh  = nullptr;

    QComboBox*      m_activeChannel = nullptr;
    QCheckBox*      m_overlay     = nullptr;
    QPushButton*    m_resetAvg    = nullptr;

    bool m_loading = false;
};

/// Bottom dock: the live parameter table and the peak list.
class MeasurementPanel : public QWidget
{
    Q_OBJECT
public:
    explicit MeasurementPanel(QWidget* parent = nullptr);

    void setFrame(const sdr::FrameResult& frame);
    /// Which channel's peak table to show — driven by the display panel.
    void setActiveChannel(int ch) { m_active = ch; }
    void clear();

private:
    void rebuildColumns(int channels);

    QTableWidget* m_params = nullptr;
    QTableWidget* m_peaks  = nullptr;
    int           m_active = 0;
    int           m_columns = 0;
};

/// Right dock: acquisition health. Everything here is a number an operator
/// checks before trusting a measurement.
class StatusPanel : public QWidget
{
    Q_OBJECT
public:
    explicit StatusPanel(QWidget* parent = nullptr);

    void setStats(const sdr::StreamStats& stats, double displayFps);
    void setChannelInfo(const sdr::Config& cfg);

private:
    QLabel*       m_state       = nullptr;
    QLabel*       m_source      = nullptr;
    QLabel*       m_throughput  = nullptr;
    QLabel*       m_sampleRate  = nullptr;
    QLabel*       m_displayFps  = nullptr;
    QLabel*       m_blocks      = nullptr;
    QLabel*       m_dropped     = nullptr;
    QLabel*       m_skipped     = nullptr;
    QLabel*       m_elapsed     = nullptr;
    QLabel*       m_recorded    = nullptr;
    QProgressBar* m_buffer      = nullptr;

    QLabel*       m_chCount     = nullptr;
    QLabel*       m_chRate      = nullptr;
    QLabel*       m_chCenter    = nullptr;
    QLabel*       m_chSpan      = nullptr;
    QLabel*       m_chRbw       = nullptr;
    QLabel*       m_chFormat    = nullptr;
};

} // namespace sdr::ui
