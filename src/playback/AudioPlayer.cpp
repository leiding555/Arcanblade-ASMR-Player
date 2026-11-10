#include "playback/AudioPlayer.h"
#include "playback/PlaybackDiagnosticLog.h"

#include <QAudioBuffer>
#include <QAudioDevice>
#include <QAudioOutput>
#include <QJsonArray>
#include <QMediaDevices>
#include <QMediaMetaData>
#include <QMetaEnum>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QAudioBufferOutput>
#endif

#include <atomic>
#include <mutex>
#include <utility>

namespace asmr {
namespace {
template<class Enum> QString enumName(Enum value)
{
    const auto *key = QMetaEnum::fromType<Enum>().valueToKey(int(value));
    return key ? QString::fromLatin1(key) : QString::number(int(value));
}

QJsonObject audioDevice(const QAudioDevice &device)
{
    return {{QStringLiteral("is_null"), device.isNull()},
            {QStringLiteral("id_hex"), QString::fromLatin1(device.id().toHex())},
            {QStringLiteral("name"), device.description()},
            {QStringLiteral("is_default"), device.isDefault()}};
}
}

// The capture gate serializes source retirement with the very short buffer
// ingress. It is never held while samples are analyzed or files are written.
struct AudioDiagnosticCapture {
    std::mutex gate;
    bool active = true;
    std::atomic_bool observed = false;
};

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

    m_diagnosticTimer = new QTimer(this);
    m_diagnosticTimer->setInterval(5'000);
    connect(m_diagnosticTimer, &QTimer::timeout, this, [this] {
        if (m_player->playbackState() == QMediaPlayer::PlayingState)
            diagnosticBoundary(QStringLiteral("heartbeat"));
    });
    connect(m_player, &QMediaPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState state) {
        if (state == QMediaPlayer::PlayingState) {
            if (m_diagnosticLog && !m_diagnosticContext.isEmpty()) m_diagnosticTimer->start();
            diagnosticSnapshot(QStringLiteral("playback_state_changed"));
        } else {
            m_diagnosticTimer->stop();
            diagnosticBoundary(state == QMediaPlayer::PausedState
                ? QStringLiteral("paused") : QStringLiteral("stopped"));
        }
    });
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia)
            diagnosticBoundary(QStringLiteral("natural_end"));
        else if (status == QMediaPlayer::InvalidMedia)
            diagnosticBoundary(QStringLiteral("invalid_media"));
        else diagnosticSnapshot(QStringLiteral("media_status_changed"));
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error error, const QString &message) {
        if (error != QMediaPlayer::NoError) {
            diagnosticEvent(QStringLiteral("error"),
                {{QStringLiteral("error_code"), int(error)},
                 {QStringLiteral("error_name"), enumName(error)},
                 {QStringLiteral("error_string"), message}});
            diagnosticBoundary(QStringLiteral("error"));
        }
    });
    connect(m_player, &QMediaPlayer::sourceChanged, this,
            [this] { diagnosticSnapshot(QStringLiteral("source_changed")); });
    connect(m_player, &QMediaPlayer::hasAudioChanged, this,
            [this] { diagnosticSnapshot(QStringLiteral("has_audio_changed")); });
    connect(m_player, &QMediaPlayer::tracksChanged, this,
            [this] { diagnosticSnapshot(QStringLiteral("tracks_changed")); });
    connect(m_player, &QMediaPlayer::activeTracksChanged, this,
            [this] { diagnosticSnapshot(QStringLiteral("active_tracks_changed")); });
    connect(m_player, &QMediaPlayer::metaDataChanged, this,
            [this] { diagnosticSnapshot(QStringLiteral("metadata_changed")); });
    connect(m_player, &QMediaPlayer::durationChanged, this,
            [this] { diagnosticSnapshot(QStringLiteral("duration_changed")); });
    connect(m_player, &QMediaPlayer::seekableChanged, this,
            [this] { diagnosticSnapshot(QStringLiteral("seekable_changed")); });
    connect(m_player, &QMediaPlayer::audioOutputChanged, this,
            [this] { diagnosticSnapshot(QStringLiteral("audio_output_changed")); });
    connect(m_audioOutput, &QAudioOutput::deviceChanged, this,
            [this] { diagnosticSnapshot(QStringLiteral("output_device_changed")); });
    connect(m_audioOutput, &QAudioOutput::volumeChanged, this,
            [this] { diagnosticSnapshot(QStringLiteral("output_volume_changed")); });
    connect(m_audioOutput, &QAudioOutput::mutedChanged, this,
            [this] { diagnosticSnapshot(QStringLiteral("output_muted_changed")); });
}

