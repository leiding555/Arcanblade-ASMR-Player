#pragma once
#include "ai/ApiKeyStore.h"
#include "ai/SubtitleTranslationDocument.h"
#include <QObject>
#include <QJsonArray>
#include <QPointer>
#include <atomic>
#include <functional>
#include <memory>
template<class T> class QFutureWatcher;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class QLockFile;
namespace asmr {
struct SubtitleTranslationProgress {
    AiSubtitleTaskState state = AiSubtitleTaskState::Preparing;
    QString message;
    qsizetype completedEntries = 0;
    qsizetype totalEntries = 0;
    int batchIndex = 0;
    int batchCount = 0;
    int requestCount = 0;
    qint64 promptTokens = -1;
    qint64 completionTokens = -1;
    qint64 peakContextTokens = -1;
    qint64 contextTokenLimit = -1;
    bool usageComplete = true;
    SubtitleTranslationPhase phase = SubtitleTranslationPhase::Translation;
    qsizetype phaseCompletedEntries = 0;
    qsizetype phaseTotalEntries = 0;
};
struct SubtitleTranslationResult {
    AiSubtitleTaskResult task;
    int requestCount = 0;
    qint64 promptTokens = -1;
    qint64 completionTokens = -1;
    // Usage is scoped to this run. The peak is a single response's input + output,
    // never the sum of repeated conversation histories across requests.
    qint64 peakContextTokens = -1;
    qint64 contextTokenLimit = -1;
    bool usageComplete = true;
    // Shared service/credential failures stop the enclosing queue, rather than
    // repeating a known failing paid request for every remaining subtitle.
    bool stopQueue = false;
    QString contextGroup, correctionFingerprint, translationFingerprint;
};
// Preparation is local only; the UI starts it after an explicit user action.
// Retries retain verified translations and rebuild the DeepSeek conversation.
class SubtitleTranslationService final : public QObject {
    Q_OBJECT
public:
    explicit SubtitleTranslationService(QObject *parent = nullptr,
        std::shared_ptr<ApiKeyStore> keyStore = {}, QNetworkAccessManager *manager = nullptr);
    // An injected manager must be unparented and on this thread; ownership is transferred.
    ~SubtitleTranslationService() override;
    bool prepare(const SubtitleTranslationRequest &request);
    bool startPrepared();
    void cancel();
    [[nodiscard]] bool isBusy() const { return m_busy; }
    [[nodiscard]] bool hasPreparedPlan() const { return m_prepared; }
    [[nodiscard]] SubtitleTranslationPlan preparedPlan() const {
        return m_plan.request.useWorkContext && m_correctionPlan.succeeded() ? m_correctionPlan : m_plan;
    }
    [[nodiscard]] SubtitleTranslationResult result() const { return m_result; }
signals:
    void busyChanged(bool busy);
    void prepared(asmr::SubtitleTranslationPlan plan);
    void progress(asmr::SubtitleTranslationProgress progress);
    void finished(asmr::SubtitleTranslationResult result);
private:
    struct Secret;
    enum class WorkStage { Prepare, Start, AdvancePhase, BeginBatch, Save, ToolRequest, ToolTurn, Publish, Finish };
    struct WorkResult {
        SubtitleTranslationPlan plan;
        SubtitleTranslations translations;
        SubtitleTranslations corrections;
        std::shared_ptr<Secret> key;
        std::shared_ptr<QLockFile> lease;
        AiSubtitleTaskResult task;
        SubtitleTranslationResult result;
        QString error;
        QJsonArray messages;
        QByteArray requestBody;
        bool sourceRead = false;
        bool contextRead = false;
        bool progressAvailable = false;
        bool madeProgress = false;
        qint64 promptTokens = -1;
        qint64 completionTokens = -1;
        bool invalidatedSource = false;
        bool checkpointReady = false;
        bool stopQueue = false;
    };
    void runWorker(WorkStage stage, std::function<WorkResult()> operation);
    void workerFinished(WorkStage stage, WorkResult value, quint64 generation);
    void sendNext();
    void advanceContextPhase();
    void postCurrentBatch();
    void buildToolRequest();
    void processToolResponse(const QByteArray &response);
    void postRequest(const QByteArray &body);
    void readAvailable(QNetworkReply *reply, quint64 generation);
    void replyFinished(QNetworkReply *reply, quint64 generation);
    void complete(AiSubtitleTaskState state, const QString &error = {});
    void complete(SubtitleTranslationResult result);
    void notifyCompleted(SubtitleTranslationResult result);
    void emitProgress(AiSubtitleTaskState state, const QString &message);
    void recordUsage(qint64 promptTokens, qint64 completionTokens);
    void clearReply();
    QNetworkAccessManager *m_manager = nullptr;
    QTimer *m_timeout = nullptr;
    QPointer<QNetworkReply> m_reply;
    QFutureWatcher<WorkResult> *m_worker = nullptr;
    std::shared_ptr<ApiKeyStore> m_keyStore;
    std::shared_ptr<Secret> m_key;
    std::shared_ptr<QLockFile> m_checkpointLease;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    SubtitleTranslationPlan m_plan;
    SubtitleTranslationPlan m_correctionPlan;
    SubtitleTranslations m_translations;
    SubtitleTranslations m_corrections;
    SubtitleTranslationResult m_result;
    QByteArray m_response;
    QJsonArray m_messages;
    bool m_sourceRead = false;
    bool m_contextRead = false;
    int m_stalledTurns = 0;
    qsizetype m_maximumResponseBytes = 128 * 1024;
    quint64 m_generation = 0;
    int m_currentBatch = -1;
    int m_requestCount = 0;
    qint64 m_promptTokens = -1;
    qint64 m_completionTokens = -1;
    qint64 m_peakContextTokens = -1;
    int m_usageResponseCount = 0;
    bool m_busy = false;
    bool m_prepared = false;
    bool m_completing = false;
    bool m_checkpointReady = false;
    bool m_stopQueue = false;
};
} // namespace asmr
Q_DECLARE_METATYPE(asmr::SubtitleTranslationProgress)
Q_DECLARE_METATYPE(asmr::SubtitleTranslationResult)
