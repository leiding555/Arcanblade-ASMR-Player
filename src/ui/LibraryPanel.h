#pragma once

#include "library/LibraryImportService.h"
#include "library/WorkCategoryTypes.h"
#include "artwork/WorkCoverService.h"
#include "ui/LibraryTrackTreeModel.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QEvent;
class QCheckBox;
class QAction;
class QHBoxLayout;
class QLabel;
class QModelIndex;
class QListView;
class QMenu;
class QPushButton;
class QStackedWidget;
class QSplitter;
class QTableView;
class QToolButton;
class QVBoxLayout;
class QTimer;
class QStandardItemModel;

namespace asmr {

class LibraryWorksModel;
class LibraryWorksFilterModel;
class CoverArtWidget;
struct CoverArtResult;

// Presentation only. The owner supplies database snapshots and task state; this
// widget never scans, opens a database, or controls the audio player.
class LibraryPanel final : public QWidget
{
    Q_OBJECT

public:
    enum DataRole {
        WorkIdRole = LibraryTrackTreeModel::WorkIdRole,
        FileIdRole,
        RootPathRole,
        RelativePathRole,
        MissingRole,
        PlayingRole,
        DirectoryRole,
        CategoryIdRole,
    };

    explicit LibraryPanel(QWidget *parent = nullptr);

    // Refreshes a snapshot without emitting workSelected. An absent/zero ID
    // leaves the work selection empty. Only this work's audio files are shown.
    void setLibrary(const QVector<LibraryWork> &works,
                    const QVector<LibraryFile> &files, qint64 selectedWorkId,
                    const QSet<qint64> &subtitleAudioFileIds = {});
    void setCategories(const QVector<WorkCategory> &categories,
                       const QVector<WorkCategoryMembership> &memberships);
    void setCategoryCover(qint64 categoryId, quint64 revision, const CoverArtResult &result);
    void setCategoryMutationBusy(bool busy);
    // Playback identity is independent of the currently browsed work/selection.
    void setPlayingTrack(qint64 workId, qint64 fileId);
    void setQuerying(bool querying);
    void setImportAvailable(bool available);
    void setImportState(LibraryImportState state, const QString &text, bool active, bool batch = false);
    void setWorkRemovalActive(bool active);
    void setImportFeedback(const QString &text, const QString &details = {});
    void setImportError(const QString &message);
    void setLibraryError(const QString &message);
    void setClosing(bool closing);
    // MainWindow supplies its sole subtitle panel and playback/status footer.
    // The directory models and browser remain owned by this widget.
    [[nodiscard]] QSplitter *workspaceSplitter() const { return m_workspaceSplitter; }
    [[nodiscard]] QWidget *sidebarWidget() const { return m_sidebar; }
    // Actual styled import icon center, in LibraryPanel coordinates.
    [[nodiscard]] QPoint importIconCenter() const;
    [[nodiscard]] QMenu *settingsMenu() const { return m_settingsMenu; }
    void setWorkspaceFooter(QWidget *footer);
    // Temporarily transfer the same footer to the playback detail page.
    [[nodiscard]] QWidget *takeWorkspaceFooter();
    // Settle the directory page's responsive header/navigation layouts before
    // a transition captures it. Does not dispatch queued input or timers.
    void prepareForSnapshot();
    // The brand is displayed above the sidebar import action, including compact layouts.
    void setApplicationTitleVisible(bool visible);
    // Only the central directory table and its viewport participate in the
    // main-window playback shortcuts; other item views keep their own keys.
    [[nodiscard]] bool isTrackViewTarget(const QWidget *widget) const;
    void setManualCoverAvailable(qint64 workId, bool manual);
    void setWorkCover(qint64 workId, const CoverArtResult &result);
    void clearWorkCover(qint64 workId);
    // Coalesced after layout; also usable after the owner connects its loader.
    void requestWorkCoverDemand();
    void requestTrackDurationDemand();
    void setTrackDuration(qint64 workId, qint64 fileId, std::optional<qint64> milliseconds);
    void clearTrackDurations(qint64 workId);
    void setSubtitleTracks(qint64 workId, const QSet<qint64> &audioFileIds);

