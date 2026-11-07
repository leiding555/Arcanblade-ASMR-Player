#pragma once
#include "ai/AiSubtitleTaskTypes.h"
#include "ai/AiTaskRepository.h"
#include "ai/SubtitleTranslationDocument.h"
#include <QPointer>
#include <QVector>
#include <QWidget>
class QAction;
class QDialog;
class QFileDialog;
class QLabel;
class QCheckBox;
class QProgressBar;
class QPushButton;
class QSettings;
class QShowEvent;
class QTableWidget;
namespace asmr {
class SubtitleTranslationService;
struct SubtitleTranslationResult;
// A persistent sequential queue. Each subtitle has its own conversation and
// execution record; one explicit run freezes settings and its input rows.
class SubtitleTranslationWidget final : public QWidget {
    Q_OBJECT
public:
    explicit SubtitleTranslationWidget(QSettings *settings, QWidget *parent = nullptr,
                                       SubtitleTranslationService *service = nullptr,
                                       const QString &databasePath = {});
    ~SubtitleTranslationWidget() override;
    // True when appended or already present; false when the queue cannot accept it.
    bool addSubtitleFile(const QString &sourcePath, const QString &audioPath = {},
                         const QString &libraryRoot = {}, const QString &libraryRelativePath = {},
                         qint64 workId = 0, qint64 audioFileId = 0, qint64 subtitleFileId = 0);
    void refreshWorkContextAvailability();
    [[nodiscard]] bool isBusy() const;
    void refreshSettings();
    [[nodiscard]] bool hasOpenDialog() const;
    // Hide transient windows; an explicitly started translation continues.
    void closeTransientDialogs();
    void shutdown();
    [[nodiscard]] bool shutdownComplete() const { return m_shutdownEmitted; }
signals:
    void settingsRequested();
    void busyChanged(bool busy);
    void completed(asmr::AiSubtitleTaskResult result);
    void batchCompleted(QVector<asmr::AiSubtitleTaskResult> successfulResults);
    void shutdownFinished();
protected:
    void showEvent(QShowEvent *event) override;
private:
    struct Item {
        QString id, createdAt, updatedAt, state = QStringLiteral("pending");
        QString sourcePath, audioPath, libraryRoot, libraryRelativePath, translationTaskId;
        QString outputBatchId, outputFileSuffix;
        QString status, error, resultPath, taskDirectory;
        QString correctedJapanesePath, contextGroup, correctionFingerprint, translationFingerprint, referenceDigest;
        qint64 workId = 0, audioFileId = 0, subtitleFileId = 0;
        bool useWorkContext = false;
        SubtitleWorkContextKind contextKind = SubtitleWorkContextKind::ReferenceFiles;
        SubtitleWorkContextKind availableContextKind = SubtitleWorkContextKind::ReferenceFiles;
        bool contextEligible = false;
        bool contextChecked = false;
        QString contextError;
        QStringList contextWarnings;
        QStringList warnings;
        bool retryable = false;
        bool succeeded = false;
        bool resultMissing = false;
    };
    struct HistoryResult { AiTaskSnapshot snapshot; QString error; std::shared_ptr<QLockFile> lease; };
    [[nodiscard]] TranslationApiSettings currentSettings() const;
    void restoreHistory();
    void scheduleHistorySave();
    void persistHistory();
    [[nodiscard]] AiTaskSnapshot historySnapshot() const;
    void runHistoryOperation(bool loading, const AiTaskSnapshot &snapshot = {});
    void historyOperationFinished(bool loading, HistoryResult result);
    void beginPersistedTranslation();
    void persistPreparedTranslation(const SubtitleTranslationPlan &plan);
    void failUnstartedTranslation(const QString &error);
    void finishAttempt(const SubtitleTranslationResult &value);
    void browseSubtitles();
    void startTranslation(bool retry = false);
    void checkPublishedResults(bool startAfterCheck);
    void launchPublishedResultCheck();
    void beginCheckedTranslation();
    void beginPreparedBatch();
    void checkWorkContexts(bool startAfterCheck = false);
    void startNextTranslation();
    void advanceBatchWhenReady();
    void settleBatch();
    void cancelTranslation();
    void updateControls();
    void setStatus(const QString &message);
    void updateRow(int row);
    void showError();
    void openResultFolder();
    void clearList();
    void finishShutdownIfReady();
    [[nodiscard]] int selectedRow() const;
    QPointer<QSettings> m_settings;
    QPointer<SubtitleTranslationService> m_service;
    QString m_databasePath;
    AiTaskSnapshot m_history;
    std::shared_ptr<QLockFile> m_historyLease;
    bool m_historyLoading = true;
    bool m_historyWorker = false;
    bool m_historyDirty = false;
    bool m_historyWritable = true;
    int m_startPending = -1;
    QString m_currentAttempt;
    QVector<Item> m_items;
    int m_activeRow = -1;
    bool m_preparing = false;
    bool m_checkingResults = false;
    bool m_resultCheckExclusive = false;
    bool m_startAfterResultCheck = false;
    bool m_resultCheckWaiting = false;
    bool m_batchRunning = false;
    bool m_retryBatch = false;
    bool m_cancelRequested = false;
    bool m_advancePending = false;
    QStringList m_batchItems;
    int m_batchCursor = 0;
    int m_batchTotal = 0;
    int m_batchStarted = 0;
    QVector<AiSubtitleTaskResult> m_batchResults;
    bool m_shuttingDown = false;
    bool m_shutdownEmitted = false;
    bool m_batchUseWorkContext = false;
    bool m_contextChecking = false;
    bool m_contextRefreshPending = false;
    bool m_contextStartAfterCheck = false;
    quint64 m_contextGeneration = 0;
    std::shared_ptr<std::atomic_bool> m_contextCancelled;
    TranslationApiSettings m_startSettings;
    QPointer<QFileDialog> m_picker;
    QPointer<QDialog> m_errorDialog;
    QLabel *m_historyStatus = nullptr;
    QLabel *m_serviceLabel = nullptr;
    QCheckBox *m_thinking = nullptr;
    QCheckBox *m_useWorkContext = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_configure = nullptr;
    QPushButton *m_add = nullptr;
    QPushButton *m_clear = nullptr;
    QPushButton *m_start = nullptr;
    QPushButton *m_retry = nullptr;
    QPushButton *m_cancel = nullptr;
    QPushButton *m_openFolder = nullptr;
    QTableWidget *m_table = nullptr;
    QProgressBar *m_progress = nullptr;
    QAction *m_removeAction = nullptr;
    QAction *m_folderAction = nullptr;
    QAction *m_errorAction = nullptr;
};
} // namespace asmr
