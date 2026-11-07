#include "artwork/CoverArtService.h"

#include <QFutureWatcher>
#include <QMetaObject>
#include <QPointer>
#include <QPromise>
#include <QThread>
#include <QThreadPool>

#include <exception>
#include <utility>

namespace asmr {
CoverArtService::CoverArtService(QObject *parent)
    : QObject(parent), m_cache(std::make_shared<CoverArtCache>())
{
    qRegisterMetaType<CoverArtResult>();
}
CoverArtService::~CoverArtService()
{
    if (m_cancelled) m_cancelled->store(true, std::memory_order_relaxed);
    // Normal window closure drains asynchronously via shutdownFinished. This is
    // only the RAII fallback for direct destruction without that protocol.
    if (m_active) m_lifetime.waitForFinished();
}
quint64 CoverArtService::request(const CoverArtRequest &request)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_shuttingDown) return 0;
    const auto id = ++m_generation;
    if (m_active) {
        m_cancelled->store(true, std::memory_order_relaxed);
        m_pending = Pending{id, request};
    } else start({id, request});
    return id;
}
bool CoverArtService::isIdle() const { return !m_active && !m_pending; }
void CoverArtService::start(Pending pending)
{
    m_active = true;
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    auto promise = std::make_shared<QPromise<CoverArtResult>>();
    auto *watcher = new QFutureWatcher<CoverArtResult>(this);
    connect(watcher, &QFutureWatcher<CoverArtResult>::finished, this,
        [this, watcher, id = pending.id] {
            const auto loaded = watcher->result();
            watcher->deleteLater();
            m_active = false;
            m_cancelled.reset();
            QPointer<CoverArtService> guard(this);
            if (!m_shuttingDown && id == m_generation && loaded.status != CoverArtStatus::Cancelled)
                emit resultReady(id, loaded);
            if (!guard) return;
            // resultReady may synchronously request another image or shutdown.
            if (!m_active && !m_shuttingDown && m_pending) {
                auto next = std::move(*m_pending);
                m_pending.reset();
                start(std::move(next));
            }
            finishShutdownIfReady();
        });
    m_lifetime = promise->future();
    watcher->setFuture(m_lifetime);
    const auto cancelled = m_cancelled;
    const auto cache = m_cache;
    const auto hook = m_beforeLoadTestHook;
    QThreadPool::globalInstance()->start([promise, request = std::move(pending.request),
                                         cancelled, cache, hook] {
        promise->start();
        CoverArtResult loaded;
        try {
            if (hook) (*hook)();
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
void CoverArtService::shutdown()
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_shuttingDown = true;
    ++m_generation;
    m_pending.reset();
    if (m_cancelled) m_cancelled->store(true, std::memory_order_relaxed);
    QMetaObject::invokeMethod(this, &CoverArtService::finishShutdownIfReady, Qt::QueuedConnection);
}
void CoverArtService::finishShutdownIfReady()
{
    if (!m_shuttingDown || m_shutdownNotified || !isIdle()) return;
    m_shutdownNotified = true;
    emit shutdownFinished();
}
} // namespace asmr
