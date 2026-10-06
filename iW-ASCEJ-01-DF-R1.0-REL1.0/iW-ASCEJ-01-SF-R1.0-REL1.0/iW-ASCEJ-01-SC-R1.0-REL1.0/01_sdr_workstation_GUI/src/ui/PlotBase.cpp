#include "PlotBase.h"
#include "Theme.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <QInputDialog>
#include <QLineEdit>

namespace sdr::ui {

namespace {

/// Qt 6 renamed the local-position accessors. One helper keeps the rest of the
/// file free of version checks.
inline QPointF localPos(const QMouseEvent* e)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return e->position();
#else
    return e->localPos();
#endif
}

inline QPointF localPos(const QWheelEvent* e)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    return e->position();
#else
    return QPointF(e->pos());
#endif
}

constexpr double kMinSpan = 1e-12;

} // namespace

// ================================================================ lifecycle

PlotBase::PlotBase(const QString& title, QWidget* parent)
    : QWidget(parent), m_title(title)
{
    setAutoFillBackground(false);
    setMinimumSize(220, 150);
    setMouseTracking(true);
    // The plot paints its whole rect every frame, so Qt need not clear it
    // first; skipping the erase removes a full-size fill per repaint.
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setFocusPolicy(Qt::StrongFocus);
}

// ==================================================================== view

void PlotBase::syncView()
{
    if (!m_userX) { m_vx0 = dataXMin(); m_vx1 = dataXMax(); }
    if (!m_userY) { m_vy0 = dataYMin(); m_vy1 = dataYMax(); }
    if (m_vx1 - m_vx0 < kMinSpan) m_vx1 = m_vx0 + 1.0;
    if (m_vy1 - m_vy0 < kMinSpan) m_vy1 = m_vy0 + 1.0;
}

void PlotBase::setViewX(double x0, double x1, bool user)
{
    if (x1 - x0 < kMinSpan) return;
    m_vx0 = x0;
    m_vx1 = x1;
    m_userX = user;
    update();
}

void PlotBase::zoomAtCentre(double factor)
{
    const QRect r = contentRect(plotRect());
    zoomAxis(m_vx0, m_vx1, (m_vx0 + m_vx1) * 0.5, factor);
    m_userX = true;
    if (yZoomSupported()) {
        zoomAxis(m_vy0, m_vy1, (m_vy0 + m_vy1) * 0.5, factor);
        m_userY = true;
    }
    Q_UNUSED(r);
    emit xViewChanged(m_vx0, m_vx1);
    update();
}

void PlotBase::promptAxisRange()
{
    // Manual axis entry: engineering work often needs an exact span
    // ("show me 2.4000-2.4005 GHz"), which no amount of scrolling hits
    // reliably.
    bool ok = false;
    const QString cur = QStringLiteral("%1, %2")
                            .arg(m_vx0, 0, 'g', 10).arg(m_vx1, 0, 'g', 10);
    const QString txt = QInputDialog::getText(
        this, QStringLiteral("Set X axis range"),
        QStringLiteral("Enter min, max (%1):").arg(xUnitLabel()),
        QLineEdit::Normal, cur, &ok);
    if (!ok) return;

    const QStringList parts = txt.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (parts.size() != 2) return;
    bool okA = false, okB = false;
    const double a = parts[0].trimmed().toDouble(&okA);
    const double b = parts[1].trimmed().toDouble(&okB);
    if (!okA || !okB || !(b > a)) return;

    m_vx0 = a; m_vx1 = b;
    m_userX = true;
    emit xViewChanged(m_vx0, m_vx1);
    update();
}

void PlotBase::resetView()
{
    m_userX = m_userY = false;
    syncView();
    emit xViewChanged(m_vx0, m_vx1);
    emit readoutChanged(QString());
    update();
}

void PlotBase::clearMarkers()
{
    m_markers.clear();
    emit readoutChanged(QString());
    update();
}

// ================================================================= mapping

double PlotBase::xToPixel(double v, const QRect& r) const
{
    const double span = m_vx1 - m_vx0;
    return r.left() + (span > kMinSpan ? (v - m_vx0) / span : 0.0) * r.width();
}

