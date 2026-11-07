#include "playback/PlaybackProgressCoordinator.h"
#include <QDebug>
#include <QFutureWatcher>
#include <QPromise>
#include <QPointer>
#include <QStringList>
#include <QThread>
#include <QThreadPool>
#include <algorithm>
#include <exception>
#include <limits>

namespace asmr {
namespace {
bool stale(const LibraryOperationResult &result)
{
    return std::any_of(result.diagnostics.cbegin(), result.diagnostics.cend(), [](const auto &item) {
        return item.code == LibraryDiagnosticCode::StalePlaybackProgress;
    });
}
void reportFallbackFailure(const PlaybackProgressTaskResult &result)
{
    if (result.success || result.kind == PlaybackProgressTaskKind::Query) return;
    QStringList messages;
    for (const auto &diagnostic : result.diagnostics) messages.append(diagnostic.message);
    qWarning().noquote() << QStringLiteral("Playback progress shutdown fallback failed: workId=%1 audioFileId=%2 session=%3 requestId=%4; %5")
        .arg(result.workId).arg(result.audioFileId).arg(result.session).arg(result.requestId)
        .arg(messages.join(QStringLiteral("; ")));
}
}
PlaybackProgressCoordinator::PlaybackProgressCoordinator(QString databasePath, QObject *parent)
    : QObject(parent), m_databasePath(std::move(databasePath))
{
    qRegisterMetaType<PlaybackProgressTaskResult>();
}
PlaybackProgressCoordinator::~PlaybackProgressCoordinator() { drainOnDestruction(); }
quint64 PlaybackProgressCoordinator::query(qint64 workId, qint64 audioFileId, quint64 session)
{
    return enqueue({PlaybackProgressTaskKind::Query, 0, workId, audioFileId, session, 0, {}});
}
quint64 PlaybackProgressCoordinator::save(qint64 workId, qint64 audioFileId, quint64 session,
                                         qint64 positionMs, std::optional<qint64> durationMs)
{
    return enqueue({PlaybackProgressTaskKind::Save, 0, workId, audioFileId, session, positionMs, durationMs});
}
quint64 PlaybackProgressCoordinator::clear(qint64 workId, qint64 audioFileId, quint64 session)
{
    return enqueue({PlaybackProgressTaskKind::Clear, 0, workId, audioFileId, session, 0, {}});
}
quint64 PlaybackProgressCoordinator::enqueue(Request request)
{
    if (QThread::currentThread() != thread() || m_shuttingDown
        || m_nextRequestId == std::numeric_limits<quint64>::max())
        return 0;
    request.requestId = ++m_nextRequestId;
    m_queue.enqueue(request);
    startNext();
    return request.requestId;
}
bool PlaybackProgressCoordinator::isIdle() const
{
    return !m_running && !m_delivering && m_queue.isEmpty();
}
bool PlaybackProgressCoordinator::isShuttingDown() const { return m_shuttingDown; }
void PlaybackProgressCoordinator::shutdown()
{
    if (QThread::currentThread() != thread()) return;
    m_shuttingDown = true;
    startNext();
    finishShutdownIfIdle();
}
void PlaybackProgressCoordinator::finishShutdownIfIdle()
{
    if (m_shuttingDown && isIdle() && !m_shutdownSignalled) {
        m_shutdownSignalled = true;
        emit shutdownFinished();
    }
}
void PlaybackProgressCoordinator::startNext()
{
    if (m_running || m_delivering || m_queue.isEmpty()) return;
    m_running = true;
    const auto request = m_queue.dequeue();
    const auto path = m_databasePath;
    const auto hook = m_testHook;
    const auto found = m_revisions.constFind(request.audioFileId);
    const auto revision = found == m_revisions.cend() ? std::nullopt : std::optional<qint64>(*found);
    auto promise = std::make_shared<QPromise<PlaybackProgressTaskResult>>();
    auto *watcher = new QFutureWatcher<PlaybackProgressTaskResult>(this);
    connect(watcher, &QFutureWatcher<PlaybackProgressTaskResult>::finished, this, [this, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        finish(result);
    });
    promise->start();
    m_lifetime = promise->future();
    watcher->setFuture(m_lifetime);
    QThreadPool::globalInstance()->start([promise, path, request, revision, hook] {
        promise->addResult(perform(path, request, revision, hook));
        promise->finish();
    });
}
PlaybackProgressTaskResult PlaybackProgressCoordinator::perform(const QString &path,
    const Request &request, std::optional<qint64> expectedRevision,
    const std::shared_ptr<const TestHook> &hook)
{
    PlaybackProgressTaskResult result;
    result.kind = request.kind;
    result.requestId = request.requestId;
    result.workId = request.workId;
    result.audioFileId = request.audioFileId;
    result.session = request.session;
    try {
        if (hook) (*hook)(TestPoint::BeforeTask, request.kind, request.requestId);
        {
            LibraryRepository repository(path);
            const auto opened = repository.open();
            if (!opened.success) {
                result.diagnostics = opened.diagnostics;
            } else {
                // Missing historical audio remains a valid final-save identity.
                const auto files = repository.files(request.workId);
                const bool belongs = files.success && request.workId > 0 && request.audioFileId > 0
                    && std::any_of(files.files.cbegin(), files.files.cend(), [&request](const auto &file) {
                        return file.id == request.audioFileId && file.workId == request.workId
                            && file.type == LibraryFileType::Audio;
                    });
                if (!files.success) {
                    result.diagnostics = files.diagnostics;
                } else if (!belongs) {
                    result.diagnostics.push_back({LibraryDiagnosticCode::InvalidPlaybackProgress, path,
                        QStringLiteral("Playback progress requires an audio ID belonging to the requested work.")});
                } else {
                    const bool query = request.kind == PlaybackProgressTaskKind::Query;
                    if (query || !expectedRevision) {
                        const auto current = repository.playbackProgress(request.audioFileId);
                        result.success = current.success;
                        result.diagnostics = current.diagnostics;
                        result.progress = current.progress;
                        result.revision = current.revision;
                        if (current.success) expectedRevision = current.revision;
                    }
                    if (!query && expectedRevision) {
                        const auto written = request.kind == PlaybackProgressTaskKind::Save
                            ? repository.savePlaybackProgress(request.audioFileId, request.positionMs,
                                                              request.durationMs, *expectedRevision)
                            : repository.clearPlaybackProgress(request.audioFileId, *expectedRevision);
                        result.success = written.success;
                        result.diagnostics = written.diagnostics;
                        result.progress.reset();
                        result.revision = written.revision;
                        result.conflict = stale(written);
                        if (result.conflict) {
                            // Observe the winner; never retry the old payload.
                            const auto current = repository.playbackProgress(request.audioFileId);
                            result.diagnostics += current.diagnostics;
                            result.progress = current.progress;
                            result.revision = current.success ? current.revision : -1;
                        }
                    }
                }
            }
            const auto closed = repository.close();
            if (!closed.success) {
                result.diagnostics += closed.diagnostics;
                // A committed write remains committed despite cleanup errors.
                if (request.kind == PlaybackProgressTaskKind::Query) result.success = false;
            }
        } // SQL objects/connections die before future completion.
        if (hook) (*hook)(TestPoint::AfterTask, request.kind, request.requestId);
    } catch (const std::exception &error) {
        result.success = false;
        result.diagnostics.push_back({LibraryDiagnosticCode::DatabaseError, path,
            QStringLiteral("Unexpected playback progress task failure: ") + QString::fromUtf8(error.what())});
    } catch (...) {
        result.success = false;
        result.diagnostics.push_back({LibraryDiagnosticCode::DatabaseError, path,
            QStringLiteral("Unexpected playback progress task failure.")});
    }
    return result;
}
void PlaybackProgressCoordinator::finish(PlaybackProgressTaskResult result)
{
    m_running = false;
    m_delivering = true;
    if (result.success || (result.conflict && result.revision >= 0))
        m_revisions.insert(result.audioFileId, result.revision);
    else if (result.conflict)
        m_revisions.remove(result.audioFileId);
    QVector<PlaybackProgressTaskResult> discarded;
    if (result.conflict) {
        QQueue<Request> keep;
        while (!m_queue.isEmpty()) {
            const auto request = m_queue.dequeue();
            if (request.audioFileId != result.audioFileId || request.kind == PlaybackProgressTaskKind::Query) {
                keep.enqueue(request);
                continue;
            }
            PlaybackProgressTaskResult dropped;
            dropped.kind = request.kind;
            dropped.requestId = request.requestId;
            dropped.workId = request.workId;
            dropped.audioFileId = request.audioFileId;
            dropped.session = request.session;
            dropped.revision = result.revision;
            dropped.conflict = true;
            dropped.discarded = true;
            dropped.diagnostics.push_back({LibraryDiagnosticCode::StalePlaybackProgress, m_databasePath,
                QStringLiteral("An earlier progress CAS conflict invalidated this queued write; it was not attempted.")});
            discarded.push_back(std::move(dropped));
        }
        m_queue = std::move(keep);
    }
    // Invalidate old queue before signals: callback-enqueued intents are new.
    const QPointer<PlaybackProgressCoordinator> guard(this);
    emit completed(result);
    if (!guard) return;
    for (const auto &dropped : discarded) {
        emit completed(dropped);
        if (!guard) return;
    }
    m_delivering = false;
    startNext();
    finishShutdownIfIdle();
}
void PlaybackProgressCoordinator::drainOnDestruction()
{
    m_shuttingDown = true;
    if (m_running) {
        m_lifetime.waitForFinished();
        const auto result = m_lifetime.result();
        reportFallbackFailure(result);
        if (result.success || (result.conflict && result.revision >= 0))
            m_revisions.insert(result.audioFileId, result.revision);
        else if (result.conflict)
            m_revisions.remove(result.audioFileId);
        if (result.conflict)
            m_queue.removeIf([&result](const auto &request) {
                return request.audioFileId == result.audioFileId && request.kind != PlaybackProgressTaskKind::Query;
            });
    }
    if (m_queue.isEmpty()) return;
    const auto path = m_databasePath;
    const auto hook = m_testHook;
    auto promise = std::make_shared<QPromise<void>>();
    promise->start();
    auto lifetime = promise->future();
    // Fallback destruction can wait; normal close uses asynchronous shutdown.
    // Fallback SQL still runs on a worker, never on the owner/UI thread.
    QThreadPool::globalInstance()->start([promise, path, hook,
            queue = std::move(m_queue), revisions = std::move(m_revisions)]() mutable {
        while (!queue.isEmpty()) {
            const auto request = queue.dequeue();
            const auto found = revisions.constFind(request.audioFileId);
            const auto expected = found == revisions.cend() ? std::nullopt : std::optional<qint64>(*found);
            const auto result = perform(path, request, expected, hook);
            reportFallbackFailure(result);
            if (result.success || (result.conflict && result.revision >= 0))
                revisions.insert(result.audioFileId, result.revision);
            else if (result.conflict)
                revisions.remove(result.audioFileId);
            if (result.conflict)
                queue.removeIf([&result](const auto &pending) {
                    return pending.audioFileId == result.audioFileId && pending.kind != PlaybackProgressTaskKind::Query;
                });
        }
        promise->finish();
    });
    lifetime.waitForFinished();
}
} // namespace asmr
