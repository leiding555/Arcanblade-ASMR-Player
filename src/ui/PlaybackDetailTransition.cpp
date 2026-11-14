#include "ui/PlaybackDetailTransition.h"
#include "ui/PlaybackDetailWidget.h"

#include <QBrush>
#include <QEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QTimer>
#include <QTransform>

#include <algorithm>
#include <cmath>

namespace asmr {
namespace {

bool matchingLogicalSizes(const QPixmap &left, const QPixmap &right)
{
    if (left.isNull() || right.isNull()) return false;
    const QSizeF leftSize = left.deviceIndependentSize();
    const QSizeF rightSize = right.deviceIndependentSize();
    return leftSize.width() > 0.0 && leftSize.height() > 0.0
        && std::abs(leftSize.width() - rightSize.width()) < 0.01
        && std::abs(leftSize.height() - rightSize.height()) < 0.01;
}

void drawDetailSnapshot(QPainter &painter, const QRectF &bounds, const QPixmap &snapshot)
{
    // A texture-filled path preserves antialiased corners without a per-frame
    // bitmap mask. Only the detail layer is rounded; the library stays intact.
    const QSizeF logicalSize = snapshot.deviceIndependentSize();
    QBrush brush(snapshot);
    QTransform mapping;
    mapping.translate(bounds.left(), bounds.top());
    mapping.scale(bounds.width() / logicalSize.width(), bounds.height() / logicalSize.height());
    brush.setTransform(mapping);
    painter.setPen(Qt::NoPen);
    painter.setBrush(brush);
    painter.drawRoundedRect(bounds, PlaybackDetailWidget::CornerRadius, PlaybackDetailWidget::CornerRadius);
}

} // namespace

PlaybackDetailTransition::PlaybackDetailTransition(QWidget *parent)
    : QWidget(parent), m_animation(new QPropertyAnimation(this, "progress", this))
{
    setObjectName(QStringLiteral("playbackDetailTransition"));
    setFocusPolicy(Qt::NoFocus);
    setAutoFillBackground(false);
    m_animation->setObjectName(QStringLiteral("playbackDetailTransitionAnimation"));
    m_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_animation, &QPropertyAnimation::finished, this, [this] {
        setProgress(m_targetDetail ? 1.0 : 0.0);
        emit finished(m_targetDetail);
    });
    hide();
}

bool PlaybackDetailTransition::setSnapshots(const QPixmap &library, const QPixmap &detail)
{
    cancel();
    if (!matchingLogicalSizes(library, detail)) {
        m_librarySnapshot = {};
        m_detailSnapshot = {};
        hide();
        update();
        return false;
    }
    m_librarySnapshot = library;
    m_detailSnapshot = detail;
    update();
    return true;
}

bool PlaybackDetailTransition::hasSnapshots() const
{
    return !m_librarySnapshot.isNull() && !m_detailSnapshot.isNull();
}

void PlaybackDetailTransition::clearSnapshots()
{
    cancel();
    m_librarySnapshot = {};
    m_detailSnapshot = {};
    hide();
    update();
}

qreal PlaybackDetailTransition::progress() const
{
    return m_progress;
}

void PlaybackDetailTransition::setProgress(qreal value)
{
    if (!std::isfinite(value)) return;
    const qreal bounded = std::clamp(value, qreal(0.0), qreal(1.0));
    if (m_progress == bounded) return;
    m_progress = bounded;
    update();
}

bool PlaybackDetailTransition::isRunning() const
{
    return m_completionPending || m_animation->state() == QAbstractAnimation::Running;
}

bool PlaybackDetailTransition::targetDetail() const
{
    return m_targetDetail;
}

void PlaybackDetailTransition::animateTo(bool detail)
{
    if (!hasSnapshots()) return;
    // A repeated request for the current destination does not restart easing.
    if (isRunning() && m_targetDetail == detail) return;
    cancel();
    m_targetDetail = detail;
    const qreal destination = detail ? 1.0 : 0.0;
    const qreal distance = std::abs(destination - m_progress);
    if (distance <= 0.000001) {
        setProgress(destination);
        m_completionPending = true;
        const quint64 generation = m_completionGeneration;
        QTimer::singleShot(0, this, [this, generation, detail] {
            if (generation != m_completionGeneration) return;
            m_completionPending = false;
            emit finished(detail);
        });
        return;
    }
    m_animation->setDuration(std::max(1, qRound((detail ? 240.0 : 200.0) * distance)));
    m_animation->setStartValue(m_progress);
    m_animation->setEndValue(destination);
    m_animation->start();
}

void PlaybackDetailTransition::snapTo(bool detail)
{
    cancel();
    m_targetDetail = detail;
    setProgress(detail ? 1.0 : 0.0);
}

void PlaybackDetailTransition::cancel()
{
    ++m_completionGeneration;
    m_completionPending = false;
    m_animation->stop();
}

bool PlaybackDetailTransition::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::Wheel:
    case QEvent::ContextMenu:
        event->accept();
        return true;
    default:
        return QWidget::event(event);
    }
}

void PlaybackDetailTransition::paintEvent(QPaintEvent *)
{
    if (!hasSnapshots()) return;
    QPainter painter(this);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    const QRectF bounds(rect());
    if (m_progress >= 1.0) {
        drawDetailSnapshot(painter, bounds, m_detailSnapshot);
        return;
    }
    painter.drawPixmap(bounds, m_librarySnapshot, QRectF(m_librarySnapshot.rect()));
    if (m_progress <= 0.0) return;
    painter.setOpacity(m_progress);
    const qreal offset = (1.0 - m_progress) * 28.0;
    drawDetailSnapshot(painter, bounds.translated(0.0, offset), m_detailSnapshot);
}

} // namespace asmr
