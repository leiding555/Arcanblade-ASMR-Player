#pragma once
#include "library/LibraryRepository.h"
#include <QFuture>
#include <QHash>
#include <QObject>
#include <QQueue>
#include <functional>
#include <memory>
#include <optional>

namespace asmr {
enum class PlaybackProgressTaskKind { Query, Save, Clear };
struct PlaybackProgressTaskResult : LibraryOperationResult {
    PlaybackProgressTaskKind kind = PlaybackProgressTaskKind::Query;
    quint64 requestId = 0;
    qint64 workId = 0;
    qint64 audioFileId = 0;
    quint64 session = 0;
    std::optional<LibraryPlaybackProgress> progress;
    qint64 revision = 0;
    bool conflict = false;
    bool discarded = false; // Not attempted after an earlier CAS conflict.
};

// One owner thread/window; all accepted reads and writes share a single FIFO.
// Every worker owns its repository and returns only values, never SQL handles.
// Callers check stable IDs and session/request generations before using results.
class PlaybackProgressCoordinator final : public QObject {
    Q_OBJECT
public:
    explicit PlaybackProgressCoordinator(QString databasePath, QObject *parent = nullptr);
    ~PlaybackProgressCoordinator() override;
    quint64 query(qint64 workId, qint64 audioFileId, quint64 session);
    quint64 save(qint64 workId, qint64 audioFileId, quint64 session, qint64 positionMs,
                 std::optional<qint64> durationMs = std::nullopt);
    quint64 clear(qint64 workId, qint64 audioFileId, quint64 session);
    // Reject new requests with ID zero, drain accepted work, signal once idle.
    void shutdown();
    bool isIdle() const;
    bool isShuttingDown() const;
signals:
    void completed(asmr::PlaybackProgressTaskResult result);
    void shutdownFinished();
private:
    friend class PlaybackProgressCoordinatorTests;
    friend class PlaybackProgressCoordinatorTestAccess;
    enum class TestPoint { BeforeTask, AfterTask };
    using TestHook = std::function<void(TestPoint, PlaybackProgressTaskKind, quint64)>;
    struct Request {
        PlaybackProgressTaskKind kind = PlaybackProgressTaskKind::Query;
        quint64 requestId = 0;
        qint64 workId = 0;
        qint64 audioFileId = 0;
        quint64 session = 0;
        qint64 positionMs = 0;
        std::optional<qint64> durationMs;
    };
    quint64 enqueue(Request request);
    void startNext();
    void finish(PlaybackProgressTaskResult result);
    void finishShutdownIfIdle();
    void drainOnDestruction();
    static PlaybackProgressTaskResult perform(const QString &databasePath,
        const Request &request, std::optional<qint64> expectedRevision,
        const std::shared_ptr<const TestHook> &hook);
    QString m_databasePath;
    QQueue<Request> m_queue;
    QHash<qint64, qint64> m_revisions;
    QFuture<PlaybackProgressTaskResult> m_lifetime;
    std::shared_ptr<const TestHook> m_testHook;
    quint64 m_nextRequestId = 0;
    bool m_running = false;
    bool m_delivering = false;
    bool m_shuttingDown = false;
    bool m_shutdownSignalled = false;
};
} // namespace asmr
Q_DECLARE_METATYPE(asmr::PlaybackProgressTaskResult)
