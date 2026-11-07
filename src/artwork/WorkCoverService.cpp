#include "artwork/WorkCoverService.h"

#include "library/LibraryRepository.h"

#include <QFutureWatcher>
#include <QMetaObject>
#include <QPointer>
#include <QPromise>
#include <QThread>
#include <QThreadPool>

#include <exception>
#include <utility>

namespace asmr {
namespace {
bool sameIdentity(const ManualCoverIdentity &left, const ManualCoverIdentity &right)
{
    return left.workId == right.workId && left.rootPath == right.rootPath
        && left.importedAtMs == right.importedAtMs;
}
}

WorkCoverService::WorkCoverService(const ManualCoverSettings &settings, QObject *parent)
    : QObject(parent), m_settings(settings), m_cache(std::make_shared<CoverArtCache>())
{
    qRegisterMetaType<CoverArtResult>();
}

WorkCoverService::~WorkCoverService()
{
    if (m_cancelled) m_cancelled->store(true, std::memory_order_relaxed);
    // Normal window closure waits through shutdownFinished without blocking the
    // GUI. Direct destruction still owns and safely drains its worker lifetime.
    if (m_active) m_lifetime.waitForFinished();
}

void WorkCoverService::setSettings(const ManualCoverSettings &settings)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_shuttingDown || (m_settings.databasePath == settings.databasePath
        && m_settings.fileName == settings.fileName && m_settings.format == settings.format
        && m_settings.managedDirectory == settings.managedDirectory)) return;
    m_settings = settings;
    for (auto &state : m_works) {
        state.revision = ++m_revisionSequence;
        state.completed = false;
        state.forceReload = true;
    }
    reconcile();
}

void WorkCoverService::setWorks(const QVector<LibraryWork> &works)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_shuttingDown) return;
    QHash<qint64, State> next;
    next.reserve(works.size());
    for (const auto &work : works) {
        if (work.id <= 0 || work.rootPath.isEmpty() || next.contains(work.id)) continue;
        ManualCoverIdentity identity{work.id, work.rootPath, work.importedAtMs};
        const auto previous = m_works.constFind(work.id);
        if (previous != m_works.cend() && sameIdentity(previous->identity, identity))
            next.insert(work.id, *previous);
        else
            next.insert(work.id, {std::move(identity), ++m_revisionSequence, false, true});
    }
    m_works = std::move(next);
    reconcile();
}

void WorkCoverService::setDemand(qint64 selectedWorkId, const QVector<qint64> &visibleIds)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_shuttingDown) return;
    QVector<qint64> demand;
    QSet<qint64> demandSet;
    demand.reserve(visibleIds.size() + 1);
    const auto append = [&demand, &demandSet](qint64 id) {
        if (id > 0 && !demandSet.contains(id)) {
            demand.push_back(id);
            demandSet.insert(id);
        }
    };
    append(selectedWorkId);
    for (const auto id : visibleIds) append(id);
    // Completion is scoped to the current demand, including failures. A row
    // returning to view is checked against the filesystem again before a cache hit.
    for (const auto id : m_demand) {
        if (demandSet.contains(id)) continue;
        const auto found = m_works.find(id);
        if (found != m_works.end()) found->completed = false;
    }
    m_demand = std::move(demand);
    m_demandSet = std::move(demandSet);
    m_selectedWorkId = selectedWorkId;
    reconcile();
}

void WorkCoverService::invalidate(qint64 workId)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_shuttingDown) return;
    const auto found = m_works.find(workId);
    if (found == m_works.end()) return;
    found->revision = ++m_revisionSequence;
    found->completed = false;
    found->forceReload = true;
    reconcile();
}

std::optional<WorkCoverService::Job> WorkCoverService::nextJob() const
{
    if (m_shuttingDown) return std::nullopt;
    for (const auto id : m_demand) {
        const auto found = m_works.constFind(id);
        if (found != m_works.cend() && !found->completed)
            return Job{found->identity, found->revision, found->forceReload};
    }
    return std::nullopt;
}

