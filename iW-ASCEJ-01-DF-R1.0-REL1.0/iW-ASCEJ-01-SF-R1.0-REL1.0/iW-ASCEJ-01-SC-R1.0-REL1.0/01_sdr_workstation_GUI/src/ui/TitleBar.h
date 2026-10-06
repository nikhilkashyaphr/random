#pragma once
// ---------------------------------------------------------------------------
// TitleBar — the application's own title bar.
//
// Why this exists: GNOME draws the window title bar itself and its modern
// header bars have no icon slot, so QWidget::setWindowIcon() puts nothing
// there. The only way to place the brand mark in that bar is for the
// application to own it, which means a frameless window plus this widget.
//
// It reproduces the behaviours the window manager was providing: drag to
// move, double-click to maximise/restore, and minimise / maximise / close
// buttons. Resizing is handled by the main window's edge hit-testing.
// ---------------------------------------------------------------------------

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QWidget>

namespace sdr::ui {

class TitleBar : public QWidget
{
    Q_OBJECT
public:
    explicit TitleBar(const QString& title, QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("appTitleBar"));
        setFixedHeight(40);
        setAttribute(Qt::WA_StyledBackground, true);

        auto* lay = new QHBoxLayout(this);
        lay->setContentsMargins(10, 0, 6, 0);
        lay->setSpacing(10);

        // Brand mark, left. The supplied logo is navy (#233666); this bar is
        // deliberately light so the mark sits on it at full contrast with no
        // colour change and no plate.
        m_logo = new QLabel;
        m_logo->setObjectName(QStringLiteral("titleLogo"));
        m_logo->setPixmap(QPixmap(QStringLiteral(":/icons/iwave_logo_bar.png")));
        m_logo->setToolTip(QStringLiteral("iWave Global"));
        lay->addWidget(m_logo, 0, Qt::AlignVCenter);

        m_title = new QLabel(title);
        m_title->setObjectName(QStringLiteral("titleText"));
        lay->addStretch(1);
        lay->addWidget(m_title, 0, Qt::AlignCenter);
        lay->addStretch(1);

        auto mkBtn = [this, lay](const QString& objName, const QString& glyph,
                                 const QString& tip) {
            auto* b = new QPushButton(glyph);
            b->setObjectName(objName);
            b->setToolTip(tip);
            b->setFocusPolicy(Qt::NoFocus);
            b->setFixedSize(38, 28);
            lay->addWidget(b, 0, Qt::AlignVCenter);
            return b;
        };
        m_min   = mkBtn(QStringLiteral("winBtn"),      QStringLiteral("—"),
                        tr("Minimise"));
        m_max   = mkBtn(QStringLiteral("winBtn"),      QStringLiteral("▢"),
                        tr("Maximise"));
        m_close = mkBtn(QStringLiteral("winBtnClose"), QStringLiteral("✕"),
                        tr("Close"));

        connect(m_min,   &QPushButton::clicked, this, &TitleBar::minimiseRequested);
        connect(m_max,   &QPushButton::clicked, this, &TitleBar::maximiseRequested);
        connect(m_close, &QPushButton::clicked, this, &TitleBar::closeRequested);
    }

    void setTitle(const QString& t) { m_title->setText(t); }

signals:
    void minimiseRequested();
    void maximiseRequested();
    void closeRequested();
    /// Emitted while dragging; carries the delta in global coordinates.
    void dragged(const QPoint& globalDelta);
    void dragStarted();

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton) {
            m_pressPos = globalPosOf(e);
            m_dragging = true;
            emit dragStarted();
        }
        QWidget::mousePressEvent(e);
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (m_dragging && (e->buttons() & Qt::LeftButton)) {
            const QPoint now = globalPosOf(e);
            emit dragged(now - m_pressPos);
            m_pressPos = now;
        }
        QWidget::mouseMoveEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        m_dragging = false;
        QWidget::mouseReleaseEvent(e);
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton) emit maximiseRequested();
        QWidget::mouseDoubleClickEvent(e);
    }

private:
    static QPoint globalPosOf(QMouseEvent* e)
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return e->globalPosition().toPoint();
#else
        return e->globalPos();
#endif
    }

    QLabel*      m_logo  = nullptr;
    QLabel*      m_title = nullptr;
    QPushButton* m_min   = nullptr;
    QPushButton* m_max   = nullptr;
    QPushButton* m_close = nullptr;
    QPoint       m_pressPos;
    bool         m_dragging = false;
};

} // namespace sdr::ui
