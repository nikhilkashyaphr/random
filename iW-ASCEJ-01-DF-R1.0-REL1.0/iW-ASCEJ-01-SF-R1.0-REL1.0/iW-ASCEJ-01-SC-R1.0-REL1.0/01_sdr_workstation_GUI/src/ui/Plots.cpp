#include "Plots.h"
#include "Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace sdr::ui {

namespace {

QString engFreq(double hz)
{
    const double a = std::abs(hz);
    if (a >= 1e9) return QString::number(hz / 1e9, 'f', 4) + QStringLiteral(" GHz");
    if (a >= 1e6) return QString::number(hz / 1e6, 'f', 3) + QStringLiteral(" MHz");
    if (a >= 1e3) return QString::number(hz / 1e3, 'f', 2) + QStringLiteral(" kHz");
    return QString::number(hz, 'f', 1) + QStringLiteral(" Hz");
}

} // namespace

// ========================================================= InstrumentPlot

QVector<int> InstrumentPlot::visibleChannels(std::size_t available) const
{
    QVector<int> out;
    const int n = static_cast<int>(available);
    if (n <= 0) return out;

    if (m_disp.channelView == ChannelView::Overlay) {
        for (int i = 0; i < n; ++i) out.append(i);
    } else {
        out.append(std::clamp(m_disp.activeChannel, 0, n - 1));
    }
    return out;
}

// ========================================================= TimeDomainPlot

TimeDomainPlot::TimeDomainPlot(QWidget* parent)
    : InstrumentPlot(QStringLiteral("Time domain — I/Q"), parent) {}

void TimeDomainPlot::clearTraces()
{
    m_traces.clear();
    m_yMax = 1.0;
    update();
}

void TimeDomainPlot::setFrame(const sdr::FrameResult& f)
{
    m_channelCount = static_cast<int>(f.channels.size());
    m_traces.clear();

    for (int c : visibleChannels(f.channels.size())) {
        const ChannelFrame* cf = f.channel(c);
        if (!cf) continue;
        Trace t;
        t.channel = c;
        t.iq      = cf->iq;
        m_traces.append(t);
    }

    if (f.blockSpanSec > 0.0) m_spanUs = f.blockSpanSec * 1e6;

    float peak = 0.1f;
    for (const Trace& t : m_traces)
        for (const cf32& s : t.iq)
            peak = std::max({peak, std::abs(s.real()), std::abs(s.imag())});

    // Slow AGC on the Y scale: an instantaneous rescale makes the trace jump
    // every frame and is unreadable.
    m_yMax = m_yMax * 0.85 + peak * 1.25 * 0.15;

    syncView();
    update();
}

QString TimeDomainPlot::cornerBadge() const
{
    if (m_traces.isEmpty()) return {};
    if (m_disp.channelView == ChannelView::Overlay)
        return QStringLiteral("ALL CH");
    return QStringLiteral("CH %1").arg(m_traces.first().channel);
}

bool TimeDomainPlot::traceValueAt(double x, double& yOut) const
{
    if (m_traces.isEmpty() || m_spanUs <= 0.0) return false;
    const IQBlock& iq = m_traces.first().iq;
    if (iq.empty()) return false;

    const double frac = std::clamp(x / m_spanUs, 0.0, 1.0);
    const auto idx = static_cast<std::size_t>(frac * (iq.size() - 1));
    yOut = iq[idx].real();
    return true;
}

