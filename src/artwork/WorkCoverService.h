#pragma once

#include "artwork/CoverArtLoader.h"

#include <QFuture>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QVector>

#include <functional>
#include <optional>

namespace asmr {
class WorkCoverServiceTests;
class MainWindowArtworkTests;
struct LibraryWork;

// Loads only the selected work and visible library rows. All filesystem and
// settings access happens on one worker at a time; decoded cache memory is bounded.
class WorkCoverService final : public QObject {
    Q_OBJECT
public:
    explicit WorkCoverService(const ManualCoverSettings &settings,
                              QObject *parent = nullptr);
    ~WorkCoverService() override;
    void setSettings(const ManualCoverSettings &settings);
    void setWorks(const QVector<LibraryWork> &works);
    void setDemand(qint64 selectedWorkId, const QVector<qint64> &visibleIds);
    void invalidate(qint64 workId);
    void shutdown();
    [[nodiscard]] bool isIdle() const;

signals:
    void resultReady(qint64 workId, const asmr::CoverArtResult &result);
    void shutdownFinished();

private:
    friend class WorkCoverServiceTests;
    friend class MainWindowArtworkTests;
    struct State {
        ManualCoverIdentity identity;
        quint64 revision = 0;
        bool completed = false;
        bool forceReload = false;
    };
    struct Job {
        ManualCoverIdentity identity;
        quint64 revision = 0;
        bool forceReload = false;
    };
    [[nodiscard]] std::optional<Job> nextJob() const;
    void reconcile();
    void start(Job job);
    void finishShutdownIfReady();

    ManualCoverSettings m_settings;
    QHash<qint64, State> m_works;
    QVector<qint64> m_demand;
    QSet<qint64> m_demandSet;
    qint64 m_selectedWorkId = 0;
    quint64 m_revisionSequence = 0;
    bool m_active = false;
    bool m_shuttingDown = false;
    bool m_shutdownNotified = false;
    std::optional<Job> m_running;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    std::shared_ptr<CoverArtCache> m_cache;
    QFuture<CoverArtResult> m_lifetime;
    std::shared_ptr<const std::function<void(qint64)>> m_beforeLoadTestHook;
};
} // namespace asmr
