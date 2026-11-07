#pragma once
#include "ai/FasterWhisperGenerationOptions.h"
#include "ai/FasterWhisperPackage.h"
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
using FasterWhisperTranscribeState = AiSubtitleTaskState;
using FasterWhisperTranscribeProgress = AiSubtitleTaskProgress;
using FasterWhisperTranscribeResult = AiSubtitleTaskResult;
struct FasterWhisperTranscribeRequest {
    QString engineDirectory;
    QString modelDirectory; // Verified CTranslate2 model leaf; no network download during inference.
    QString audioPath;
    QString outputRoot; // Internal staging/diagnostic root; published SRT is beside the audio.
    AiSubtitleComputeMode computeMode = AiSubtitleComputeMode::Gpu;
    // Explicit local process-test seam; never populated by production UI.
    QString executableForTests;
    QStringList argumentsForTests;
    // Optional bounded model fixture; accepted only with executableForTests.
    QList<FasterWhisperDownloadFile> modelFilesForTests;
    QString libraryRoot;
    QString libraryRelativePath;
    FasterWhisperGenerationOptions generationOptions;
    QString outputBatchId;
    QString outputFileSuffix;
};
class FasterWhisperTranscribeService final : public QObject {
    Q_OBJECT
public:
    using Request = FasterWhisperTranscribeRequest;
    using Progress = FasterWhisperTranscribeProgress;
    using Result = FasterWhisperTranscribeResult;
    using Executor = std::function<Result(const Request &, const std::atomic_bool &,
        const std::function<void(const Progress &)> &)>;
    explicit FasterWhisperTranscribeService(QObject *parent = nullptr, Executor executor = {});
    ~FasterWhisperTranscribeService() override;
    bool start(const Request &request);
    void cancel();
    void shutdown();
    [[nodiscard]] bool isBusy() const;
    [[nodiscard]] Progress progress() const;
    [[nodiscard]] Result result() const;
signals:
    void progressChanged(asmr::FasterWhisperTranscribeProgress progress);
    void finished(asmr::FasterWhisperTranscribeResult result);
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
