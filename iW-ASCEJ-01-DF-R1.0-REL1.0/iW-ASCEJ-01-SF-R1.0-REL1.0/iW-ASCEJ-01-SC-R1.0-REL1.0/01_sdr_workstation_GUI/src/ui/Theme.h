#pragma once
#include <QColor>
#include <QFont>
#include <QString>

namespace sdr::ui::theme {

// Single source of truth for the instrument palette. dark.qss mirrors these
// values for the widget chrome; the painted plots read them from here.

inline const QColor appBg      (0x0d, 0x11, 0x17);
inline const QColor panel      (0x16, 0x1b, 0x22);
inline const QColor panelAlt   (0x1c, 0x21, 0x28);
inline const QColor titleBar   (0x1f, 0x26, 0x2e);
inline const QColor plotBg     (0x0a, 0x0d, 0x12);
inline const QColor border     (0x2d, 0x35, 0x40);
inline const QColor borderLite (0x3d, 0x47, 0x53);
inline const QColor grid       (0x1b, 0x22, 0x2b);
inline const QColor gridMajor  (0x25, 0x2e, 0x39);

inline const QColor text       (0xc9, 0xd4, 0xe0);
inline const QColor textDim    (0x7d, 0x8b, 0x9a);
inline const QColor textFaint  (0x55, 0x62, 0x70);

inline const QColor accent     (0x2f, 0x9b, 0xff);
inline const QColor accentDim  (0x1d, 0x5f, 0x9e);
inline const QColor ok         (0x33, 0xd1, 0x7a);
inline const QColor warn       (0xfb, 0xbf, 0x24);
inline const QColor danger     (0xf8, 0x71, 0x71);

inline const QColor traceI     (0x38, 0xbd, 0xf8);
inline const QColor traceQ     (0xfb, 0x92, 0x3c);
inline const QColor traceMag   (0xa7, 0x8b, 0xfa);
inline const QColor spectrum   (0xa3, 0xe6, 0x35);
inline const QColor marker     (0xfb, 0xbf, 0x24);
inline const QColor cursor     (0x60, 0xa5, 0xfa);

/// Per-channel trace colours for overlay mode.
inline QColor channelColor(int ch)
{
    static const QColor c[] = {
        QColor(0x38, 0xbd, 0xf8), QColor(0xa3, 0xe6, 0x35),
        QColor(0xfb, 0x92, 0x3c), QColor(0xf4, 0x72, 0xb6),
        QColor(0xa7, 0x8b, 0xfa), QColor(0x2d, 0xd4, 0xbf),
        QColor(0xfa, 0xcc, 0x15), QColor(0xf8, 0x71, 0x71)};
    return c[static_cast<std::size_t>(ch) % (sizeof(c) / sizeof(c[0]))];
}

inline QFont uiFont(int px = 11, bool bold = false)
{
    QFont f(QStringLiteral("DejaVu Sans"));
    f.setPixelSize(px);
    f.setBold(bold);
    return f;
}

/// Monospace for anything numeric that updates in place — proportional digits
/// make a live readout jitter sideways and are much harder to read.
inline QFont monoFont(int px = 11, bool bold = false)
{
    QFont f(QStringLiteral("DejaVu Sans Mono"));
    f.setStyleHint(QFont::Monospace);
    f.setPixelSize(px);
    f.setBold(bold);
    return f;
}

} // namespace sdr::ui::theme
