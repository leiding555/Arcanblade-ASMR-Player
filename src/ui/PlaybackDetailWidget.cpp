#include "ui/PlaybackDetailWidget.h"

#include <QBrush>
#include <QFutureWatcher>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPromise>
#include <QThreadPool>
#include <QTransform>

#include <algorithm>
#include <memory>

namespace asmr {
namespace {

// A few small box passes approximate a broad blur without allocating an image
// at window resolution. All image preparation runs outside the GUI thread.
QImage blurPass(const QImage &source, bool horizontal)
{
    constexpr int radius = 7;
    constexpr int samples = radius * 2 + 1;
    QImage result(source.size(), QImage::Format_RGB32);
    const int length = horizontal ? source.width() : source.height();
    const int lines = horizontal ? source.height() : source.width();
    for (int line = 0; line < lines; ++line) {
        auto pixel = [&](int offset) {
            offset = std::clamp(offset, 0, length - 1);
            return horizontal
                ? reinterpret_cast<const QRgb *>(source.constScanLine(line))[offset]
                : reinterpret_cast<const QRgb *>(source.constScanLine(offset))[line];
        };
        int red = 0;
        int green = 0;
        int blue = 0;
        for (int offset = -radius; offset <= radius; ++offset) {
            const QRgb color = pixel(offset);
            red += qRed(color);
            green += qGreen(color);
            blue += qBlue(color);
        }
        for (int offset = 0; offset < length; ++offset) {
            const QRgb color = qRgb(red / samples, green / samples, blue / samples);
            if (horizontal)
                reinterpret_cast<QRgb *>(result.scanLine(line))[offset] = color;
            else
                reinterpret_cast<QRgb *>(result.scanLine(offset))[line] = color;
            const QRgb leaving = pixel(offset - radius);
            const QRgb entering = pixel(offset + radius + 1);
            red += qRed(entering) - qRed(leaving);
            green += qGreen(entering) - qGreen(leaving);
            blue += qBlue(entering) - qBlue(leaving);
        }
    }
    return result;
}

QImage makeBackground(const QImage &image)
{
    constexpr int maximumSide = 192;
    const qreal factor = std::min<qreal>(1.0, qreal(maximumSide)
        / std::max(image.width(), image.height()));
    const QSize size(std::max(1, qRound(image.width() * factor)),
                     std::max(1, qRound(image.height() * factor)));
    QImage prepared(size, QImage::Format_RGB32);
    prepared.fill(QColor("#1F232A"));
    {
        QPainter painter(&prepared);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(prepared.rect(), image);
    }
    for (int pass = 0; pass < 3; ++pass) {
        prepared = blurPass(prepared, true);
        prepared = blurPass(prepared, false);
    }
    for (int y = 0; y < prepared.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(prepared.scanLine(y));
        for (int x = 0; x < prepared.width(); ++x) {
            const QRgb color = line[x];
            // Retain the cover's hue while keeping white transcript text clear.
            line[x] = qRgb(8 + qRed(color) * 24 / 100,
                           9 + qGreen(color) * 24 / 100,
                           11 + qBlue(color) * 24 / 100);
        }
    }
    return prepared;
}

} // namespace

PlaybackDetailWidget::PlaybackDetailWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("playbackDetailPage"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void PlaybackDetailWidget::setCoverImage(const QImage &image)
{
    if (image.isNull()) {
        clearCoverImage();
        return;
    }
    if (m_coverImageKey == image.cacheKey()) return;
    m_coverImageKey = image.cacheKey();
    const quint64 generation = ++m_backgroundGeneration;
    // A new source never briefly displays the previous track's background.
    m_backgroundImage = {};
    update();

    auto promise = std::make_shared<QPromise<QImage>>();
    auto *watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcher<QImage>::finished, this,
            [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != m_backgroundGeneration || watcher->future().resultCount() == 0)
            return;
        m_backgroundImage = watcher->result();
        update();
    });
    watcher->setFuture(promise->future());
    // QImage is implicitly shared. The worker captures values only and never
    // accesses a widget, even when its parent and watcher have been destroyed.
    QThreadPool::globalInstance()->start([promise, image] {
        promise->start();
        promise->addResult(makeBackground(image));
        promise->finish();
    });
}

void PlaybackDetailWidget::clearCoverImage()
{
    ++m_backgroundGeneration;
    m_coverImageKey = 0;
    m_backgroundImage = {};
    update();
}

void PlaybackDetailWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    QPainterPath background;
    background.addRoundedRect(QRectF(rect()), CornerRadius, CornerRadius);
    if (!m_backgroundImage.isNull()) {
        const qreal scale = std::max(qreal(width()) / m_backgroundImage.width(),
                                     qreal(height()) / m_backgroundImage.height());
        const QSizeF sourceSize(width() / scale, height() / scale);
        const QRectF source((m_backgroundImage.width() - sourceSize.width()) / 2.0,
                            (m_backgroundImage.height() - sourceSize.height()) / 2.0,
                            sourceSize.width(), sourceSize.height());
        QBrush artwork(m_backgroundImage);
        QTransform mapping;
        mapping.translate(-source.left() * scale, -source.top() * scale);
        mapping.scale(scale, scale);
        artwork.setTransform(mapping);
        painter.fillPath(background, artwork);
        return;
    }
    QLinearGradient gradient(rect().topLeft(), rect().bottomRight());
    gradient.setColorAt(0.0, QColor("#1F232A"));
    gradient.setColorAt(1.0, QColor("#14171B"));
    painter.fillPath(background, gradient);
}

} // namespace asmr
