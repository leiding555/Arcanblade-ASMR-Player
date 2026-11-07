#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

class QThread;

namespace asmr {

struct TrackDurationRequest {
    qint64 workId = 0;
    qint64 fileId = 0;
    QString rootPath;
    QString relativePath;
    bool operator==(const TrackDurationRequest &) const = default;
};

struct TrackDurationResult {
    TrackDurationRequest request;
    std::optional<qint64> durationMs;
    QString error;
    bool fromCache = false;
};

class TrackDurationWorker;
class TrackDurationServiceTests;

// Owner-thread facade. Filesystem inspection and the separate, silent media
// reader run on one dedicated event-loop thread. Demand is ordered (visible
// rows first), replaces earlier demand and never starts audio playback.
class TrackDurationService final : public QObject {
    Q_OBJECT
public:
    explicit TrackDurationService(QObject *parent = nullptr);
    ~TrackDurationService() override;
    void setDemand(QVector<TrackDurationRequest> requests);
    void invalidateWork(qint64 workId);
    void shutdown();
    [[nodiscard]] bool isIdle() const { return m_idle; }

signals:
    void durationReady(const asmr::TrackDurationResult &result);
    void idleChanged(bool idle);
    void shutdownFinished();

private:
    friend class TrackDurationWorker;
    friend class TrackDurationServiceTests;
    // A deterministic asynchronous media probe for service lifecycle tests.
    // Path validation and metadata collection still use the actual worker.
    struct ProbeReply {
        std::optional<qint64> durationMs;
        QString error;
        int delayMs = 0;
        bool neverFinish = false;
    };
    using Probe = std::function<ProbeReply(const TrackDurationRequest &, const QString &)>;
    TrackDurationService(int timeoutMs, int maximumCacheEntries, Probe probe,
                         QObject *parent = nullptr);
    void updateIdle(bool idle);
    void submitDemand();

    std::shared_ptr<struct TrackDurationCancellation> m_cancellation;
    QThread *m_workerThread = nullptr;
    TrackDurationWorker *m_worker = nullptr;
    QVector<TrackDurationRequest> m_demand;
    quint64 m_generation = 0;
    bool m_idle = true;
    bool m_shuttingDown = false;
    bool m_shutdownNotified = false;
};

} // namespace asmr

Q_DECLARE_METATYPE(asmr::TrackDurationRequest)
Q_DECLARE_METATYPE(asmr::TrackDurationResult)