double PlotBase::yToPixel(double v, const QRect& r) const
{
    const double span = m_vy1 - m_vy0;
    const double f = (span > kMinSpan) ? (v - m_vy0) / span : 0.0;
    return yInverted() ? r.top() + f * r.height()
                       : r.bottom() - f * r.height();
}

double PlotBase::pixelToX(double px, const QRect& r) const
{
    if (r.width() <= 0) return m_vx0;
    return m_vx0 + (px - r.left()) / r.width() * (m_vx1 - m_vx0);
}

double PlotBase::pixelToY(double py, const QRect& r) const
{
    if (r.height() <= 0) return m_vy0;
    const double f = yInverted() ? (py - r.top()) / r.height()
                                 : (r.bottom() - py) / r.height();
    return m_vy0 + f * (m_vy1 - m_vy0);
}

// =================================================================== ticks

QVector<double> PlotBase::niceTicks(double lo, double hi, int target)
{
    QVector<double> out;
    if (!(hi > lo) || target < 2) return out;

    const double raw  = (hi - lo) / target;
    const double mag  = std::pow(10.0, std::floor(std::log10(raw)));
    const double norm = raw / mag;

    double step;
    if      (norm < 1.5) step = 1.0  * mag;
    else if (norm < 3.0) step = 2.0  * mag;
    else if (norm < 7.0) step = 5.0  * mag;
    else                 step = 10.0 * mag;

    const double first = std::ceil(lo / step) * step;
    for (double v = first; v <= hi + step * 1e-6; v += step) {
        // Snap values that land a hair off zero, so an axis reads "0" and not
        // "-2.8e-17" at the origin.
        if (std::abs(v) < step * 1e-9) v = 0.0;
        out.append(v);
        if (out.size() > 64) break;
    }
    return out;
}

QString PlotBase::formatX(double v) const
{
    const double span = std::abs(m_vx1 - m_vx0);
    const int dec = span >= 100 ? 0 : span >= 10 ? 1 : span >= 1 ? 2 : 3;
    return QString::number(v, 'f', dec);
}

QString PlotBase::formatY(double v) const
{
    const double span = std::abs(m_vy1 - m_vy0);
    const int dec = span >= 100 ? 0 : span >= 10 ? 1 : span >= 1 ? 2 : 3;
    return QString::number(v, 'f', dec);
}

// ================================================================ painting

QRect PlotBase::plotRect() const
{
    return QRect(m_marginL, m_marginT,
                 std::max(10, width()  - m_marginL - m_marginR),
                 std::max(10, height() - m_marginT - m_marginB));
}

void PlotBase::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    p.fillRect(rect(), theme::panel);

    // Title strip
    const QRect titleRect(0, 0, width(), m_marginT - 4);
    p.fillRect(titleRect, theme::titleBar);
    p.setFont(theme::uiFont(10, true));
    p.setPen(theme::text);
    p.drawText(titleRect.adjusted(9, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft,
               m_title.toUpper());

    if (isZoomed()) {
        p.setFont(theme::uiFont(9));
        p.setPen(theme::warn);
        p.drawText(titleRect.adjusted(0, 0, -9, 0), Qt::AlignVCenter | Qt::AlignRight,
                   QStringLiteral("ZOOM  ·  double-click to reset"));
    }

    p.setPen(QPen(theme::border, 1));
    p.drawLine(0, m_marginT - 4, width(), m_marginT - 4);

    const QRect r = contentRect(plotRect());
    p.fillRect(r, theme::plotBg);

    drawGrid(p, r);

    p.save();
    p.setClipRect(r);
    drawContent(p, r);
    p.restore();

    drawAxes(p, r);

    p.save();
    p.setClipRect(r);
    drawOverlay(p, r);
    p.restore();

    const QString badge = cornerBadge();
    if (!badge.isEmpty()) {
        p.setFont(theme::uiFont(9, true));
        const QFontMetrics fm(p.font());
        const int w = fm.horizontalAdvance(badge) + 12;
        const QRect b(r.right() - w - 4, r.top() + 4, w, fm.height() + 5);
        p.fillRect(b, QColor(0x0a, 0x0d, 0x12, 215));
        p.setPen(QPen(theme::borderLite, 1));
        p.drawRect(b);
        p.setPen(theme::textDim);
        p.drawText(b, Qt::AlignCenter, badge);
    }

    p.setPen(QPen(theme::borderLite, 1));
    p.drawRect(r);

    p.setPen(QPen(theme::border, 1));
    p.drawRect(rect().adjusted(0, 0, -1, -1));
}

