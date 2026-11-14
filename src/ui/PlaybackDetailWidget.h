#pragma once

#include <QImage>
#include <QWidget>

namespace asmr {

// A background for the shared playback views. Artwork is already decoded;
// this widget does not load files or own playback and subtitle state.
class PlaybackDetailWidget final : public QWidget
{
    Q_OBJECT

public:
    static constexpr qreal CornerRadius = 12.0;

    explicit PlaybackDetailWidget(QWidget *parent = nullptr);

    void setCoverImage(const QImage &image);
    void clearCoverImage();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QImage m_backgroundImage;
    qint64 m_coverImageKey = 0;
    quint64 m_backgroundGeneration = 0;
};

} // namespace asmr