void TimeDomainPlot::drawContent(QPainter& p, const QRect& r)
{
    if (m_traces.isEmpty()) return;

    QList<QPair<QColor, QString>> legend;
    const bool overlay = (m_disp.channelView == ChannelView::Overlay);

    for (const Trace& t : m_traces) {
        if (t.iq.size() < 2) continue;

        const double step = m_spanUs / static_cast<double>(t.iq.size() - 1);

        // Count the points inside the view to choose a rendering strategy.
        std::size_t first = 0, last = t.iq.size();
        if (m_vx0 > 0.0)
            first = static_cast<std::size_t>(std::max(0.0, std::floor((m_vx0 - step) / step)));
        if (m_vx1 < m_spanUs)
            last = std::min<std::size_t>(t.iq.size(),
                     static_cast<std::size_t>(std::ceil((m_vx1 + step) / step)) + 1);
        const std::size_t visible = (last > first) ? last - first : 0;

        // Dense traces (more than ~2 samples per pixel column) are drawn as a
        // per-column min/max envelope: one antialias-free vertical line per
        // column, exactly as an oscilloscope renders. The naive path — an
        // antialiased QPainterPath through every point — is catastrophically
        // slow for high-frequency content, because AA cost scales with the
        // area each segment covers and a fast waveform makes every segment
        // span nearly the full plot height. In the field that measured as
        // 0.6–2.4 s per paint on the GUI thread: the plot froze the window,
        // not the pipeline. The envelope is O(width) whatever the density,
        // and is also *more* faithful for dense data, since a decimated
        // polyline aliases while min/max preserves the true extremes.
        const bool envelope = visible > static_cast<std::size_t>(r.width()) * 2;

        QPainterPath pi, pq;
        if (envelope) {
            const int w = std::max(1, r.width());
            m_envelope.assign(static_cast<std::size_t>(w) * 4,
                              std::numeric_limits<float>::quiet_NaN());
            float* loI = m_envelope.data();
            float* hiI = loI + w;
            float* loQ = hiI + w;
            float* hiQ = loQ + w;
            const double invSpan = (m_vx1 - m_vx0) > 0 ? w / (m_vx1 - m_vx0) : 0.0;

            for (std::size_t n = first; n < last; ++n) {
                const double tx = static_cast<double>(n) * step;
                int col = static_cast<int>((tx - m_vx0) * invSpan);
                if (col < 0 || col >= w) continue;
                const float vi = t.iq[n].real();
                const float vq = t.iq[n].imag();
                if (std::isnan(loI[col]) || vi < loI[col]) loI[col] = vi;
                if (std::isnan(hiI[col]) || vi > hiI[col]) hiI[col] = vi;
                if (std::isnan(loQ[col]) || vq < loQ[col]) loQ[col] = vq;
                if (std::isnan(hiQ[col]) || vq > hiQ[col]) hiQ[col] = vq;
            }

            const bool aa = true;
            p.setRenderHint(QPainter::Antialiasing, false);
            auto drawEnv = [&](const float* lo, const float* hi, const QPen& pen) {
                p.setPen(pen);
                int prevTop = 0, prevBot = 0; bool has = false;
                for (int cx = 0; cx < w; ++cx) {
                    if (std::isnan(lo[cx])) { has = false; continue; }
                    int top = static_cast<int>(yToPixel(hi[cx], r));
                    int bot = static_cast<int>(yToPixel(lo[cx], r));
                    if (top > bot) std::swap(top, bot);
                    // Bridge to the previous column so the envelope stays
                    // connected even when adjacent columns do not overlap.
                    if (has) {
                        if (top > prevBot) top = prevBot;
                        if (bot < prevTop) bot = prevTop;
                    }
                    p.drawLine(r.left() + cx, top, r.left() + cx, bot);
                    prevTop = top; prevBot = bot; has = true;
                }
            };

            if (overlay) {
                const QColor c = theme::channelColor(t.channel);
                QColor dim = c; dim.setAlpha(110);
                drawEnv(loQ, hiQ, QPen(dim, 1));
                drawEnv(loI, hiI, QPen(c, 1));
                legend.append({c, QStringLiteral("CH %1").arg(t.channel)});
            } else {
                drawEnv(loQ, hiQ, QPen(theme::traceQ, 1));
                drawEnv(loI, hiI, QPen(theme::traceI, 1));
                legend.append({theme::traceI, QStringLiteral("I")});
                legend.append({theme::traceQ, QStringLiteral("Q")});
            }
            p.setRenderHint(QPainter::Antialiasing, aa);
            continue;
        }

        bool startedI = false, startedQ = false;
        for (std::size_t n = first; n < last; ++n) {
            const double tx = static_cast<double>(n) * step;
            if (tx < m_vx0 - step || tx > m_vx1 + step) continue;

            const double x  = xToPixel(tx, r);
            const double yi = yToPixel(t.iq[n].real(), r);
            const double yq = yToPixel(t.iq[n].imag(), r);

            if (!startedI) { pi.moveTo(x, yi); startedI = true; } else pi.lineTo(x, yi);
            if (!startedQ) { pq.moveTo(x, yq); startedQ = true; } else pq.lineTo(x, yq);
        }

        if (overlay) {
            // In overlay mode one colour per channel reads far better than
            // 2N interleaved I/Q colours.
            const QColor c = theme::channelColor(t.channel);
            p.setPen(QPen(c, 1.2));
            p.drawPath(pi);
            QColor dim = c;
            dim.setAlpha(110);
            p.setPen(QPen(dim, 1.0));
            p.drawPath(pq);
            legend.append({c, QStringLiteral("CH %1").arg(t.channel)});
        } else {
            p.setPen(QPen(theme::traceI, 1.2));
            p.drawPath(pi);
            p.setPen(QPen(theme::traceQ, 1.2));
            p.drawPath(pq);
            legend.append({theme::traceI, QStringLiteral("I")});
            legend.append({theme::traceQ, QStringLiteral("Q")});
        }
    }

    // Zero line
    const int y0 = static_cast<int>(yToPixel(0.0, r));
    if (y0 > r.top() && y0 < r.bottom()) {
        p.setPen(QPen(theme::gridMajor, 1));
        p.drawLine(r.left(), y0, r.right(), y0);
    }

    drawLegend(p, r, legend);
}