    [[nodiscard]] qint64 selectedWorkId() const { return m_selectedWorkId; }
    [[nodiscard]] QString currentDirectory() const { return m_currentDirectory; }
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString diagnosticDetails() const { return m_diagnosticDetails; }

signals:
    void importDirectoryRequested();
    void importRootDirectoryRequested();
    void openAudioRequested();
    void manageScanRootsRequested();
    void refreshScanRootsRequested();
    void cancelImportRequested();
    void aiSubtitlesRequested();
    void workSelected(qint64 workId);
    void workFolderRequested(qint64 workId);
    void workRemovalRequested(qint64 workId);
    void workReferencesRequested(qint64 workId);
    void trackPlayRequested(qint64 fileId);
    void directoryPlayRequested(qint64 workId, const QString &relativeDirectory, bool recursive);
    void trackEnqueueRequested(qint64 fileId);
    void tracksEnqueueRequested(qint64 workId, const QVector<qint64> &fileIds);
    void generateSubtitlesRequested(qint64 workId, const QVector<qint64> &fileIds, const QString &modeId);
    void translateTracksRequested(qint64 workId, const QVector<qint64> &fileIds);
    void editWorkRequested(qint64 workId);
    void chooseCoverRequested(qint64 workId);
    void restoreAutoCoverRequested(qint64 workId);
    void workCoverDemandChanged(qint64 selectedWorkId, const QVector<qint64> &visibleWorkIds);
    void createCategoryRequested(qint64 workId = 0);
    void renameCategoryRequested(qint64 categoryId);
    void removeCategoryRequested(qint64 categoryId);
    void workCategoryRequested(qint64 workId, qint64 categoryId, bool present);
    void categoryCoverDemandChanged(const QVector<WorkCategoryCoverDemand> &categories);
    void trackDurationDemandChanged(qint64 workId, const QVector<qint64> &visibleAudioFileIds);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void activateItem(const QModelIndex &index);
    void requestTrackPlayback(const QModelIndex &index);
    [[nodiscard]] bool canRequestQueuePlayback() const;
    [[nodiscard]] bool hasAvailableDirectoryAudio() const;
    void showTrackContextMenu(const QModelIndex &index, const QPoint &globalPosition);
    void triggerTrackSubtitleAction(const QString &modeId);
    [[nodiscard]] QVector<qint64> selectedTranslationFileIds() const;
    void updateTranslationActions();
    void setTrackBatchMode(bool enabled);
    void updateTrackBatchActions();
    void triggerTrackBatchAction(const QString &modeId);
    [[nodiscard]] QVector<qint64> checkedTrackFileIds() const;
    void showWorkContextMenu(const QModelIndex &index, const QPoint &globalPosition);
    [[nodiscard]] bool canRequestWorkRemoval() const;
    void invalidateWorkContextMenu();
    void showCategoryContextMenu(const QModelIndex &index, const QPoint &globalPosition);
    void invalidateCategoryContextMenu();
    void setSidebarLocation(bool categories, qint64 categoryId = 0);
    void updateCategoryFilter();
    void rebuildCategoryRows();
    void publishCategoryCoverDemand(bool force);
    [[nodiscard]] bool canMutateCategories() const;
    [[nodiscard]] QVector<qint64> categoryWorkIds(qint64 categoryId) const;
    [[nodiscard]] bool isCurrentPageIndex(const QModelIndex &index) const;
    void navigateToDirectory(const QString &path, const QString &selectedDirectory = {});
    void navigateUp();
    void rebuildBreadcrumbs();
    void scheduleBreadcrumbVisibility();
    void updateBreadcrumbLayout();
    void updatePresentation();
    void restoreVisibleWorkSelection();
    void rememberBrowseState();
    void restoreBrowseState();
    void scheduleWorkCoverDemand(bool force = false);
    void publishWorkCoverDemand();
    void scheduleTrackDurationDemand(bool force = false);
    void publishTrackDurationDemand();
    void updateBrowsedCover();
    void updateWorkHeaderGeometry();
    void updateWelcomeGeometry();
    struct DirectoryPageState {
        qint64 fileId = 0;
        QString selectedDirectory;
        int column = 0;
        int horizontalScroll = 0;
        int verticalScroll = 0;
    };
    struct BrowseState {
        QString currentDirectory;
        QHash<QString, DirectoryPageState> pages;
    };
    QHash<qint64, BrowseState> m_browseStates;
    QString m_currentDirectory; // Original model path; empty denotes the work root.
    qint64 m_displayedWorkId = 0; // Zero during a work-switch query transition.

