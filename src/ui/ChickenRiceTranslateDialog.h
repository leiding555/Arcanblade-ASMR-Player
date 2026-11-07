#pragma once

#include "ai/ChickenRiceTranslateService.h"
#include "ai/AiTaskRepository.h"
#include "ai/FasterWhisperTranscribeService.h"
#include "ui/GpuRecommendationDialog.h"

#include <QDialog>
#include <QElapsedTimer>
#include <QPointer>
#include <QVector>

#include <array>
#include <functional>
#include <atomic>
#include <memory>
#include <optional>

class QAction;
class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QSpinBox;
class QFileDialog;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QHBoxLayout;
class QResizeEvent;
class QSettings;
class QShowEvent;
class QTimer;
class QStackedWidget;
class QTableWidget;
class QVBoxLayout;

namespace asmr {
class DialogFrame;
class FasterWhisperInstallWidget;
class SubtitleTranslationWidget;
class TranslationApiSettingsWidget;

// A persistent sequential queue; closing hides it while work continues.
// Successful batches are handed to MainWindow for library refresh/association.
// Settings and an injected service remain owned by their caller. An injected
// embedded component panels are adopted by this dialog.
class ChickenRiceTranslateDialog final : public QDialog {
    Q_OBJECT
public:
    // Empty error means that the local component files passed validation.
    using EngineValidator = std::function<QString(const QString &, const std::atomic_bool &)>;

    explicit ChickenRiceTranslateDialog(QSettings *settings, QWidget *parent = nullptr,
                                        ChickenRiceTranslateService *service = nullptr,
                                        GpuRecommendationDialog *componentPanel = nullptr,
                                        GpuRecommendationDialog::Probe probe = {},
                                        EngineValidator validator = {},
                                        FasterWhisperTranscribeService *transcribeService = nullptr,
                                        FasterWhisperInstallWidget *fasterWhisperPanel = nullptr,
                                        TranslationApiSettingsWidget *translationSettingsPanel = nullptr,
                                        QString databasePath = {});
    ~ChickenRiceTranslateDialog() override;

    void addAudioFiles(const QStringList &paths);
    void addLibraryAudioFiles(const QString &workRoot, const QStringList &relativePaths);
    // Explicit work-page action: append to the target mode's session queue.
    bool addLibraryAudioFilesForMode(const QString &workRoot, const QStringList &relativePaths,
                                    const QString &modeId);
    void setCurrentAudioPath(const QString &path);
    [[nodiscard]] bool isBusy() const;
    void shutdown();
    void showComponentSettings();
    void refreshTranslationSettings();
    bool addSubtitleForTranslation(const QString &sourcePath, const QString &audioPath,
        const QString &libraryRoot, const QString &libraryRelativePath,
        qint64 workId = 0, qint64 audioFileId = 0, qint64 subtitleFileId = 0);

public slots:
    void done(int result) override;

signals:
    void batchFinished(QVector<asmr::AiSubtitleTaskResult> successfulResults);
    void loadSubtitleRequested(const QString &sourceAudioPath, const QString &srtPath);
    void shutdownFinished();

protected:
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    struct QueueItem {
        QString audioPath;
        AiSubtitleTaskState state = AiSubtitleTaskState::Idle;
        AiSubtitleTaskResult result;
        QString libraryRoot;
        QString libraryRelativePath;
        bool interrupted = false;
        QString taskId;
        QString createdAt;
        QVector<AiTaskAttempt> attempts;
        QString outputBatchId;
        QString outputFileSuffix;
    };
    struct ModeSession {
        QVector<QueueItem> items;
        QString feedback;
        QString logTail;
        int selectedRow = -1;
        int scrollValue = 0;
    };
    void restoreSettings();
    void restoreQueueHistory();
    void scheduleQueueHistorySave();
    void persistQueueHistory();
    void restoreGenerationOptions();
    void applyGenerationOptions(const ChickenRiceGenerationOptions &options);
    [[nodiscard]] ChickenRiceGenerationOptions generationOptions() const;
    void persistGenerationOptions();
    void ensureComponentPanel();
    void ensureFasterWhisperPanel();
    void attachFasterWhisperPanel(FasterWhisperInstallWidget *panel);
    void ensureTranscribeService();
    void ensureTranslationSettingsPanel();
    void attachTranslationSettingsPanel(TranslationApiSettingsWidget *panel);
    [[nodiscard]] bool japaneseMode() const;
    [[nodiscard]] bool subtitleTranslationMode() const;
    void showSubtitleTranslation();
    void transferSelectedJapaneseSubtitle();
    void changeGenerationMode();
    [[nodiscard]] bool modeSwitchAllowed() const;
    void setModeFeedback(int mode, const QString &text);
    void attachComponentPanel(GpuRecommendationDialog *panel);
    void showGenerationPage();
    void refreshContentMinimum();
    void updateQueueButtonLayout();
    [[nodiscard]] bool translationBusy() const;
    [[nodiscard]] bool taskEditingAllowed() const;
    void discoverInstalledEngine(const QString &root);
    void persistSettings();
    void persistDraftSettings();
    void browseFolder();
    void scheduleEngineCheck();
    void startEngineCheck();
    void settleBatch();
    void updateContextActions();
    void showQueueContextMenu(const QPoint &position);
    void showSelectedError();
    void browseAudioFiles();
    void startQueue(bool retry);
    void startNext();
    void launchActiveTask();
    void cancelQueue();
    void applyProgress(const AiSubtitleTaskProgress &progress);
    void applyResult(const AiSubtitleTaskResult &result);
    void updateProgressFeedback();
    void updatePresentation();
    void updateRow(int row);
    void finishShutdownIfReady();
    void closePickers();