// =========================================================== SpectrumPlot

SpectrumPlot::SpectrumPlot(QWidget* parent)
    : InstrumentPlot(QStringLiteral("Spectrum — FFT"), parent) {}

void SpectrumPlot::setDisplayConfig(const sdr::DisplayConfig& cfg)
{
    InstrumentPlot::setDisplayConfig(cfg);
    syncView();
}

void SpectrumPlot::clearTraces()
{
    m_traces.clear();
    update();
}

void SpectrumPlot::setFrame(const sdr::FrameResult& f)
{
    m_channelCount = static_cast<int>(f.channels.size());
    m_centerHz = f.centerFreqHz;
    m_spanHz   = f.displayRateHz > 0 ? f.displayRateHz : 1.0;

    m_traces.clear();
    for (int c : visibleChannels(f.channels.size())) {
        const ChannelFrame* cf = f.channel(c);
        if (!cf) continue;
        Trace t;
        t.channel  = c;
        t.spectrum = cf->spectrum;
        t.metrics  = cf->metrics;
        m_traces.append(t);
    }

    syncView();
    update();
}

QString SpectrumPlot::formatX(double v) const
{
    const double spanMHz = std::abs(m_vx1 - m_vx0) / 1e6;
    const int dec = spanMHz >= 100 ? 1 : spanMHz >= 10 ? 2 : spanMHz >= 1 ? 3 : 4;
    return QString::number(v / 1e6, 'f', dec);
}

QString SpectrumPlot::cornerBadge() const
{
    if (m_traces.isEmpty()) return {};
    if (m_disp.channelView == ChannelView::Overlay)
        return QStringLiteral("ALL CH");
    return QStringLiteral("CH %1").arg(m_traces.first().channel);
}

bool SpectrumPlot::traceValueAt(double x, double& yOut) const
{
    if (m_traces.isEmpty()) return false;
    const std::vector<float>& s = m_traces.first().spectrum;
    if (s.size() < 2) return false;

    const double frac = (x - dataXMin()) / (dataXMax() - dataXMin());
    if (frac < 0.0 || frac > 1.0) return false;
    const auto idx = static_cast<std::size_t>(frac * (s.size() - 1));
    yOut = s[idx];
    return true;
}