bool WorkCoverService::isIdle() const
{
    return !m_active && !nextJob();
}

void WorkCoverService::reconcile()
{
    if (m_active) {
        const auto id = m_running->identity.workId;
        const auto state = m_works.constFind(id);
        const auto selected = m_works.constFind(m_selectedWorkId);
        const bool selectedWaiting = m_selectedWorkId != id
            && m_demandSet.contains(m_selectedWorkId) && selected != m_works.cend()
            && !selected->completed;
        if (m_shuttingDown || !m_demandSet.contains(id) || state == m_works.cend()
            || state->revision != m_running->revision || selectedWaiting)
            m_cancelled->store(true, std::memory_order_relaxed);
        return;
    }
    if (auto job = nextJob()) start(std::move(*job));
}

void WorkCoverService::start(Job job)
{
    m_active = true;
    m_running = job;
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = m_cancelled;
    const auto cache = m_cache;
    const auto hook = m_beforeLoadTestHook;
    CoverArtRequest request;
    request.rootPath = job.identity.rootPath;
    request.libraryWork = true;
    request.forceReload = job.forceReload;
    request.manualLookup = ManualCoverLookup{m_settings, job.identity};
    auto promise = std::make_shared<QPromise<CoverArtResult>>();
    auto *watcher = new QFutureWatcher<CoverArtResult>(this);
    connect(watcher, &QFutureWatcher<CoverArtResult>::finished, this,
        [this, watcher, job, cancelled] {
            const auto loaded = watcher->result();
            watcher->deleteLater();
            m_active = false;
            m_running.reset();
            m_cancelled.reset();
            auto found = m_works.find(job.identity.workId);
            const bool accepted = !m_shuttingDown
                && !cancelled->load(std::memory_order_relaxed)
                && loaded.status != CoverArtStatus::Cancelled
                && m_demandSet.contains(job.identity.workId)
                && found != m_works.end() && found->revision == job.revision
                && sameIdentity(found->identity, job.identity);
            QPointer<WorkCoverService> guard(this);
            if (accepted) {
                found->completed = true;
                if (loaded.status == CoverArtStatus::Ready) found->forceReload = false;
                emit resultReady(job.identity.workId, loaded);
            }
            if (!guard) return;
            // A result handler can synchronously change demand, invalidate a
            // work, begin another job, shut down, or destroy this service.
            if (!m_active) reconcile();
            finishShutdownIfReady();
        });
    m_lifetime = promise->future();
    watcher->setFuture(m_lifetime);
    QThreadPool::globalInstance()->start(
        [promise, request = std::move(request), cancelled, cache, hook, id = job.identity.workId] {
            promise->start();
            CoverArtResult loaded;
            try {
                if (hook) (*hook)(id);
                loaded = CoverArtLoader::load(request, {}, cancelled, cache.get());
            } catch (const std::exception &) {
                loaded = {CoverArtStatus::Error, {}, {}, QStringLiteral("封面读取失败。"), false};
            } catch (...) {
                loaded = {CoverArtStatus::Error, {}, {}, QStringLiteral("封面读取失败。"), false};
            }
            promise->addResult(std::move(loaded));
            promise->finish();
        });
}

void WorkCoverService::shutdown()
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_shuttingDown = true;
    m_demand.clear();
    m_demandSet.clear();
    if (m_cancelled) m_cancelled->store(true, std::memory_order_relaxed);
    QMetaObject::invokeMethod(this, &WorkCoverService::finishShutdownIfReady, Qt::QueuedConnection);
}

void WorkCoverService::finishShutdownIfReady()
{
    if (!m_shuttingDown || m_shutdownNotified || !isIdle()) return;
    m_shutdownNotified = true;
    emit shutdownFinished();
}
} // namespace asmr
