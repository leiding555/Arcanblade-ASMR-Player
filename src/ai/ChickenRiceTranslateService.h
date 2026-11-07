#pragma once
#include "ai/ChickenRiceGenerationOptions.h"
#include "ai/AiSubtitleTaskTypes.h"
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
using ChickenRiceTranslateState = AiSubtitleTaskState;
using ChickenRiceComputeMode = AiSubtitleComputeMode;
using ChickenRiceTranslateProgress = AiSubtitleTaskProgress;
using ChickenRiceTranslateResult = AiSubtitleTaskResult;
struct ChickenRiceTranslateRequest {
    QString engineDirectory;
    QString audioPath;
    QString outputRoot; // Internal staging/diagnostic root; published SRT is beside the audio.
    ChickenRiceComputeMode computeMode = ChickenRiceComputeMode::Gpu;
    // Explicit local process-test seam; never populated by production UI.
    QString executableForTests;
    QStringList argumentsForTests;
    QString libraryRoot;
    QString libraryRelativePath;
    ChickenRiceGenerationOptions generationOptions;
    QString outputBatchId;
    QString outputFileSuffix;
};
class ChickenRiceTranslateService final : public QObject {
    Q_OBJECT
public:
    using Request = ChickenRiceTranslateRequest;
    using Progress = ChickenRiceTranslateProgress;
    using Result = ChickenRiceTranslateResult;
    using Executor = std::function<Result(const Request &, const std::atomic_bool &,
        const std::function<void(const Progress &)> &)>;
    explicit ChickenRiceTranslateService(QObject *parent = nullptr, Executor executor = {});
    ~ChickenRiceTranslateService() override;
    bool start(const Request &request);
    void cancel();
    void shutdown();
    [[nodiscard]] bool isBusy() const;
    [[nodiscard]] Progress progress() const;
    [[nodiscard]] Result result() const;
signals:
    void progressChanged(asmr::ChickenRiceTranslateProgress progress);
    void finished(asmr::ChickenRiceTranslateResult result);
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