void SpectrumPlot::drawContent(QPainter& p, const QRect& r)
{
    if (m_traces.isEmpty()) return;

    const bool overlay = (m_disp.channelView == ChannelView::Overlay);
    const double fMin = dataXMin(), fMax = dataXMax();
    QList<QPair<QColor, QString>> legend;

    for (const Trace& t : m_traces) {
        const std::vector<float>& s = t.spectrum;
        if (s.size() < 2) continue;

        const QColor col = overlay ? theme::channelColor(t.channel) : theme::spectrum;
        const double binHz = (fMax - fMin) / static_cast<double>(s.size() - 1);

        // Only walk the bins inside the current view; at 8192 bins zoomed to
        // 1 % of span, drawing all of them is wasted work every frame.
        auto binOf = [&](double hz) {
            return std::clamp<long long>(
                static_cast<long long>((hz - fMin) / binHz),
                0, static_cast<long long>(s.size()) - 1);
        };
        const long long i0 = binOf(m_vx0 - binHz);
        const long long i1 = binOf(m_vx1 + binHz);

        // One vertex per pixel column, taking the loudest bin that maps to
        // it — spectrum noise is dense the same way a fast waveform is, and
        // an antialiased vertex per *bin* pays the same pathological AA cost
        // the time plot did. Max-per-column is also what a swept analyser
        // shows, so peaks are preserved exactly; only the noise floor loses
        // sub-pixel texture nobody could see anyway.
        QPainterPath line;
        bool started = false;
        const long long span  = i1 - i0 + 1;
        const long long perPx = std::max<long long>(1, span / std::max(1, r.width()));
        for (long long i = i0; i <= i1; i += perPx) {
            const long long end = std::min(i1, i + perPx - 1);
            float v = s[static_cast<std::size_t>(i)];
            for (long long j = i + 1; j <= end; ++j)
                v = std::max(v, s[static_cast<std::size_t>(j)]);
            const double hz = fMin + static_cast<double>(i + end) * 0.5 * binHz;
            const double x  = xToPixel(hz, r);
            const double y  = yToPixel(v, r);
            if (!started) { line.moveTo(x, y); started = true; }
            else          line.lineTo(x, y);
        }
        if (!started) continue;

        if (!overlay) {
            QPainterPath fill = line;
            fill.lineTo(xToPixel(fMin + static_cast<double>(i1) * binHz, r), r.bottom() + 2);
            fill.lineTo(xToPixel(fMin + static_cast<double>(i0) * binHz, r), r.bottom() + 2);
            fill.closeSubpath();

            QLinearGradient g(0, r.top(), 0, r.bottom());
            g.setColorAt(0.0, QColor(col.red(), col.green(), col.blue(), 140));
            g.setColorAt(1.0, QColor(col.red(), col.green(), col.blue(), 18));
            // Antialiasing off for the area fill: at a full-width plot the AA
            // fill of a jagged 2 k-point region is the second-hottest paint in
            // the window, and the trace drawn on top hides its edge anyway.
            p.setRenderHint(QPainter::Antialiasing, false);
            p.fillPath(fill, g);
            p.setRenderHint(QPainter::Antialiasing, true);
        }

        p.setPen(QPen(col, 1.3));
        p.drawPath(line);
        legend.append({col, QStringLiteral("CH %1").arg(t.channel)});
    }

    // Detail overlays belong to the primary trace only; drawn for every
    // channel at once they turn the plot into noise.
    const Trace& primary = m_traces.first();
    const Metrics& m = primary.metrics;

    if (m.occupiedBwHz > 0.0) {
        const double lo = m_centerHz - m.occupiedBwHz / 2.0;
        const double hi = m_centerHz + m.occupiedBwHz / 2.0;
        const int xa = static_cast<int>(xToPixel(lo, r));
        const int xb = static_cast<int>(xToPixel(hi, r));
        const int y  = r.bottom() - r.height() / 4;

        p.fillRect(QRect(xa, r.top(), xb - xa, r.height()),
                   QColor(theme::accent.red(), theme::accent.green(),
                          theme::accent.blue(), 16));
        p.setPen(QPen(theme::textFaint, 1, Qt::DashLine));
        p.drawLine(xa, r.top(), xa, r.bottom());
        p.drawLine(xb, r.top(), xb, r.bottom());
        p.setPen(QPen(theme::textDim, 1));
        p.drawLine(xa, y, xb, y);
        p.setFont(theme::uiFont(9));
        p.drawText(QRect(xa, y + 3, xb - xa, 14), Qt::AlignHCenter | Qt::AlignTop,
                   QStringLiteral("OBW %1").arg(engFreq(m.occupiedBwHz)));
    }

    // Detected peaks
    p.setFont(theme::monoFont(9));
    for (int i = 0; i < static_cast<int>(m.peaks.size()); ++i) {
        const Peak& pk = m.peaks[static_cast<std::size_t>(i)];
        if (pk.freqHz < m_vx0 || pk.freqHz > m_vx1) continue;

        const QPointF pt(xToPixel(pk.freqHz, r), yToPixel(pk.levelDbfs, r));
        const QColor col = (i == 0) ? theme::marker : theme::textDim;

        p.setPen(QPen(col, 1.3));
        p.drawLine(pt + QPointF(-4, -4), pt + QPointF(4, 4));
        p.drawLine(pt + QPointF(-4,  4), pt + QPointF(4, -4));

        if (i == 0) {
            const QString l1 = QStringLiteral("%1").arg(engFreq(pk.freqHz));
            const QString l2 = QStringLiteral("%1 dBFS").arg(pk.levelDbfs, 0, 'f', 2);
            const QFontMetrics fm(p.font());
            const int w = std::max(fm.horizontalAdvance(l1), fm.horizontalAdvance(l2));
            int tx = static_cast<int>(pt.x()) + 8;
            if (tx + w > r.right() - 4) tx = static_cast<int>(pt.x()) - w - 8;
            p.setPen(theme::marker);
            p.drawText(tx, static_cast<int>(pt.y()) - 14, l1);
            p.drawText(tx, static_cast<int>(pt.y()) - 3,  l2);
        } else {
            p.setPen(theme::textDim);
            p.drawText(static_cast<int>(pt.x()) + 5, static_cast<int>(pt.y()) - 3,
                       QStringLiteral("P%1").arg(i + 1));
        }
    }

    // Noise floor reference
    const int nf = static_cast<int>(yToPixel(m.noiseFloorDbfs, r));
    if (nf > r.top() && nf < r.bottom()) {
        p.setPen(QPen(QColor(theme::danger.red(), theme::danger.green(),
                             theme::danger.blue(), 120), 1, Qt::DashLine));
        p.drawLine(r.left(), nf, r.right(), nf);
        p.setFont(theme::uiFont(8));
        p.setPen(QColor(theme::danger.red(), theme::danger.green(), theme::danger.blue(), 190));
        p.drawText(r.left() + 4, nf - 3, QStringLiteral("noise floor"));
    }

    if (m_traces.size() > 1) drawLegend(p, r, legend);

    drawInfoBox(p, r,
                {QStringLiteral("Peak   %1 dBFS").arg(m.peakDbfs, 7, 'f', 2),
                 QStringLiteral("Floor  %1 dBFS").arg(m.noiseFloorDbfs, 7, 'f', 2),
                 QStringLiteral("SNR    %1 dB").arg(m.snrDb, 7, 'f', 2),
                 QStringLiteral("OBW    %1").arg(engFreq(m.occupiedBwHz))},
                theme::spectrum,
                m_traces.size() > 1 ? (Qt::AlignLeft | Qt::AlignTop)
                                    : (Qt::AlignRight | Qt::AlignTop));
}

