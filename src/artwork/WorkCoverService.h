#pragma once

#include "artwork/CoverArtLoader.h"

#include <QFuture>
#include <QCache>
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

struct WorkCategoryCoverDemand {
    qint64 categoryId = 0;
    quint64 revision = 0;
    QVector<qint64> orderedWorkIds;
    QString managedCoverFile;
    bool operator==(const WorkCategoryCoverDemand &) const = default;
};

// Loads the selected work, visible work rows and ordered candidates of visible
// categories. Filesystem/settings access uses one worker; image caches are bounded.
class WorkCoverService final : public QObject {
    Q_OBJECT
public:
    explicit WorkCoverService(const ManualCoverSettings &settings,
                              QObject *parent = nullptr);
    ~WorkCoverService() override;
    void setSettings(const ManualCoverSettings &settings);
    void setWorks(const QVector<LibraryWork> &works);
    void setDemand(qint64 selectedWorkId, const QVector<qint64> &visibleIds);
    // A new revision revalidates actual candidates, including earlier failures.
    void setCategoryDemand(const QVector<WorkCategoryCoverDemand> &categories);
    void invalidate(qint64 workId);
    void shutdown();
    [[nodiscard]] bool isIdle() const;

signals:
    void resultReady(qint64 workId, const asmr::CoverArtResult &result);
    void categoryResultReady(qint64 categoryId, quint64 revision, const asmr::CoverArtResult &result);
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
        qint64 categoryId = 0;
        QString managedCoverFile;
    };
    struct CategoryState {
        WorkCategoryCoverDemand demand;
        qsizetype candidate = 0;
        bool completed = false;
        bool manualChecked = false;
        quint64 generation = 0;
    };
    [[nodiscard]] std::optional<Job> nextJob() const;
    [[nodiscard]] bool isDemanded(qint64 workId) const;
    void advanceCategories();
    void resetCategoriesForWork(qint64 workId);
    void trimResults();
    void reconcile();
    void start(Job job);
    void finishShutdownIfReady();

    ManualCoverSettings m_settings;
    QHash<qint64, State> m_works;
    QVector<qint64> m_demand;
    QSet<qint64> m_demandSet;
    QVector<CategoryState> m_categories;
    QCache<qint64, CoverArtResult> m_results{16 * 1024 * 1024};
    bool m_advancingCategories = false;
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
    std::shared_ptr<const std::function<void(qint64)>> m_beforeCategoryCoverLoadTestHook;
};
} // namespace asmr

Q_DECLARE_METATYPE(asmr::WorkCategoryCoverDemand)