void PlotBase::drawGrid(QPainter& p, const QRect& r) const
{
    p.setPen(QPen(theme::grid, 1, Qt::SolidLine));
    for (double t : niceTicks(m_vx0, m_vx1, 8)) {
        const int x = static_cast<int>(xToPixel(t, r));
        if (x > r.left() && x < r.right()) p.drawLine(x, r.top(), x, r.bottom());
    }
    for (double t : niceTicks(m_vy0, m_vy1, 6)) {
        const int y = static_cast<int>(yToPixel(t, r));
        if (y > r.top() && y < r.bottom()) p.drawLine(r.left(), y, r.right(), y);
    }
}

void PlotBase::drawAxes(QPainter& p, const QRect& r) const
{
    p.setFont(theme::monoFont(9));
    p.setPen(theme::textDim);

    for (double t : niceTicks(m_vx0, m_vx1, 8)) {
        const int x = static_cast<int>(xToPixel(t, r));
        if (x < r.left() - 1 || x > r.right() + 1) continue;
        p.drawLine(x, r.bottom(), x, r.bottom() + 3);
        p.drawText(QRect(x - 44, r.bottom() + 5, 88, 12),
                   Qt::AlignHCenter | Qt::AlignTop, formatX(t));
    }
    for (double t : niceTicks(m_vy0, m_vy1, 6)) {
        const int y = static_cast<int>(yToPixel(t, r));
        if (y < r.top() - 1 || y > r.bottom() + 1) continue;
        p.drawLine(r.left() - 3, y, r.left(), y);
        p.drawText(QRect(2, y - 7, m_marginL - 8, 14),
                   Qt::AlignRight | Qt::AlignVCenter, formatY(t));
    }

    p.setFont(theme::uiFont(9));
    p.setPen(theme::text);
    if (!xAxisLabel().isEmpty())
        p.drawText(QRect(r.left(), height() - 16, r.width(), 14),
                   Qt::AlignHCenter | Qt::AlignVCenter, xAxisLabel());
    if (!yAxisLabel().isEmpty()) {
        p.save();
        p.translate(12, r.center().y());
        p.rotate(-90);
        p.drawText(QRect(-70, -7, 140, 14), Qt::AlignCenter, yAxisLabel());
        p.restore();
    }
}

void PlotBase::drawInfoBox(QPainter& p, const QRect& r, const QStringList& lines,
                           const QColor& borderColor, Qt::Alignment corner) const
{
    if (lines.isEmpty()) return;
    p.setFont(theme::monoFont(9));
    const QFontMetrics fm = p.fontMetrics();

    int w = 0;
    for (const QString& l : lines) w = std::max(w, fm.horizontalAdvance(l));
    const int h = lines.size() * (fm.height() + 1) + 9;
    w += 14;

    const int x = (corner & Qt::AlignRight)  ? r.right()  - w - 6 : r.left() + 6;
    const int y = (corner & Qt::AlignBottom) ? r.bottom() - h - 6 : r.top()  + 6;

    const QRect box(x, y, w, h);
    p.fillRect(box, QColor(0x0a, 0x0d, 0x12, 225));
    p.setPen(QPen(borderColor, 1));
    p.drawRect(box);

    p.setPen(theme::text);
    int ty = y + 4;
    for (const QString& l : lines) {
        p.drawText(QRect(x + 7, ty, w - 14, fm.height()),
                   Qt::AlignLeft | Qt::AlignVCenter, l);
        ty += fm.height() + 1;
    }
}