// ========================================================== WaterfallPlot

WaterfallPlot::WaterfallPlot(QWidget* parent)
    : InstrumentPlot(QStringLiteral("Waterfall — spectrogram"), parent)
{
    m_marginR = 52;   // room for the colour bar
}

void WaterfallPlot::setDisplayConfig(const sdr::DisplayConfig& cfg)
{
    const bool resize = (cfg.waterfallRows != m_rows) || (cfg.colorMap != m_disp.colorMap);
    InstrumentPlot::setDisplayConfig(cfg);

    m_map.setPreset(cfg.colorMap);
    // The waterfall reads best over a narrower window than the spectrum axis:
    // mapping the full 140 dB range leaves everything a uniform dark blue.
    m_floorDb = cfg.floorDb + 20.0;
    m_ceilDb  = cfg.ceilDb  - 20.0;

    if (resize) {
        m_rows = std::max(32, cfg.waterfallRows);
        clearTraces();
    }
    syncView();
}

void WaterfallPlot::clearTraces()
{
    if (!m_image.isNull()) m_image.fill(theme::plotBg);
    m_filled = false;
    m_shown  = 0;
    m_head   = 0;
    update();
}

void WaterfallPlot::drawRing(QPainter& p, const QRectF& dst,
                             double sx, double sw) const
{
    const int top = m_rows - m_head;          // rows above the wrap point
    const double h = dst.height();
    if (top <= 0 || top >= m_rows) {
        p.drawImage(dst, m_image, QRectF(sx, 0, sw, m_image.height()));
        return;
    }
    const double topH = h * top / m_rows;
    p.drawImage(QRectF(dst.x(), dst.y(), dst.width(), topH),
                m_image, QRectF(sx, m_head, sw, top));
    p.drawImage(QRectF(dst.x(), dst.y() + topH, dst.width(), h - topH),
                m_image, QRectF(sx, 0, sw, m_head));
}

