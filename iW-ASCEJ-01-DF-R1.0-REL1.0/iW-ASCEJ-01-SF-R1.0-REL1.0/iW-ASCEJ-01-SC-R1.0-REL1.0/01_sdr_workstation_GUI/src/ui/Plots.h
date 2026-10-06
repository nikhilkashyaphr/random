#pragma once
#include <vector>
#include <limits>
#include "../core/Types.h"
#include "ColorMap.h"
#include "PlotBase.h"
#include <QImage>

namespace sdr::ui {

/// Shared state every instrument plot needs from the display settings.
class InstrumentPlot : public PlotBase
{
    Q_OBJECT
public:
    using PlotBase::PlotBase;

    virtual void setFrame(const sdr::FrameResult& frame) = 0;
    virtual void setDisplayConfig(const sdr::DisplayConfig& cfg) { m_disp = cfg; update(); }
    virtual void clearTraces() {}

protected:
    /// Channels to draw, honouring the Active / Overlay selection.
    QVector<int> visibleChannels(std::size_t available) const;

    sdr::DisplayConfig m_disp;
    int                m_channelCount = 1;
};

// ---------------------------------------------------------------- time domain

class TimeDomainPlot : public InstrumentPlot
{
    Q_OBJECT
public:
    explicit TimeDomainPlot(QWidget* parent = nullptr);

    void setFrame(const sdr::FrameResult& f) override;
    void clearTraces() override;

protected:
    void drawContent(QPainter& p, const QRect& r) override;
    double dataXMin() const override { return 0.0; }
    double dataXMax() const override { return m_spanUs; }
    double dataYMin() const override { return -m_yMax; }
    double dataYMax() const override { return  m_yMax; }
    QString xAxisLabel() const override { return QStringLiteral("Time (µs)"); }
    QString yAxisLabel() const override { return QStringLiteral("Amplitude"); }
    QString cornerBadge() const override;
    bool traceValueAt(double x, double& yOut) const override;

private:
    struct Trace { int channel = 0; IQBlock iq; };

    QVector<Trace> m_traces;
    double m_spanUs = 100.0;
    /// Scratch for the min/max column envelope (4 lanes: loI hiI loQ hiQ).
    std::vector<float> m_envelope;
    double m_yMax   = 1.0;
};

// ------------------------------------------------------------------- spectrum

class SpectrumPlot : public InstrumentPlot
{
    Q_OBJECT
public:
    explicit SpectrumPlot(QWidget* parent = nullptr);

    void setFrame(const sdr::FrameResult& f) override;
    void setDisplayConfig(const sdr::DisplayConfig& cfg) override;
    void clearTraces() override;

protected:
    void drawContent(QPainter& p, const QRect& r) override;
    double dataXMin() const override { return m_centerHz - m_spanHz / 2.0; }
    double dataXMax() const override { return m_centerHz + m_spanHz / 2.0; }
    double dataYMin() const override { return m_disp.floorDb; }
    double dataYMax() const override { return m_disp.ceilDb; }
    QString xAxisLabel() const override { return QStringLiteral("Frequency (MHz)"); }
    QString yAxisLabel() const override { return QStringLiteral("Power (dBFS)"); }
    QString formatX(double v) const override;
    QString cornerBadge() const override;
    bool traceValueAt(double x, double& yOut) const override;

private:
    struct Trace { int channel = 0; std::vector<float> spectrum; Metrics metrics; };

    QVector<Trace> m_traces;
    double m_centerHz = 0.0, m_spanHz = 1.0;
};

// ------------------------------------------------------------------ waterfall

class WaterfallPlot : public InstrumentPlot
{
    Q_OBJECT
public:
    explicit WaterfallPlot(QWidget* parent = nullptr);

    void setFrame(const sdr::FrameResult& f) override;
    void setDisplayConfig(const sdr::DisplayConfig& cfg) override;
    void clearTraces() override;

protected:
    void drawContent(QPainter& p, const QRect& r) override;
    double dataXMin() const override { return m_centerHz - m_spanHz / 2.0; }
    double dataXMax() const override { return m_centerHz + m_spanHz / 2.0; }
    double dataYMin() const override { return 0.0; }
    double dataYMax() const override { return static_cast<double>(m_rows); }
    QString xAxisLabel() const override { return QStringLiteral("Frequency (MHz)"); }
    QString yAxisLabel() const override { return QStringLiteral("History (frames)"); }
    QString formatX(double v) const override;
    QString formatY(double v) const override
    { return QString::number(static_cast<int>(v)); }
    QString cornerBadge() const override;
    bool yZoomSupported() const override { return false; }
    /// Newest frame at the top, history running downward.
    bool yInverted() const override { return true; }

private:
    void drawRing(QPainter& p, const QRectF& dst, double sx, double sw) const;
    void drawColorBar(QPainter& p, const QRect& r) const;

    QImage   m_image;
    ColorMap m_map{ColorMap::Jet};
    double   m_centerHz = 0.0, m_spanHz = 1.0;
    double   m_floorDb = -110.0, m_ceilDb = -10.0;
    int      m_rows = 320;
    bool     m_filled = false;
    int      m_shown  = 0;
    /// Ring-buffer write cursor: the row index holding the newest spectrum.
    /// Scrolling by moving this index instead of memmoving the whole image
    /// turns an O(rows x cols) copy per frame into a single row write.
    int      m_head   = 0;
};

// --------------------------------------------------------------- constellation

class ConstellationPlot : public InstrumentPlot
{
    Q_OBJECT
public:
    explicit ConstellationPlot(QWidget* parent = nullptr);

    void setFrame(const sdr::FrameResult& f) override;
    void setDisplayConfig(const sdr::DisplayConfig& cfg) override;
    void clearTraces() override;

protected:
    void drawContent(QPainter& p, const QRect& r) override;
    QRect contentRect(const QRect& r) const override;
    double dataXMin() const override { return -m_axisMax; }
    double dataXMax() const override { return  m_axisMax; }
    double dataYMin() const override { return -m_axisMax; }
    double dataYMax() const override { return  m_axisMax; }
    QString xAxisLabel() const override { return QStringLiteral("In-phase (I)"); }
    QString yAxisLabel() const override { return QStringLiteral("Quadrature (Q)"); }
    QString cornerBadge() const override;
    bool markersSupported() const override { return false; }

private:
    static constexpr int kGrid = 256;

    std::vector<float> m_density{std::vector<float>(kGrid * kGrid, 0.0f)};
    QImage   m_image;
    ColorMap m_map{ColorMap::Jet};
    Metrics  m_metrics;
    int      m_channel  = 0;
    float    m_axisMax  = 1.6f;
};

} // namespace sdr::ui
