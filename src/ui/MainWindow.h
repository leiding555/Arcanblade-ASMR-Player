#pragma once

#include "subtitle/SubtitleSynchronizer.h"
#include "library/LibraryImportService.h"
#include "library/LibraryBatchImportService.h"
#include "library/LibraryRootRefreshService.h"
#include "library/ScanRootStore.h"
#include "playback/LibrarySubtitleResolver.h"
#include "playback/PlaybackQueue.h"
#include "artwork/CoverArtLoader.h"
#include "artwork/ManagedCoverFile.h"
#include <QMediaPlayer>

#include <QSettings>
#include <QElapsedTimer>
#include <QUrl>
#include <QFuture>
#include <QQueue>
#include <QSet>
#include <QMap>
#include <QPointer>
#include <QWidget>

#include <memory>
#include <atomic>
#include <functional>

class QCloseEvent;
class QResizeEvent;
class QSplitter;
class QStackedWidget;
class QVBoxLayout;
class QPushButton;
class QAction;
class QLabel;
class QMessageBox;
class QFileDialog;
class QSystemTrayIcon;

namespace asmr {

class AudioPlayer;
class FloatingSubtitleWindow;
class FloatingSubtitleAppearanceDialog;
class PlaybackControlsWidget;
class PlaybackDetailWidget;
class PlaybackDetailTransition;
class SubtitlePanel;
class LibraryPanel;
class BoundSubtitleDialog;
class ChickenRiceTranslateDialog;
struct UpdateDownloadResult;
struct GpuDetectionResult;
struct AiSubtitleTaskResult;
class BatchImportPreviewDialog;
class ScanRootStore;
class ScanRootManagerDialog;
class PlaybackProgressCoordinator;
class PlaybackQueuePanel;
class TitleBarWidget;
class WindowFrameController;
class CoverArtService;
class WorkCoverService;
class TrackDurationService;
class CoverSelectionDialog;
class WorkEditDialog;
class WorkReferenceDialog;
class UpdateNotificationController;
struct PlaybackProgressTaskResult;
struct AudioFileValidationResult;
struct SubtitleFileLoadResult;
struct FloatingSubtitleAppearance;

class MainWindow final : public QWidget
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    explicit MainWindow(const QString &libraryDatabasePath, QWidget *parent = nullptr);
    ~MainWindow() override;
    void startAutomaticUpdateCheck();

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    friend class MainWindowSubtitleTests;
    friend class MainWindowLibraryTests;
    friend class MainWindowBatchImportTests;
    friend class MainWindowDirectoryBrowserTests;
    friend class MainWindowScanRootsTests;
    friend class MainWindowGpuRecommendationTests;
    friend class MainWindowChickenRiceTests;
    friend class MainWindowAutoSubtitleTests;
    friend class MainWindowSubtitlePreferenceTests;
    friend class MainWindowPlaybackProgressTests;
    friend class MainWindowQueueTests;
    friend class MainWindowTitleBarTests;
    friend class MainWindowArtworkTests;
    friend class MainWindowUpdateTests;

    [[nodiscard]] QString updateRestartBlockedReason() const;
    void restartForUpdate(const UpdateDownloadResult &result);
    bool m_updateInstallPreparing = false;
    std::shared_ptr<std::atomic_bool> m_updateInstallCancelled;
    std::shared_ptr<const std::function<void()>> m_beforeUpdatePrepareTestHook;
    QString m_updateSessionRootTestOverride;

    void connectTrackDurations();
    void requestVisibleTrackDurations(qint64 workId, const QVector<qint64> &fileIds);
    void invalidateTrackDurations(qint64 workId);
    TrackDurationService *m_trackDurationService = nullptr;
    bool m_durationShutdownFinished = false;