void WaterfallPlot::setFrame(const sdr::FrameResult& f)
{
    m_channelCount = static_cast<int>(f.channels.size());

    // The waterfall shows one channel; overlaying spectrograms would just
    // hide one under the other.
    const int ch = std::clamp(m_disp.activeChannel, 0,
                              std::max(0, static_cast<int>(f.channels.size()) - 1));
    const ChannelFrame* cf = f.channel(ch);
    if (!cf || cf->spectrum.size() < 2) return;

    m_centerHz = f.centerFreqHz;
    m_spanHz   = f.displayRateHz > 0 ? f.displayRateHz : 1.0;

    const int cols = std::min<int>(static_cast<int>(cf->spectrum.size()), 2048);
    if (m_image.isNull() || m_image.width() != cols || m_image.height() != m_rows) {
        m_image = QImage(cols, m_rows, QImage::Format_RGB32);
        // Background, not black: rows not yet written should read as empty
        // plot area rather than as a measured value of zero.
        m_image.fill(theme::plotBg);
        m_shown = 0;
        m_head  = 0;
    }

    // Advance the ring cursor rather than scrolling pixels. The newest row is
    // written in place and paintEvent stitches the ring back into visual order
    // with two blits, so cost per frame is one row instead of the whole image.
    m_head = (m_head + m_rows - 1) % m_rows;

    QRgb* row = reinterpret_cast<QRgb*>(m_image.scanLine(m_head));
    const double binsPerCol = static_cast<double>(cf->spectrum.size()) / cols;
    const double range = (m_ceilDb - m_floorDb) > 0 ? (m_ceilDb - m_floorDb) : 1.0;

    for (int c = 0; c < cols; ++c) {
        const auto b0 = static_cast<std::size_t>(c * binsPerCol);
        const auto b1 = std::min(cf->spectrum.size(),
                                 static_cast<std::size_t>((c + 1) * binsPerCol) + 1);
        float peak = -200.0f;
        for (std::size_t b = b0; b < b1; ++b) peak = std::max(peak, cf->spectrum[b]);
        row[c] = m_map.rgb(static_cast<float>((peak - m_floorDb) / range));
    }

    m_filled = true;
    m_shown  = std::min(m_rows, m_shown + 1);
    syncView();
    update();
}

