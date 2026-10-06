#pragma once
#include <QColor>
#include <QRgb>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <vector>

namespace sdr::ui {

/// 256-entry lookup tables. Kept tiny and self-contained so the waterfall can
/// map a whole row with a single array index per bin.
class ColorMap
{
public:
    enum Preset { Jet, Viridis, Inferno, Grayscale };

    static QStringList names()
    {
        return {QStringLiteral("Jet"), QStringLiteral("Viridis"),
                QStringLiteral("Inferno"), QStringLiteral("Grayscale")};
    }

    explicit ColorMap(Preset p = Jet) { build(p); }

    void setPreset(Preset p) { build(p); }
    void setPreset(int i)    { build(static_cast<Preset>(std::clamp(i, 0, 3))); }

    /// t is clamped to [0,1].
    inline QRgb rgb(float t) const
    {
        const int i = static_cast<int>(std::clamp(t, 0.0f, 1.0f) * 255.0f);
        return m_lut[static_cast<std::size_t>(i)];
    }

    QColor color(float t) const { return QColor::fromRgb(rgb(t)); }

private:
    void build(Preset p)
    {
        m_lut.resize(256);
        for (int i = 0; i < 256; ++i) {
            const float t = i / 255.0f;
            float r = 0, g = 0, b = 0;
            switch (p) {
            case Jet:
                r = std::clamp(1.5f - std::abs(4.0f * t - 3.0f), 0.0f, 1.0f);
                g = std::clamp(1.5f - std::abs(4.0f * t - 2.0f), 0.0f, 1.0f);
                b = std::clamp(1.5f - std::abs(4.0f * t - 1.0f), 0.0f, 1.0f);
                break;
            case Viridis:
                r = std::clamp(0.28f + 0.10f * t + 0.75f * t * t * t, 0.0f, 1.0f);
                g = std::clamp(0.00f + 0.90f * t, 0.0f, 1.0f);
                b = std::clamp(0.33f + 0.85f * t - 1.05f * t * t, 0.0f, 1.0f);
                break;
            case Inferno:
                r = std::clamp(1.35f * t + 0.05f, 0.0f, 1.0f);
                g = std::clamp(1.25f * t * t, 0.0f, 1.0f);
                b = std::clamp(0.55f * std::sin(3.14159f * t) + 0.85f * t * t * t * t, 0.0f, 1.0f);
                break;
            case Grayscale:
                r = g = b = t;
                break;
            }
            m_lut[static_cast<std::size_t>(i)] =
                qRgb(int(r * 255), int(g * 255), int(b * 255));
        }
    }

    std::vector<QRgb> m_lut;
};

} // namespace sdr::ui
