#include "playback/AudioPlayer.h"
#include "playback/DirectSoundOutputTest.h"
#include "playback/PlaybackDiagnosticLog.h"

#include <QAudioBuffer>
#include <QAudioDevice>
#include <QAudioOutput>
#include <QBuffer>
#include <QDataStream>
#include <QDateTime>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QMediaDevices>
#include <QMediaMetaData>
#include <QMetaEnum>
#include <QPromise>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QAudioBufferOutput>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <numbers>
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

constexpr int outputTestSampleRate = 24000;
constexpr int outputTestWaveHeaderBytes = 44;

QByteArray outputTestWave(float gain = 1.0f)
{
    constexpr quint32 rate = outputTestSampleRate;
    constexpr quint32 toneFrames = rate / 4;
    constexpr quint32 secondToneStart = rate / 2;
    constexpr quint32 fadeFrames = rate / 50;
    constexpr quint32 frameCount = rate;
    constexpr quint32 dataSize = frameCount * 4;
    QByteArray wave;
    wave.reserve(outputTestWaveHeaderBytes + dataSize);
    QDataStream out(&wave, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("RIFF", 4);
    out << quint32(36 + dataSize);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(2) << rate << quint32(rate * 4)
        << quint16(4) << quint16(16);
    out.writeRawData("data", 4);
    out << dataSize;
    const auto sample = [gain](quint32 frame) -> qint16 {
        if (frame >= toneFrames) return 0;
        const auto fade = std::min({frame, toneFrames - 1 - frame, fadeFrames});
        const auto phase = 2.0 * std::numbers::pi * 440.0 * frame / rate;
        return qint16(9600.0 * gain * std::sin(phase) * fade / fadeFrames);
    };
    for (quint32 frame = 0; frame < frameCount; ++frame) {
        const qint16 right = frame >= secondToneStart ? sample(frame - secondToneStart) : 0;
        out << sample(frame) << right;
    }
    return wave;
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
    m_outputAvailable = !m_audioOutput->device().isNull();

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
    connect(m_player, &QMediaPlayer::playbackStateChanged,
            this, &AudioPlayer::outputTestAvailabilityChanged);
    connect(m_audioOutput, &QAudioOutput::volumeChanged, this, [this](float volume) {
        if (volume <= 0.0f) stopOutputTest();
        emit outputTestAvailabilityChanged();
    });
    connect(m_audioOutput, &QAudioOutput::mutedChanged, this, [this](bool muted) {
        if (muted) stopOutputTest();
        emit outputTestAvailabilityChanged();
    });
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

    auto *devices = new QMediaDevices(this);
    connect(devices, &QMediaDevices::audioOutputsChanged, this, [this] {
        const auto reason = QStringLiteral("audio_outputs_changed");
        synchronizeOutput(reason);
        diagnosticSnapshot(reason);
        emit outputConfigurationChanged();
    });
}

AudioPlayer::~AudioPlayer()
{
    if (m_directSoundCancel) m_directSoundCancel->store(true);
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
    stopOutputTest();
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
    stopOutputTest();
    synchronizeOutput(QStringLiteral("before_play"));
    if (!m_preferredOutputId.isEmpty() && !m_outputAvailable) {
        diagnosticEvent(QStringLiteral("play_blocked"),
            {{QStringLiteral("reason"), QStringLiteral("preferred_output_unavailable")}});
        return;
    }
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
    stopOutputTest();
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

void AudioPlayer::setPreferredOutput(QByteArray id, QString name)
{
    if (id.isEmpty()) name.clear();
    if (id == m_preferredOutputId && name == m_preferredOutputName) return;
    m_preferredOutputId = std::move(id);
    m_preferredOutputName = std::move(name);
    synchronizeOutput(QStringLiteral("preference_changed"), true);
}

QString AudioPlayer::currentOutputName() const
{
    if (!m_outputAvailable || !m_player->audioOutput()) return {};
    return m_audioOutput->device().description();
}

QJsonObject AudioPlayer::outputDiagnostic() const
{
    const auto *output = m_player->audioOutput();
    return {{QStringLiteral("output_mode"), m_preferredOutputId.isEmpty()
                ? QStringLiteral("system_default") : QStringLiteral("manual")},
            {QStringLiteral("preferred_output_id_hex"), QString::fromLatin1(m_preferredOutputId.toHex())},
            {QStringLiteral("preferred_output_name"), m_preferredOutputName},
            {QStringLiteral("default_output_device"), audioDevice(QMediaDevices::defaultAudioOutput())},
            {QStringLiteral("output_device"), audioDevice(output && m_outputAvailable ? output->device() : QAudioDevice{})},
            {QStringLiteral("output_available"), m_outputAvailable},
            {QStringLiteral("volume"), double(m_audioOutput->volume())},
            {QStringLiteral("muted"), m_audioOutput->isMuted()},
            {QStringLiteral("playback_state"), enumName(playbackState())},
            {QStringLiteral("last_output_change"), m_lastOutputChange},
            {QStringLiteral("last_directsound_test"), m_lastDirectSoundTest},
            {QStringLiteral("qt_audio_capture"), m_outputLogCapture.snapshot()}};
}

void AudioPlayer::synchronizeOutput(const QString &reason, bool preferenceChanged)
{
    const bool followsDefault = m_preferredOutputId.isEmpty();
    const auto defaultDevice = QMediaDevices::defaultAudioOutput();
    QAudioDevice target = defaultDevice;
    if (!followsDefault) {
        const auto devices = QMediaDevices::audioOutputs();
        const auto match = std::find_if(devices.cbegin(), devices.cend(), [this](const auto &device) {
            return device.id() == m_preferredOutputId;
        });
        target = match == devices.cend() ? QAudioDevice{} : *match;
    }
    const bool wasAttached = m_player->audioOutput() == m_audioOutput;
    const auto previousDevice = wasAttached ? m_audioOutput->device() : QAudioDevice{};
    const bool available = !target.isNull();
    const bool changed = available != m_outputAvailable
        || (available && (!wasAttached || previousDevice.id() != target.id()))
        || (!available && !followsDefault && wasAttached);
    if (!changed && !preferenceChanged) return;

    stopOutputTest();
    m_outputAvailable = available;
    if (available) {
        if (m_audioOutput->device().id() != target.id()) m_audioOutput->setDevice(target);
        if (!wasAttached) m_player->setAudioOutput(m_audioOutput);
    } else if (!followsDefault) {
        if (playbackState() == QMediaPlayer::PlayingState) m_player->pause();
        if (wasAttached) m_player->setAudioOutput(nullptr);
    } else if (!wasAttached) {
        // Default mode keeps the normal Qt behavior even before devices appear.
        m_audioOutput->setDevice(QAudioDevice{});
        m_player->setAudioOutput(m_audioOutput);
    }
    const auto current = available ? m_audioOutput->device() : QAudioDevice{};
    m_lastOutputChange = {
        {QStringLiteral("utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("reason"), reason},
        {QStringLiteral("result"), available ? QStringLiteral("selected") : QStringLiteral("unavailable")},
        {QStringLiteral("previous_output_device"), audioDevice(previousDevice)},
        {QStringLiteral("default_output_device"), audioDevice(defaultDevice)},
        {QStringLiteral("output_device"), audioDevice(current)}};
    diagnosticEvent(followsDefault ? QStringLiteral("default_output_synchronized")
                                  : QStringLiteral("manual_output_synchronized"), m_lastOutputChange);
    if (reason != QStringLiteral("audio_outputs_changed")) emit outputConfigurationChanged();
}

bool AudioPlayer::canTestOutput() const
{
    return m_outputAvailable && m_player->audioOutput() == m_audioOutput
        && playbackState() != QMediaPlayer::PlayingState
        && !m_audioOutput->isMuted() && m_audioOutput->volume() > 0.0f;
}

void AudioPlayer::startOutputTest()
{
    synchronizeOutput(QStringLiteral("before_output_test"));
    if (!canTestOutput() || outputTestRunning()) return;

    auto *player = new QMediaPlayer(this);
    player->setObjectName(QStringLiteral("outputTestPlayer"));
    auto *output = new QAudioOutput(m_audioOutput->device(), player);
    output->setVolume(std::min(m_audioOutput->volume(), 0.25f));
    player->setAudioOutput(output);
    auto *wave = new QBuffer(player);
    wave->setData(outputTestWave());
    wave->open(QIODevice::ReadOnly);
    m_outputTestPlayer = player;
    connect(player, &QMediaPlayer::mediaStatusChanged, player, [this, player](auto status) {
        if (player == m_outputTestPlayer && status == QMediaPlayer::EndOfMedia)
            finishOutputTest(QStringLiteral("测试播放结束，请以实际听到的声音为准。"));
    }, Qt::QueuedConnection);
    connect(player, &QMediaPlayer::errorOccurred, player, [this, player](auto error, const QString &message) {
        if (player == m_outputTestPlayer && error != QMediaPlayer::NoError)
            finishOutputTest(QStringLiteral("测试声音失败：%1").arg(message));
    }, Qt::QueuedConnection);
    QTimer::singleShot(5000, player, [this, player] {
        if (player == m_outputTestPlayer) finishOutputTest(QStringLiteral("测试声音超时。"));
    });
    player->setSourceDevice(wave, QUrl(QStringLiteral("output-test.wav")));
    player->play();
    emit outputTestAvailabilityChanged();
}

void AudioPlayer::stopOutputTest()
{
    if (m_directSoundCancel) m_directSoundCancel->store(true);
    finishOutputTest({});
}

void AudioPlayer::startDirectSoundTest(quintptr window)
{
    synchronizeOutput(QStringLiteral("before_directsound_test"));
    if (!canTestOutput() || outputTestRunning()) return;
    const auto device = m_audioOutput->device();
    const float gain = std::min(m_audioOutput->volume(), 0.25f);
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    m_directSoundCancel = cancelled;
    m_lastDirectSoundTest = {{QStringLiteral("started_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                             {QStringLiteral("device"), audioDevice(device)},
                             {QStringLiteral("stage"), QStringLiteral("running")}};
    auto promise = std::make_shared<QPromise<DirectSoundTestResult>>();
    auto *watcher = new QFutureWatcher<DirectSoundTestResult>(this);
    connect(watcher, &QFutureWatcher<DirectSoundTestResult>::finished, this, [this, watcher, cancelled] {
        const auto result = watcher->result();
        watcher->deleteLater();
        m_directSoundCancel.reset();
        const auto stage = cancelled->load() ? QStringLiteral("cancelled") : result.stage;
        m_lastDirectSoundTest.insert(QStringLiteral("finished_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        m_lastDirectSoundTest.insert(QStringLiteral("stage"), stage);
        const auto code = QStringLiteral("0x%1").arg(result.status, 8, 16, QChar('0'));
        m_lastDirectSoundTest.insert(QStringLiteral("hresult"), code);
        if (stage == QStringLiteral("cancelled"))
            emit outputTestFinished({});
        else if (result.status)
            emit outputTestFinished(QStringLiteral("DS 测试失败（%1，%2）。").arg(stage, code));
        else
            emit outputTestFinished(QStringLiteral("DS 测试播放结束，请以实际听到的声音为准。"));
        emit outputTestAvailabilityChanged();
    });
    watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([promise, cancelled, id = device.id(), window, gain] {
        promise->start();
        const auto pcm = outputTestWave(gain).mid(outputTestWaveHeaderBytes);
        promise->addResult(testDirectSoundOutput(id, window, pcm, outputTestSampleRate, *cancelled));
        promise->finish();
    });
    emit outputTestAvailabilityChanged();
}

void AudioPlayer::finishOutputTest(const QString &status)
{
    auto *player = std::exchange(m_outputTestPlayer, nullptr);
    if (!player) return;
    player->stop();
    player->deleteLater();
    emit outputTestFinished(status);
    emit outputTestAvailabilityChanged();
}

void AudioPlayer::setDiagnosticLog(PlaybackDiagnosticLog *log)
{
    if (m_diagnosticLog == log) return;
    endDiagnosticSource(QStringLiteral("diagnostic_log_changed"));
    m_diagnosticLog = log;
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