AudioPlayer::~AudioPlayer()
{
    endDiagnosticSource(QStringLiteral("player_destroyed"));
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
    // MainWindow retires the old capture before stopping the old media. Never
    // leave an earlier source's buffer output attached across setSource().
    detachDiagnosticCapture();
    diagnosticEvent(QStringLiteral("set_source_requested"), {{QStringLiteral("path"), path}});
    m_diagnosticSourceInstalling = true;
    m_player->setSource(QUrl::fromLocalFile(path));
    m_diagnosticSourceInstalling = false;
    installDiagnosticCapture();
    diagnosticSnapshot(QStringLiteral("source_installed"));
}

void AudioPlayer::play()
{
    diagnosticEvent(QStringLiteral("play_requested"));
    m_player->play();
}

void AudioPlayer::togglePlayback()
{
    if (m_player->playbackState() == QMediaPlayer::PlayingState) {
        diagnosticEvent(QStringLiteral("pause_requested"));
        m_player->pause();
    } else play();
}

void AudioPlayer::stop()
{
    diagnosticEvent(QStringLiteral("stop_requested"));
    diagnosticBoundary(QStringLiteral("stop_requested"));
    m_player->stop();
}

void AudioPlayer::setPosition(qint64 position)
{
    diagnosticEvent(QStringLiteral("seek_requested"),
        {{QStringLiteral("target_ms"), position}, {QStringLiteral("seekable"), m_player->isSeekable()}});
    diagnosticBoundary(QStringLiteral("seek"));
    if (m_player->isSeekable())
        m_player->setPosition(position);
}

void AudioPlayer::setVolumePercent(int volume)
{
    diagnosticEvent(QStringLiteral("volume_requested"), {{QStringLiteral("percent"), volume}});
    m_audioOutput->setVolume(static_cast<float>(volume) / 100.0f);
}

void AudioPlayer::setDiagnosticLog(PlaybackDiagnosticLog *log)
{
    if (m_diagnosticLog == log) return;
    endDiagnosticSource(QStringLiteral("diagnostic_log_changed"));
    m_diagnosticLog = log;
    if (log && !m_diagnosticDevices) {
        m_diagnosticDevices = new QMediaDevices(this);
        connect(m_diagnosticDevices, &QMediaDevices::audioOutputsChanged, this,
            [this] { diagnosticSnapshot(QStringLiteral("audio_outputs_changed")); });
    }
}