void PlotBase::drawLegend(QPainter& p, const QRect& r,
                          const QList<QPair<QColor, QString>>& entries) const
{
    if (entries.isEmpty()) return;
    p.setFont(theme::uiFont(9));
    const QFontMetrics fm = p.fontMetrics();

    int w = 0;
    for (const auto& e : entries) w = std::max(w, fm.horizontalAdvance(e.second));
    w += 32;
    const int rowH = fm.height() + 3;
    const int h = entries.size() * rowH + 7;

    const QRect box(r.right() - w - 6, r.top() + 6, w, h);
    p.fillRect(box, QColor(0x0a, 0x0d, 0x12, 215));
    p.setPen(QPen(theme::border, 1));
    p.drawRect(box);

    int y = box.top() + 4;
    for (const auto& e : entries) {
        p.setPen(QPen(e.first, 2));
        p.drawLine(box.left() + 6, y + rowH / 2, box.left() + 20, y + rowH / 2);
        p.setPen(theme::text);
        p.drawText(QRect(box.left() + 25, y, w - 30, rowH),
                   Qt::AlignLeft | Qt::AlignVCenter, e.second);
        y += rowH;
    }
}

void PlotBase::drawOverlay(QPainter& p, const QRect& r) const
{
    // Markers
    for (int i = 0; i < m_markers.size(); ++i) {
        const double mx = m_markers[i];
        if (mx < m_vx0 || mx > m_vx1) continue;
        const int x = static_cast<int>(xToPixel(mx, r));

        p.setPen(QPen(theme::marker, 1, Qt::DashLine));
        p.drawLine(x, r.top(), x, r.bottom());

        double yv = 0.0;
        const bool have = traceValueAt(mx, yv);
        if (have) {
            const int y = static_cast<int>(yToPixel(yv, r));
            p.setPen(QPen(theme::marker, 1.5));
            p.drawLine(x - 5, y, x + 5, y);
            p.drawLine(x, y - 5, x, y + 5);
        }

        p.setFont(theme::monoFont(9, true));
        p.setPen(theme::marker);
        const QString tag = QStringLiteral("M%1").arg(i + 1);
        p.drawText(QRect(x + 3, r.top() + 2, 40, 12), Qt::AlignLeft | Qt::AlignTop, tag);
    }

    // Box-zoom rubber band
    if (m_boxZoom) {
        const QRectF band = QRectF(m_dragFrom, m_dragTo).normalized();
        p.fillRect(band, QColor(theme::accent.red(), theme::accent.green(),
                                theme::accent.blue(), 40));
        p.setPen(QPen(theme::accent, 1, Qt::DashLine));
        p.drawRect(band);
        return;
    }

    // Crosshair
    if (!m_crosshair || !m_inside || m_panning) return;
    if (!r.contains(m_mouse.toPoint())) return;

    p.setPen(QPen(QColor(theme::cursor.red(), theme::cursor.green(),
                         theme::cursor.blue(), 130), 1, Qt::DotLine));
    p.drawLine(static_cast<int>(m_mouse.x()), r.top(),
               static_cast<int>(m_mouse.x()), r.bottom());
    p.drawLine(r.left(),  static_cast<int>(m_mouse.y()),
               r.right(), static_cast<int>(m_mouse.y()));

    const double dx = pixelToX(m_mouse.x(), r);
    const double dy = pixelToY(m_mouse.y(), r);
    const QString label = QStringLiteral("%1, %2").arg(formatX(dx), formatY(dy));

    p.setFont(theme::monoFont(9));
    const QFontMetrics fm = p.fontMetrics();
    const int w = fm.horizontalAdvance(label) + 10;
    int bx = static_cast<int>(m_mouse.x()) + 8;
    int by = static_cast<int>(m_mouse.y()) - fm.height() - 8;
    if (bx + w > r.right()) bx = static_cast<int>(m_mouse.x()) - w - 8;
    if (by < r.top())       by = static_cast<int>(m_mouse.y()) + 8;

    const QRect box(bx, by, w, fm.height() + 4);
    p.fillRect(box, QColor(0x0a, 0x0d, 0x12, 230));
    p.setPen(QPen(theme::cursor, 1));
    p.drawRect(box);
    p.setPen(theme::text);
    p.drawText(box, Qt::AlignCenter, label);
}

// ============================================================ interaction