    QPointer<QSettings> m_settings;
    QPointer<GpuRecommendationDialog> m_componentPanel;
    QPointer<FasterWhisperInstallWidget> m_fasterWhisperPanel;
    GpuRecommendationDialog::Probe m_gpuProbe;
    EngineValidator m_validator;
    QString m_validatedEngineDirectory;
    QString m_validationError;
    quint64 m_validationGeneration = 0;
    int m_validationWorkers = 0;
    bool m_checkingEngine = false;
    std::shared_ptr<std::atomic_bool> m_validationCancelled;
    QTimer *m_validationTimer = nullptr;
    QPointer<ChickenRiceTranslateService> m_service;
    bool m_ownsService = false;
    QPointer<FasterWhisperTranscribeService> m_transcribeService;
    bool m_ownsTranscribeService = false;
    bool m_transcribeConnected = false;
    bool m_componentDetectionStarted = false;
    bool m_fasterWhisperCheckStarted = false;
    bool m_loadingSettings = true;
    bool m_preferencesDirty = false;
    struct QueueHistoryWriter;
    std::shared_ptr<QueueHistoryWriter> m_queueHistoryWriter;
    QString m_taskDatabasePath;
    std::shared_ptr<QLockFile> m_taskQueueLease;
    bool m_queueHistoryLoading = false;
    bool m_launchAfterQueueSave = false;
    QTimer *m_queueHistoryTimer = nullptr;
    QLabel *m_queueHistoryStatus = nullptr;
    int m_queueHistoryWorkers = 0;
    bool m_queueHistoryDirty = false;
    bool m_queueHistoryWritable = true;
    // Only the visible queue can run. Both offline queues retain saved history.
    std::array<ModeSession, 3> m_modeSessions;
    int m_sessionMode = 0;
    QVector<QueueItem> m_items;
    int m_activeRow = -1;
    bool m_queueRunning = false;
    bool m_batchActive = false;
    QVector<AiSubtitleTaskResult> m_batchResults;
    QString m_batchMode;
    int m_batchTotal = 0;
    int m_batchRowLimit = 0;
    int m_batchStarted = 0;
    int m_currentPercent = -1;
    QElapsedTimer m_taskElapsed;
    QTimer *m_elapsedTimer = nullptr;
    bool m_cancelRequested = false;
    bool m_shuttingDown = false;
    bool m_shutdownNotified = false;
    bool m_shutdownStarting = false;
    bool m_discovering = false;
    std::shared_ptr<std::atomic_bool> m_discoveryCancelled;
    ChickenRiceTranslateRequest m_frozenRequest;
    FasterWhisperTranscribeRequest m_frozenTranscribeRequest;
    QString m_feedback;
    QString m_logTail;
    QPointer<QDialog> m_errorDialog;
    QPointer<QFileDialog> m_folderPicker;
    QPointer<QFileDialog> m_audioPicker;
    QLineEdit *m_engineDirectory = nullptr;
    QComboBox *m_computeMode = nullptr;
    QWidget *m_generationParameters = nullptr;
    QComboBox *m_generationPreset = nullptr;
    QWidget *m_customGenerationParameters = nullptr;
    QDoubleSpinBox *m_vadThreshold = nullptr;
    QSpinBox *m_minSpeechDuration = nullptr;
    QSpinBox *m_minSilenceDuration = nullptr;
    QSpinBox *m_speechPad = nullptr;
    QCheckBox *m_mergeSegments = nullptr;
    QPushButton *m_restoreGenerationButton = nullptr;
    QLabel *m_generationPresetHint = nullptr;
    QLabel *m_generationSaveStatus = nullptr;
    QPushButton *m_browseEngineButton = nullptr;
    QPushButton *m_addAudioButton = nullptr;
    QAction *m_clearAction = nullptr;
    QPushButton *m_clearButton = nullptr;
    QAction *m_removeAction = nullptr;
    QAction *m_openFolderAction = nullptr;
    QAction *m_errorAction = nullptr;
    QAction *m_translateSubtitleAction = nullptr;
    QPushButton *m_translateSubtitleButton = nullptr;
    QPushButton *m_openResultFolderButton = nullptr;
    QHBoxLayout *m_queueButtons = nullptr;
    SubtitleTranslationWidget *m_subtitleTranslation = nullptr;
    QPointer<TranslationApiSettingsWidget> m_translationSettingsPanel;
    QPushButton *m_componentSettingsButton = nullptr;
    QComboBox *m_modeCombo = nullptr;
    QLabel *m_settingsHeading = nullptr;
    QStackedWidget *m_componentPages = nullptr;
    QWidget *m_fasterWhisperPage = nullptr;
    QVBoxLayout *m_fasterWhisperLayout = nullptr;
    QStackedWidget *m_pages = nullptr;
    QStackedWidget *m_modeConfiguration = nullptr;
    QWidget *m_generationPage = nullptr;
    QWidget *m_settingsPage = nullptr;
    QWidget *m_componentContainer = nullptr;
    QVBoxLayout *m_componentLayout = nullptr;
    QLabel *m_introduction = nullptr;
    QLabel *m_componentStatus = nullptr;
    QLabel *m_componentStatusIcon = nullptr;
    QLabel *m_settingsComponentStatus = nullptr;
    QLabel *m_settingsComponentStatusIcon = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_retryButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_activity = nullptr;
    DialogFrame *m_dialogFrame = nullptr;
};
} // namespace asmr
