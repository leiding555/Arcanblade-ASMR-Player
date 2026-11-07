#include "ui/FloatingSubtitleSettings.h"

#include <QFontDatabase>
#include <QMetaType>
#include <QRegularExpression>
#include <QSettings>
#include <QVariant>

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace asmr {
namespace {

std::optional<int> readInteger(const QVariant &value)
{
    // INI settings lose numeric type information across processes. Accept only
    // integral variants or complete decimal strings, never bool/double coercion.
    switch (value.metaType().id()) {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
        break;
    case QMetaType::ULongLong: {
        const auto number = value.toULongLong();
        if (number > static_cast<qulonglong>(std::numeric_limits<int>::max()))
            return std::nullopt;
        return static_cast<int>(number);
    }
    case QMetaType::QString: {
        static const QRegularExpression decimal(QStringLiteral("\\A[+-]?[0-9]+\\z"));
        if (!decimal.match(value.toString()).hasMatch())
            return std::nullopt;
        break;
    }
    default:
        return std::nullopt;
    }
    bool ok = false;
    const qint64 number = value.toLongLong(&ok);
    if (!ok || number < std::numeric_limits<int>::min()
        || number > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(number);
}

int coordinate(qint64 value)
{
    return static_cast<int>(std::clamp<qint64>(value,
        std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
}

bool hasRecoverableIntersection(QPoint position, QSize size, const QRect &screen)
{
    if (screen.isEmpty() || size.isEmpty())
        return false;
    // Promote before adding dimensions; saved coordinates may be INT_MAX.
    // Never construct a possibly overflowing QRect for the window itself.
    const qint64 x = position.x();
    const qint64 y = position.y();
    const qint64 visibleWidth = std::min(x + size.width(),
        qint64(screen.x()) + screen.width()) - std::max<qint64>(x, screen.x());
    const qint64 visibleHeight = std::min(y + size.height(),
        qint64(screen.y()) + screen.height()) - std::max<qint64>(y, screen.y());
    const qint64 minimumWidth = std::min({64, size.width(), screen.width()});
    const qint64 minimumHeight = std::min({32, size.height(), screen.height()});
    return visibleWidth >= minimumWidth && visibleHeight >= minimumHeight;
}

QPoint nearestPositionInArea(QPoint position, QSize size, const QRect &screen)
{
    // Use exclusive bounds promoted to qint64, never QRect::right()/bottom().
    // An oversized dimension starts at the work area's corresponding edge.
    const qint64 minX = screen.x();
    const qint64 minY = screen.y();
    const qint64 maxX = minX + std::max<qint64>(0,
        qint64(screen.width()) - std::max(0, size.width()));
    const qint64 maxY = minY + std::max<qint64>(0,
        qint64(screen.height()) - std::max(0, size.height()));
    return QPoint(coordinate(std::clamp<qint64>(position.x(), minX, maxX)),
                  coordinate(std::clamp<qint64>(position.y(), minY, maxY)));
}

} // namespace

QList<QColor> floatingSubtitleFontColors()
{
    return {QColor(QStringLiteral("#ffffff")), QColor(QStringLiteral("#ffd54f")),
        QColor(QStringLiteral("#81d4fa")), QColor(QStringLiteral("#80cbc4")),
        QColor(QStringLiteral("#f8bbd0"))};
}

int floatingSubtitleHeightForFont(int fontPointSize)
{
    const int font = std::clamp(fontPointSize,
        MinimumFloatingFontPointSize, MaximumFloatingFontPointSize);
    constexpr int fontSpan = MaximumFloatingFontPointSize - MinimumFloatingFontPointSize;
    constexpr int heightSpan = MaximumFloatingSubtitleHeight - MinimumFloatingSubtitleHeight;
    return MinimumFloatingSubtitleHeight
        + ((font - MinimumFloatingFontPointSize) * heightSpan + fontSpan / 2) / fontSpan;
}

int floatingSubtitleFontForHeight(int height)
{
    const int boundedHeight = std::clamp(height,
        MinimumFloatingSubtitleHeight, MaximumFloatingSubtitleHeight);
    constexpr int fontSpan = MaximumFloatingFontPointSize - MinimumFloatingFontPointSize;
    constexpr int heightSpan = MaximumFloatingSubtitleHeight - MinimumFloatingSubtitleHeight;
    return MinimumFloatingFontPointSize
        + ((boundedHeight - MinimumFloatingSubtitleHeight) * fontSpan + heightSpan / 2) / heightSpan;
}

FloatingSubtitleAppearance defaultFloatingSubtitleAppearance()
{
    FloatingSubtitleAppearance appearance;
    appearance.fontFamily = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    return appearance;
}

FloatingSubtitleAppearance validatedFloatingSubtitleAppearance(
    const FloatingSubtitleAppearance &appearance)
{
    const auto defaults = defaultFloatingSubtitleAppearance();
    auto result = appearance;
    const QString family = result.fontFamily.trimmed();
    const auto families = QFontDatabase::families();
    const auto match = std::find_if(families.cbegin(), families.cend(),
        [&family](const QString &installed) {
            return installed.compare(family, Qt::CaseInsensitive) == 0;
        });
    result.fontFamily = match == families.cend() ? defaults.fontFamily : *match;
    if (result.fontPointSize < MinimumFloatingFontPointSize
        || result.fontPointSize > MaximumFloatingFontPointSize) {
        result.fontPointSize = defaults.fontPointSize;
    }
    if (result.backgroundOpacityPercent < 0 || result.backgroundOpacityPercent > 100)
        result.backgroundOpacityPercent = defaults.backgroundOpacityPercent;
    if (!floatingSubtitleFontColors().contains(result.fontColor))
        result.fontColor = defaults.fontColor;
    return result;
}

FloatingSubtitleSettings readFloatingSubtitleSettings(QSettings &settings)
{
    FloatingSubtitleSettings result{defaultFloatingSubtitleAppearance(), std::nullopt, std::nullopt};
    settings.beginGroup(QStringLiteral("FloatingSubtitles"));
    const auto family = settings.value(QStringLiteral("fontFamily"));
    if (family.metaType().id() == QMetaType::QString)
        result.appearance.fontFamily = family.toString();
    if (const auto size = readInteger(settings.value(QStringLiteral("fontPointSize"))))
        result.appearance.fontPointSize = *size;
    if (const auto opacity = readInteger(settings.value(QStringLiteral("backgroundOpacityPercent"))))
        result.appearance.backgroundOpacityPercent = *opacity;
    const auto color = settings.value(QStringLiteral("fontColor"));
    if (color.metaType().id() == QMetaType::QString)
        result.appearance.fontColor = QColor(color.toString());
    const auto width = readInteger(settings.value(QStringLiteral("width")));
    const auto height = readInteger(settings.value(QStringLiteral("height")));
    if (width && height && *width >= MinimumFloatingSubtitleWidth
        && *height >= MinimumFloatingSubtitleHeight && *height <= MaximumFloatingSubtitleHeight) {
        result.size = QSize(*width, *height);
    }
    const auto x = readInteger(settings.value(QStringLiteral("positionX")));
    const auto y = readInteger(settings.value(QStringLiteral("positionY")));
    if (x && y)
        result.position = QPoint(*x, *y);
    settings.endGroup();
    result.appearance = validatedFloatingSubtitleAppearance(result.appearance);
    return result;
}

bool writeFloatingSubtitleSettings(QSettings &settings,
    const FloatingSubtitleAppearance &appearance, QPoint position, QSize size)
{
    const auto valid = validatedFloatingSubtitleAppearance(appearance);
    settings.beginGroup(QStringLiteral("FloatingSubtitles"));
    settings.setValue(QStringLiteral("fontFamily"), valid.fontFamily);
    settings.setValue(QStringLiteral("fontPointSize"), valid.fontPointSize);
    settings.setValue(QStringLiteral("backgroundOpacityPercent"), valid.backgroundOpacityPercent);
    settings.setValue(QStringLiteral("fontColor"), valid.fontColor.name(QColor::HexRgb));
    if (size.width() >= MinimumFloatingSubtitleWidth
        && size.height() >= MinimumFloatingSubtitleHeight
        && size.height() <= MaximumFloatingSubtitleHeight) {
        settings.setValue(QStringLiteral("width"), size.width());
        settings.setValue(QStringLiteral("height"), size.height());
    } else {
        settings.remove(QStringLiteral("width"));
        settings.remove(QStringLiteral("height"));
    }
    settings.setValue(QStringLiteral("positionX"), position.x());
    settings.setValue(QStringLiteral("positionY"), position.y());
    settings.endGroup();
    settings.sync();
    return settings.status() == QSettings::NoError;
}

QPoint safeFloatingSubtitlePosition(std::optional<QPoint> savedPosition,
    QSize windowSize, const QList<QRect> &availableScreens, QRect primaryAvailable)
{
    QList<QRect> screens;
    for (const auto &screen : availableScreens) {
        if (!screen.isEmpty())
            screens.push_back(screen);
    }
    if (screens.isEmpty() && !primaryAvailable.isEmpty())
        screens.push_back(primaryAvailable);
    // A display change can briefly expose an empty topology. Do not overwrite
    // established coordinates with a false origin during that transition.
    if (screens.isEmpty())
        return savedPosition.value_or(QPoint());
    auto primaryIndex = screens.indexOf(primaryAvailable);
    if (primaryIndex < 0)
        primaryIndex = 0;

    if (savedPosition) {
        for (const auto &screen : screens) {
            if (hasRecoverableIntersection(*savedPosition, windowSize, screen))
                return *savedPosition;
        }
        QPoint nearest;
        qint64 nearestDistance = std::numeric_limits<qint64>::max();
        bool nearestIsPrimary = false;
        for (qsizetype index = 0; index < screens.size(); ++index) {
            const auto candidate = nearestPositionInArea(*savedPosition, windowSize, screens[index]);
            const qint64 distance = std::abs(qint64(candidate.x()) - savedPosition->x())
                + std::abs(qint64(candidate.y()) - savedPosition->y());
            const bool isPrimary = index == primaryIndex;
            if (distance < nearestDistance
                || (distance == nearestDistance && isPrimary && !nearestIsPrimary)) {
                nearest = candidate;
                nearestDistance = distance;
                nearestIsPrimary = isPrimary;
            }
        }
        return nearest;
    }

    const auto &primary = screens[primaryIndex];
    // First launch retains the bottom-centered placement and 40 logical-pixel
    // margin. A saved but unreachable position instead uses the nearest edge.
    const qint64 xOffset = std::max<qint64>(0,
        (qint64(primary.width()) - std::max(0, windowSize.width())) / 2);
    const qint64 yOffset = std::max<qint64>(0,
        qint64(primary.height()) - std::max(0, windowSize.height()) - 40);
    return QPoint(coordinate(qint64(primary.x()) + xOffset),
                  coordinate(qint64(primary.y()) + yOffset));
}

} // namespace asmr