void PlotBase::zoomAxis(double& lo, double& hi, double anchor, double factor)
{
    const double cur = hi - lo;
    if (!std::isfinite(cur) || cur <= 0.0) return;

    double span = cur * factor;

    // Unlimited zoom in both directions, bounded only by what double precision
    // can still represent meaningfully. Previously this snapped the view back
    // to the data extent whenever the user zoomed out past it, and the
    // override flag was then inferred by comparing the view *to* that extent —
    // so the plot silently re-armed auto-fit and every subsequent frame yanked
    // the user back to "fit to screen". That made sustained analysis at an
    // arbitrary span impossible, which is what an engineering tool is for.
    constexpr double kMinUsableSpan = 1e-9;
    constexpr double kMaxUsableSpan = 1e15;
    span = std::clamp(span, kMinUsableSpan, kMaxUsableSpan);

    // Keep the value under the pointer pinned while the span changes.
    const double frac = (anchor - lo) / cur;
    lo = anchor - frac * span;
    hi = lo + span;
}

void PlotBase::wheelEvent(QWheelEvent* e)
{
    const QRect r = contentRect(plotRect());
    const QPointF pos = localPos(e);
    if (!r.contains(pos.toPoint())) { e->ignore(); return; }

    const int steps = e->angleDelta().y();
    if (steps == 0) { e->ignore(); return; }

    const double factor = steps > 0 ? 0.80 : 1.25;
    const bool ctrl  = e->modifiers().testFlag(Qt::ControlModifier);
    const bool shift = e->modifiers().testFlag(Qt::ShiftModifier);

    const bool doY = (ctrl || shift) && yZoomSupported();
    const bool doX = !ctrl || shift;

    // The override is set explicitly by the act of zooming — never inferred
    // from where the view happens to sit. Inferring it meant a view that
    // coincidentally matched the data extent was treated as "not zoomed" and
    // was recaptured by auto-fit on the next frame.
    if (doX) {
        zoomAxis(m_vx0, m_vx1, pixelToX(pos.x(), r), factor);
        m_userX = true;
        emit xViewChanged(m_vx0, m_vx1);
    }
    if (doY) {
        zoomAxis(m_vy0, m_vy1, pixelToY(pos.y(), r), factor);
        m_userY = true;
    }

    update();
    e->accept();
}

void PlotBase::mousePressEvent(QMouseEvent* e)
{
    const QRect r = contentRect(plotRect());
    const QPointF pos = localPos(e);

    if (e->button() != Qt::LeftButton && e->button() != Qt::MiddleButton) {
        QWidget::mousePressEvent(e);
        return;
    }

    if (e->modifiers().testFlag(Qt::ControlModifier) && markersSupported()) {
        if (m_markers.size() >= 4) m_markers.removeFirst();
        m_markers.append(pixelToX(pos.x(), r));
        emitReadout();
        update();
        return;
    }

    if (e->modifiers().testFlag(Qt::ShiftModifier) || e->button() == Qt::MiddleButton) {
        m_boxZoom  = true;
        m_dragFrom = m_dragTo = pos;
        return;
    }

    m_panning = true;
    m_dragFrom = pos;
    m_panX0 = m_vx0; m_panX1 = m_vx1;
    m_panY0 = m_vy0; m_panY1 = m_vy1;
    setCursor(Qt::ClosedHandCursor);
}

void PlotBase::mouseMoveEvent(QMouseEvent* e)
{
    const QRect r = contentRect(plotRect());
    m_mouse  = localPos(e);
    m_inside = true;

    if (m_boxZoom) {
        m_dragTo = m_mouse;
        update();
        return;
    }

    if (m_panning && r.width() > 0 && r.height() > 0) {
        const double dxPix = m_mouse.x() - m_dragFrom.x();
        const double dyPix = m_mouse.y() - m_dragFrom.y();
        const double dx = dxPix / r.width()  * (m_panX1 - m_panX0);
        const double dy = dyPix / r.height() * (m_panY1 - m_panY0);

        m_vx0 = m_panX0 - dx; m_vx1 = m_panX1 - dx;
        m_userX = true;
        if (yZoomSupported()) {
            m_vy0 = m_panY0 + dy; m_vy1 = m_panY1 + dy;
            m_userY = true;
        }
        emit xViewChanged(m_vx0, m_vx1);
        update();
        return;
    }

    emitReadout();
    update();
}