    void connectCoverArt();
    void syncWorkCoverCatalog();
    void adoptCoverArt(const QString &rootPath, const QString &audioPath, qint64 workId);
    void refreshCoverArtForWork(qint64 workId);
    void applyCoverArtResult(quint64 requestId, const CoverArtResult &result);
    void chooseWorkCover(qint64 workId);
    void setWorkManualCover(qint64 workId, const QString &relativePath);
    void restoreWorkAutoCover(qint64 workId);
    void forgetCoverSelection(qint64 workId);
    void updateCoverActions();
    void startNextCoverMutation();
    void finishCoverMutations();
    void showCoverError(const QString &message);
    void showWorkEditor(qint64 workId);
    void saveWorkEditor();
    void applyWorkTitle(qint64 workId, const QString &title);
    QString latestWorkTitle(qint64 workId, const QString &fallback) const;
    void syncQueueWorkNames(PlaybackQueue &queue) const;
    enum class CoverMutationKind { Inspect, Set, Restore, EditorInspect, Edit };
    enum class WorkCoverChange { Keep, Replace, Automatic };
    struct CoverMutationRequest {
        CoverMutationKind kind = CoverMutationKind::Inspect;
        ManualCoverIdentity identity;
        QString relativePath;
        quint64 revision = 0;
        std::shared_ptr<std::atomic_bool> cancelled;
        LibraryWork expectedWork;
        QString displayName;
        WorkCoverChange coverChange = WorkCoverChange::Keep;
        ManagedCoverDraft draft;
        quint64 editorSession = 0;
    };
    struct CoverMutationResult {
        bool success = false;
        QString relativePath;
        QString errorMessage;
        bool manualUnavailable = false;
        QString managedFile;
        QString displayName;
        CoverArtResult cover;
        bool coverChanged = false;
        bool titleChanged = false;
    };
    static CoverMutationResult executeCoverMutation(const CoverMutationRequest &request,
                                                    const ManualCoverSettings &settings);
    static CoverMutationResult executeWorkEdit(const CoverMutationRequest &request,
                                               const ManualCoverSettings &settings);
    ManualCoverIdentity coverIdentity(qint64 workId) const;
    ManualCoverSettings m_manualCoverSettings;
    QHash<qint64, QString> m_manualCoverPaths;
    QHash<qint64, quint64> m_coverMutationRevisions;
    QQueue<CoverMutationRequest> m_coverMutations;
    CoverMutationRequest m_activeCoverMutation;
    QFuture<CoverMutationResult> m_coverMutationLifetime;
    bool m_coverMutationInProgress = false;
    QPointer<CoverSelectionDialog> m_coverSelectionDialog;
    qint64 m_coverSelectionWorkId = 0;
    QPointer<WorkEditDialog> m_workEditDialog;
    LibraryWork m_workEditExpected;
    quint64 m_workEditSession = 0;
    void connectWorkReferences();
    void showWorkReferences(qint64 workId);
    void updateWorkReferences(bool forceReextract);
    QPointer<WorkReferenceDialog> m_workReferenceDialog;
    qint64 m_pendingReferenceWorkId = 0;
    QString m_referenceUpdateTaskId;
    qint64 m_referenceUpdateWorkId = 0;
    QPointer<QMessageBox> m_coverErrorDialog;
    std::shared_ptr<const std::function<void()>> m_beforeCoverMutationTestHook;
    CoverArtService *m_coverArtService = nullptr;
    WorkCoverService *m_workCoverService = nullptr;
    CoverArtRequest m_coverContext;
    QUrl m_coverSource;
    quint64 m_coverRequestId = 0;
    quint64 m_coverAudioGeneration = 0;
    qint64 m_coverWorkId = 0;
    bool m_coverHasImage = false;
    bool m_coverShutdownFinished = false;
    bool m_workCoverShutdownFinished = false;

    void toggleMaximized();
    void syncTitleBrandAlignment();
    TitleBarWidget *m_titleBar = nullptr;
    WindowFrameController *m_windowFrame = nullptr;