    LibraryWorksModel *m_worksModel = nullptr;
    LibraryWorksFilterModel *m_worksFilter = nullptr;
    LibraryTrackTreeModel *m_tracksModel = nullptr;
    QListView *m_worksView = nullptr;
    QListView *m_categoriesView = nullptr;
    QStandardItemModel *m_categoriesModel = nullptr;
    QToolButton *m_allWorksButton = nullptr;
    QToolButton *m_categoriesButton = nullptr;
    QToolButton *m_categoryBackButton = nullptr;
    QToolButton *m_createCategoryButton = nullptr;
    QLabel *m_categoryNameLabel = nullptr;
    QWidget *m_categoryNavigation = nullptr;
    QVector<WorkCategory> m_categories;
    QVector<WorkCategoryMembership> m_categoryMemberships;
    bool m_categoryMode = false;
    qint64 m_currentCategoryId = 0;
    qint64 m_lastCategoryId = 0;
    bool m_categoryMutationBusy = false;
    QHash<qint64, int> m_sidebarScrollPositions;
    QHash<qint64, quint64> m_categoryCoverRevisions;
    quint64 m_categoryCoverSequence = 0;
    QVector<WorkCategoryCoverDemand> m_lastCategoryCoverDemand;
    QMenu *m_categoryContextMenu = nullptr;
    QAction *m_renameCategoryAction = nullptr;
    QAction *m_removeCategoryAction = nullptr;
    qint64 m_categoryMenuId = 0;
    quint64 m_categorySnapshotRevision = 0;
    quint64 m_categoryMenuSnapshotRevision = 0;
    CoverArtWidget *m_workCover = nullptr;
    QWidget *m_workInformation = nullptr;
    QTimer *m_coverDemandTimer = nullptr;
    qint64 m_lastCoverDemandSelected = -1;
    QVector<qint64> m_lastCoverDemandVisible;
    bool m_forceCoverDemand = false;
    QTimer *m_durationDemandTimer = nullptr;
    qint64 m_lastDurationDemandWork = -1;
    QVector<qint64> m_lastDurationDemandVisible;
    bool m_forceDurationDemand = false;
    QTableView *m_tracksView = nullptr;
    QPushButton *m_playDirectoryButton = nullptr;
    QToolButton *m_coverMenuButton = nullptr;
    QHash<qint64, bool> m_manualCovers;
    QMenu *m_trackContextMenu = nullptr;
    QAction *m_enqueueTrackAction = nullptr;
    QAction *m_offlineTranslateTracksAction = nullptr;
    QAction *m_japaneseAsrTracksAction = nullptr;
    QPushButton *m_batchButton = nullptr;
    QWidget *m_batchToolbar = nullptr;
    QCheckBox *m_batchSelectAll = nullptr;
    QLabel *m_batchSelectionLabel = nullptr;
    QPushButton *m_batchAddButton = nullptr;
    QPushButton *m_batchExitButton = nullptr;
    QMenu *m_batchAddMenu = nullptr;
    QAction *m_batchEnqueueAction = nullptr;
    QAction *m_batchOfflineTranslateAction = nullptr;
    QAction *m_batchJapaneseAsrAction = nullptr;
    bool m_trackBatchMode = false;
    quint64 m_batchMenuRevision = 0;
    qint64 m_batchMenuWorkId = 0;
    QString m_batchMenuDirectory;
    QVector<qint64> m_batchMenuIds;
    QVector<qint64> m_translationMenuIds;
    QMenu *m_workContextMenu = nullptr;
    QAction *m_editWorkAction = nullptr;
    QAction *m_openWorkFolderAction = nullptr;
    QAction *m_removeWorkAction = nullptr;
    QAction *m_workReferencesAction = nullptr;
    QMenu *m_addWorkToCategoryMenu = nullptr;
    QAction *m_removeCurrentCategoryAction = nullptr;
    qint64 m_workMenuCategoryId = 0;
    struct WorkMenuContext {
        quint64 revision = 0;
        LibraryWork work;
    };
    quint64 m_workMenuRevision = 1;
    WorkMenuContext m_workMenuContext;
    // Menus may remain open while a queued snapshot/query/navigation arrives.
    // Never reinterpret their previous target against a newer browser page.
    struct QueueMenuContext {
        quint64 revision = 0;
        qint64 workId = 0;
        qint64 fileId = 0;
        QString directory;
        QString relativePath;
    };
    quint64 m_browseRevision = 1;
    QueueMenuContext m_trackMenuContext;
    QWidget *m_navigationBar = nullptr;
    bool m_updatingWorkHeaderGeometry = false;
    QWidget *m_breadcrumbBar = nullptr;
    QHBoxLayout *m_breadcrumbLayout = nullptr;
    QToolButton *m_breadcrumbRootButton = nullptr;
    QToolButton *m_breadcrumbOverflowButton = nullptr;
    QLabel *m_breadcrumbOverflowSeparator = nullptr;
    QMenu *m_breadcrumbOverflowMenu = nullptr;
    struct BreadcrumbSegment {
        QToolButton *button = nullptr;
        QLabel *separator = nullptr;
        QString path;
        QString name;
    };
    QVector<BreadcrumbSegment> m_breadcrumbSegments;
    QStringList m_hiddenBreadcrumbPaths;
    bool m_breadcrumbVisibilityScheduled = false;
    QPushButton *m_navigateUpButton = nullptr;
    QStackedWidget *m_worksStack = nullptr;
    QStackedWidget *m_tracksStack = nullptr;
    QSplitter *m_workspaceSplitter = nullptr;
    QWidget *m_sidebar = nullptr;
    QVBoxLayout *m_workspaceLayout = nullptr;
    QWidget *m_workspaceFooter = nullptr;
    QPushButton *m_importMenuButton = nullptr;
    QPushButton *m_aiSubtitlesButton = nullptr;
    QPushButton *m_settingsButton = nullptr;
    QMenu *m_settingsMenu = nullptr;
    QAction *m_importAction = nullptr;
    QAction *m_addScanRootAction = nullptr;
    QAction *m_manageScanRootsAction = nullptr;
    QPushButton *m_refreshScanRootsButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_welcomeImportButton = nullptr;
    QPushButton *m_welcomeAddScanRootButton = nullptr;
    QLabel *m_queryLabel = nullptr;
    QLabel *m_errorLabel = nullptr;
    QLabel *m_workCountLabel = nullptr;
    QLabel *m_worksEmptyLabel = nullptr;
    QLabel *m_tracksEmptyLabel = nullptr;
    QWidget *m_welcomePage = nullptr;
    QWidget *m_welcomeContent = nullptr;
    QLabel *m_welcomeIconLabel = nullptr;
    QLabel *m_welcomeTitleLabel = nullptr;
    QLabel *m_browseTitleLabel = nullptr;
    QString m_statusText;
    QString m_diagnosticDetails;
    QString m_taskErrorText;
    QString m_libraryErrorText;
    qint64 m_selectedWorkId = 0;
    LibraryImportState m_importState = LibraryImportState::Idle;
    bool m_active = false;
    bool m_batchImportMode = false;
    bool m_workRemovalActive = false;
    bool m_querying = false;
    bool m_importAvailable = true;
    bool m_closing = false;
    bool m_applicationTitleVisible = true;
};

} // namespace asmr
