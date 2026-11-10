#pragma once

#include <QMediaPlayer>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <memory>

class QAudioOutput;
class QMediaDevices;
class QTimer;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
class QAudioBufferOutput;
#endif

namespace asmr {

class PlaybackDiagnosticLog;
struct AudioDiagnosticCapture;

class AudioPlayer final : public QObject
{
    Q_OBJECT

public:
    explicit AudioPlayer(QObject *parent = nullptr);
    ~AudioPlayer() override;

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

    // Optional, passive diagnostics. Context is supplied only for an accepted
    // library source; external media and existing callers remain unobserved.
    void setDiagnosticLog(PlaybackDiagnosticLog *log);
    void beginDiagnosticSource(QJsonObject context);
    void endDiagnosticSource(QString reason);
    void diagnosticSnapshot(QString reason);
    void diagnosticBoundary(QString reason);

signals:
    void mediaStatusChanged(QMediaPlayer::MediaStatus status);
    void durationChanged(qint64 duration);
    void seekableChanged(bool seekable);
    void positionChanged(qint64 position);
    void playbackStateChanged(QMediaPlayer::PlaybackState state);
    void errorOccurred(const QString &message);

private:
    void diagnosticEvent(const QString &event, QJsonObject fields = {});
    void installDiagnosticCapture();
    void detachDiagnosticCapture();

    QMediaPlayer *m_player;
    QAudioOutput *m_audioOutput;
    QPointer<PlaybackDiagnosticLog> m_diagnosticLog;
    QJsonObject m_diagnosticContext;
    bool m_diagnosticSourceInstalling = false;
    QMediaDevices *m_diagnosticDevices = nullptr;
    QTimer *m_diagnosticTimer = nullptr;
    std::shared_ptr<AudioDiagnosticCapture> m_diagnosticCapture;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QAudioBufferOutput *m_diagnosticBufferOutput = nullptr;
    QMetaObject::Connection m_diagnosticBufferConnection;
#endif
};

} // namespace asmr
