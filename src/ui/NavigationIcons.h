#pragma once

#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>

namespace asmr {

inline QIcon navigationChevronIcon(Qt::ArrowType direction)
{
    QPixmap pixmap(48, 48);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(QStringLiteral("#B8BEC8")), 1.8,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const bool left = direction == Qt::LeftArrow;
    painter.drawPolyline(QPolygonF{QPointF(left ? 15 : 9, 5),
        QPointF(left ? 8 : 16, 12), QPointF(left ? 15 : 9, 19)});
    painter.end();
    return QIcon(pixmap);
}

} // namespace asmr
