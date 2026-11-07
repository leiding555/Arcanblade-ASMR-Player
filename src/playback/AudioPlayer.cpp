#include "playback/AudioPlayer.h"

#include <QAudioOutput>
#include <QUrl>

namespace asmr {

AudioPlayer::AudioPlayer(QObject *parent)
    : QObject(parent),
      m_player(new QMediaPlayer(this)),
      m_audioOutput(new QAudioOutput(this))
{
    m_player->setAudioOutput(m_audioOutput);
    m_audioOutput->setVolume(0.70f);

    connect(m_player, &QMediaPlayer::mediaStatusChanged,
            this, &AudioPlayer::mediaStatusChanged);
    connect(m_player, &QMediaPlayer::durationChanged,
            this, &AudioPlayer::durationChanged);
    connect(m_player, &QMediaPlayer::seekableChanged,
            this, &AudioPlayer::seekableChanged);
    connect(m_player, &QMediaPlayer::positionChanged,
            this, &AudioPlayer::positionChanged);
    connect(m_player, &QMediaPlayer::playbackStateChanged,
            this, &AudioPlayer::playbackStateChanged);
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error error, const QString &message) {
        if (error != QMediaPlayer::NoError)
            emit errorOccurred(message);
    });
}

QMediaPlayer::PlaybackState AudioPlayer::playbackState() const
{
    return m_player->playbackState();
}

QMediaPlayer::MediaStatus AudioPlayer::mediaStatus() const
{
    return m_player->mediaStatus();
}

QMediaPlayer::Error AudioPlayer::error() const
{
    return m_player->error();
}

QUrl AudioPlayer::source() const
{
    return m_player->source();
}

qint64 AudioPlayer::position() const
{
    return m_player->position();
}

qint64 AudioPlayer::duration() const
{
    return m_player->duration();
}

bool AudioPlayer::isSeekable() const
{
    return m_player->isSeekable();
}

void AudioPlayer::setLocalFile(const QString &path)
{
    m_player->setSource(QUrl::fromLocalFile(path));
}

void AudioPlayer::play()
{
    m_player->play();
}

void AudioPlayer::togglePlayback()
{
    if (m_player->playbackState() == QMediaPlayer::PlayingState)
        m_player->pause();
    else
        m_player->play();
}

void AudioPlayer::stop()
{
    m_player->stop();
}

void AudioPlayer::setPosition(qint64 position)
{
    if (m_player->isSeekable())
        m_player->setPosition(position);
}

void AudioPlayer::setVolumePercent(int volume)
{
    m_audioOutput->setVolume(static_cast<float>(volume) / 100.0f);
}

} // namespace asmr