void PlotBase::mouseReleaseEvent(QMouseEvent* e)
{
    const QRect r = contentRect(plotRect());

    if (m_boxZoom) {
        m_boxZoom = false;
        const QRectF band = QRectF(m_dragFrom, m_dragTo).normalized();
        // Ignore an accidental click-sized band, otherwise a stray click
        // zooms to a single pixel and looks like the plot died.
        if (band.width() > 6 && band.height() > 6) {
            const double x0 = pixelToX(band.left(),  r);
            const double x1 = pixelToX(band.right(), r);
            m_vx0 = std::min(x0, x1); m_vx1 = std::max(x0, x1);
            m_userX = true;
            if (yZoomSupported()) {
                const double y0 = pixelToY(band.bottom(), r);
                const double y1 = pixelToY(band.top(),    r);
                m_vy0 = std::min(y0, y1); m_vy1 = std::max(y0, y1);
                m_userY = true;
            }
            emit xViewChanged(m_vx0, m_vx1);
        }
        update();
        return;
    }

    if (m_panning) {
        m_panning = false;
        setCursor(Qt::ArrowCursor);
    }
    QWidget::mouseReleaseEvent(e);
}

void PlotBase::mouseDoubleClickEvent(QMouseEvent*)
{
    resetView();
}

void PlotBase::leaveEvent(QEvent* e)
{
    m_inside = false;
    update();
    QWidget::leaveEvent(e);
}

void PlotBase::contextMenuEvent(QContextMenuEvent* e)
{
    QMenu menu(this);
    menu.addAction(QStringLiteral("Auto fit (follow data)"), this, &PlotBase::resetView);
    menu.addAction(QStringLiteral("Zoom in"),  this, [this] { zoomAtCentre(0.80); });
    menu.addAction(QStringLiteral("Zoom out"), this, [this] { zoomAtCentre(1.25); });
    menu.addAction(QStringLiteral("Set axis range…"), this, &PlotBase::promptAxisRange);
    menu.addSeparator();

    if (markersSupported()) {
        const QRect r = contentRect(plotRect());
        const double mx = pixelToX(e->pos().x(), r);
        menu.addAction(QStringLiteral("Add marker here"), this, [this, mx] {
            if (m_markers.size() >= 4) m_markers.removeFirst();
            m_markers.append(mx);
            emitReadout();
            update();
        });
        QAction* clear = menu.addAction(QStringLiteral("Clear markers"),
                                        this, &PlotBase::clearMarkers);
        clear->setEnabled(!m_markers.isEmpty());
    }

    QAction* cross = menu.addAction(QStringLiteral("Crosshair"));
    cross->setCheckable(true);
    cross->setChecked(m_crosshair);
    connect(cross, &QAction::toggled, this, &PlotBase::setCrosshairEnabled);

    extendContextMenu(&menu);
    menu.exec(e->globalPos());
}

void PlotBase::emitReadout()
{
    if (m_markers.isEmpty()) {
        emit readoutChanged(QString());
        return;
    }

    QStringList parts;
    for (int i = 0; i < m_markers.size(); ++i) {
        double y = 0.0;
        const bool have = traceValueAt(m_markers[i], y);
        parts << (have ? QStringLiteral("M%1 %2, %3").arg(i + 1)
                                 .arg(formatX(m_markers[i]), formatY(y))
                       : QStringLiteral("M%1 %2").arg(i + 1).arg(formatX(m_markers[i])));
    }

    if (m_markers.size() >= 2) {
        const double a = m_markers[m_markers.size() - 2];
        const double b = m_markers[m_markers.size() - 1];
        double ya = 0.0, yb = 0.0;
        const bool have = traceValueAt(a, ya) && traceValueAt(b, yb);
        parts << (have ? QStringLiteral("Δ %1, %2").arg(formatX(b - a), formatY(yb - ya))
                       : QStringLiteral("Δ %1").arg(formatX(b - a)));
    }

    emit readoutChanged(QStringLiteral("%1:  %2").arg(m_title, parts.join(QStringLiteral("   "))));
}

} // namespace sdr::ui
