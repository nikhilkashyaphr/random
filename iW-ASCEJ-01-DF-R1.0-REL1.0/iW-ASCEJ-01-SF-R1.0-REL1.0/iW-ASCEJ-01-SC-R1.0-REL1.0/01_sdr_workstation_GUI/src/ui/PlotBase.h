#pragma once
#include "../core/Types.h"
#include <QRectF>
#include <QVector>
#include <QWidget>

class QMenu;

namespace sdr::ui {

/// Common chrome, axis handling and interaction for every instrument plot.
///
/// The view is held in *data* units, not pixels, so a plot stays where the
/// user put it while the data underneath keeps changing. Derived classes
/// declare their full data extent and draw in pixel space; PlotBase owns the
/// mapping, the tick generation, zoom/pan, the crosshair and the markers.
///
/// Interaction:
///   wheel               zoom X about the pointer   (Ctrl: Y, Shift: both)
///   left drag           pan
///   Shift + left drag   box zoom
///   double click        reset view
///   Ctrl + left click   drop a marker
///   right click         context menu
class PlotBase : public QWidget
{
    Q_OBJECT
public:
    explicit PlotBase(const QString& title, QWidget* parent = nullptr);

    void setTitle(const QString& t) { m_title = t; update(); }
    QString title() const { return m_title; }

    QSize minimumSizeHint() const override { return {220, 150}; }

    /// Current X view in data units. Used to link plots that share an axis.
    double viewX0() const { return m_vx0; }
    double viewX1() const { return m_vx1; }
    void   setViewX(double x0, double x1, bool user = true);

    bool isZoomed() const { return m_userX || m_userY; }

public slots:
    void resetView();
    /// Zoom about the view centre — used by the context menu and any
    /// external toolbar wiring.
    void zoomAtCentre(double factor);
    /// Prompt for an explicit X range (manual axis control).
    void promptAxisRange();
    void clearMarkers();
    void setCrosshairEnabled(bool on) { m_crosshair = on; update(); }

signals:
    /// Emitted whenever the X view changes, so a linked plot can follow.
    void xViewChanged(double x0, double x1);
    /// Human-readable marker / cursor state for the status bar.
    void readoutChanged(const QString& text);

protected:
    // ---- subclass contract -------------------------------------------------

    virtual void drawContent(QPainter& p, const QRect& r) = 0;

    /// Full data extent in data units. The view is a sub-range of this.
    virtual double dataXMin() const = 0;
    virtual double dataXMax() const = 0;
    virtual double dataYMin() const = 0;
    virtual double dataYMax() const = 0;

    virtual QString xAxisLabel() const { return {}; }
    virtual QString yAxisLabel() const { return {}; }
    virtual QString formatX(double v) const;
    virtual QString formatY(double v) const;

    /// Value of the primary trace at a data-space X, for marker readouts.
    virtual bool traceValueAt(double x, double& yOut) const { Q_UNUSED(x); Q_UNUSED(yOut); return false; }

    /// Plots that must preserve an aspect ratio narrow the drawing rectangle.
    virtual QRect contentRect(const QRect& r) const { return r; }

    virtual bool markersSupported() const { return true; }
    virtual bool yZoomSupported()    const { return true; }
    /// True when dataYMin should sit at the *top* of the plot. The waterfall
    /// needs this: "0 frames ago" is the newest row and belongs at the top.
    virtual bool yInverted()         const { return false; }
    /// Unit shown in the manual axis-range prompt.
    virtual QString xUnitLabel()     const { return QStringLiteral("x"); }

    /// Extra entries appended to the right-click menu.
    virtual void extendContextMenu(QMenu* menu) { Q_UNUSED(menu); }

    /// Small badge drawn at the top-right of the plot area, e.g. channel name.
    virtual QString cornerBadge() const { return {}; }

    // ---- coordinate mapping ------------------------------------------------

    double xToPixel(double v, const QRect& r) const;
    double yToPixel(double v, const QRect& r) const;
    double pixelToX(double px, const QRect& r) const;
    double pixelToY(double py, const QRect& r) const;

    /// Re-sync the view to the data extent for any axis the user has not
    /// taken manual control of. Call from setFrame() once the extent is known.
    void syncView();

    // ---- painting helpers --------------------------------------------------

    QRect plotRect() const;
    void  drawGrid(QPainter& p, const QRect& r) const;
    void  drawAxes(QPainter& p, const QRect& r) const;
    void  drawInfoBox(QPainter& p, const QRect& r, const QStringList& lines,
                      const QColor& borderColor,
                      Qt::Alignment corner = Qt::AlignRight | Qt::AlignTop) const;
    void  drawLegend(QPainter& p, const QRect& r,
                     const QList<QPair<QColor, QString>>& entries) const;

    /// 1-2-5 "nice" tick positions covering [lo, hi].
    static QVector<double> niceTicks(double lo, double hi, int target);

    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

    QString m_title;
    int m_marginL = 58, m_marginR = 14, m_marginT = 24, m_marginB = 36;

    // View in data units
    double m_vx0 = 0.0, m_vx1 = 1.0, m_vy0 = 0.0, m_vy1 = 1.0;
    bool   m_userX = false, m_userY = false;

    QVector<double> m_markers;          ///< marker X positions, data units
    bool            m_crosshair = true;

private:
    void drawOverlay(QPainter& p, const QRect& r) const;
    void emitReadout();
    void zoomAxis(double& lo, double& hi, double anchor, double factor);

    QPointF m_mouse{-1, -1};
    bool    m_inside   = false;
    bool    m_panning  = false;
    bool    m_boxZoom  = false;
    QPointF m_dragFrom;
    QPointF m_dragTo;
    double  m_panX0 = 0, m_panX1 = 0, m_panY0 = 0, m_panY1 = 0;
};

} // namespace sdr::ui