QString WaterfallPlot::formatX(double v) const
{
    const double spanMHz = std::abs(m_vx1 - m_vx0) / 1e6;
    const int dec = spanMHz >= 100 ? 1 : spanMHz >= 10 ? 2 : spanMHz >= 1 ? 3 : 4;
    return QString::number(v / 1e6, 'f', dec);
}

QString WaterfallPlot::cornerBadge() const
{
    return QStringLiteral("CH %1").arg(std::max(0, m_disp.activeChannel));
}

void WaterfallPlot::drawContent(QPainter& p, const QRect& r)
{
    if (m_filled && !m_image.isNull()) {
        // Rows [m_head, m_rows) are the newer part and belong at the top;
        // rows [0, m_head) wrap round underneath them.
        // Map only the visible frequency window out of the history image, so
        // zooming the spectrum zooms the waterfall with it.
        const double fMin = dataXMin(), fMax = dataXMax();
        const double span = fMax - fMin;
        if (span > 0.0) {
            const double u0 = std::clamp((m_vx0 - fMin) / span, 0.0, 1.0);
            const double u1 = std::clamp((m_vx1 - fMin) / span, 0.0, 1.0);
            const double sx0 = u0 * m_image.width();
            const double sx1 = std::max(sx0 + 1.0, u1 * m_image.width());
            drawRing(p, QRectF(r), sx0, sx1 - sx0);
        } else {
            drawRing(p, QRectF(r), 0.0, m_image.width());
        }
    }
    drawColorBar(p, r);
}

void WaterfallPlot::drawColorBar(QPainter& p, const QRect& r) const
{
    const QRect bar(r.right() + 10, r.top(), 13, r.height());
    QLinearGradient g(0, bar.bottom(), 0, bar.top());
    for (int i = 0; i <= 16; ++i)
        g.setColorAt(i / 16.0, m_map.color(i / 16.0f));

    p.save();
    p.setClipping(false);
    p.fillRect(bar, g);
    p.setPen(QPen(theme::borderLite, 1));
    p.drawRect(bar);

    p.setFont(theme::monoFont(8));
    p.setPen(theme::textDim);
    for (int i = 0; i <= 4; ++i) {
        const int y = bar.bottom() - bar.height() * i / 4;
        const double v = m_floorDb + (m_ceilDb - m_floorDb) * i / 4.0;
        p.drawText(QRect(bar.right() + 3, y - 7, 34, 14),
                   Qt::AlignLeft | Qt::AlignVCenter, QString::number(v, 'f', 0));
    }
    p.restore();
}

// ====================================================== ConstellationPlot

ConstellationPlot::ConstellationPlot(QWidget* parent)
    : InstrumentPlot(QStringLiteral("Constellation — I/Q"), parent)
{
    m_image = QImage(kGrid, kGrid, QImage::Format_RGB32);
    m_image.fill(Qt::black);
}

void ConstellationPlot::setDisplayConfig(const sdr::DisplayConfig& cfg)
{
    const bool mapChanged = (cfg.colorMap != m_disp.colorMap);
    InstrumentPlot::setDisplayConfig(cfg);
    if (mapChanged) m_map.setPreset(cfg.colorMap);
    syncView();
}

void ConstellationPlot::clearTraces()
{
    std::fill(m_density.begin(), m_density.end(), 0.0f);
    m_image.fill(Qt::black);
    update();
}

