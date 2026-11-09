#include "artwork/WorkCoverService.h"

#include "artwork/ManagedCoverFile.h"
#include "library/LibraryRepository.h"

#include <QFileInfo>
#include <QFutureWatcher>
#include <QMetaObject>
#include <QPointer>
#include <QPromise>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <exception>
#include <limits>
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
    m_results.clear();
    for (auto &state : m_works) {
        state.revision = ++m_revisionSequence;
        state.completed = false;
        state.forceReload = true;
    }
    for (auto &category : m_categories) {
        category.candidate = 0;
        category.completed = false;
        category.manualChecked = false;
        category.generation = ++m_revisionSequence;
    }
    reconcile();
}

void WorkCoverService::setWorks(const QVector<LibraryWork> &works)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_shuttingDown) return;
    QHash<qint64, State> next;
    QSet<qint64> changed;
    next.reserve(works.size());
    for (const auto &work : works) {
        if (work.id <= 0 || work.rootPath.isEmpty() || next.contains(work.id)) continue;
        ManualCoverIdentity identity{work.id, work.rootPath, work.importedAtMs};
        const auto previous = m_works.constFind(work.id);
        if (previous != m_works.cend() && sameIdentity(previous->identity, identity))
            next.insert(work.id, *previous);
        else {
            next.insert(work.id, {std::move(identity), ++m_revisionSequence, false, true});
            changed.insert(work.id);
        }
    }
    for (auto it = m_works.cbegin(); it != m_works.cend(); ++it)
        if (!next.contains(it.key())) changed.insert(it.key());
    m_works = std::move(next);
    for (const auto id : changed) {
        m_results.remove(id);
        resetCategoriesForWork(id);
    }
    trimResults();
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
    QVector<QPair<Job, CoverArtResult>> replays;
    for (const auto id : demand) {
        if (m_demandSet.contains(id)) continue;
        auto found = m_works.find(id);
        if (found == m_works.end()) continue;
        // A category may already have loaded this same identity. Deliver its
        // result to a newly visible ordinary row without repeating the worker.
        if (found->completed && m_results.contains(id))
            replays.append({{found->identity, found->revision, found->forceReload}, *m_results.object(id)});
        else
            found->completed = false;
    }
    m_demand = std::move(demand);
    m_demandSet = std::move(demandSet);
    m_selectedWorkId = selectedWorkId;
    trimResults();
    QPointer<WorkCoverService> guard(this);
    for (const auto &replay : replays) {
        const auto found = m_works.constFind(replay.first.identity.workId);
        if (found != m_works.cend() && found->revision == replay.first.revision
            && m_demandSet.contains(replay.first.identity.workId))
            emit resultReady(replay.first.identity.workId, replay.second);
        if (!guard) return;
    }
    reconcile();
}

void WorkCoverService::setCategoryDemand(const QVector<WorkCategoryCoverDemand> &categories)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_shuttingDown) return;
    QVector<CategoryState> next;
    QSet<qint64> seen;
    QSet<qint64> recheck;
    for (const auto &demand : categories) {
        if (demand.categoryId <= 0 || seen.contains(demand.categoryId)) continue;
        seen.insert(demand.categoryId);
        const auto old = std::find_if(m_categories.cbegin(), m_categories.cend(),
            [&](const auto &state) { return state.demand == demand; });
        if (old != m_categories.cend()) next.append(*old);
        else {
            next.append({demand, 0, false, false, ++m_revisionSequence});
            m_results.remove(-demand.categoryId);
            for (const auto id : demand.orderedWorkIds) recheck.insert(id);
        }
    }
    m_categories = std::move(next);
    // Entering/revisiting a category checks actual files before trusting a
    // decoded-image cache, just as an ordinary row returning to view does.
    for (const auto id : recheck) {
        const auto found = m_works.find(id);
        if (found != m_works.end()) found->completed = false;
        m_results.remove(id);
        resetCategoriesForWork(id);
    }
    trimResults();
    reconcile();
}

void WorkCoverService::resetCategoriesForWork(qint64 workId)
{
    for (auto &category : m_categories) {
        if (!category.demand.orderedWorkIds.contains(workId)) continue;
        category.candidate = 0;
        category.completed = false;
        category.manualChecked = false;
        category.generation = ++m_revisionSequence;
        m_results.remove(-category.demand.categoryId);
    }
}

void WorkCoverService::trimResults()
{
    QSet<qint64> wanted = m_demandSet;
    for (const auto &category : m_categories) {
        if (!category.demand.managedCoverFile.isEmpty()) wanted.insert(-category.demand.categoryId);
        for (const auto id : category.demand.orderedWorkIds) wanted.insert(id);
    }
    for (const auto id : m_results.keys())
        if (!wanted.contains(id)) m_results.remove(id);
    for (auto it = m_works.begin(); it != m_works.end(); ++it)
        if (!wanted.contains(it.key())) it->completed = false;
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
    m_results.remove(workId);
    resetCategoriesForWork(workId);
    reconcile();
}

