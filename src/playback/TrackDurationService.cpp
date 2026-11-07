#include "playback/TrackDurationService.h"
#include "playback/AudioFileValidation.h"

#include <QCache>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <utility>

namespace asmr {

struct TrackDurationCancellation {
    std::atomic<quint64> generation{0};
    std::atomic_bool stopping{false};
};

namespace {
QString identity(const TrackDurationRequest &request)
{
    // Length prefixes keep paths containing separators, '%' or digits literal.
    return QString::number(request.workId) + u':' + QString::number(request.fileId)
        + u':' + QString::number(request.rootPath.size()) + u':' + request.rootPath
        + u':' + QString::number(request.relativePath.size()) + u':' + request.relativePath;
}
QString metadataKey(const TrackDurationRequest &request, const QFileInfo &file)
{
    return identity(request) + u':' + QString::number(file.size()) + u':'
        + QString::number(file.lastModified().toMSecsSinceEpoch());
}
QString safePath(const TrackDurationRequest &request, QString &error)
{
    if (request.workId <= 0 || request.fileId <= 0) {
        error = QStringLiteral("音轨身份无效。");
        return {};
    }
    const auto validated = validateLibraryAudioFile(request.rootPath, request.relativePath);
    if (!validated.success) {
        error = validated.errorMessage;
        return {};
    }
    // The shared validator proves containment, including junction aliases.
    // Duration reads also reject all links below the work root, even links
    // which happen to resolve back inside the same work.
    const QString root = QDir::cleanPath(QDir::fromNativeSeparators(request.rootPath));
    const QFileInfo rootInfo(root);
    if (rootInfo.isSymbolicLink() || rootInfo.isJunction()) {
        error = QStringLiteral("不读取链接目录中的音轨时长。");
        return {};
    }
    QString path = root;
    const auto components = QDir::fromNativeSeparators(request.relativePath).split(u'/', Qt::SkipEmptyParts);
    for (const QString &component : components) {
        path = QDir(path).filePath(component);
        const QFileInfo information(path);
        if (information.isSymbolicLink() || information.isJunction()) {
            error = QStringLiteral("不读取链接文件或链接目录中的音轨时长。");
            return {};
        }
    }
    return validated.path;
}
} // namespace

class TrackDurationWorker final : public QObject {
    Q_OBJECT
public:
    TrackDurationWorker(int timeoutMs, int maximumCacheEntries,
                        TrackDurationService::Probe probe,
                        std::shared_ptr<TrackDurationCancellation> cancellation)
        : m_timeoutMs(qMax(1, timeoutMs)), m_cache(qMax(0, maximumCacheEntries)),
          m_probe(std::move(probe)), m_cancellation(std::move(cancellation))
    {
    }
    void setDemand(quint64 generation, QVector<TrackDurationRequest> requests,
                   std::optional<qint64> invalidatedWork = std::nullopt)
    {
        if (m_stopping) return;
        cancelActive();
        m_generation = generation;
        m_queue = std::move(requests);
        if (invalidatedWork) {
            const auto keys = m_cache.keys();
            for (const QString &key : keys) {
                const auto *cached = m_cache.object(key);
                if (cached && cached->result.request.workId == *invalidatedWork)
                    m_cache.remove(key);
            }
        }
        scheduleNext();
    }
    void stop()
    {
        if (m_stopping) return;
        m_stopping = true;
        m_queue.clear();
        cancelActive();
        m_cache.clear();
        thread()->quit();
    }

signals:
    void resultReady(quint64 generation, const asmr::TrackDurationResult &result);
    void idleChanged(quint64 generation, bool idle);

private:
    struct Cached { TrackDurationResult result; };
    bool current() const
    {
        return !m_stopping && !m_cancellation->stopping.load(std::memory_order_relaxed)
            && m_generation == m_cancellation->generation.load(std::memory_order_relaxed);
    }
    void cancelActive()
    {
        ++m_token;
        m_active = false;
        if (m_timeout) { delete m_timeout; m_timeout = nullptr; }
        if (m_player) {
            disconnect(m_player, nullptr, this, nullptr);
            // Destruction/cancellation happens only on the worker event loop.
            delete m_player;
            m_player = nullptr;
        }
    }
    void scheduleNext()
    {
        const auto generation = m_generation;
        QTimer::singleShot(0, this, [this, generation] {
            if (generation == m_generation && !m_active && current()) startNext();
        });
    }
    void startNext()
    {
        if (!current()) return;
        if (m_queue.isEmpty()) { emit idleChanged(m_generation, true); return; }
        emit idleChanged(m_generation, false);
        m_active = true;
        m_request = m_queue.takeFirst();
        m_key.clear();
        m_path.clear();
        QString error;
        const QString path = safePath(m_request, error);
        if (!current()) { m_active = false; return; }
        if (path.isEmpty()) { finish(std::nullopt, error); return; }
        const QFileInfo information(path);
        m_path = path;
        m_key = metadataKey(m_request, information);
        if (const auto *cached = m_cache.object(m_key)) {
            auto result = cached->result;
            result.fromCache = true;
            emit resultReady(m_generation, result);
            m_active = false;
            scheduleNext();
            return;
        }
        const auto token = ++m_token;
        m_timeout = new QTimer(this);
        m_timeout->setSingleShot(true);
        connect(m_timeout, &QTimer::timeout, this, [this, token] {
            if (m_active && token == m_token)
                finish(std::nullopt, QStringLiteral("音轨时长读取超时。"));
        });
        m_timeout->start(m_timeoutMs);
        if (m_probe) {
            const auto reply = m_probe(m_request, path);
            if (!reply.neverFinish) {
                QTimer::singleShot(qMax(0, reply.delayMs), this, [this, token, reply] {
                    if (m_active && token == m_token) finish(reply.durationMs, reply.error);
                });
            }
            return;
        }
        m_player = new QMediaPlayer(this);
        // No QAudioOutput and no call to play: metadata inspection cannot
        // borrow, start, seek or otherwise disturb the application's player.
        connect(m_player, &QMediaPlayer::durationChanged, this, [this, token](qint64 duration) {
            if (m_active && token == m_token && duration > 0) finish(duration, {});
        });
        connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
            [this, token](QMediaPlayer::MediaStatus status) {
                if (!m_active || token != m_token) return;
                if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia) {
                    const auto duration = m_player->duration();
                    if (duration > 0) finish(duration, {});
                    // Some backends publish duration just after LoadedMedia;
                    // keep listening until a positive value or timeout.
                } else if (status == QMediaPlayer::InvalidMedia) {
                    finish(std::nullopt, QStringLiteral("无法读取此音轨的时长。"));
                }
            });
        connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this, token](QMediaPlayer::Error, const QString &message) {
                if (m_active && token == m_token)
                    finish(std::nullopt, message.isEmpty()
                        ? QStringLiteral("无法读取此音轨的时长。") : message);
            });
        m_player->setSource(QUrl::fromLocalFile(path));
    }
    void finish(std::optional<qint64> duration, QString error)
    {
        if (!m_active) return;
        if (duration && *duration <= 0) duration.reset();
        if (!m_key.isEmpty()) {
            const QFileInfo information(m_path);
            if (!information.exists() || !information.isFile()
                || metadataKey(m_request, information) != m_key) {
                duration.reset();
                error = QStringLiteral("读取期间音轨文件已改变，请刷新目录。");
                m_key.clear();
            }
        }
        TrackDurationResult result{m_request, duration, std::move(error), false};
        // Deleting a QMediaPlayer from its own signal stack is unsafe on some
        // backends. Retire it now, destroy it on a later worker event.
        if (m_timeout) { m_timeout->stop(); m_timeout->deleteLater(); m_timeout = nullptr; }
        if (m_player) {
            disconnect(m_player, nullptr, this, nullptr);
            m_player->deleteLater();
            m_player = nullptr;
        }
        ++m_token;
        m_active = false;
        if (current()) {
            // Metadata failures are negative-cached with the same metadata
            // identity. Path/permission validation still runs on every demand.
            if (!m_key.isEmpty()) m_cache.insert(m_key, new Cached{result}, 1);
            emit resultReady(m_generation, result);
            scheduleNext();
        }
    }

    const int m_timeoutMs;
    QCache<QString, Cached> m_cache;
    TrackDurationService::Probe m_probe;
    std::shared_ptr<TrackDurationCancellation> m_cancellation;
    QVector<TrackDurationRequest> m_queue;
    TrackDurationRequest m_request;
    QString m_key;
    QString m_path;
    QMediaPlayer *m_player = nullptr;
    QTimer *m_timeout = nullptr;
    quint64 m_generation = 0;
    quint64 m_token = 0;
    bool m_active = false;
    bool m_stopping = false;
};