void AudioPlayer::beginDiagnosticSource(QJsonObject context)
{
    endDiagnosticSource(QStringLiteral("source_replaced"));
    if (!m_diagnosticLog || context.value(QStringLiteral("work_id")).toInteger() <= 0
        || context.value(QStringLiteral("audio_file_id")).toInteger() <= 0)
        return;
    context.insert(QStringLiteral("capture_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_diagnosticContext = std::move(context);
    diagnosticEvent(QStringLiteral("source_begin"),
        {{QStringLiteral("requested_backend"), qEnvironmentVariable("QT_MEDIA_BACKEND")},
         {QStringLiteral("actual_backend"), QStringLiteral("unknown")},
         {QStringLiteral("pcm_backend_requirement"), QStringLiteral("ffmpeg")}});
}

void AudioPlayer::endDiagnosticSource(QString reason)
{
    m_diagnosticTimer->stop();
    detachDiagnosticCapture();
    if (m_diagnosticLog && !m_diagnosticContext.isEmpty()) {
        diagnosticSnapshot(reason);
        diagnosticEvent(QStringLiteral("source_end"), {{QStringLiteral("reason"), reason}});
        // Retires the worker's per-capture accumulator after all earlier buffer
        // submissions. Ingress cannot enqueue behind this retirement marker.
        m_diagnosticLog->flushPcm(m_diagnosticContext, QStringLiteral("source_end"));
    }
    m_diagnosticContext = {};
    m_diagnosticCapture.reset();
}

void AudioPlayer::diagnosticEvent(const QString &event, QJsonObject fields)
{
    if (m_diagnosticLog && !m_diagnosticContext.isEmpty())
        m_diagnosticLog->record(event, std::move(fields), m_diagnosticContext);
}

void AudioPlayer::diagnosticSnapshot(QString reason)
{
    if (!m_diagnosticLog || m_diagnosticContext.isEmpty()) return;
    QJsonArray outputs;
    for (const auto &device : QMediaDevices::audioOutputs()) outputs.append(audioDevice(device));
    QJsonArray tracks;
    const auto audioTracks = m_player->audioTracks();
    for (qsizetype index = 0; index < audioTracks.size(); ++index) {
        const auto &metadata = audioTracks.at(index);
        tracks.append(QJsonObject{{QStringLiteral("index"), qint64(index)},
            {QStringLiteral("codec"), metadata.stringValue(QMediaMetaData::AudioCodec)},
            {QStringLiteral("bit_rate"), QJsonValue::fromVariant(metadata.value(QMediaMetaData::AudioBitRate))}});
    }
    auto *output = m_player->audioOutput();
    QJsonObject data{
        {QStringLiteral("reason"), std::move(reason)},
        {QStringLiteral("source"), m_player->source().toLocalFile()},
        {QStringLiteral("source_installing"), m_diagnosticSourceInstalling},
        {QStringLiteral("source_matches_context"), m_player->source()
            == QUrl::fromLocalFile(m_diagnosticContext.value(QStringLiteral("path")).toString())},
        {QStringLiteral("position_ms"), m_player->position()},
        {QStringLiteral("duration_ms"), m_player->duration()},
        {QStringLiteral("playback_state"), enumName(m_player->playbackState())},
        {QStringLiteral("media_status"), enumName(m_player->mediaStatus())},
        {QStringLiteral("error_code"), int(m_player->error())},
        {QStringLiteral("error_name"), enumName(m_player->error())},
        {QStringLiteral("error_string"), m_player->errorString()},
        {QStringLiteral("seekable"), m_player->isSeekable()},
        {QStringLiteral("has_audio"), m_player->hasAudio()},
        {QStringLiteral("audio_track_count"), qint64(audioTracks.size())},
        {QStringLiteral("active_audio_track"), m_player->activeAudioTrack()},
        {QStringLiteral("audio_tracks"), tracks},
        {QStringLiteral("audio_output_attached"), output != nullptr},
        {QStringLiteral("expected_output_attached"), output == m_audioOutput},
        {QStringLiteral("output_device"), output ? QJsonValue(audioDevice(output->device())) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("volume"), output ? QJsonValue(double(output->volume())) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("muted"), output ? QJsonValue(output->isMuted()) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("default_output_device"), audioDevice(QMediaDevices::defaultAudioOutput())},
        {QStringLiteral("available_output_devices"), outputs}};
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    data.insert(QStringLiteral("pcm_status"), m_diagnosticCapture && m_diagnosticCapture->observed.load()
        ? QStringLiteral("observed") : QStringLiteral("not_observed"));
    data.insert(QStringLiteral("pcm_capture_attached"), m_diagnosticBufferOutput != nullptr
        && m_player->audioBufferOutput() == m_diagnosticBufferOutput);
#else
    data.insert(QStringLiteral("pcm_status"), QStringLiteral("api_unavailable"));
    data.insert(QStringLiteral("pcm_capture_attached"), false);
#endif
    diagnosticEvent(QStringLiteral("snapshot"), std::move(data));
}

void AudioPlayer::diagnosticBoundary(QString reason)
{
    diagnosticSnapshot(reason);
    if (m_diagnosticLog && !m_diagnosticContext.isEmpty())
        m_diagnosticLog->flushPcm(m_diagnosticContext, std::move(reason));
}

void AudioPlayer::installDiagnosticCapture()
{
    if (!m_diagnosticLog || m_diagnosticContext.isEmpty()) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    m_diagnosticCapture = std::make_shared<AudioDiagnosticCapture>();
    const auto capture = m_diagnosticCapture;
    const auto sink = m_diagnosticLog->pcmSink(m_diagnosticContext);
    m_diagnosticBufferOutput = new QAudioBufferOutput(this);
    m_diagnosticBufferConnection = connect(m_diagnosticBufferOutput, &QAudioBufferOutput::audioBufferReceived,
        m_diagnosticBufferOutput, [capture, sink](const QAudioBuffer &buffer) {
            std::lock_guard lock(capture->gate);
            if (!capture->active) return;
            if (buffer.isValid() && buffer.frameCount() > 0) capture->observed.store(true);
            sink(buffer); // Shared value copy and bounded ingress; no sample traversal or I/O.
        }, Qt::DirectConnection);
    m_player->setAudioBufferOutput(m_diagnosticBufferOutput);
#endif
}

void AudioPlayer::detachDiagnosticCapture()
{
    if (m_diagnosticCapture) {
        std::lock_guard lock(m_diagnosticCapture->gate);
        m_diagnosticCapture->active = false;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    if (m_diagnosticBufferOutput) {
        if (m_player->audioBufferOutput() == m_diagnosticBufferOutput)
            m_player->setAudioBufferOutput(nullptr);
        disconnect(m_diagnosticBufferConnection);
        delete m_diagnosticBufferOutput;
        m_diagnosticBufferOutput = nullptr;
    }
#endif
    // Retain the observation flag until the final source snapshot; the next
    // capture replaces it, and an empty context never exposes it to another source.
}

} // namespace asmr
