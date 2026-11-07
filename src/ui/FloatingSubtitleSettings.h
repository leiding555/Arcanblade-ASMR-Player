#pragma once

#include <QColor>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

#include <optional>

class QSettings;

namespace asmr {

inline constexpr int MinimumFloatingFontPointSize = 16;
inline constexpr int MaximumFloatingFontPointSize = 96;
inline constexpr int MinimumFloatingSubtitleWidth = 400;
inline constexpr int MinimumFloatingSubtitleHeight = 120;
inline constexpr int MaximumFloatingSubtitleHeight = 480;

struct FloatingSubtitleAppearance {
    QString fontFamily;
    int fontPointSize = 20;
    int backgroundOpacityPercent = 82;
    QColor fontColor = Qt::white;

    bool operator==(const FloatingSubtitleAppearance &) const = default;
};

struct FloatingSubtitleSettings {
    FloatingSubtitleAppearance appearance;
    std::optional<QPoint> position;
    std::optional<QSize> size;
};

[[nodiscard]] QList<QColor> floatingSubtitleFontColors();
// Shared, monotonic mapping in Qt logical pixels; inverse rounding preserves
// every supported font-size choice when height is derived from that choice.
[[nodiscard]] int floatingSubtitleHeightForFont(int fontPointSize);
[[nodiscard]] int floatingSubtitleFontForHeight(int height);
[[nodiscard]] FloatingSubtitleAppearance defaultFloatingSubtitleAppearance();
[[nodiscard]] FloatingSubtitleAppearance validatedFloatingSubtitleAppearance(
    const FloatingSubtitleAppearance &appearance);
[[nodiscard]] FloatingSubtitleSettings readFloatingSubtitleSettings(QSettings &settings);
[[nodiscard]] bool writeFloatingSubtitleSettings(QSettings &settings,
    const FloatingSubtitleAppearance &appearance, QPoint position, QSize size = QSize());

// All coordinates are Qt logical pixels. A position is retained only if a
// single work area contains min(64, panel width, area width) by
// min(32, panel height, area height) visible pixels. Otherwise choose the nearest
// work area; never combine intersections across separate screens or their gaps.
[[nodiscard]] QPoint safeFloatingSubtitlePosition(std::optional<QPoint> savedPosition,
    QSize windowSize, const QList<QRect> &availableScreens, QRect primaryAvailable);

} // namespace asmr