    void connectPlaybackQueue();
    bool eventFilter(QObject *watched, QEvent *event) override;
    bool handlePlaybackKeyEvent(QObject *watched, QEvent *event);
    bool isPlaybackPassiveClickTarget(const QWidget *widget, const QPoint &position) const;
    bool isPlaybackKeyboardTarget(const QWidget *widget) const;
    bool playbackKeyboardReady(bool seeking) const;
    void togglePlaybackFromControls();
    void seekPlaybackTo(qint64 position, bool subtitleClick = false);
    bool subtitleSeekReady() const;
    void seekToSubtitle(qint64 position);
    void clearKeyboardSeek();
    void seekPlaybackByKeyboard(qint64 offset);
    std::optional<qint64> m_keyboardSeekTarget;
    QUrl m_keyboardSeekSource;
    quint64 m_keyboardSeekAudioGeneration = 0;
    quint64 m_keyboardSeekRequestGeneration = 0;
    QElapsedTimer m_keyboardSeekClock;
    void playDirectory(qint64 workId, const QString &directory, bool recursive);
    void enqueueLibraryTrack(qint64 fileId);
    void enqueueLibraryTracks(qint64 workId, const QVector<qint64> &fileIds);
    void playQueueEntry(quint64 entryId,
                        PlaybackQueueNavigation navigation = PlaybackQueueNavigation::Select);
    void setPlaybackMode(PlaybackMode mode);
    void scheduleQueueContinuation();
    void restartCurrentQueueEntry();
    QString playbackProgressIdentity(qint64 workId, qint64 fileId, const QString &path) const;
    void startQueuePreparation();
    void updatePlaybackQueue();
    void updateQueuePanelGeometry();
    void showPlaybackQueue(bool visible);
    void removeQueueEntry(quint64 entryId);
    void clearOtherQueueEntries();
    void cancelQueueTransition();
    void onQueueMediaStatus(QMediaPlayer::MediaStatus status);
    void adoptPlaybackQueue(const QString &path, qint64 workId, qint64 fileId,
                            const QString &workName, quint64 entryId,
                            const std::optional<PlaybackQueue> &replacement,
                            PlaybackQueueNavigation navigation = PlaybackQueueNavigation::Select);
    void failQueueRequest(quint64 entryId, const QString &message, bool unavailable = false);
    PlaybackQueue m_playbackQueue;
    PlaybackQueuePanel *m_queuePanel = nullptr;
    QPointer<QWidget> m_queueReturnFocus;
    quint64 m_audioRequestedQueueEntryId = 0;
    std::optional<PlaybackQueue> m_audioRequestedQueueReplacement;
    PlaybackQueueNavigation m_audioRequestedQueueNavigation = PlaybackQueueNavigation::Select;
    bool m_audioRequestedFromBeginning = false;
    QUrl m_queueSource;
    quint64 m_queueAudioGeneration = 0;
    quint64 m_queueEndedGeneration = 0;
    bool m_queueAdvanceArmed = false;
    bool m_queueContinuationPending = false;
    bool m_queueNaturalRequest = false;
    bool m_queueRestartingCurrent = false;
    QSet<QString> m_completedAudioIdentities;
    bool m_queueReplacementPending = false;
    QVector<PlaybackQueueEntry> m_queueAppendDuringReplacement;
    enum class QueuePreparationKind { Directory, Entry };
    struct QueuePreparationRequest {
        QueuePreparationKind kind = QueuePreparationKind::Entry;
        quint64 generation = 0;
        qint64 workId = 0;
        QString directory;
        bool recursive = false;
        qint64 startFileId = 0; // Directory playback starts here; zero selects the first audio.
        quint64 entryId = 0;
        PlaybackQueueEntry entry;
        std::optional<PlaybackQueue> replacement;
        PlaybackQueueNavigation navigation = PlaybackQueueNavigation::Select;
    };
    QueuePreparationRequest m_queuePreparationRequest;
    bool m_queuePreparationInProgress = false;
    bool m_queuePreparationPending = false;

    void connectPlaybackProgress();
    void beginPlaybackProgress(bool fromBeginning = false);
    void onProgressPosition(qint64 position);
    void onProgressState(QMediaPlayer::PlaybackState state);
    void onProgressMediaStatus(QMediaPlayer::MediaStatus status);
    void saveCurrentPlaybackProgress(bool force = false);
    void clearCurrentPlaybackProgress(bool explicitRequest = false);
    void beginAutomaticPlaybackRestore();
    void tryRestorePlaybackProgress();
    void cancelPlaybackRestore(bool startPlayback = true);
    void startPlaybackFromProgressDecision(const QString &diagnostic = {});
    void applyPlaybackProgressResult(const PlaybackProgressTaskResult &result);
    void finishPlaybackProgressForClose();
    bool acknowledgePlaybackProgressCloseFailure();
    PlaybackProgressCoordinator *m_playbackProgress = nullptr;
    QElapsedTimer m_progressClock;
    QUrl m_progressSource;
    std::optional<LibraryPlaybackProgress> m_savedPlaybackProgress;
    quint64 m_progressQueryRequest = 0;
    quint64 m_progressRequestGeneration = 0;
    quint64 m_progressLastWriteRequest = 0;
    qint64 m_progressRestoreTarget = 0;
    qint64 m_progressLastAttemptMs = 0;
    qint64 m_progressLastQueuedPosition = 0;
    std::optional<qint64> m_progressLastQueuedDuration;
    bool m_progressHasQueuedSave = false;
    bool m_progressSessionActive = false;
    bool m_progressSourceChanging = false;
    bool m_progressQueryPending = false;
    bool m_progressRestorePending = false;
    bool m_progressSeekIssued = false;
    bool m_progressClearRequested = false;
    bool m_progressError = false;
    bool m_progressShutdownFinished = false;
    bool m_progressClosingFinalized = false;
    bool m_progressCloseFailureAcknowledged = false;
    QString m_progressCloseFailure;
    QString m_subtitlePreferenceCloseFailure;
    QMap<qint64, QStringList> m_progressCloseFailuresByWork;
    QMap<qint64, QStringList> m_subtitleCloseFailuresByWork;
    QMessageBox *m_progressCloseErrorDialog = nullptr;