bool WorkCoverService::isDemanded(qint64 workId) const
{
    if (m_demandSet.contains(workId)) return true;
    return std::any_of(m_categories.cbegin(), m_categories.cend(), [&](const auto &category) {
        return !category.completed && category.candidate < category.demand.orderedWorkIds.size()
            && (category.demand.managedCoverFile.isEmpty() || category.manualChecked)
            && category.demand.orderedWorkIds[category.candidate] == workId;
    });
}

std::optional<WorkCoverService::Job> WorkCoverService::nextJob() const
{
    if (m_shuttingDown) return std::nullopt;
    for (const auto id : m_demand) {
        const auto found = m_works.constFind(id);
        if (found != m_works.cend() && !found->completed)
            return Job{found->identity, found->revision, found->forceReload};
    }
    for (const auto &category : m_categories) {
        if (!category.completed && !category.manualChecked && !category.demand.managedCoverFile.isEmpty())
            return Job{{}, category.generation, false, category.demand.categoryId, category.demand.managedCoverFile};
        if (category.completed || category.candidate >= category.demand.orderedWorkIds.size()) continue;
        const auto id = category.demand.orderedWorkIds[category.candidate];
        const auto found = m_works.constFind(id);
        if (found != m_works.cend() && (!found->completed || !m_results.contains(id)))
            return Job{found->identity, found->revision, found->forceReload};
    }
    return std::nullopt;
}

void WorkCoverService::advanceCategories()
{
    if (m_advancingCategories || m_shuttingDown) return;
    m_advancingCategories = true;
    struct Reply { qint64 id; quint64 revision; quint64 generation; CoverArtResult result; };
    QVector<Reply> replies;
    for (auto &category : m_categories) {
        if (category.completed) continue;
        const auto reply = [&](CoverArtResult result) {
            if (!result.manual) {
                result.manualFallback = !category.demand.managedCoverFile.isEmpty();
                result.manualRelativePath = category.demand.managedCoverFile;
                if (result.manualFallback) {
                    const auto message = QStringLiteral("分类手动封面暂不可用，已保留选择并回退自动封面。");
                    result.message = result.message.isEmpty() ? message : message + u'\n' + result.message;
                }
            }
            replies.append({category.demand.categoryId, category.demand.revision, category.generation, std::move(result)});
        };
        if (!category.demand.managedCoverFile.isEmpty()) {
            if (!category.manualChecked) continue;
            const auto *manual = m_results.object(-category.demand.categoryId);
            if (manual && manual->status == CoverArtStatus::Ready && !manual->image.isNull()) {
                category.completed = true;
                reply(*manual);
                continue;
            }
        }
        while (category.candidate < category.demand.orderedWorkIds.size()) {
            const auto id = category.demand.orderedWorkIds[category.candidate];
            const auto work = m_works.constFind(id);
            if (work == m_works.cend()) { ++category.candidate; continue; }
            const auto *result = m_results.object(id);
            if (!work->completed || !result) break;
            if (result->status == CoverArtStatus::Ready && !result->image.isNull()) {
                category.completed = true;
                auto automatic = *result;
                automatic.manual = false;
                reply(std::move(automatic));
                break;
            }
            ++category.candidate;
        }
        if (!category.completed && category.candidate >= category.demand.orderedWorkIds.size()) {
            category.completed = true;
            reply({CoverArtStatus::NotFound, {}, {}, QStringLiteral("分类暂无封面。"), false});
        }
    }
    m_advancingCategories = false;
    QPointer<WorkCoverService> guard(this);
    for (const auto &reply : replies) {
        // A previous receiver may synchronously replace this demand or remove
        // the category. Never publish a reply under a new category revision.
        const auto current = std::find_if(m_categories.cbegin(), m_categories.cend(), [&](const auto &category) {
            return category.demand.categoryId == reply.id && category.demand.revision == reply.revision
                && category.generation == reply.generation
                && category.completed;
        });
        if (!m_shuttingDown && current != m_categories.cend())
            emit categoryResultReady(reply.id, reply.revision, reply.result);
        if (!guard) return;
    }
}

bool WorkCoverService::isIdle() const
{
    return !m_active && !nextJob()
        && std::all_of(m_categories.cbegin(), m_categories.cend(),
            [](const auto &category) { return category.completed; });
}

