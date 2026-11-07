#pragma once

#include "ai/FasterWhisperPackage.h"

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

enum class FasterWhisperInstallState {
    Idle, Preparing, Downloading, Verifying, Extracting, Ready, Cancelled, Failed
};

struct FasterWhisperInstallRequest {
    FasterWhisperPackage package;
    QString installRoot;
    QString existingEngineDirectory; // Explicit existing components are validated read-only.
    QString existingModelDirectory; // Leaf folder containing model.bin and configuration.
    bool allowLoopbackHttp = false; // Test fixture URLs only; production requires HTTPS.
    int networkTimeoutMs = 30000;
    int maximumAttempts = 3;
    QString extractionProgramForTests; // Requires an entirely loopback test catalog.
};

struct FasterWhisperInstallProgress {
    FasterWhisperInstallState state = FasterWhisperInstallState::Idle;
    qint64 completedBytes = 0;
    qint64 totalBytes = 0;
    QString message;
    QString component; // "engine" or "model"; separate byte totals for each component.
};

struct FasterWhisperInstallResult {
    FasterWhisperInstallState state = FasterWhisperInstallState::Idle;
    QString engineDirectory;
    QString modelDirectory;
    QString error;
    QStringList warnings;
};

class FasterWhisperInstallService final : public QObject {
    Q_OBJECT
public:
    using Request = FasterWhisperInstallRequest;
    using Progress = FasterWhisperInstallProgress;
    using Result = FasterWhisperInstallResult;
    using Executor = std::function<Result(const Request &, const std::atomic_bool &,
        const std::function<void(const Progress &)> &)>;

    explicit FasterWhisperInstallService(QObject *parent = nullptr, Executor executor = {});
    ~FasterWhisperInstallService() override;
    bool start(const Request &request);
    void cancel();
    void shutdown();
    [[nodiscard]] bool isBusy() const;
    [[nodiscard]] Progress progress() const;
    [[nodiscard]] Result result() const;

signals:
    void progressChanged(asmr::FasterWhisperInstallProgress progress);
    void finished(asmr::FasterWhisperInstallResult result);
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

Q_DECLARE_METATYPE(asmr::FasterWhisperInstallState)
Q_DECLARE_METATYPE(asmr::FasterWhisperInstallProgress)
Q_DECLARE_METATYPE(asmr::FasterWhisperInstallResult)
