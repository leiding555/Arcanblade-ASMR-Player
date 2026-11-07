#pragma once

#include <QMediaPlayer>
#include <QObject>
#include <QString>
#include <QUrl>

class QAudioOutput;

namespace asmr {

class AudioPlayer final : public QObject
{
    Q_OBJECT

public:
    explicit AudioPlayer(QObject *parent = nullptr);

    [[nodiscard]] QMediaPlayer::PlaybackState playbackState() const;
    [[nodiscard]] QMediaPlayer::MediaStatus mediaStatus() const;
    [[nodiscard]] QMediaPlayer::Error error() const;
    [[nodiscard]] QUrl source() const;
    [[nodiscard]] qint64 position() const;
    [[nodiscard]] qint64 duration() const;
    [[nodiscard]] bool isSeekable() const;

    void setLocalFile(const QString &path);
    void play();
    void togglePlayback();
    void stop();
    void setPosition(qint64 position);
    void setVolumePercent(int volume);

signals:
    void mediaStatusChanged(QMediaPlayer::MediaStatus status);
    void durationChanged(qint64 duration);
    void seekableChanged(bool seekable);
    void positionChanged(qint64 position);
    void playbackStateChanged(QMediaPlayer::PlaybackState state);
    void errorOccurred(const QString &message);

private:
    QMediaPlayer *m_player;
    QAudioOutput *m_audioOutput;
};

} // namespace asmr