    void createUi();
    void updateWorkspaceLayout();
    void applySubtitlePanelVisibility(bool visible);
    void setPlaybackDetailVisible(bool visible);
    void applyPlaybackDetailLayout(bool visible);
    void synchronizePlaybackDetailLayout();
    void finishPlaybackDetailTransition(bool restoreFocus = true);
    bool handlePlaybackDetailTransitionEvent(QObject *watched, QEvent *event);
    void updatePlaybackIdentity(const QString &audioName, const QString &workName);
    QSplitter *m_workspaceSplitter = nullptr;
    QStackedWidget *m_contentStack = nullptr;
    PlaybackDetailWidget *m_playbackDetailPage = nullptr;
    PlaybackDetailTransition *m_playbackDetailTransition = nullptr;
    QPointer<QWidget> m_playbackDetailTransitionEndFocus;
    QRect m_playbackDetailLibraryCoverRect;
    QRect m_playbackDetailExpandedCoverRect;
    QRect m_playbackDetailCollapseRect;
    bool m_playbackDetailTransitionPreparing = false;
    int m_playbackDetailPointerAction = 0; // 0 none, 1 cover toggle, 2 collapse.
    QVBoxLayout *m_playbackDetailLayout = nullptr;
    QPushButton *m_collapsePlaybackDetailButton = nullptr;
    QWidget *m_playbackFooter = nullptr;
    QPointer<QWidget> m_playbackDetailReturnFocus;
    QList<int> m_savedWorkspaceSizes;
    QString m_playingAudioName;
    QString m_playingWorkName;
    bool m_playbackDetailVisible = false;
    bool m_compactLayout = false;
    bool m_subtitlePanelRequested = false;
    void connectLibrary();
    void openLibraryDirectory();
    void connectChickenRiceTranslation();
    void showChickenRiceTranslation(const QStringList &audioPaths = {});
    void translateLibraryTracks(qint64 workId, const QVector<qint64> &fileIds);
    void generateLibrarySubtitles(qint64 workId, const QVector<qint64> &fileIds, const QString &modeId);
    void loadChickenRiceResult(const QString &audioPath, const QString &srtPath);
    void queueAiSubtitleRefresh(const QVector<AiSubtitleTaskResult> &results);
    void tryStartAiSubtitleRefresh();
    QMap<QString, QString> m_aiSubtitlePendingResults; // Audio path -> completed SRT path.
    QMap<qint64, QString> m_aiSubtitlePendingWorks;
    QString m_aiSubtitleRefreshTaskId;
    ChickenRiceTranslateDialog *m_chickenRiceTranslateDialog = nullptr;
    // Empty in production; deterministic value-only probe for lifecycle tests.
    std::function<GpuDetectionResult(const std::atomic_bool &)> m_gpuDetectionProbe;
    void connectScanRoots();
    bool startScanRootSettingsChange(int kind, const QString &root = {}, bool enabled = true);
    void tryStartScanRootInspection();
    void showScanRootManager();
    bool addScanRoot(const QString &directory);
    bool removeScanRoot(const QString &directory);
    void requestScanRootRemoval(const QString &directory);
    void updateScanRootManager();
    bool libraryScanBusy() const;
    void startRootRefresh(const QStringList &roots);
    void applyRootRefreshFinished(const LibraryRootRefreshResult &result);
    void forgetRemovedLibraryWork(qint64 workId);
    void requestLibraryWorkRemoval(qint64 workId);
    void scheduleStartupRootRefresh();
    void tryStartStartupRootRefresh();
    void connectBatchImport();
    void openLibraryRootDirectory();
    void submitLibraryBatchDiscovery(const QString &directory,
                                     const LibraryBatchImportOptions &options = {});
    void applyBatchPreview(const QString &batchId, const WorkDirectoryDiscoveryResult &result);
    void applyBatchFinished(const LibraryBatchImportResult &result);
    void submitLibraryImport(const QString &directory, const DirectoryScanOptions &options = {});
    void cancelLibraryImport();
    void applyLibraryImportState(const QString &taskId, LibraryImportState state);
    void applyLibraryScan(const QString &taskId, const DirectoryScanResult &scan);
    void applyLibraryImportResult(const LibraryImportTaskResult &result);
    void applyLibraryImportCommitted(const QString &taskId, const LibraryImportResult &result);
    // -1 keeps browsing empty after an explicit removal; 0 preserves selection
    // with the existing first-work fallback used by startup/ordinary refresh.
    void refreshLibrary(qint64 preferredWorkId = 0);
    void startLibraryQuery();
    void openWorkFolder(qint64 workId);
    quint64 m_workFolderGeneration = 0;
    int m_workFolderWorkers = 0;
    std::shared_ptr<std::atomic_bool> m_workFolderCancelled;
    std::shared_ptr<const std::function<void()>> m_beforeWorkFolderCheckTestHook;
    // A binding edit refreshes only badges; it must not reload playback or reset browsing.
    void refreshSubtitleBadges(qint64 workId);
    void startSubtitleBadgeQuery();
    enum class SubtitleBadgeTestPoint { BeforeQuery, AfterQuery };
    std::shared_ptr<const std::function<void(SubtitleBadgeTestPoint, qint64)>> m_subtitleBadgeTaskTestHook;
    quint64 m_subtitleBadgeQueryGeneration = 0;
    qint64 m_subtitleBadgeRequestedWorkId = 0;
    bool m_subtitleBadgeQueryInProgress = false;
    bool m_subtitleBadgeQueryPending = false;
    void finishCloseIfReady();
    void createSystemTray();
    void configureTabOrder();
    void showFloatingSubtitleAppearance();
    void applyFloatingSubtitleAppearance(const FloatingSubtitleAppearance &appearance);
    void connectSignals();
    void stopPlayback();
    void updateFloatingSubtitleControls();
    void updateFloatingPlaybackControls();
    void restoreMainWindow();
    void openAudioFile();
    void playLibraryTrack(qint64 fileId);
    // With a root, path is the raw library-relative path; otherwise absolute.
    void requestOpenAudio(const QString &path, const QString &libraryRoot = {},
                          qint64 workId = 0, qint64 fileId = 0, const QString &workName = {},
                          quint64 queueEntryId = 0,
                          const std::optional<PlaybackQueue> &replacement = std::nullopt,
                          PlaybackQueueNavigation navigation = PlaybackQueueNavigation::Select,
                          bool fromBeginning = false);
    void startAudioOpen();
    void applyAudioOpenResult(const AudioFileValidationResult &result, quint64 requestGeneration,
                              qint64 workId = 0, qint64 fileId = 0, const QString &workName = {},
                              quint64 queueEntryId = 0,
                              const std::optional<PlaybackQueue> &replacement = std::nullopt,
                              const QString &libraryRoot = {},
                              PlaybackQueueNavigation navigation = PlaybackQueueNavigation::Select,
                              bool fromBeginning = false);
    void updatePlaybackStatus(QMediaPlayer::PlaybackState state);
    void showAudioOpenError(const QString &message);
    void openSubtitleFile();
    void updateSubtitleActions();
    void showSubtitleLoadError(const QString &message, const QString &details = {});
    void requestManualSubtitle(const QString &path);
    void requestLibrarySubtitles();
    void startLibrarySubtitleQuery();
    void startSubtitleLoad();
    void showBoundSubtitleDialog();
    void refreshBoundSubtitleChoices();
    void startBoundSubtitleQuery();
    void requestSubtitlePreference(SubtitlePreferenceState state, qint64 subtitleFileId = 0);
    enum class SubtitleChangeKind { Preference, BindManual, Unbind };
    void requestSubtitleBinding(qint64 subtitleFileId, bool remove = false);
    void enqueueSubtitleChange(SubtitlePreferenceState state, qint64 subtitleFileId, SubtitleChangeKind kind);
    void startSubtitlePreferenceWrite();
    void finishSubtitlePreferenceWrites();
    void resetBoundSubtitleContext();
    void setLibrarySubtitleStatus(const QString &message);
    bool hasPendingSubtitlePreference(qint64 workId, qint64 audioFileId) const;
    struct SubtitlePreferenceRequest {
        qint64 workId = 0;
        qint64 audioFileId = 0;
        SubtitlePreferenceState state = SubtitlePreferenceState::Unset;
        qint64 subtitleFileId = 0;
        quint64 audioGeneration = 0;
        quint64 subtitleGeneration = 0;
        quint64 operationGeneration = 0;
        SubtitleChangeKind kind = SubtitleChangeKind::Preference;
        bool refreshPlayback = true;
    };
    static LibraryOperationResult writeSubtitlePreference(
        const QString &databasePath, const SubtitlePreferenceRequest &request);
    enum class PreferenceTestPoint { BeforeChoices, AfterChoices, BeforeWrite, AfterWrite };
    std::shared_ptr<const std::function<void(PreferenceTestPoint)>> m_preferenceTaskTestHook;
    BoundSubtitleDialog *m_boundSubtitleDialog = nullptr;
    QPointer<QFileDialog> m_subtitleImportDialog;
    LibraryTrackSubtitlesResult m_subtitleChoiceSnapshot;
    QVector<qint64> m_availableSubtitleChoiceIds;
    QQueue<SubtitlePreferenceRequest> m_preferenceRequests;
    SubtitlePreferenceRequest m_activeSubtitlePreference;
    QFuture<LibraryOperationResult> m_preferenceTaskLifetime;
    LibraryOperationResult m_lastSubtitlePreferenceWrite;
    quint64 m_subtitleChoiceQueryGeneration = 0;
    quint64 m_subtitleChoiceAudioGeneration = 0;
    quint64 m_preferenceOperationGeneration = 0;
    bool m_preferenceWriteInProgress = false;
    bool m_subtitleChoiceQueryInProgress = false;
    bool m_subtitleChoiceQueryPending = false;
    qint64 m_pendingSubtitleTranslationId = 0;
    qint64 m_pendingSubtitleFolderId = 0;
    QString m_subtitlePreferenceFeedback;
    bool m_subtitlePreferenceFeedbackError = false;
    bool isCurrentSubtitleRequest(quint64 audioGeneration, quint64 requestGeneration,
                                  qint64 workId, qint64 fileId) const;
    struct SubtitleLoadRequest {
        QString path;
        QString root;
        LibraryFile file;
        quint64 audioGeneration = 0;
        quint64 requestGeneration = 0;
        qint64 workId = 0;
        qint64 audioFileId = 0;
        bool automatic = false;
    };
    // Private deterministic scheduling seam; production leaves the hook empty.
    enum class SubtitleTestPoint { BeforeQuery, AfterQuery, BeforeLoad, AfterLoad };
    std::shared_ptr<const std::function<void(SubtitleTestPoint)>> m_subtitleTaskTestHook;
    SubtitleLoadRequest m_requestedSubtitleLoad;
    LibrarySubtitleResolution m_librarySubtitleResolution;
    SubtitleFileLoadResult m_lastLibrarySubtitleLoad;
    quint64 m_subtitleRequestGeneration = 0;
    qint64 m_playingLibraryWorkId = 0;
    qint64 m_playingLibraryFileId = 0;
    qint64 m_audioRequestedWorkId = 0;
    qint64 m_audioRequestedFileId = 0;
    qint64 m_libraryAssociationWorkId = 0;
    quint64 m_rootSubtitleWaitAudioGeneration = 0;
    quint64 m_rootSubtitleWaitRequestGeneration = 0;
    qint64 m_rootSubtitleWaitWorkId = 0;
    qint64 m_rootSubtitleWaitFileId = 0;
    bool m_manualSubtitleChoice = false;
    bool m_subtitleQueryInProgress = false;
    bool m_subtitleQueryPending = false;
    bool m_subtitleLoadPending = false;
    void applySubtitleLoad(const QString &path, const SubtitleFileLoadResult &result,
                           quint64 audioGeneration);
    void clearSubtitles();
    void refreshSubtitles();
    void refreshSubtitles(qint64 positionMs, bool playbackStopped);
    void setSubtitleText(const QString &text);