TrackDurationService::TrackDurationService(QObject *parent)
    : TrackDurationService(10000, 512, {}, parent)
{
}
TrackDurationService::TrackDurationService(int timeoutMs, int maximumCacheEntries,
                                         Probe probe, QObject *parent)
    : QObject(parent), m_cancellation(std::make_shared<TrackDurationCancellation>())
{
    qRegisterMetaType<TrackDurationRequest>();
    qRegisterMetaType<TrackDurationResult>();
    m_workerThread = new QThread;
    m_worker = new TrackDurationWorker(timeoutMs, maximumCacheEntries,
                                     std::move(probe), m_cancellation);
    m_worker->moveToThread(m_workerThread);
    connect(m_worker, &TrackDurationWorker::resultReady, this,
        [this](quint64 generation, const TrackDurationResult &result) {
            if (!m_shuttingDown && generation == m_generation && m_demand.contains(result.request))
                emit durationReady(result);
        });
    connect(m_worker, &TrackDurationWorker::idleChanged, this,
        [this](quint64 generation, bool idle) {
            if (!m_shuttingDown && generation == m_generation) updateIdle(idle);
        });
    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_workerThread, &QThread::finished, m_workerThread, &QObject::deleteLater);
    connect(m_workerThread, &QThread::finished, this, [this] {
        m_worker = nullptr;
        m_workerThread = nullptr;
        updateIdle(true);
        if (m_shuttingDown && !m_shutdownNotified) {
            m_shutdownNotified = true;
            emit shutdownFinished();
        }
    });
    m_workerThread->start();
}
TrackDurationService::~TrackDurationService()
{
    // Normal window close waits on shutdownFinished asynchronously. Direct
    // owner destruction also cancels safely: the self-owned worker/thread
    // finishes without a GUI-thread wait or a callback into a dead facade.
    shutdown();
}
void TrackDurationService::updateIdle(bool idle)
{
    if (m_idle == idle) return;
    m_idle = idle;
    emit idleChanged(idle);
}
void TrackDurationService::setDemand(QVector<TrackDurationRequest> requests)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_shuttingDown) return;
    QVector<TrackDurationRequest> unique;
    QSet<QString> seen;
    unique.reserve(requests.size());
    for (auto &request : requests) {
        const auto key = identity(request);
        if (seen.contains(key)) continue;
        seen.insert(key);
        unique.push_back(std::move(request));
    }
    if (unique == m_demand) return;
    m_demand = std::move(unique);
    submitDemand();
}
void TrackDurationService::submitDemand()
{
    const auto generation = ++m_generation;
    m_cancellation->generation.store(generation, std::memory_order_relaxed);
    updateIdle(false);
    auto *worker = m_worker;
    QMetaObject::invokeMethod(worker,
        [worker, generation, requests = m_demand] { worker->setDemand(generation, requests); },
        Qt::QueuedConnection);
}
void TrackDurationService::invalidateWork(qint64 workId)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_shuttingDown || workId <= 0) return;
    const auto generation = ++m_generation;
    m_cancellation->generation.store(generation, std::memory_order_relaxed);
    updateIdle(false);
    auto *worker = m_worker;
    QMetaObject::invokeMethod(worker,
        [worker, generation, requests = m_demand, workId] {
            worker->setDemand(generation, requests, workId);
        }, Qt::QueuedConnection);
}
void TrackDurationService::shutdown()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_shuttingDown) return;
    m_shuttingDown = true;
    ++m_generation;
    m_cancellation->generation.store(m_generation, std::memory_order_relaxed);
    m_cancellation->stopping.store(true, std::memory_order_relaxed);
    m_demand.clear();
    updateIdle(false);
    if (m_worker) {
        auto *worker = m_worker;
        QMetaObject::invokeMethod(worker, [worker] { worker->stop(); }, Qt::QueuedConnection);
    }
}

} // namespace asmr

#include "TrackDurationService.moc"
