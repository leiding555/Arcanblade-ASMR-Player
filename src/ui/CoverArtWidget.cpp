#include "ui/CoverArtWidget.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace asmr {

CoverArtWidget::CoverArtWidget(QWidget *parent)
    : QLabel(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(64, 64);
    setFocusPolicy(Qt::NoFocus);
    setTextFormat(Qt::PlainText);
    setScaledContents(false);
    clearImage();
}

void CoverArtWidget::setImage(const QImage &image, const QString &sourcePath, const QString &detail)
{
    if (image.isNull()) {
        clearImage(QStringLiteral("封面不可用"));
        return;
    }
    m_image = image;
    m_sourcePath = sourcePath;
    m_detail = detail;
    setAccessibleName(m_contextName);
    QString description = sourcePath.isEmpty()
        ? m_contextName : m_contextName + QStringLiteral("：") + sourcePath;
    if (!detail.isEmpty()) description += u'\n' + detail;
    setAccessibleDescription(description);
    // Qt tooltips auto-detect rich text, so paths must be explicitly escaped.
    setToolTip(QStringLiteral("<qt>%1</qt>").arg(description.toHtmlEscaped()));
    update();
}

void CoverArtWidget::clearImage(const QString &message)
{
    m_image = {};
    m_sourcePath.clear();
    m_detail.clear();
    const QString description = message.isEmpty() ? QStringLiteral("暂无封面") : message;
    m_placeholderMessage = description;
    setAccessibleName(QStringLiteral("封面占位图"));
    setAccessibleDescription(description);
    setToolTip(QStringLiteral("<qt>%1</qt>").arg(description.toHtmlEscaped()));
    update();
}

void CoverArtWidget::setContextName(const QString &name)
{
    m_contextName = name.isEmpty() ? QStringLiteral("当前播放封面") : name;
    if (hasImage()) setImage(m_image, m_sourcePath, m_detail);
    else clearImage(m_placeholderMessage);
}

void CoverArtWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF target(contentsRect());
    const qreal radius = std::min<qreal>(8.0, std::min(target.width(), target.height()) / 2.0);
    QPainterPath rounded;
    rounded.addRoundedRect(target, radius, radius);
    painter.fillPath(rounded, QColor("#2B313A"));

    if (!m_image.isNull()) {
        const qreal side = std::min(m_image.width(), m_image.height());
        const QRectF source((m_image.width() - side) / 2.0,
                            (m_image.height() - side) / 2.0, side, side);
        painter.setClipPath(rounded);
        painter.drawImage(target, m_image, source);
        return;
    }

    // Center the visible note, including the round pen caps, on the 64px grid.
    // The image branch above keeps its existing centered crop and clipping.
    painter.translate(target.topLeft());
    painter.scale(target.width() / 64.0, target.height() / 64.0);
    painter.translate(0.75, 3.25);
    painter.setPen(QPen(QColor("#B9C1CD"), 3, Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin));
    painter.drawLine(QPointF(27, 39), QPointF(27, 20));
    painter.drawLine(QPointF(27, 20), QPointF(44, 16));
    painter.drawLine(QPointF(44, 16), QPointF(44, 35));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#B9C1CD"));
    painter.drawEllipse(QRectF(17, 35, 11, 8));
    painter.drawEllipse(QRectF(34, 31, 11, 8));
}

} // namespace asmr