    QVector<LibraryWork> m_libraryWorksSnapshot;
    QVector<LibraryFile> m_libraryFilesSnapshot;
    qint64 m_librarySnapshotWorkId = 0;
    quint64 m_audioOpenGeneration = 0;
    quint64 m_audioRequestedGeneration = 0;
    QMessageBox *m_audioErrorDialog = nullptr;
    QMessageBox *m_subtitleErrorDialog = nullptr;
    QString m_audioRequestedPath;
    QString m_audioRequestedRoot;
    QString m_audioRequestedWorkName;
    bool m_audioOpenInProgress = false;
    bool m_audioOpenPending = false;
    // Private deterministic scheduling seam; never populated in production.
    std::shared_ptr<const std::function<void()>> m_audioValidationTestHook;
    QString m_libraryDatabasePath;
    LibraryPanel *m_libraryPanel = nullptr;
    LibraryImportService *m_libraryImportService = nullptr;
    LibraryBatchImportService *m_libraryBatchImportService = nullptr;
    struct ScanRootSettingsResult {
        bool success = false;
        ScanRootSnapshot snapshot;
        QString root;
        QString directory;
        QString errorMessage;
    };
    bool m_scanRootSettingsReady = false;
    bool m_scanRootSettingsInProgress = false;
    bool m_scanRootInspectionPending = false;
    quint64 m_scanRootManagerSession = 0;
    QFuture<ScanRootSettingsResult> m_scanRootSettingsLifetime;
    enum class ScanRootSettingsTestPoint { BeforeTask, AfterTask };
    std::shared_ptr<const std::function<void(ScanRootSettingsTestPoint, int)>> m_scanRootSettingsTestHook;
    std::unique_ptr<ScanRootStore> m_scanRootStore;
    LibraryRootRefreshService *m_libraryRootRefreshService = nullptr;
    ScanRootManagerDialog *m_scanRootManagerDialog = nullptr;
    QHash<QString, QString> m_scanRootStatuses;
    struct ScanRootPresentation {
        QString summary;
        bool reviewRequired = false;
    };
    QHash<QString, ScanRootPresentation> m_scanRootPresentations;
    QSet<qint64> m_removedLibraryWorkIds; // Invalidates this window's obsolete asynchronous requests.
    QString m_libraryRootChildTaskId;
    bool m_rootShutdownFinished = false;
    bool m_startupRootScanScheduled = false;
    bool m_startupRootScanPending = false;
    BatchImportPreviewDialog *m_batchImportPreviewDialog = nullptr;
    QString m_batchPreviewId;
    QString m_libraryBatchChildTaskId;
    bool m_batchShutdownFinished = false;
    QString m_libraryTaskId;
    QString m_libraryManualRemovalTaskId;
    qint64 m_libraryManualRemovalWorkId = 0;
    QMessageBox *m_workRemovalConfirmation = nullptr;
    QMessageBox *m_scanRootRemovalConfirmation = nullptr;
    QMessageBox *m_workRemovalErrorDialog = nullptr;
    QString m_libraryScanSummary;
    LibraryImportState m_libraryImportState = LibraryImportState::Idle;
    quint64 m_libraryQueryGeneration = 0;
    qint64 m_libraryRequestedWorkId = 0;
    bool m_libraryQueryInProgress = false;
    bool m_libraryQueryPending = false;
    bool m_libraryDatabaseReady = false;
    bool m_closing = false;
    bool m_libraryShutdownFinished = false;
    bool m_closeScheduled = false;
    QSettings m_settings;
    UpdateNotificationController *m_updateNotifications = nullptr;
    AudioPlayer *m_audioPlayer;
    std::unique_ptr<FloatingSubtitleWindow> m_floatingSubtitleWindow;
    SubtitleSynchronizer m_subtitleSynchronizer;
    quint64 m_audioGeneration = 0;
    bool m_subtitleLoadInProgress = false;
    PlaybackControlsWidget *m_playbackControls = nullptr;
    SubtitlePanel *m_subtitlePanel = nullptr;
    FloatingSubtitleAppearanceDialog *m_floatingAppearanceDialog = nullptr;
    QAction *m_toggleFloatingAction = nullptr;
    QAction *m_lockFloatingAction = nullptr;
    QAction *m_floatingAppearanceAction = nullptr;
    QAction *m_restoreMainWindowAction = nullptr;
    QSystemTrayIcon *m_systemTrayIcon = nullptr;
};

} // namespace asmr
