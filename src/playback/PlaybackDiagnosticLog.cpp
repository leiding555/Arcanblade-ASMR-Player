#include "playback/PlaybackDiagnosticLog.h"
#include "playback/PcmDiagnosticSummary.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QLockFile>
#include <QSysInfo>
#include <QThread>
#include <QUuid>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace asmr {
namespace {
using Clock = std::chrono::steady_clock;
struct PcmIngress { bool active = true; }; // Protected by the state's mutex.
QString captureKey(const QJsonObject &context)
{
    const auto value = context.value(QStringLiteral("capture_id"));
    return value.isString() ? value.toString() : value.isDouble() ? QString::number(value.toInteger()) : QString();
}
struct Message {
    enum class Kind { Event, Pcm, Flush } kind = Kind::Event;
    QString event;
    QJsonObject data, context;
    QAudioBuffer buffer;
    qint64 elapsed = 0;
    qint64 utcMs = 0;
};
}

struct PlaybackDiagnosticState {
    explicit PlaybackDiagnosticState(PlaybackDiagnosticLogOptions value)
        : options(std::move(value)), instance(QUuid::createUuid().toString(QUuid::WithoutBraces)),
          environment{{QStringLiteral("application_version"), QCoreApplication::applicationVersion()},
              {QStringLiteral("qt_version"), QString::fromLatin1(qVersion())},
              {QStringLiteral("operating_system"), QSysInfo::prettyProductName()},
              {QStringLiteral("os_version"), QSysInfo::kernelVersion()},
              {QStringLiteral("architecture"), QSysInfo::currentCpuArchitecture()},
              {QStringLiteral("requested_backend"), qEnvironmentVariable("QT_MEDIA_BACKEND")},
              {QStringLiteral("actual_backend"), QStringLiteral("unknown")},
              {QStringLiteral("pcm_api_available"), QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)}} {}
    PlaybackDiagnosticLogOptions options;
    const QString instance;
    const QJsonObject environment;
    const Clock::time_point started = Clock::now();
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Message> queue;
    QHash<QString, std::weak_ptr<PcmIngress>> ingresses;
    qsizetype events = 0, controls = 0, pcmBuffers = 0;
    qint64 pcmBytes = 0;
    quint64 droppedEvents = 0, droppedPcm = 0;
    qint64 droppedPcmBytes = 0;
    bool coverageIncomplete = false;
    bool stopping = false, disabled = false;
    std::atomic_bool finished = false;
    qint64 elapsed() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    }
    void stamp(Message &message) const
    {
        message.elapsed = elapsed();
        message.utcMs = QDateTime::currentMSecsSinceEpoch();
    }
};

namespace {
void enqueueEvent(const std::shared_ptr<PlaybackDiagnosticState> &state, Message message)
{
    {
        const std::lock_guard lock(state->mutex);
        if (state->stopping || state->disabled) return;
        const bool terminal = message.kind == Message::Kind::Flush && message.event == QStringLiteral("source_end");
        if (terminal) {
            const auto key = captureKey(message.context);
            if (const auto ingress = state->ingresses.take(key).lock()) ingress->active = false;
        }
        // Terminal capture boundaries have a separate, bounded reservation so
        // a burst of ordinary diagnostics cannot strand a completed stream.
        if (terminal ? state->controls >= 128 : state->events >= qMax(qsizetype(0), state->options.maximumEvents)) {
            ++state->droppedEvents;
            state->coverageIncomplete = true;
        } else {
            state->stamp(message);
            state->queue.push_back(std::move(message));
            if (terminal) ++state->controls;
            else ++state->events;
        }
    }
    state->ready.notify_one();
}
struct Stream {
    QJsonObject context;
    PcmDiagnosticSummary summary;
    std::optional<qint64> playRequested, firstPcm, firstWindowPcm, lastWindowPcm;
    bool observed = false, incomplete = false;
};
}