void ConstellationPlot::setFrame(const sdr::FrameResult& f)
{
    m_channelCount = static_cast<int>(f.channels.size());

    const int ch = std::clamp(m_disp.activeChannel, 0,
                              std::max(0, static_cast<int>(f.channels.size()) - 1));
    if (ch != m_channel) {
        m_channel = ch;
        clearTraces();
    }

    const ChannelFrame* cf = f.channel(ch);
    if (!cf) return;
    m_metrics = cf->metrics;

    const float decay = m_disp.persistence
        ? static_cast<float>(std::clamp(m_disp.persistDecay, 0.0, 0.995)) : 0.0f;
    for (float& v : m_density) v *= decay;

    const float half = kGrid / 2.0f;
    for (const cf32& s : cf->symbols) {
        const int gx = static_cast<int>(half + s.real() / m_axisMax * half);
        const int gy = static_cast<int>(half - s.imag() / m_axisMax * half);
        if (gx < 1 || gy < 1 || gx >= kGrid - 1 || gy >= kGrid - 1) continue;
        // Splat with a 3x3 kernel so sparse frames still read as a density.
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                m_density[static_cast<std::size_t>((gy + dy) * kGrid + (gx + dx))] +=
                    (dx == 0 && dy == 0) ? 1.0f : 0.35f;
    }

    const float peak = std::max(1.0f, *std::max_element(m_density.begin(), m_density.end()));
    for (int y = 0; y < kGrid; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(m_image.scanLine(y));
        for (int x = 0; x < kGrid; ++x) {
            const float t = std::sqrt(m_density[static_cast<std::size_t>(y * kGrid + x)] / peak);
            row[x] = (t < 0.02f) ? qRgb(0, 0, 0) : m_map.rgb(t);
        }
    }

    syncView();
    update();
}

QRect ConstellationPlot::contentRect(const QRect& r) const
{
    // The I/Q plane must stay square, otherwise the constellation shears.
    const int side = std::min(r.width(), r.height());
    return QRect(r.center().x() - side / 2, r.center().y() - side / 2, side, side);
}

QString ConstellationPlot::cornerBadge() const
{
    return QStringLiteral("CH %1").arg(m_channel);
}

void ConstellationPlot::drawContent(QPainter& p, const QRect& r)
{
    // Draw the sub-region of the density image the current zoom selects.
    const double span = 2.0 * m_axisMax;
    const double u0 = std::clamp((m_vx0 + m_axisMax) / span, 0.0, 1.0);
    const double u1 = std::clamp((m_vx1 + m_axisMax) / span, 0.0, 1.0);
    const double v0 = std::clamp((m_axisMax - m_vy1) / span, 0.0, 1.0);
    const double v1 = std::clamp((m_axisMax - m_vy0) / span, 0.0, 1.0);

    const QRectF src(u0 * kGrid, v0 * kGrid,
                     std::max(1.0, (u1 - u0) * kGrid),
                     std::max(1.0, (v1 - v0) * kGrid));
    p.drawImage(QRectF(r), m_image, src);

    // Axes through the origin
    const int cx = static_cast<int>(xToPixel(0.0, r));
    const int cy = static_cast<int>(yToPixel(0.0, r));
    p.setPen(QPen(QColor(0x4a, 0x54, 0x5e), 1));
    if (cx > r.left() && cx < r.right()) p.drawLine(cx, r.top(), cx, r.bottom());
    if (cy > r.top()  && cy < r.bottom()) p.drawLine(r.left(), cy, r.right(), cy);

    // Unit circle: a quick visual reference for amplitude normalisation.
    p.setPen(QPen(QColor(0x3a, 0x44, 0x4e), 1, Qt::DotLine));
    const double rx = xToPixel(1.0, r) - xToPixel(0.0, r);
    const double ry = yToPixel(0.0, r) - yToPixel(1.0, r);
    p.drawEllipse(QPointF(cx, cy), rx, ry);

    drawInfoBox(p, r,
                {QStringLiteral("EVM    %1 %").arg(m_metrics.evm * 100.0, 6, 'f', 2),
                 QStringLiteral("RMS    %1").arg(m_metrics.rms, 6, 'f', 3),
                 QStringLiteral("Phase  %1°").arg(m_metrics.phaseErrDeg, 6, 'f', 2),
                 QStringLiteral("DC     %1").arg(m_metrics.dcOffset, 6, 'f', 4)},
                theme::marker);
}

} // namespace sdr::ui
