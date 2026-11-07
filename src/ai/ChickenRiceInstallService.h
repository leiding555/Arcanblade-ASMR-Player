#pragma once

#include "ai/ChickenRicePackage.h"

#include <QObject>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>

QT_BEGIN_NAMESPACE
class QTimer;
template<typename T> class QFutureWatcher;
QT_END_NAMESPACE

namespace asmr {

enum class ChickenRiceInstallState {
    Idle, Preparing, Downloading, Verifying, Extracting, Ready, Cancelled, Failed
};

struct ChickenRiceInstallRequest {
    ChickenRicePackage package;
    QString installRoot;
    bool allowLoopbackHttp = false; // Tests only; production requires HTTPS.
    int networkTimeoutMs = 30000;
    int maximumAttempts = 3;
    // Accepted only with allowLoopbackHttp and loopback package URLs.
    QString extractionProgramForTests;
};

struct ChickenRiceInstallProgress {
    ChickenRiceInstallState state = ChickenRiceInstallState::Idle;
    qint64 completedBytes = 0;
    qint64 totalBytes = 0;
    QString message;
};

struct ChickenRiceInstallResult {
    ChickenRiceInstallState state = ChickenRiceInstallState::Idle;
    QString engineDirectory;
    QString error;
    QStringList warnings;
};

class ChickenRiceInstallService final : public QObject {
    Q_OBJECT
public:
    using Request = ChickenRiceInstallRequest;
    using Progress = ChickenRiceInstallProgress;
    using Result = ChickenRiceInstallResult;
    using Executor = std::function<Result(const Request &, const std::atomic_bool &,
        const std::function<void(const Progress &)> &)>;

    explicit ChickenRiceInstallService(QObject *parent = nullptr, Executor executor = {});
    ~ChickenRiceInstallService() override;
    bool start(const Request &request);
    void cancel();
    void shutdown();
    [[nodiscard]] bool isBusy() const;
    [[nodiscard]] Progress progress() const;
    [[nodiscard]] Result result() const;

signals:
    void progressChanged(asmr::ChickenRiceInstallProgress progress);
    void finished(asmr::ChickenRiceInstallResult result);
    void shutdownFinished();

private:
    struct WorkerState;
    void flushProgress();
    Executor m_executor;
    std::shared_ptr<WorkerState> m_worker;
    QFutureWatcher<Result> *m_watcher = nullptr;
    QTimer *m_progressTimer = nullptr;
    Progress m_progress;
    Result m_result;
    bool m_busy = false;
    bool m_shuttingDown = false;
    bool m_shutdownEmitted = false;
};

} // namespace asmr

Q_DECLARE_METATYPE(asmr::ChickenRiceInstallState)
Q_DECLARE_METATYPE(asmr::ChickenRiceInstallProgress)
Q_DECLARE_METATYPE(asmr::ChickenRiceInstallResult)