class PlaybackDiagnosticThread final : public QThread {
public:
    using Hook = std::function<QString(int)>;
    PlaybackDiagnosticThread(std::shared_ptr<PlaybackDiagnosticState> state, Hook hook)
        : m_state(std::move(state)), m_hook(std::move(hook)) {}
protected:
    void run() override
    {
        if (!open()) { m_file.close(); m_lock.reset(); finish(); return; }
        for (;;) {
            Message message;
            bool hasMessage = false, stop = false;
            quint64 droppedEvents = 0, droppedPcm = 0;
            qint64 droppedBytes = 0;
            {
                std::unique_lock lock(m_state->mutex);
                m_state->ready.wait(lock, [this] {
                    return m_state->stopping || m_state->disabled || !m_state->queue.empty()
                        || m_state->droppedEvents || m_state->droppedPcm;
                });
                droppedEvents = std::exchange(m_state->droppedEvents, 0);
                droppedPcm = std::exchange(m_state->droppedPcm, 0);
                droppedBytes = std::exchange(m_state->droppedPcmBytes, 0);
                m_coverageIncomplete = m_state->coverageIncomplete;
                if (!m_state->queue.empty()) {
                    message = std::move(m_state->queue.front());
                    m_state->queue.pop_front();
                    if (message.kind == Message::Kind::Pcm) {
                        --m_state->pcmBuffers;
                        m_state->pcmBytes -= message.buffer.byteCount();
                    } else if (message.kind == Message::Kind::Flush && message.event == QStringLiteral("source_end"))
                        --m_state->controls;
                    else --m_state->events;
                    hasMessage = true;
                } else stop = m_state->stopping || m_state->disabled;
            }
            if (m_coverageIncomplete)
                for (auto it = m_streams.begin(); it != m_streams.end(); ++it) it.value().incomplete = true;
            if (!m_environmentWritten && (hasMessage || droppedEvents || droppedPcm)) {
                m_environmentWritten = true;
                if (!write(QStringLiteral("environment"), m_state->environment, {}, m_state->elapsed())) break;
            }
            if (droppedEvents || droppedPcm) {
                if (!write(QStringLiteral("diagnostics_dropped"), {
                        {QStringLiteral("events"), qint64(droppedEvents)},
                        {QStringLiteral("pcm_buffers"), qint64(droppedPcm)},
                        {QStringLiteral("pcm_bytes"), droppedBytes},
                        {QStringLiteral("coverage_incomplete"), true}}, {}, m_state->elapsed())) break;
            }
            if (hasMessage && !process(message)) break;
            if (stop) {
                const auto keys = m_streams.keys();
                for (const auto &key : keys)
                    if (!flush(key, QStringLiteral("shutdown"), m_state->elapsed())) break;
                break;
            }
        }
        if (m_file.isOpen()) {
            bool disabled;
            { const std::lock_guard lock(m_state->mutex); disabled = m_state->disabled; }
            if (!disabled && (!check(3) || !m_file.flush()))
                disable(QStringLiteral("Cannot flush playback diagnostics: %1").arg(m_file.errorString()));
            m_file.close();
        }
        m_lock.reset();
        finish();
    }
private:
    bool check(int point)
    {
        if (!m_hook) return true;
        const QString error = m_hook(point);
        if (error.isEmpty()) return true;
        disable(error); return false;
    }
    void disable(const QString &error)
    {
        bool first;
        {
            const std::lock_guard guard(m_state->mutex);
            first = !m_state->disabled;
            m_state->disabled = true;
            m_state->queue.clear();
            m_state->events = m_state->controls = m_state->pcmBuffers = 0;
            m_state->pcmBytes = 0;
            for (auto it = m_state->ingresses.begin(); it != m_state->ingresses.end(); ++it)
                if (const auto ingress = it.value().lock()) ingress->active = false;
            m_state->ingresses.clear();
        }
        if (first) qWarning().noquote() << QStringLiteral("Playback diagnostics disabled: %1").arg(error);
    }
    void finish()
    {
        const std::lock_guard lock(m_state->mutex);
        m_state->disabled = true;
        m_state->finished.store(true, std::memory_order_release);
    }
    bool open()
    {
        if (!check(0)) return false;
        const auto &options = m_state->options;
        if (options.directory.isEmpty() || options.maximumFileBytes <= 0 || options.backupCount < 0) {
            disable(QStringLiteral("Invalid playback diagnostic file options.")); return false;
        }
        if (!QDir().mkpath(options.directory)) {
            disable(QStringLiteral("Cannot create playback diagnostic directory: %1").arg(options.directory)); return false;
        }
        m_path = QDir(options.directory).filePath(QStringLiteral("playback.log"));
        m_lock = std::make_unique<QLockFile>(m_path + QStringLiteral(".lock"));
        m_lock->setStaleLockTime(0);
        if (!m_lock->tryLock(0)) {
            disable(QStringLiteral("Playback diagnostic directory is locked by another writer: %1").arg(options.directory));
            m_lock.reset(); return false;
        }
        m_file.setFileName(m_path);
        if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            disable(QStringLiteral("Cannot open playback diagnostics: %1").arg(m_file.errorString())); return false;
        }
        return true;
    }
    QString backup(int number) const
    {
        return QDir(m_state->options.directory).filePath(QStringLiteral("playback.%1.log").arg(number));
    }
    bool rotate()
    {
        if (!check(2)) return false;
        if (!m_file.flush()) { disable(QStringLiteral("Cannot flush playback diagnostics before rotation.")); return false; }
        m_file.close();
        const int count = m_state->options.backupCount;
        const QString oldest = count ? backup(count) : m_path;
        if (QFile::exists(oldest) && !QFile::remove(oldest)) {
            disable(QStringLiteral("Cannot remove old playback diagnostic file: %1").arg(oldest)); return false;
        }
        for (int index = count - 1; index >= 1; --index) {
            if (QFile::exists(backup(index)) && !QFile::rename(backup(index), backup(index + 1))) {
                disable(QStringLiteral("Cannot rotate playback diagnostic backup.")); return false;
            }
        }
        if (count && QFile::exists(m_path) && !QFile::rename(m_path, backup(1))) {
            disable(QStringLiteral("Cannot rotate current playback diagnostics.")); return false;
        }
        if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            disable(QStringLiteral("Cannot reopen playback diagnostics after rotation.")); return false;
        }
        return true;
    }
    bool write(const QString &event, const QJsonObject &data, const QJsonObject &context,
               qint64 elapsed, qint64 utcMs = -1)
    {
        {
            const std::lock_guard lock(m_state->mutex);
            if (m_state->disabled) return false;
        }
        if (!check(1)) return false;
        const QJsonObject object{{QStringLiteral("event"), event}, {QStringLiteral("data"), data},
            {QStringLiteral("context"), context},
            {QStringLiteral("utc"), (utcMs < 0 ? QDateTime::currentDateTimeUtc()
                : QDateTime::fromMSecsSinceEpoch(utcMs).toUTC()).toString(Qt::ISODateWithMs)},
            {QStringLiteral("monotonic_ms"), elapsed}, {QStringLiteral("sequence"), ++m_sequence},
            {QStringLiteral("instance"), m_state->instance}, {QStringLiteral("pid"), QCoreApplication::applicationPid()}};
        QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
        bytes.append('\n');
        if (bytes.size() > m_state->options.maximumFileBytes) {
            disable(QStringLiteral("A playback diagnostic record exceeds the configured file capacity.")); return false;
        }
        if (m_file.size() > m_state->options.maximumFileBytes - bytes.size() && !rotate()) return false;
        if (m_file.write(bytes) != bytes.size()) {
            disable(QStringLiteral("Cannot write playback diagnostics: %1").arg(m_file.errorString())); return false;
        }
        if (!check(3)) return false;
        if (!m_file.flush()) {
            disable(QStringLiteral("Cannot flush playback diagnostics: %1").arg(m_file.errorString())); return false;
        }
        return true;
    }
    Stream &stream(const QString &key, const QJsonObject &context)
    {
        // A lost terminal boundary must never make metadata grow for the life
        // of the application. This cap is independent of the ingress queue.
        if (!m_streams.contains(key) && m_streams.size() >= 128) {
            const auto evicted = m_streams.begin().key();
            m_streams[evicted].incomplete = true;
            flush(evicted, QStringLiteral("stream_capacity"), m_state->elapsed());
            m_streams.remove(evicted);
        }
        auto &entry = m_streams[key];
        entry.context = context;
        if (m_coverageIncomplete) entry.incomplete = true;
        return entry;
    }
    bool flush(const QString &key, const QString &reason, qint64 elapsed)
    {
        auto it = m_streams.find(key);
        if (it == m_streams.end()) return true;
        auto &entry = it.value();
        QJsonObject data = entry.summary.snapshot();
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        data.insert(QStringLiteral("pcm_status"), entry.observed ? QStringLiteral("observed") : QStringLiteral("not_observed"));
#else
        data.insert(QStringLiteral("pcm_status"), QStringLiteral("api_unavailable"));
#endif
        data.insert(QStringLiteral("reason"), reason);
        data.insert(QStringLiteral("coverage_incomplete"), entry.incomplete);
        data.insert(QStringLiteral("coverage_scope"), QStringLiteral("process"));
        data.insert(QStringLiteral("first_pcm_ms"), entry.firstPcm ? QJsonValue(*entry.firstPcm) : QJsonValue(QJsonValue::Null));
        data.insert(QStringLiteral("first_window_pcm_ms"), entry.firstWindowPcm ? QJsonValue(*entry.firstWindowPcm) : QJsonValue(QJsonValue::Null));
        data.insert(QStringLiteral("last_window_pcm_ms"), entry.lastWindowPcm ? QJsonValue(*entry.lastWindowPcm) : QJsonValue(QJsonValue::Null));
        const bool success = write(QStringLiteral("pcm_summary"), data, entry.context, elapsed);
        entry.summary.reset();
        entry.firstWindowPcm.reset(); entry.lastWindowPcm.reset();
        return success;
    }
    bool process(const Message &message)
    {
        const QString key = captureKey(message.context);
        if (message.kind == Message::Kind::Event) {
            if (!key.isEmpty() && message.event == QStringLiteral("play_requested")) {
                auto &entry = stream(key, message.context);
                if (!entry.playRequested) entry.playRequested = message.elapsed;
            }
            return write(message.event, message.data, message.context, message.elapsed, message.utcMs);
        }
        if (key.isEmpty()) return true;
        if (message.kind == Message::Kind::Flush) {
            stream(key, message.context);
            const bool success = flush(key, message.event, message.elapsed);
            if (message.event == QStringLiteral("source_end")) {
                m_streams.remove(key);
            }
            return success;
        }
        auto &entry = stream(key, message.context);
        { const std::lock_guard lock(m_state->mutex); if (m_state->disabled) return false; }
        const auto &buffer = message.buffer;
        if (buffer.byteCount() == 0) {
            entry.summary.addBuffer(buffer);
            return write(QStringLiteral("pcm_end_marker"), {}, message.context, message.elapsed, message.utcMs);
        }
        if (buffer.isValid() && buffer.frameCount() > 0) {
            if (entry.summary.hasFormat() && (entry.summary.format() != buffer.format()
                    || entry.summary.format().channelConfig() != buffer.format().channelConfig())) {
                if (!flush(key, QStringLiteral("format_changed"), message.elapsed)) return false;
            }
            if (!entry.observed) {
                entry.observed = true; entry.firstPcm = message.elapsed;
                const QJsonObject fields{{QStringLiteral("since_play_request_ms"), entry.playRequested
                    ? QJsonValue(message.elapsed - *entry.playRequested) : QJsonValue(QJsonValue::Null)}};
                if (!write(QStringLiteral("first_pcm"), fields, message.context, message.elapsed, message.utcMs)) return false;
            }
            if (!entry.firstWindowPcm) entry.firstWindowPcm = message.elapsed;
            entry.lastWindowPcm = message.elapsed;
        }
        entry.summary.addBuffer(buffer);
        return true;
    }
    std::shared_ptr<PlaybackDiagnosticState> m_state;
    Hook m_hook;
    QFile m_file;
    QString m_path;
    std::unique_ptr<QLockFile> m_lock;
    QHash<QString, Stream> m_streams;
    bool m_coverageIncomplete = false;
    qint64 m_sequence = 0;
    bool m_environmentWritten = false;
};