void WorkCoverService::reconcile()
{
    QPointer<WorkCoverService> guard(this);
    advanceCategories();
    if (!guard) return;
    if (m_active) {
        if (m_running->categoryId > 0) {
            const auto current = std::find_if(m_categories.cbegin(), m_categories.cend(), [&](const auto &category) {
                return category.demand.categoryId == m_running->categoryId
                    && category.generation == m_running->revision
                    && category.demand.managedCoverFile == m_running->managedCoverFile
                    && !category.completed && !category.manualChecked;
            });
            const bool ordinaryWaiting = std::any_of(m_demand.cbegin(), m_demand.cend(), [&](qint64 id) {
                const auto work = m_works.constFind(id);
                return work != m_works.cend() && !work->completed;
            });
            if (m_shuttingDown || current == m_categories.cend() || ordinaryWaiting)
                m_cancelled->store(true, std::memory_order_relaxed);
            return;
        }
        const auto id = m_running->identity.workId;
        const auto state = m_works.constFind(id);
        const auto selected = m_works.constFind(m_selectedWorkId);
        const bool selectedWaiting = m_selectedWorkId != id
            && m_demandSet.contains(m_selectedWorkId) && selected != m_works.cend()
            && !selected->completed;
        if (m_shuttingDown || !isDemanded(id) || state == m_works.cend()
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
    const auto categoryHook = m_beforeCategoryCoverLoadTestHook;
    const auto settings = m_settings;
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
            auto category = std::find_if(m_categories.begin(), m_categories.end(), [&](const auto &state) {
                return state.demand.categoryId == job.categoryId && state.generation == job.revision
                    && state.demand.managedCoverFile == job.managedCoverFile
                    && !state.completed && !state.manualChecked;
            });
            const bool current = job.categoryId > 0 ? category != m_categories.end()
                : isDemanded(job.identity.workId) && found != m_works.end()
                    && found->revision == job.revision && sameIdentity(found->identity, job.identity);
            const bool accepted = !m_shuttingDown
                && !cancelled->load(std::memory_order_relaxed)
                && loaded.status != CoverArtStatus::Cancelled
                && current;
            QPointer<WorkCoverService> guard(this);
            if (accepted) {
                if (job.categoryId > 0) category->manualChecked = true;
                else {
                    found->completed = true;
                    if (loaded.status == CoverArtStatus::Ready) found->forceReload = false;
                }
                const auto bytes = qint64(sizeof(CoverArtResult)) + loaded.image.sizeInBytes()
                    + 2 * (loaded.sourcePath.size() + loaded.message.size() + loaded.manualRelativePath.size());
                m_results.insert(job.categoryId > 0 ? -job.categoryId : job.identity.workId, new CoverArtResult(loaded),
                    int(std::min<qint64>(bytes, std::numeric_limits<int>::max())));
                if (job.categoryId == 0 && m_demandSet.contains(job.identity.workId))
                    emit resultReady(job.identity.workId, loaded);
            }
            if (!guard) return;
            // A result handler can synchronously change demand, invalidate a
            // work, begin another job, shut down, or destroy this service.
            if (!m_active) reconcile();
            if (!guard) return;
            finishShutdownIfReady();
        });
    m_lifetime = promise->future();
    watcher->setFuture(m_lifetime);
    QThreadPool::globalInstance()->start(
        [promise, request = std::move(request), cancelled, cache, hook, categoryHook, job, settings] {
            promise->start();
            CoverArtResult loaded;
            try {
                if (job.categoryId > 0) {
                    if (categoryHook) (*categoryHook)(job.categoryId);
                    if (cancelled->load(std::memory_order_relaxed)) loaded.status = CoverArtStatus::Cancelled;
                    else {
                        const auto path = ManagedCoverFile::categoryPath(settings, job.categoryId, job.managedCoverFile);
                        if (path.isEmpty()) loaded = {CoverArtStatus::Error, {}, {}, QStringLiteral("分类封面副本不存在或不可读取。"), false};
                        else {
                            // Saved tokens identify immutable private copies;
                            // the loader still rechecks the actual file before
                            // reusing its bounded decoded-image cache.
                            CoverArtRequest selectedRequest;
                            selectedRequest.rootPath = QFileInfo(path).absolutePath();
                            selectedRequest.libraryWork = true;
                            selectedRequest.manualRelativePath = QFileInfo(path).fileName();
                            selectedRequest.manualOnly = true;
                            loaded = CoverArtLoader::load(selectedRequest, {}, cancelled, cache.get());
                        }
                        loaded.manual = loaded.status == CoverArtStatus::Ready;
                        loaded.manualRelativePath = job.managedCoverFile;
                    }
                } else {
                    if (hook) (*hook)(job.identity.workId);
                    loaded = CoverArtLoader::load(request, {}, cancelled, cache.get());
                }
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
    m_categories.clear();
    m_results.clear();
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