PlaybackDiagnosticLog::PlaybackDiagnosticLog(PlaybackDiagnosticLogOptions options, QObject *parent)
    : PlaybackDiagnosticLog(std::move(options), {}, parent) {}

PlaybackDiagnosticLog::PlaybackDiagnosticLog(PlaybackDiagnosticLogOptions options, TestHook hook, QObject *parent)
    : QObject(parent), m_state(std::make_shared<PlaybackDiagnosticState>(std::move(options)))
{
    auto translatedHook = [hook = std::move(hook)](int point) { return hook ? hook(static_cast<TestPoint>(point)) : QString(); };
    m_thread = std::make_unique<PlaybackDiagnosticThread>(m_state, std::move(translatedHook));
    connect(m_thread.get(), &QThread::finished, this, &PlaybackDiagnosticLog::shutdownFinished);
    m_thread->start();
}

PlaybackDiagnosticLog::~PlaybackDiagnosticLog()
{
    shutdown();
    m_thread->wait(); // Normal close has already asynchronously drained the worker.
}

void PlaybackDiagnosticLog::record(QString event, QJsonObject fields, QJsonObject context)
{
    Message message; message.event = std::move(event); message.data = std::move(fields); message.context = std::move(context);
    enqueueEvent(m_state, std::move(message));
}

PlaybackDiagnosticLog::PcmSink PlaybackDiagnosticLog::pcmSink(QJsonObject context)
{
    const auto state = m_state;
    const auto ingress = std::make_shared<PcmIngress>();
    {
        const std::lock_guard lock(state->mutex);
        const auto key = captureKey(context);
        ingress->active = !state->stopping && !state->disabled && !key.isEmpty();
        if (ingress->active) {
            if (const auto previous = state->ingresses.value(key).lock()) previous->active = false;
            state->ingresses.insert(key, ingress);
        }
    }
    return [state, ingress, context = std::move(context)](const QAudioBuffer &buffer) {
        {
            const std::lock_guard lock(state->mutex);
            if (!ingress->active || state->stopping || state->disabled) return;
            const qint64 bytes = qMax(qint64(0), qint64(buffer.byteCount()));
            if (state->pcmBuffers >= qMax(qsizetype(0), state->options.maximumPcmBuffers)
                || bytes > qMax(qint64(0), state->options.maximumPcmBytes) - state->pcmBytes) {
                ++state->droppedPcm; state->droppedPcmBytes += bytes;
                state->coverageIncomplete = true;
            } else {
                Message message; message.kind = Message::Kind::Pcm; message.buffer = buffer; message.context = context;
                state->stamp(message); state->queue.push_back(std::move(message));
                ++state->pcmBuffers; state->pcmBytes += bytes;
            }
        }
        state->ready.notify_one();
    };
}

void PlaybackDiagnosticLog::flushPcm(QJsonObject context, QString reason)
{
    Message message; message.kind = Message::Kind::Flush; message.event = std::move(reason); message.context = std::move(context);
    enqueueEvent(m_state, std::move(message));
}

void PlaybackDiagnosticLog::shutdown()
{
    {
        const std::lock_guard lock(m_state->mutex);
        m_state->stopping = true;
        for (auto it = m_state->ingresses.begin(); it != m_state->ingresses.end(); ++it)
            if (const auto ingress = it.value().lock()) ingress->active = false;
        m_state->ingresses.clear();
    }
    m_state->ready.notify_one();
}

bool PlaybackDiagnosticLog::isShutdownFinished() const
{
    return m_state->finished.load(std::memory_order_acquire);
}

} // namespace asmr
