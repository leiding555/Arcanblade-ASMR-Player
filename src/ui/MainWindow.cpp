#include "ui/MainWindow.h"
#include "ui/ApplicationBrand.h"
#include "ui/PlayerMessageBox.h"
#include "ui/UpdateNotificationController.h"
#include "update/ApplicationVersion.h"
#include "update/UpdatePackage.h"

#include "playback/AudioPlayer.h"
#include "playback/PlaybackProgressCoordinator.h"
#include "playback/AudioFileValidation.h"
#include "subtitle/SubtitleFileLoader.h"
#include "ui/FloatingSubtitleWindow.h"
#include "ui/FloatingSubtitleAppearanceDialog.h"
#include "ui/FloatingSubtitleSettings.h"
#include "ui/PlaybackControlsWidget.h"
#include "ui/PlaybackDetailWidget.h"
#include "ui/PlaybackDetailTransition.h"
#include "ui/SubtitlePanel.h"
#include "ui/LibraryPanel.h"
#include "ui/PlayerTheme.h"
#include "library/ScanRootStore.h"
#include "ui/ScanRootManagerDialog.h"
#include "ui/PlaybackQueuePanel.h"
#include "ui/TitleBarWidget.h"
#include "ui/WindowFrameController.h"
#include "artwork/CoverArtService.h"
#include "artwork/WorkCoverService.h"
#include "playback/TrackDurationService.h"
#include "ui/CoverSelectionDialog.h"
#include "ui/WorkEditDialog.h"
#include "ui/WorkReferenceDialog.h"
#include "ui/BoundSubtitleDialog.h"
#include "ui/ChickenRiceTranslateDialog.h"

#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QLabel>
#include <QHBoxLayout>
#include <QPushButton>
#include <QStackedWidget>
#include <QMenu>
#include <QPalette>
#include <QPainter>
#include <QPixmap>
#include <QPromise>
#include <QScreen>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QStringList>
#include <QStyle>
#include <QSplitter>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QToolButton>

#include <algorithm>
#include <memory>
#include <exception>
#include <filesystem>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace asmr {
namespace {

// Observe only geometry changes, independently of playback's application-wide
// key filter. Coalescing lets splitter/layout events settle before mapping the
// sidebar and prevents a title-label resize from recursing into layout work.
class BrandAlignmentObserver final : public QObject {
public:
    BrandAlignmentObserver(QObject *parent, std::function<void()> synchronize)
        : QObject(parent), m_synchronize(std::move(synchronize)) {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Move:
        case QEvent::Show:
        case QEvent::LayoutRequest:
        case QEvent::StyleChange:
        case QEvent::PolishRequest:
            if (!m_pending) {
                m_pending = true;
                QTimer::singleShot(0, this, [this] {
                    m_pending = false;
                    m_synchronize();
                });
            }
            break;
        default: break;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    std::function<void()> m_synchronize;
    bool m_pending = false;
};

QString defaultLibraryDatabasePath()
{
    const QString location = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return location.isEmpty() ? QString() : QDir(location).filePath(QStringLiteral("library.sqlite"));
}

QString diagnosticDetails(const QList<SubtitleLoadDiagnostic> &diagnostics)
{
    constexpr qsizetype maximumDetails = 20;
    QStringList details;
    const qsizetype count = std::min(diagnostics.size(), maximumDetails);
    for (qsizetype i = 0; i < count; ++i) {
        const auto &diagnostic = diagnostics.at(i);
        details.append(QStringLiteral("第 %1 行，%2：%3")
                           .arg(diagnostic.line)
                           .arg(diagnostic.severity == SubtitleLoadDiagnosticSeverity::Error
                                    ? QStringLiteral("错误") : QStringLiteral("警告"),
                                diagnostic.message));
    }
    if (diagnostics.size() > maximumDetails) {
        details.append(QStringLiteral("另有 %1 条诊断未显示。")
                           .arg(diagnostics.size() - maximumDetails));
    }
    return details.join(QChar('\n'));
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : MainWindow(defaultLibraryDatabasePath(), parent)
{
}

MainWindow::MainWindow(const QString &libraryDatabasePath, QWidget *parent)
    : QWidget(parent), m_libraryDatabasePath(libraryDatabasePath),
      m_audioPlayer(new AudioPlayer(this)),
      m_floatingSubtitleWindow(std::make_unique<FloatingSubtitleWindow>())
{
    setWindowTitle(applicationDisplayName());
    setWindowIcon(applicationIcon());
    setObjectName(QStringLiteral("asmrMainWindow"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint
                   | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint
                   | Qt::WindowCloseButtonHint);
    setMinimumSize(760, 520 + TitleBarWidget::Height);
    QSize initialSize(1180, 760 + TitleBarWidget::Height);
    if (const auto *screen = QGuiApplication::primaryScreen()) {
        const QSize available = screen->availableGeometry().size();
        setMinimumSize(minimumSize().boundedTo(available));
        initialSize = initialSize.boundedTo(available);
    }
    resize(initialSize);
    m_settings.setFallbacksEnabled(false);
    const auto saved = readFloatingSubtitleSettings(m_settings);
    // Choose the saved width and monitor before applying its height limit.
    m_floatingSubtitleWindow->restoreSize(saved.size);
    m_floatingSubtitleWindow->restorePosition(saved.position);
    m_floatingSubtitleWindow->setAppearance(saved.appearance);
    m_floatingSubtitleWindow->restorePosition(saved.position);
    createUi();
    createSystemTray();
    connectSignals();
    configureTabOrder();
    updateFloatingSubtitleControls();
    connectPlaybackProgress();
    connectLibrary();
    connectPlaybackQueue();
    connectCoverArt();
    connectTrackDurations();
    connectChickenRiceTranslation();
    refreshLibrary();
    // Application dispatch sees keys before focused child widgets. The filter
    // accepts only this window's central table or passive presentation areas.
    qApp->installEventFilter(this);
    UpdateNotificationOptions updateOptions;
    updateOptions.restartBlockedReason = [this] { return updateRestartBlockedReason(); };
    updateOptions.requestRestart = [this](const UpdateDownloadResult &result) { restartForUpdate(result); };
    m_updateNotifications = new UpdateNotificationController(
        this, m_libraryPanel->settingsMenu(), &m_settings, nullptr, std::move(updateOptions), this);
    connect(m_updateNotifications, &UpdateNotificationController::shutdownFinished,
            this, &MainWindow::finishCloseIfReady);
}

void MainWindow::startAutomaticUpdateCheck()
{
    if (!m_closing && m_updateNotifications)
        m_updateNotifications->startAutomaticCheck();
}

QString MainWindow::updateRestartBlockedReason() const
{
    if (m_closing) return QStringLiteral("软件正在关闭，请稍后重试。");
    if (m_chickenRiceTranslateDialog && m_chickenRiceTranslateDialog->isBusy())
        return QStringLiteral("AI 任务或模型准备正在进行，请结束后再重启更新。");
    return {};
}

void MainWindow::restartForUpdate(const UpdateDownloadResult &result)
{
    if (m_closing || m_updateInstallPreparing || !m_updateNotifications) return;
    const auto blocked = updateRestartBlockedReason();
    if (!blocked.isEmpty() || result.state != UpdateDownloadState::Ready
        || result.preparedDirectory.isEmpty()) {
        m_updateNotifications->installPreparationFailed(blocked.isEmpty()
            ? QStringLiteral("更新包尚未准备就绪，请重新下载。") : blocked);
        return;
    }
    m_updateInstallPreparing = true;
    m_updateInstallCancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = m_updateInstallCancelled;
    // Prevent new UI tasks while the already-verified package is checked again
    // and the helper performs its ready handshake. No process has exited yet.
    setEnabled(false);
    const QString cache = m_updateSessionRootTestOverride.isEmpty()
        ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
              .filePath(QStringLiteral("updates")) : m_updateSessionRootTestOverride;
    const QString session = QDir(cache).filePath(QStringLiteral("install-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces));
    auto promise = std::make_shared<QPromise<UpdateInstallPreparation>>();
    auto *watcher = new QFutureWatcher<UpdateInstallPreparation>(this);
    connect(watcher, &QFutureWatcher<UpdateInstallPreparation>::finished, this,
            [this, watcher, cancelled] {
        const auto preparation = watcher->result();
        watcher->deleteLater();
        m_updateInstallPreparing = false;
        if (m_closing || cancelled->load()) {
            cancelPreparedUpdate(preparation);
            finishCloseIfReady();
            return;
        }
        setEnabled(true);
        QString error = preparation.error;
        const auto blockedNow = updateRestartBlockedReason();
        if (error.isEmpty() && !blockedNow.isEmpty()) error = blockedNow;
        if (error.isEmpty() && !commitPreparedUpdate(preparation, &error)) {
            if (error.isEmpty()) error = QStringLiteral("无法启动更新安装，请从发布页手动下载。");
        }
        if (!error.isEmpty()) {
            cancelPreparedUpdate(preparation);
            m_updateNotifications->installPreparationFailed(error);
            return;
        }
        // The helper has acknowledged readiness but still waits for THIS
        // process to terminate. Existing close barriers save progress/drain
        // workers; a delayed or failed close never permits early replacement.
        close();
    });
    watcher->setFuture(promise->future());
    const auto hook = m_beforeUpdatePrepareTestHook;
    QThreadPool::globalInstance()->start([promise, prepared = result.preparedDirectory,
        target = QCoreApplication::applicationDirPath(), current = applicationBuildVersion(),
        version = result.release.version, manifestSha256 = result.preparedManifestSha256,
        session, cancelled, hook] {
        promise->start();
        UpdateInstallPreparation preparation;
        try {
            if (hook) (*hook)();
            if (!cancelled->load()) {
                preparation = prepareUpdateInstall(prepared, target, current, version, session, *cancelled, manifestSha256);
                if (preparation.success() && !launchPreparedUpdate(preparation, *cancelled, &preparation.error)) {
                    if (preparation.error.isEmpty()) preparation.error = QStringLiteral("更新程序启动失败。");
                    cancelPreparedUpdate(preparation);
                }
            }
        } catch (...) { preparation.error = QStringLiteral("准备更新失败，请重试或从发布页手动下载。"); }
        promise->addResult(std::move(preparation));
        promise->finish();
    });
}

// Keep subtitles independent of the native main-window owner/minimize group.
// The unique_ptr still destroys the window with MainWindow, including while visible.
MainWindow::~MainWindow()
{
    if (m_updateInstallCancelled) m_updateInstallCancelled->store(true);
    if (m_updateNotifications) m_updateNotifications->shutdown();
    if (m_workFolderCancelled) m_workFolderCancelled->store(true);
    if (qApp) qApp->removeEventFilter(this);
    if (m_chickenRiceTranslateDialog) {
        disconnect(m_chickenRiceTranslateDialog, nullptr, this, nullptr);
        m_chickenRiceTranslateDialog->shutdown();
        delete m_chickenRiceTranslateDialog;
        m_chickenRiceTranslateDialog = nullptr;
    }
    finishPlaybackDetailTransition(false);
    if (m_trackDurationService) {
        disconnect(m_trackDurationService, nullptr, this, nullptr);
        m_trackDurationService->shutdown();
        delete m_trackDurationService; // Normal close has already drained its worker.
        m_trackDurationService = nullptr;
    }
    if (m_coverSelectionDialog) {
        disconnect(m_coverSelectionDialog, nullptr, this, nullptr);
        delete m_coverSelectionDialog.data();
    }
    if (m_scanRootSettingsInProgress) m_scanRootSettingsLifetime.waitForFinished();
    finishCoverMutations();
    if (m_coverArtService) {
        disconnect(m_coverArtService, nullptr, this, nullptr);
        m_coverArtService->shutdown();
        delete m_coverArtService; // Normal close has already drained value-only work.
        m_coverArtService = nullptr;
    }
    if (m_workCoverService) {
        disconnect(m_workCoverService, nullptr, this, nullptr);
        m_workCoverService->shutdown();
        delete m_workCoverService;
        m_workCoverService = nullptr;
    }
    if (m_libraryRootRefreshService) {
        disconnect(m_libraryRootRefreshService, nullptr, this, nullptr);
        m_libraryRootRefreshService->shutdown();
        delete m_libraryRootRefreshService;
        m_libraryRootRefreshService = nullptr;
    }
    if (m_libraryBatchImportService) {
        disconnect(m_libraryBatchImportService, nullptr, this, nullptr);
        m_libraryBatchImportService->shutdown();
        delete m_libraryBatchImportService;
        m_libraryBatchImportService = nullptr;
    }
    finishPlaybackProgressForClose();
    if (m_playbackProgress) {
        disconnect(m_playbackProgress, nullptr, this, nullptr);
        delete m_playbackProgress; // Direct-delete fallback drains value-only work.
        m_playbackProgress = nullptr;
    }
    finishSubtitlePreferenceWrites();
    // Save the final stable logical position, never intermediate native-window
    // moves/hides produced by setLocked(). Visibility and lock are not persisted.
    if (!writeFloatingSubtitleSettings(m_settings,
            m_floatingSubtitleWindow->appearance(), m_floatingSubtitleWindow->pos(),
            m_floatingSubtitleWindow->size())) {
        qWarning() << "Failed to save floating subtitle settings:" << m_settings.status();
    }
    if (m_systemTrayIcon) {
        m_systemTrayIcon->hide();
        m_systemTrayIcon->setContextMenu(nullptr);
    }
}

void MainWindow::createUi()
{
    setAttribute(Qt::WA_StyledBackground);
    auto colors = palette();
    colors.setColor(QPalette::Window, QColor(QStringLiteral("#14171B")));
    colors.setColor(QPalette::Base, QColor(QStringLiteral("#1C2127")));
    colors.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#252B33")));
    colors.setColor(QPalette::WindowText, QColor(QStringLiteral("#F0F2F5")));
    colors.setColor(QPalette::Text, QColor(QStringLiteral("#F0F2F5")));
    colors.setColor(QPalette::Button, QColor(QStringLiteral("#2A3038")));
    colors.setColor(QPalette::ButtonText, QColor(QStringLiteral("#F0F2F5")));
    colors.setColor(QPalette::Highlight, QColor(QStringLiteral("#284C42")));
    colors.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#F0FFF8")));
    colors.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#77818D")));
    colors.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#77818D")));
    setPalette(colors);
    setStyleSheet(playerStyleSheet());

    m_titleBar = new TitleBarWidget(this);
    m_titleBar->setTitleVisible(true);
    m_windowFrame = new WindowFrameController(this, m_titleBar);
    connect(m_titleBar, &TitleBarWidget::minimizeRequested, this, [this] {
        if (!m_closing) showMinimized();
    });
    connect(m_titleBar, &TitleBarWidget::maximizeRestoreRequested,
            this, &MainWindow::toggleMaximized);
    // Always enter the existing asynchronous close protocol.
    connect(m_titleBar, &TitleBarWidget::closeRequested, this, &QWidget::close);
    connect(m_titleBar, &TitleBarWidget::systemMoveRequested,
            m_windowFrame, &WindowFrameController::startSystemMove);
    connect(m_titleBar, &TitleBarWidget::systemMenuRequested,
            m_windowFrame, &WindowFrameController::showSystemMenu);

    // The library composes its existing sidebar and directory view. The main
    // window supplies the single subtitle pane and playback footer once.
    m_libraryPanel = new LibraryPanel(this);
    m_libraryPanel->setObjectName(QStringLiteral("libraryPanel"));
    m_libraryPanel->setImportAvailable(false);
    m_libraryPanel->setApplicationTitleVisible(false);
    // Startup already bounds the usual window minimum to the available screen.
    // Share that reduced logical width instead of overflowing a small screen.
    const int sidebarMinimum = qBound(120, minimumWidth() - 38 - 360, 180);
    m_libraryPanel->sidebarWidget()->setMinimumWidth(sidebarMinimum);
    m_subtitlePanel = new SubtitlePanel(m_libraryPanel);
    m_subtitlePanel->setObjectName(QStringLiteral("subtitlePanel"));
    m_subtitlePanel->setAttribute(Qt::WA_StyledBackground);
    m_subtitlePanel->setMinimumWidth(260);
    m_workspaceSplitter = m_libraryPanel->workspaceSplitter();
    m_workspaceSplitter->addWidget(m_subtitlePanel);
    m_workspaceSplitter->setStretchFactor(0, 1);
    m_workspaceSplitter->setStretchFactor(1, 0);

    auto *footer = new QWidget(m_libraryPanel);
    m_playbackFooter = footer;
    footer->setObjectName(QStringLiteral("playbackFooter"));
    footer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // Reserve the tested compact width for normal desktops. On a smaller
    // logical screen, 38px accounts for the outer margins and splitter handle.
    footer->setMinimumWidth(qMin(510, qMax(240, minimumWidth() - 38 - sidebarMinimum)));
    m_playbackControls = new PlaybackControlsWidget(footer);
    m_playbackControls->setObjectName(QStringLiteral("playbackControls"));
    m_playbackControls->setAttribute(Qt::WA_StyledBackground);
    m_playbackControls->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *footerLayout = new QVBoxLayout(footer);
    footerLayout->setContentsMargins(0, 0, 0, 0);
    footerLayout->setSpacing(4);
    footerLayout->addWidget(m_playbackControls);
    m_libraryPanel->setWorkspaceFooter(footer);

    auto *body = new QWidget(this);
    body->setObjectName(QStringLiteral("mainWindowBody"));
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(16, 12, 16, 12);
    bodyLayout->setSpacing(0);
    m_contentStack = new QStackedWidget(body);
    m_contentStack->setObjectName(QStringLiteral("playbackContentStack"));
    m_contentStack->addWidget(m_libraryPanel);
    m_playbackDetailPage = new PlaybackDetailWidget(m_contentStack);
    m_playbackDetailPage->setObjectName(QStringLiteral("playbackDetailPage"));
    m_playbackDetailLayout = new QVBoxLayout(m_playbackDetailPage);
    m_playbackDetailLayout->setContentsMargins(12, 8, 12, 12);
    m_playbackDetailLayout->setSpacing(8);
    auto *detailHeader = new QHBoxLayout;
    QPixmap collapseIcon(20, 20);
    collapseIcon.fill(Qt::transparent);
    {
        QPainter painter(&collapseIcon);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor("#F0F2F5"), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(QPointF(5, 7), QPointF(10, 12));
        painter.drawLine(QPointF(10, 12), QPointF(15, 7));
    }
    m_collapsePlaybackDetailButton = new QPushButton(QIcon(collapseIcon),
        QStringLiteral("收起播放详情"), m_playbackDetailPage);
    m_collapsePlaybackDetailButton->setObjectName(QStringLiteral("collapsePlaybackDetailButton"));
    m_collapsePlaybackDetailButton->setToolTip(QStringLiteral("收起播放详情（Esc）"));
    m_collapsePlaybackDetailButton->setAccessibleName(QStringLiteral("收起播放详情"));
    m_collapsePlaybackDetailButton->setCursor(Qt::PointingHandCursor);
    detailHeader->addWidget(m_collapsePlaybackDetailButton);
    detailHeader->addStretch();
    m_playbackDetailLayout->addLayout(detailHeader);
    m_contentStack->addWidget(m_playbackDetailPage);
    m_contentStack->setCurrentWidget(m_libraryPanel);
    bodyLayout->addWidget(m_contentStack);
    m_playbackDetailTransition = new PlaybackDetailTransition(m_contentStack);
    connect(m_playbackDetailTransition, &PlaybackDetailTransition::finished, this, [this](bool detail) {
        if (detail == m_playbackDetailVisible) finishPlaybackDetailTransition();
    });
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_titleBar);
    layout->addWidget(body, 1);
    auto *brandObserver = new BrandAlignmentObserver(this, [this] { syncTitleBrandAlignment(); });
    m_libraryPanel->sidebarWidget()->installEventFilter(brandObserver);
    m_titleBar->installEventFilter(brandObserver);
    if (auto *importButton = m_libraryPanel->findChild<QWidget *>(QStringLiteral("libraryImportMenuButton")))
        importButton->installEventFilter(brandObserver);
    m_queuePanel = new PlaybackQueuePanel(this);
    m_floatingAppearanceDialog = new FloatingSubtitleAppearanceDialog(this);
    m_floatingAppearanceDialog->setAppearance(m_floatingSubtitleWindow->appearance());
    // Explicit initialization is required even when no compact-width transition
    // happens. Panel visibility is never persisted or used as subtitle state.
    applySubtitlePanelVisibility(false);
    updateWorkspaceLayout();
    m_windowFrame->syncWindowState();
}

void MainWindow::syncTitleBrandAlignment()
{
    if (!m_titleBar || !m_libraryPanel) return;
    auto *sidebar = m_libraryPanel->sidebarWidget();
    if (!sidebar) return;
    m_titleBar->setBrandAlignmentRect(
        QRect(m_titleBar->mapFrom(this, sidebar->mapTo(this, QPoint())), sidebar->size()));
    m_titleBar->setBrandIconCenterX(
        m_titleBar->mapFrom(this,
            m_libraryPanel->mapTo(this, m_libraryPanel->importIconCenter())).x());
}

void MainWindow::toggleMaximized()
{
    if (m_closing) return;
    if (isMaximized()) showNormal();
    else showMaximized();
}

void MainWindow::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (m_windowFrame && (event->type() == QEvent::WindowStateChange
                         || event->type() == QEvent::ActivationChange))
        m_windowFrame->syncWindowState();
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    if (m_windowFrame && m_windowFrame->handleNativeEvent(eventType, message, result))
        return true;
    return QWidget::nativeEvent(eventType, message, result);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateWorkspaceLayout();
}

void MainWindow::applySubtitlePanelVisibility(bool visible)
{
    if (m_playbackDetailVisible) return;
    m_subtitlePanel->setVisible(visible);
    m_playbackControls->setSubtitlePanelVisible(visible);
    if (visible)
        m_workspaceSplitter->setSizes({qMax(220, m_workspaceSplitter->width() - 328), 320});
}

void MainWindow::updateWorkspaceLayout()
{
    if (!m_workspaceSplitter || !m_playbackControls)
        return;
    const int sidebarMinimum = qBound(120, width() - 38 - 360, 180);
    const int footerMinimum = qMin(510, qMax(240, width() - 38 - sidebarMinimum));
    m_libraryPanel->sidebarWidget()->setMinimumWidth(sidebarMinimum);
    m_playbackControls->parentWidget()->setMinimumWidth(footerMinimum);
    m_workspaceSplitter->parentWidget()->setMinimumWidth(footerMinimum);
    updateQueuePanelGeometry();
    syncTitleBrandAlignment();
    const bool compact = width() < 980;
    if (compact == m_compactLayout)
        return;
    m_compactLayout = compact;
    // Automatic compact folding does not overwrite the user's explicit choice.
    if (!m_playbackDetailVisible)
        applySubtitlePanelVisibility(!compact && m_subtitlePanelRequested);
}

namespace {

struct LibraryQuerySnapshot {
    bool success = false;
    QVector<LibraryWork> works;
    QVector<LibraryFile> files;
    QSet<qint64> subtitleAudioFileIds;
    qint64 selectedWorkId = 0;
    QString error;
};

QString libraryDiagnostics(const QVector<LibraryDiagnostic> &diagnostics)
{
    QStringList lines;
    for (const auto &diagnostic : diagnostics)
        lines.append(diagnostic.path + QStringLiteral(": ") + diagnostic.message);
    return lines.join(QLatin1Char('\n'));
}

bool sameWorkIdentity(const LibraryWork &first, const LibraryWork &second)
{
    return first.id == second.id && first.rootPath == second.rootPath
        && first.importedAtMs == second.importedAtMs;
}

struct WorkFolderResult {
    QString directory;
    QString error;
};

// Worker-only: do not follow a root/ancestor that has become a link, and prove
// that the exact registered work still exists after checking its directory.
WorkFolderResult checkedWorkFolder(const QString &database, const LibraryWork &expected,
                                   const std::atomic_bool &cancelled)
{
    const QString root = ScanRootStore::normalizePath(expected.rootPath);
    if (root.isEmpty()) return {{}, QStringLiteral("作品根目录路径无效，未打开。")};
    QString current = root;
    for (;;) {
        if (cancelled.load()) return {};
        const QFileInfo info(current);
        if (!info.isDir() || !info.isReadable() || info.isSymLink() || info.isJunction())
            return {{}, QStringLiteral("作品根目录不存在、无法访问或包含链接，未打开。")};
#ifdef Q_OS_WIN
        const auto native = QDir::toNativeSeparators(current);
        const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(native.utf16()));
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return {{}, QStringLiteral("作品根目录不存在、无法访问或包含链接，未打开。")};
#endif
        const QString parent = info.absolutePath();
        if (parent == current || QDir(current).isRoot()) break;
        current = parent;
    }
    std::error_code error;
#ifdef Q_OS_WIN
    const std::filesystem::path path(root.toStdWString());
#else
    const std::filesystem::path path(QFile::encodeName(root).constData());
#endif
    const std::filesystem::directory_iterator entries(path, error);
    if (error) return {{}, QStringLiteral("作品根目录无法访问，未打开。")};
    if (cancelled.load()) return {};
    LibraryRepository repository(database);
    const auto opened = repository.open();
    if (!opened.success)
        return {{}, QStringLiteral("无法核验作品身份：") + libraryDiagnostics(opened.diagnostics)};
    const auto works = repository.works();
    if (!works.success)
        return {{}, QStringLiteral("无法核验作品身份：") + libraryDiagnostics(works.diagnostics)};
    const auto found = std::find_if(works.works.cbegin(), works.works.cend(), [&expected](const auto &work) {
        return sameWorkIdentity(work, expected);
    });
    if (found == works.works.cend()) return {{}, QStringLiteral("作品已删除或身份已变化，请重新选择。")};
    return cancelled.load() ? WorkFolderResult{} : WorkFolderResult{root, {}};
}

QString importDiagnostics(const LibraryImportTaskResult &result)
{
    QStringList lines;
    for (const auto &diagnostic : result.scan.diagnostics) {
        lines.append(QStringLiteral("[%1] %2: %3")
            .arg(diagnostic.severity == ScanDiagnosticSeverity::Error ? QStringLiteral("错误")
                : diagnostic.severity == ScanDiagnosticSeverity::Warning ? QStringLiteral("警告")
                                                                         : QStringLiteral("信息"),
                 diagnostic.absolutePath, diagnostic.message));
    }
    if (result.scan.suppressedDiagnostics)
        lines.append(QStringLiteral("另有 %1 条扫描诊断被扫描器上限省略。")
                         .arg(result.scan.suppressedDiagnostics));
    const QString sql = libraryDiagnostics(result.repositoryResult.diagnostics);
    if (!sql.isEmpty())
        lines.append(sql);
    const QString association = libraryDiagnostics(result.autoAssociation.diagnostics);
    if (!association.isEmpty())
        lines.append(QStringLiteral("字幕自动关联：") + association);
    if (!result.errorMessage.isEmpty())
        lines.append(result.errorMessage);
    if (!result.references.error.isEmpty())
        lines.append(QStringLiteral("参考资料：") + result.references.error);
    for (const auto &warning : result.references.warnings)
        lines.append(QStringLiteral("参考资料：") + warning);
    if (result.references.failedCount)
        lines.append(QStringLiteral("参考资料：%1 份资料提取失败，详情见参考资料窗口。")
            .arg(result.references.failedCount));
    return lines.join(QLatin1Char('\n'));
}

QString scanSummary(const DirectoryScanResult &scan)
{
    return QStringLiteral("扫描快照：访问 %1 项，音频 %2，字幕 %3，跳过 %4")
        .arg(scan.visitedEntries).arg(scan.audioFiles.size())
        .arg(scan.subtitleFiles.size()).arg(scan.skippedEntries);
}

LibraryQuerySnapshot queryLibrary(const QString &path, qint64 preferredWorkId)
{
    LibraryQuerySnapshot snapshot;
    // All filesystem access, schema checks and SQL queries stay off the UI thread.
    if (path.isEmpty() || path == QStringLiteral(":memory:")) {
        snapshot.error = QStringLiteral("作品库需要有效的文件数据库路径，不支持空路径或 :memory: 独立内存连接。");
        return snapshot;
    }
    const QString parentDirectory = QFileInfo(path).absolutePath();
    if (path != QStringLiteral(":memory:") && !QDir().mkpath(parentDirectory)) {
        snapshot.error = QStringLiteral("无法创建作品库数据目录：%1").arg(parentDirectory);
        return snapshot;
    }
    LibraryRepository repository(path);
    const auto opened = repository.open();
    if (!opened.success) {
        snapshot.error = QStringLiteral("无法打开作品库数据库：\n%1")
                             .arg(libraryDiagnostics(opened.diagnostics));
        return snapshot;
    }
    const auto works = repository.works();
    if (!works.success) {
        snapshot.error = libraryDiagnostics(works.diagnostics);
        return snapshot;
    }
    snapshot.works = works.works;
    for (const auto &work : snapshot.works) {
        if (work.id == preferredWorkId) {
            snapshot.selectedWorkId = work.id;
            break;
        }
    }
    // A negative preference explicitly leaves browsing empty after the user
    // removes the work being viewed. Ordinary refresh/startup keeps its fallback.
    if (!snapshot.selectedWorkId && preferredWorkId >= 0 && !snapshot.works.isEmpty())
        snapshot.selectedWorkId = snapshot.works.first().id;
    if (snapshot.selectedWorkId) {
        const auto files = repository.workBrowseSnapshot(snapshot.selectedWorkId);
        if (!files.success) {
            snapshot.error = libraryDiagnostics(files.diagnostics);
            return snapshot;
        }
        snapshot.files = files.files;
        snapshot.subtitleAudioFileIds = files.subtitleAudioFileIds;
    }
    const auto closed = repository.close();
    if (!closed.success) {
        snapshot.error = libraryDiagnostics(closed.diagnostics);
        return snapshot;
    }
    snapshot.success = true;
    return snapshot;
} // repository is destroyed before publishing the value snapshot.

} // namespace

void MainWindow::connectLibrary()
{
    m_libraryImportService = new LibraryImportService(m_libraryDatabasePath, this);
    connect(m_libraryPanel, &LibraryPanel::openAudioRequested,
            this, &MainWindow::openAudioFile);
    connect(m_libraryPanel, &LibraryPanel::importDirectoryRequested,
            this, &MainWindow::openLibraryDirectory);
    connect(m_libraryPanel, &LibraryPanel::cancelImportRequested,
            this, &MainWindow::cancelLibraryImport);
    connect(m_libraryPanel, &LibraryPanel::trackPlayRequested,
            this, &MainWindow::playLibraryTrack);
    connect(m_libraryPanel, &LibraryPanel::workSelected,
            this, &MainWindow::refreshLibrary);
    connect(m_libraryPanel, &LibraryPanel::workRemovalRequested,
            this, &MainWindow::requestLibraryWorkRemoval);
    connect(m_libraryPanel, &LibraryPanel::workFolderRequested,
            this, &MainWindow::openWorkFolder);
    // submit() emits its first state synchronously. Queue all notifications so
    // the accepted ID is installed before any of them can update the panel.
    connect(m_libraryImportService, &LibraryImportService::stateChanged,
            this, &MainWindow::applyLibraryImportState, Qt::QueuedConnection);
    connect(m_libraryImportService, &LibraryImportService::scanCompleted,
            this, &MainWindow::applyLibraryScan, Qt::QueuedConnection);
    connect(m_libraryImportService, &LibraryImportService::importCommitted,
            this, &MainWindow::applyLibraryImportCommitted, Qt::QueuedConnection);
    connect(m_libraryImportService, &LibraryImportService::taskFinished,
            this, &MainWindow::applyLibraryImportResult, Qt::QueuedConnection);
    connect(m_libraryImportService, &LibraryImportService::shutdownFinished, this, [this]() {
        m_libraryShutdownFinished = true;
        finishCloseIfReady();
    }, Qt::QueuedConnection);
    connectBatchImport();
    connectScanRoots();
    connectWorkReferences();
}

void MainWindow::openLibraryDirectory()
{
    if (m_closing)
        return;
    if (libraryScanBusy()) {
        m_libraryPanel->setImportError(QStringLiteral("Busy：已有导入任务，请等待完成或取消扫描。"));
        return;
    }
    const QString directory = QFileDialog::getExistingDirectory(
        this, QStringLiteral("导入作品目录"), QString(), QFileDialog::ShowDirsOnly);
    if (!directory.isEmpty())
        submitLibraryImport(directory);
}

void MainWindow::openWorkFolder(qint64 workId)
{
    if (m_closing || !m_libraryDatabaseReady || libraryScanBusy()
        || m_libraryQueryInProgress || m_libraryQueryPending || workId <= 0
        || m_removedLibraryWorkIds.contains(workId)) return;
    const auto found = std::find_if(m_libraryWorksSnapshot.cbegin(), m_libraryWorksSnapshot.cend(),
        [workId](const auto &work) { return work.id == workId; });
    if (found == m_libraryWorksSnapshot.cend()) return;
    const auto expected = *found;
    if (m_workFolderCancelled) m_workFolderCancelled->store(true);
    m_workFolderCancelled = std::make_shared<std::atomic_bool>(false);
    const auto cancelled = m_workFolderCancelled;
    const auto generation = ++m_workFolderGeneration;
    const auto libraryGeneration = m_libraryQueryGeneration;
    ++m_workFolderWorkers;
    auto promise = std::make_shared<QPromise<WorkFolderResult>>();
    auto *watcher = new QFutureWatcher<WorkFolderResult>(this);
    connect(watcher, &QFutureWatcher<WorkFolderResult>::finished, this,
        [this, watcher, generation, libraryGeneration, expected, cancelled] {
        const auto result = watcher->result();
        watcher->deleteLater(); --m_workFolderWorkers;
        const auto current = std::find_if(m_libraryWorksSnapshot.cbegin(), m_libraryWorksSnapshot.cend(),
            [&expected](const auto &work) { return sameWorkIdentity(work, expected); });
        if (!m_closing && !cancelled->load() && generation == m_workFolderGeneration
            && libraryGeneration == m_libraryQueryGeneration && !libraryScanBusy()
            && !m_libraryQueryInProgress && !m_libraryQueryPending
            && !m_removedLibraryWorkIds.contains(expected.id) && current != m_libraryWorksSnapshot.cend()) {
            if (!result.error.isEmpty()) m_libraryPanel->setImportError(result.error);
            else if (!result.directory.isEmpty() && !QDesktopServices::openUrl(QUrl::fromLocalFile(result.directory)))
                m_libraryPanel->setImportError(QStringLiteral("无法打开作品文件夹。"));
        }
        finishCloseIfReady();
    });
    watcher->setFuture(promise->future());
    const auto hook = m_beforeWorkFolderCheckTestHook;
    QThreadPool::globalInstance()->start([promise, database = m_libraryDatabasePath, expected, cancelled, hook] {
        promise->start();
        WorkFolderResult result;
        try {
            if (hook) (*hook)();
            if (!cancelled->load()) result = checkedWorkFolder(database, expected, *cancelled);
        } catch (...) { result.error = QStringLiteral("检查作品文件夹失败，请重试。"); }
        promise->addResult(std::move(result)); promise->finish();
    });
}

void MainWindow::submitLibraryImport(const QString &directory, const DirectoryScanOptions &options)
{
    if (m_closing)
        return;
    if (libraryScanBusy()) {
        m_libraryPanel->setImportError(QStringLiteral("Busy：已有导入任务，请等待最终结果。"));
        return;
    }
    if (!m_libraryDatabaseReady) {
        m_libraryPanel->setImportError(QStringLiteral("作品库尚未成功打开，请先查看数据库状态。"));
        return;
    }
    const auto submission = m_libraryImportService->submit(directory, options);
    if (submission.status != LibraryImportSubmissionStatus::Accepted) {
        m_libraryPanel->setImportError(submission.status == LibraryImportSubmissionStatus::Busy
            ? QStringLiteral("Busy：已有导入任务。") : QStringLiteral("当前无法提交导入请求，服务正在关闭或线程不正确。"));
        return;
    }
    m_libraryTaskId = submission.taskId;
    m_libraryImportState = LibraryImportState::Scanning;
    m_libraryScanSummary.clear();
    m_libraryPanel->setImportFeedback(QString());
    m_libraryPanel->setImportState(m_libraryImportState, QStringLiteral("正在扫描作品目录…"), true);
    updateScanRootManager();
}

void MainWindow::cancelLibraryImport()
{
    if (!m_closing && m_libraryRootRefreshService->isActive()) {
        m_libraryRootRefreshService->cancel();
        return;
    }
    if (!m_closing && m_libraryBatchImportService->isActive()) {
        m_libraryBatchImportService->cancel();
        return;
    }
    if (m_closing || m_libraryTaskId.isEmpty())
        return;
    switch (m_libraryImportService->cancelCurrent()) {
    case LibraryImportCancelResult::Requested:
    case LibraryImportCancelResult::AlreadyRequested:
        applyLibraryImportState(m_libraryTaskId, LibraryImportState::Cancelling);
        m_libraryPanel->setImportFeedback(m_libraryManualRemovalTaskId == m_libraryTaskId
            ? QStringLiteral("已请求取消删除，正在等待最终结果。")
            : QStringLiteral("已请求取消，正在等待扫描实际停止。"));
        break;
    case LibraryImportCancelResult::TooLate:
        m_libraryPanel->setImportFeedback(
            QStringLiteral("TooLate：已进入不可取消阶段或任务正在结束，等待最终结果；尚未报告已取消。"));
        break;
    case LibraryImportCancelResult::NoActiveTask:
        m_libraryPanel->setImportFeedback(QStringLiteral("任务已结束，正在等待最终结果。"));
        break;
    case LibraryImportCancelResult::WrongThread:
        m_libraryPanel->setImportError(QStringLiteral("取消请求线程不正确。"));
        break;
    }
}

void MainWindow::applyLibraryImportState(const QString &taskId, LibraryImportState state)
{
    if (m_closing || taskId.isEmpty() || taskId != m_libraryTaskId)
        return;
    // A queued Scanning notification can arrive after a direct cancel request.
    if (state == LibraryImportState::Scanning && m_libraryImportState != LibraryImportState::Scanning)
        return;
    if (state == LibraryImportState::Committing && m_libraryImportState == LibraryImportState::Cancelling)
        return;
    m_libraryImportState = state;
    updateScanRootManager();
    if (taskId == m_libraryBatchChildTaskId || taskId == m_libraryRootChildTaskId)
        return; // The batch coordinator owns progress and keeps both entries busy.
    QString text;
    switch (state) {
    case LibraryImportState::Scanning: text = QStringLiteral("正在扫描作品目录…"); break;
    case LibraryImportState::Committing: text = QStringLiteral("正在提交作品库，提交阶段不可取消。"); break;
    case LibraryImportState::ProcessingReferences: text = QStringLiteral("作品已提交，正在提取和缓存参考资料…"); break;
    case LibraryImportState::Cancelling: text = QStringLiteral("正在取消，等待扫描实际停止…"); break;
    case LibraryImportState::Completed: text = QStringLiteral("数据库已提交，正在处理结果…"); break;
    case LibraryImportState::Cancelled: text = QStringLiteral("扫描已停止，正在处理取消结果…"); break;
    case LibraryImportState::Failed: text = QStringLiteral("任务失败，正在处理诊断…"); break;
    case LibraryImportState::Idle: text = QStringLiteral("空闲"); break;
    }
    if (taskId == m_libraryManualRemovalTaskId) {
        switch (state) {
        case LibraryImportState::Scanning: text = QStringLiteral("正在核对待删除作品…"); break;
        case LibraryImportState::Committing: text = QStringLiteral("正在从作品库移除；提交阶段不可取消，本地文件保留。"); break;
        case LibraryImportState::Cancelling: text = QStringLiteral("正在取消删除，等待最终结果…"); break;
        case LibraryImportState::Cancelled: text = QStringLiteral("删除已取消，正在处理结果…"); break;
        default: break;
        }
    }
    if (!m_libraryScanSummary.isEmpty())
        text += QLatin1Char('\n') + m_libraryScanSummary;
    m_libraryPanel->setImportState(state, text, true);
}

void MainWindow::applyLibraryScan(const QString &taskId, const DirectoryScanResult &scan)
{
    if (m_closing || taskId.isEmpty() || taskId != m_libraryTaskId)
        return;
    m_libraryScanSummary = scanSummary(scan);
    applyLibraryImportState(taskId, m_libraryImportState);
}

void MainWindow::applyLibraryImportCommitted(const QString &taskId, const LibraryImportResult &result)
{
    if (taskId != m_libraryTaskId || !result.success)
        return;
    if (result.removed) {
        forgetRemovedLibraryWork(result.workId);
        if (!m_closing) {
            const auto selected = m_libraryPanel->selectedWorkId();
            refreshLibrary(taskId == m_libraryManualRemovalTaskId
                && (selected == result.workId || selected == 0) ? -1 : 0);
        }
        return;
    }
    if (m_closing)
        return;
    m_libraryAssociationWorkId = result.workId;
    invalidateTrackDurations(result.workId);
    const bool rootChild = taskId == m_libraryRootChildTaskId;
    const bool aiRefresh = taskId == m_aiSubtitleRefreshTaskId;
    const bool batchChild = taskId == m_libraryBatchChildTaskId || rootChild || aiRefresh;
    if (!batchChild)
        m_libraryPanel->setImportState(LibraryImportState::Committing,
            QStringLiteral("作品已提交；字幕自动关联处理中，结果尚未就绪。"), true);
    refreshLibrary(batchChild ? 0 : result.workId);
    if (!rootChild && !aiRefresh && m_playingLibraryWorkId == result.workId && !m_manualSubtitleChoice)
        requestLibrarySubtitles();
}

void MainWindow::applyLibraryImportResult(const LibraryImportTaskResult &result)
{
    if (result.taskId.isEmpty() || result.taskId != m_libraryTaskId)
        return;
    const bool manualRemoval = result.taskId == m_libraryManualRemovalTaskId;
    const bool aiRefresh = result.taskId == m_aiSubtitleRefreshTaskId;
    if (aiRefresh)
        m_aiSubtitleRefreshTaskId.clear();
    if (result.databaseCommitted && result.repositoryResult.removed)
        forgetRemovedLibraryWork(result.repositoryResult.workId);
    const bool rootChild = result.taskId == m_libraryRootChildTaskId;
    const bool retryRootSubtitle = rootChild && m_rootSubtitleWaitAudioGeneration
        && isCurrentSubtitleRequest(m_rootSubtitleWaitAudioGeneration,
            m_rootSubtitleWaitRequestGeneration, m_rootSubtitleWaitWorkId, m_rootSubtitleWaitFileId);
    if (rootChild)
        m_rootSubtitleWaitAudioGeneration = 0;
    const bool batchChild = result.taskId == m_libraryBatchChildTaskId || rootChild;
    m_libraryTaskId.clear();
    if (manualRemoval) {
        m_libraryManualRemovalTaskId.clear();
        m_libraryManualRemovalWorkId = 0;
        m_libraryPanel->setWorkRemovalActive(false);
    }
    m_libraryBatchChildTaskId.clear();
    m_libraryRootChildTaskId.clear();
    m_libraryAssociationWorkId = 0;
    updateScanRootManager();
    if (m_closing)
        return;
    if (manualRemoval) {
        m_libraryImportState = result.state;
        const bool removed = result.databaseCommitted && result.repositoryResult.removed;
        const QString text = removed
            ? QStringLiteral("作品已从作品库移除；本地文件保留，后续扫描可能重新导入。")
            : result.state == LibraryImportState::Cancelled
                ? QStringLiteral("删除已取消，作品库未修改。")
                : QStringLiteral("删除失败，作品库未提交；已有作品保持不变。");
        m_libraryPanel->setImportFeedback({}, importDiagnostics(result));
        m_libraryPanel->setImportState(result.state, text, false);
        if (removed) {
            const auto selected = m_libraryPanel->selectedWorkId();
            refreshLibrary(selected == result.repositoryResult.workId || selected == 0 ? -1 : 0);
        } else if (result.state != LibraryImportState::Cancelled) {
            if (!m_workRemovalErrorDialog) {
                m_workRemovalErrorDialog = new PlayerMessageBox(QMessageBox::Warning,
                    QStringLiteral("删除作品失败"), {}, QMessageBox::Ok, this);
                m_workRemovalErrorDialog->setObjectName(QStringLiteral("workRemovalErrorDialog"));
                m_workRemovalErrorDialog->setTextFormat(Qt::PlainText);
                m_workRemovalErrorDialog->setWindowModality(Qt::WindowModal);
            }
            m_workRemovalErrorDialog->setText(text);
            m_workRemovalErrorDialog->setDetailedText(importDiagnostics(result));
            m_workRemovalErrorDialog->open();
        }
        return;
    }
    if (result.databaseCommitted && !result.repositoryResult.removed)
        refreshCoverArtForWork(result.repositoryResult.workId);
    if (aiRefresh) {
        m_libraryImportState = result.state;
        const bool associated = result.databaseCommitted
            && result.autoAssociation.status == LibraryAutoAssociationStatus::Succeeded;
        m_libraryPanel->setImportFeedback({}, importDiagnostics(result));
        m_libraryPanel->setImportState(result.state, associated
            ? QStringLiteral("生成字幕所属作品已更新，字幕关联已处理。")
            : QStringLiteral("字幕文件已保存；作品更新或关联未完成，可稍后重新扫描。"), false);
        if (!associated)
            m_libraryPanel->setImportError(QStringLiteral("字幕文件已保留，但作品更新或关联未完成。"));
        if (result.databaseCommitted) {
            refreshLibrary();
            if (m_chickenRiceTranslateDialog) m_chickenRiceTranslateDialog->refreshTranslationSettings();
            if (m_playingLibraryWorkId == result.repositoryResult.workId) {
                refreshBoundSubtitleChoices();
                if (!m_manualSubtitleChoice)
                    requestLibrarySubtitles();
            }
        }
        return;
    }
    if (batchChild) {
        // Do not let each completed work take over the user's browse selection.
        if (result.databaseCommitted) {
            refreshLibrary();
            if (m_playingLibraryWorkId == result.repositoryResult.workId) {
                refreshBoundSubtitleChoices();
                if ((!rootChild || retryRootSubtitle) && !m_manualSubtitleChoice)
                    requestLibrarySubtitles();
            }
        }
        return;
    }
    m_libraryImportState = result.state;
    QString text;
    if (result.databaseCommitted) {
        text = QStringLiteral("导入完成，数据库已提交。新增文件 %1，更新 %2，缺失记录 %3。")
            .arg(result.repositoryResult.insertedFiles).arg(result.repositoryResult.updatedFiles)
            .arg(result.repositoryResult.missingFiles);
    } else if (result.state == LibraryImportState::Cancelled) {
        text = QStringLiteral("导入已取消，数据库未修改。");
    } else {
        text = QStringLiteral("导入失败，数据库未提交；已有作品保持不变。");
    }
    if (result.databaseCommitted) {
        switch (result.autoAssociation.status) {
        case LibraryAutoAssociationStatus::Succeeded:
            text += QStringLiteral("\n字幕关联完成：新增 %1，已有 %2。")
                .arg(result.autoAssociation.addedBindings).arg(result.autoAssociation.existingBindings);
            break;
        case LibraryAutoAssociationStatus::Failed:
            text += QStringLiteral("\n作品导入成功；字幕自动关联失败，作品数据已保留。"); break;
        case LibraryAutoAssociationStatus::StaleSnapshot:
            text += QStringLiteral("\n作品导入成功；字幕快照已过期，本批关联未应用。"); break;
        case LibraryAutoAssociationStatus::NotRun:
            text += QStringLiteral("\n字幕自动关联未执行。"); break;
        }
        if (result.references.status == WorkReferenceSyncStatus::Succeeded)
            text += QStringLiteral("\n参考资料同步完成：可用 %1，失败 %2，移除 %3。")
                .arg(result.references.readyCount).arg(result.references.failedCount).arg(result.references.removedCount);
        else if (result.references.status == WorkReferenceSyncStatus::Failed
            || result.references.status == WorkReferenceSyncStatus::Cancelled)
            text += QStringLiteral("\n作品已保留；参考资料同步未完成，详情见参考资料窗口。");
    }
    text += QLatin1Char('\n') + scanSummary(result.scan);
    m_libraryPanel->setImportFeedback(QString(), importDiagnostics(result));
    m_libraryPanel->setImportState(result.state, text, false);
    if (result.databaseCommitted
        && (result.autoAssociation.status == LibraryAutoAssociationStatus::Failed
            || result.autoAssociation.status == LibraryAutoAssociationStatus::StaleSnapshot))
        m_libraryPanel->setImportError(result.autoAssociation.status == LibraryAutoAssociationStatus::Failed
            ? QStringLiteral("作品已导入，但字幕自动关联失败。")
            : QStringLiteral("作品已导入，但字幕快照已过期，自动关联未完成。"));
    // Only a confirmed commit refreshes stored work/file records.
    if (result.databaseCommitted) {
        refreshLibrary(result.repositoryResult.workId);
        if (m_chickenRiceTranslateDialog) m_chickenRiceTranslateDialog->refreshTranslationSettings();
        if (m_playingLibraryWorkId == result.repositoryResult.workId) {
            refreshBoundSubtitleChoices();
            if (!m_manualSubtitleChoice)
                requestLibrarySubtitles();
        }
    }
}

void MainWindow::refreshLibrary(qint64 preferredWorkId)
{
    if (m_closing)
        return;
    // A new SQL snapshot must not accept duration replies for the old page.
    m_trackDurationService->setDemand({});
    ++m_subtitleBadgeQueryGeneration;
    m_subtitleBadgeQueryPending = false;
    m_subtitleBadgeRequestedWorkId = 0;
    ++m_libraryQueryGeneration;
    m_libraryRequestedWorkId = preferredWorkId ? preferredWorkId : m_libraryPanel->selectedWorkId();
    m_libraryQueryPending = true;
    m_libraryPanel->setQuerying(true);
    if (!m_libraryQueryInProgress)
        startLibraryQuery();
}

void MainWindow::startLibraryQuery()
{
    m_libraryQueryInProgress = true;
    m_libraryQueryPending = false;
    const quint64 generation = m_libraryQueryGeneration;
    const qint64 workId = m_libraryRequestedWorkId;
    const QString path = m_libraryDatabasePath;
    auto promise = std::make_shared<QPromise<LibraryQuerySnapshot>>();
    auto *watcher = new QFutureWatcher<LibraryQuerySnapshot>(this);
    connect(watcher, &QFutureWatcher<LibraryQuerySnapshot>::finished, this,
            [this, watcher, generation]() {
        const auto snapshot = watcher->result();
        watcher->deleteLater();
        m_libraryQueryInProgress = false;
        if (!m_closing && generation == m_libraryQueryGeneration) {
            m_libraryDatabaseReady = snapshot.success;
            m_libraryPanel->setImportAvailable(snapshot.success);
            if (snapshot.success) {
                m_libraryWorksSnapshot = snapshot.works;
                m_libraryFilesSnapshot = snapshot.files;
                m_librarySnapshotWorkId = snapshot.selectedWorkId;
                syncWorkCoverCatalog();
                m_libraryPanel->setLibrary(snapshot.works, snapshot.files, snapshot.selectedWorkId,
                                           snapshot.subtitleAudioFileIds);
                updateCoverActions();
                QSet<qint64> missingFileIds;
                for (const auto &file : snapshot.files)
                    if (file.missing) missingFileIds.insert(file.id);
                QVector<quint64> missingQueueIds;
                for (const auto &entry : m_playbackQueue.entries()) {
                    if (entry.workId != snapshot.selectedWorkId) continue;
                    if (missingFileIds.contains(entry.fileId))
                        missingQueueIds.append(entry.id);
                }
                for (const auto id : missingQueueIds)
                    m_playbackQueue.setIssue(id, QStringLiteral("最近完整扫描标记缺失"), true);
                updatePlaybackQueue();
                tryStartAiSubtitleRefresh();
                scheduleStartupRootRefresh();
            } else {
                m_libraryPanel->setLibraryError(snapshot.error);
            }
        }
        updateScanRootManager();
        if (m_closing) {
            finishCloseIfReady();
        } else if (m_libraryQueryPending) {
            startLibraryQuery();
        } else {
            m_libraryPanel->setQuerying(false);
            tryStartAiSubtitleRefresh();
            if (m_subtitleBadgeQueryPending && !m_subtitleBadgeQueryInProgress)
                startSubtitleBadgeQuery();
        }
    });
    watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([promise, path, workId]() {
        promise->start();
        LibraryQuerySnapshot snapshot;
        try {
            snapshot = queryLibrary(path, workId);
        } catch (const std::exception &exception) {
            snapshot.error = QStringLiteral("查询作品库失败：%1").arg(QString::fromUtf8(exception.what()));
        } catch (...) {
            snapshot.error = QStringLiteral("查询作品库发生未知错误。");
        }
        promise->addResult(std::move(snapshot));
        promise->finish();
    });
}

void MainWindow::refreshSubtitleBadges(qint64 workId)
{
    // Other works have no cached badge projection; their next browse query is fresh.
    if (m_closing || workId <= 0 || workId != m_libraryPanel->selectedWorkId())
        return;
    ++m_subtitleBadgeQueryGeneration;
    m_subtitleBadgeRequestedWorkId = workId;
    m_subtitleBadgeQueryPending = true;
    if (!m_libraryQueryInProgress && !m_libraryQueryPending && !m_subtitleBadgeQueryInProgress)
        startSubtitleBadgeQuery();
}

void MainWindow::startSubtitleBadgeQuery()
{
    const qint64 workId = m_subtitleBadgeRequestedWorkId;
    if (m_closing || workId <= 0 || workId != m_libraryPanel->selectedWorkId()) {
        m_subtitleBadgeQueryPending = false;
        return;
    }
    m_subtitleBadgeQueryPending = false;
    m_subtitleBadgeQueryInProgress = true;
    const quint64 generation = m_subtitleBadgeQueryGeneration;
    const quint64 libraryGeneration = m_libraryQueryGeneration;
    const QString path = m_libraryDatabasePath;
    const auto hook = m_subtitleBadgeTaskTestHook;
    auto promise = std::make_shared<QPromise<LibraryWorkBrowseResult>>();
    auto *watcher = new QFutureWatcher<LibraryWorkBrowseResult>(this);
    connect(watcher, &QFutureWatcher<LibraryWorkBrowseResult>::finished, this,
            [this, watcher, workId, generation, libraryGeneration] {
        const auto result = watcher->result();
        watcher->deleteLater();
        m_subtitleBadgeQueryInProgress = false;
        const bool current = !m_closing && generation == m_subtitleBadgeQueryGeneration
            && libraryGeneration == m_libraryQueryGeneration && !m_libraryQueryInProgress
            && !m_libraryQueryPending && workId == m_librarySnapshotWorkId
            && workId == m_libraryPanel->selectedWorkId() && !m_removedLibraryWorkIds.contains(workId);
        if (current) {
            const auto work = std::find_if(m_libraryWorksSnapshot.cbegin(), m_libraryWorksSnapshot.cend(),
                [workId](const auto &entry) { return entry.id == workId; });
            // A concurrent rescan may have changed file identities. Its full refresh
            // owns that transition; never project a summary onto different files.
            if (result.success && work != m_libraryWorksSnapshot.cend()
                && result.work.rootPath == work->rootPath && result.files == m_libraryFilesSnapshot)
                m_libraryPanel->setSubtitleTracks(workId, result.subtitleAudioFileIds);
            else if (!result.success)
                qWarning().noquote() << QStringLiteral("字幕标签查询失败，保留已有显示：")
                    + libraryDiagnostics(result.diagnostics);
        }
        if (m_closing)
            finishCloseIfReady();
        else if (m_subtitleBadgeQueryPending && !m_libraryQueryInProgress && !m_libraryQueryPending)
            startSubtitleBadgeQuery();
    });
    watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([promise, path, workId, hook] {
        promise->start();
        LibraryWorkBrowseResult result;
        try {
            if (hook) (*hook)(SubtitleBadgeTestPoint::BeforeQuery, workId);
            LibraryRepository repository(path);
            const auto opened = repository.open();
            if (opened.success) {
                result = repository.workBrowseSnapshot(workId);
                const auto closed = repository.close();
                if (!closed.success) result.success = false;
                result.diagnostics += closed.diagnostics;
            } else {
                result.diagnostics = opened.diagnostics;
            }
            if (hook) (*hook)(SubtitleBadgeTestPoint::AfterQuery, workId);
        } catch (const std::exception &exception) {
            result.success = false;
            result.diagnostics.push_back({LibraryDiagnosticCode::DatabaseError, path,
                                          QString::fromUtf8(exception.what())});
        } catch (...) {
            result.success = false;
            result.diagnostics.push_back({LibraryDiagnosticCode::DatabaseError, path,
                                          QStringLiteral("字幕标签查询发生未知错误。")});
        }
        promise->addResult(std::move(result));
        promise->finish();
    });
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_closing && !m_updateInstallPreparing
        && (!m_updateNotifications || m_updateNotifications->isShutdownFinished())
        && m_libraryShutdownFinished && m_batchShutdownFinished && m_rootShutdownFinished
        && !m_libraryQueryInProgress && !m_subtitleBadgeQueryInProgress && m_workFolderWorkers == 0
        && !m_audioOpenInProgress && !m_queuePreparationInProgress
        && !m_subtitleQueryInProgress && !m_subtitleLoadInProgress
        && !m_subtitleChoiceQueryInProgress && !m_preferenceWriteInProgress
        && m_preferenceRequests.isEmpty() && m_progressShutdownFinished && m_coverShutdownFinished
        && m_workCoverShutdownFinished && m_durationShutdownFinished
        && !m_coverMutationInProgress && m_coverMutations.isEmpty() && !m_coverErrorDialog
        && (!m_coverSelectionDialog || m_coverSelectionDialog->isIdle())
        && (!m_workEditDialog || m_workEditDialog->isIdle())
        && (!m_workReferenceDialog || m_workReferenceDialog->isIdle())
        && (!m_chickenRiceTranslateDialog || !m_chickenRiceTranslateDialog->isBusy())
        && acknowledgePlaybackProgressCloseFailure()) {
        event->accept();
        return;
    }
    event->ignore();
    if (m_closing)
        return;
    finishPlaybackProgressForClose();
    m_closing = true;
    if (m_updateInstallCancelled) m_updateInstallCancelled->store(true);
    if (m_updateNotifications) m_updateNotifications->shutdown();
    ++m_workFolderGeneration;
    if (m_workFolderCancelled) m_workFolderCancelled->store(true);
    m_aiSubtitlePendingResults.clear();
    m_aiSubtitlePendingWorks.clear();
    if (m_chickenRiceTranslateDialog) {
        m_chickenRiceTranslateDialog->hide();
        m_chickenRiceTranslateDialog->shutdown();
    }
    finishPlaybackDetailTransition(false);
    if (m_workRemovalConfirmation)
        m_workRemovalConfirmation->reject();
    if (m_scanRootRemovalConfirmation)
        m_scanRootRemovalConfirmation->reject();
    if (m_workRemovalErrorDialog)
        m_workRemovalErrorDialog->hide();
    ++m_subtitleBadgeQueryGeneration;
    m_subtitleBadgeQueryPending = false;
    m_coverRequestId = 0;
    m_coverArtService->shutdown();
    m_workCoverService->shutdown();
    m_trackDurationService->shutdown();
    if (m_coverSelectionDialog) {
        m_coverSelectionDialog->hide();
        m_coverSelectionDialog->shutdown();
    }
    if (m_workEditDialog) {
        m_workEditDialog->hide();
        m_workEditDialog->shutdown();
    }
    if (m_workReferenceDialog) {
        m_workReferenceDialog->hide();
        m_workReferenceDialog->shutdown();
    }
    m_titleBar->setClosing(true);
    cancelQueueTransition();
    m_queuePanel->setClosing(true);
    showPlaybackQueue(false);
    m_progressSourceChanging = true;
    m_audioPlayer->stop();
    m_playbackControls->setEnabled(false);
    ++m_subtitleChoiceQueryGeneration;
    m_subtitleChoiceQueryPending = false;
    resetBoundSubtitleContext();
    m_subtitlePanel->setSelectionEnabled(false);
    updateSubtitleActions();
    ++m_subtitleRequestGeneration;
    m_subtitleQueryPending = false;
    m_subtitleLoadPending = false;
    ++m_audioOpenGeneration;
    m_audioOpenPending = false;
    if (m_audioErrorDialog)
        m_audioErrorDialog->hide();
    if (m_subtitleErrorDialog)
        m_subtitleErrorDialog->hide();
    m_subtitlePanel->setImportEnabled(false);
    m_libraryPanel->settingsMenu()->setEnabled(false);
    m_floatingAppearanceDialog->hide();
    m_toggleFloatingAction->setEnabled(false);
    m_lockFloatingAction->setEnabled(false);
    m_floatingAppearanceAction->setEnabled(false);
    // Independent subtitle controls must stop with the asynchronous close protocol.
    m_floatingSubtitleWindow->hideWindow();
    updateFloatingPlaybackControls();
    ++m_libraryQueryGeneration;
    m_libraryQueryPending = false;
    m_libraryPanel->setClosing(true);
    m_startupRootScanPending = false;
    if (m_scanRootManagerDialog)
        m_scanRootManagerDialog->hide();
    m_libraryRootRefreshService->shutdown();
    m_libraryBatchImportService->shutdown();
    m_libraryImportService->shutdown();
    // No nested event loop or GUI-thread join: the window stays responsive.
}

void MainWindow::finishCloseIfReady()
{
    if (!m_closing || m_updateInstallPreparing
        || (m_updateNotifications && !m_updateNotifications->isShutdownFinished())
        || !m_libraryShutdownFinished || !m_batchShutdownFinished || !m_rootShutdownFinished || m_libraryQueryInProgress
        || m_subtitleBadgeQueryInProgress || m_workFolderWorkers > 0
        || m_audioOpenInProgress || m_queuePreparationInProgress || m_subtitleQueryInProgress
        || m_subtitleLoadInProgress || m_subtitleChoiceQueryInProgress
        || m_preferenceWriteInProgress || !m_preferenceRequests.isEmpty()
        || !m_progressShutdownFinished || !m_coverShutdownFinished || !m_workCoverShutdownFinished
        || !m_durationShutdownFinished
        || (m_chickenRiceTranslateDialog && m_chickenRiceTranslateDialog->isBusy())
        || m_closeScheduled)
        return;
    if (m_scanRootSettingsInProgress || m_coverMutationInProgress || !m_coverMutations.isEmpty() || m_coverErrorDialog
        || (m_coverSelectionDialog && !m_coverSelectionDialog->isIdle())
        || (m_workEditDialog && !m_workEditDialog->isIdle())
        || (m_workReferenceDialog && !m_workReferenceDialog->isIdle()))
        return;
    if (!acknowledgePlaybackProgressCloseFailure())
        return;
    m_closeScheduled = true;
    QMetaObject::invokeMethod(this, [this]() { close(); }, Qt::QueuedConnection);
}

void MainWindow::showFloatingSubtitleAppearance()
{
    if (m_closing)
        return;
    // Reuse one modeless dialog and project the actual window's current settings.
    // Only this explicitly requested settings window may activate, never the subtitles.
    m_floatingAppearanceDialog->setAppearance(m_floatingSubtitleWindow->appearance());
    m_floatingAppearanceDialog->show();
    m_floatingAppearanceDialog->raise();
    m_floatingAppearanceDialog->activateWindow();
}

void MainWindow::applyFloatingSubtitleAppearance(const FloatingSubtitleAppearance &appearance)
{
    if (m_closing)
        return;
    m_floatingSubtitleWindow->setAppearance(appearance);
    m_floatingAppearanceDialog->setAppearance(m_floatingSubtitleWindow->appearance());
}

void MainWindow::createSystemTray()
{
    // Actions also exist without a system tray; all controls use the window's state.
    m_toggleFloatingAction = new QAction(QStringLiteral("显示悬浮字幕"), this);
    m_toggleFloatingAction->setObjectName(QStringLiteral("toggleFloatingSubtitlesAction"));
    m_toggleFloatingAction->setCheckable(true);
    m_toggleFloatingAction->setToolTip(QStringLiteral("显示或隐藏当前同步字幕。"));
    m_lockFloatingAction = new QAction(QStringLiteral("锁定悬浮字幕"), this);
    m_lockFloatingAction->setObjectName(QStringLiteral("lockFloatingSubtitlesAction"));
    m_lockFloatingAction->setCheckable(true);
    m_lockFloatingAction->setToolTip(
        QStringLiteral("锁定后鼠标操作穿透字幕，悬停显示解锁按钮；也可在托盘菜单解锁。"));
    m_floatingAppearanceAction = new QAction(QStringLiteral("悬浮字幕外观"), this);
    m_floatingAppearanceAction->setObjectName(QStringLiteral("floatingSubtitleAppearanceAction"));
    m_restoreMainWindowAction = new QAction(QStringLiteral("恢复主窗口"), this);
    m_restoreMainWindowAction->setObjectName(QStringLiteral("restoreMainWindowAction"));

    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("subtitleTrayMenu"));
    menu->addAction(m_toggleFloatingAction);
    menu->addAction(m_lockFloatingAction);
    menu->addAction(m_floatingAppearanceAction);
    menu->addSeparator();
    menu->addAction(m_restoreMainWindowAction);

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        m_systemTrayIcon = new QSystemTrayIcon(applicationIcon(), this);
        m_systemTrayIcon->setToolTip(applicationDisplayName());
        m_systemTrayIcon->setContextMenu(menu);
        m_systemTrayIcon->show();
    }
}

void MainWindow::configureTabOrder()
{
    setTabOrder(m_titleBar->minimizeButton(), m_titleBar->maximizeButton());
    setTabOrder(m_titleBar->maximizeButton(), m_titleBar->closeButton());
    setTabOrder(m_titleBar->closeButton(), m_libraryPanel);
    // Join the last sidebar/directory control to the next component, rather
    // than linking the library ancestor back into one of its descendants.
    setTabOrder(m_libraryPanel->settingsMenu()->parentWidget(), m_subtitlePanel);
    setTabOrder(m_subtitlePanel, m_playbackControls);
}

void MainWindow::updateFloatingSubtitleControls()
{
    const bool visible = m_floatingSubtitleWindow->isVisible();
    const bool locked = m_floatingSubtitleWindow->isLocked();
    const QString visibilityText = visible ? QStringLiteral("隐藏悬浮字幕")
                                           : QStringLiteral("显示悬浮字幕");
    const QString lockText = locked ? QStringLiteral("解锁悬浮字幕")
                                   : QStringLiteral("锁定悬浮字幕");
    const QSignalBlocker visibilityActionBlocker(m_toggleFloatingAction);
    const QSignalBlocker lockActionBlocker(m_lockFloatingAction);
    m_toggleFloatingAction->setChecked(visible);
    m_toggleFloatingAction->setText(visibilityText);
    m_playbackControls->setFloatingSubtitleVisible(visible);
    m_lockFloatingAction->setChecked(locked);
    m_lockFloatingAction->setText(lockText);
    updateFloatingPlaybackControls();
}

void MainWindow::updateFloatingPlaybackControls()
{
    m_subtitlePanel->setSeekEnabled(subtitleSeekReady());
    m_floatingSubtitleWindow->setPlaybackControlsState(
        m_audioPlayer->playbackState() == QMediaPlayer::PlayingState,
        playbackKeyboardReady(false), playbackKeyboardReady(true),
        !m_closing && m_playbackQueue.previousId() != 0,
        !m_closing && m_playbackQueue.nextId() != 0);
}

void MainWindow::restoreMainWindow()
{
    if (isMinimized())
        setWindowState(windowState() & ~Qt::WindowMinimized);
    show();
    raise();
    // Only the explicit restore action requests activation of the main window.
    activateWindow();
}

void MainWindow::connectSignals()
{
    connect(m_playbackControls, &PlaybackControlsWidget::togglePlaybackDetailRequested,
            this, [this] { setPlaybackDetailVisible(!m_playbackDetailVisible); });
    connect(m_collapsePlaybackDetailButton, &QPushButton::clicked,
            this, [this] { setPlaybackDetailVisible(false); });
    connect(m_subtitlePanel, &SubtitlePanel::subtitleActionsChanged,
            this, [this] { updateSubtitleActions(); });
    updateSubtitleActions();
    connect(m_playbackControls, &PlaybackControlsWidget::selectSubtitleRequested,
            this, &MainWindow::showBoundSubtitleDialog);
    connect(m_playbackControls, &PlaybackControlsWidget::toggleFloatingSubtitleRequested,
            this, [this] {
        if (!m_closing)
            m_toggleFloatingAction->trigger();
    });
    connect(m_subtitlePanel, &SubtitlePanel::seekRequested,
            this, &MainWindow::seekToSubtitle);
    connect(m_subtitlePanel, &SubtitlePanel::collapseRequested, this, [this] {
        if (m_closing || m_playbackDetailVisible) return;
        m_subtitlePanelRequested = false;
        applySubtitlePanelVisibility(false);
        if (auto *toggle = m_playbackControls->findChild<QPushButton *>(
                QStringLiteral("toggleSubtitlePanelButton")))
            toggle->setFocus(Qt::OtherFocusReason);
    });
    connect(m_playbackControls, &PlaybackControlsWidget::toggleSubtitlePanelRequested, this, [this] {
        if (m_closing || m_playbackDetailVisible)
            return;
        m_subtitlePanelRequested = m_subtitlePanel->isHidden();
        applySubtitlePanelVisibility(m_subtitlePanelRequested);
    });
    // Footer, subtitle controls and tray all project the actual window's state.
    connect(m_toggleFloatingAction, &QAction::toggled, this, [this](bool visible) {
        if (m_closing) {
            updateFloatingSubtitleControls();
            return;
        }
        if (visible)
            m_floatingSubtitleWindow->showWindow();
        else
            m_floatingSubtitleWindow->hideWindow();
    });
    connect(m_lockFloatingAction, &QAction::toggled, this, [this](bool locked) {
        if (m_closing) {
            updateFloatingSubtitleControls();
            return;
        }
        m_floatingSubtitleWindow->setLocked(locked);
    });
    connect(m_floatingAppearanceAction, &QAction::triggered,
            this, &MainWindow::showFloatingSubtitleAppearance);
    connect(m_floatingSubtitleWindow.get(), &FloatingSubtitleWindow::appearanceRequested,
            this, &MainWindow::showFloatingSubtitleAppearance);
    connect(m_floatingAppearanceDialog, &FloatingSubtitleAppearanceDialog::appearanceChangeRequested,
            this, &MainWindow::applyFloatingSubtitleAppearance);
    connect(m_floatingSubtitleWindow.get(), &FloatingSubtitleWindow::appearanceChanged,
            m_floatingAppearanceDialog, &FloatingSubtitleAppearanceDialog::setAppearance);
    connect(m_restoreMainWindowAction, &QAction::triggered,
            this, &MainWindow::restoreMainWindow);
    connect(m_floatingSubtitleWindow.get(), &FloatingSubtitleWindow::visibilityChanged,
            this, &MainWindow::updateFloatingSubtitleControls);
    connect(m_floatingSubtitleWindow.get(), &FloatingSubtitleWindow::lockedChanged,
            this, &MainWindow::updateFloatingSubtitleControls);
    connect(m_playbackControls, &PlaybackControlsWidget::playPauseRequested,
            this, &MainWindow::togglePlaybackFromControls);
    connect(m_floatingSubtitleWindow.get(), &FloatingSubtitleWindow::playPauseRequested, this, [this] {
        if (playbackKeyboardReady(false)) togglePlaybackFromControls();
    });
    connect(m_floatingSubtitleWindow.get(), &FloatingSubtitleWindow::previousRequested, this, [this] {
        if (!m_closing)
            playQueueEntry(m_playbackQueue.previousId(), PlaybackQueueNavigation::Previous);
    });
    connect(m_floatingSubtitleWindow.get(), &FloatingSubtitleWindow::nextRequested, this, [this] {
        if (!m_closing)
            playQueueEntry(m_playbackQueue.nextId(), PlaybackQueueNavigation::Next);
    });
    connect(m_floatingSubtitleWindow.get(), &FloatingSubtitleWindow::seekByRequested,
            this, &MainWindow::seekPlaybackByKeyboard);
    connect(m_playbackControls, &PlaybackControlsWidget::volumeChangeRequested,
            m_audioPlayer, &AudioPlayer::setVolumePercent);
    connect(m_playbackControls, &PlaybackControlsWidget::seekRequested,
            this, [this](qint64 position) { seekPlaybackTo(position); });

    connect(m_audioPlayer, &AudioPlayer::durationChanged, this,
            [this](qint64 duration) {
        if (duration <= 0) clearKeyboardSeek();
        m_playbackControls->setDuration(duration);
        m_playbackControls->setSeekable(m_audioPlayer->isSeekable());
        m_playbackControls->setPosition(m_audioPlayer->position());
        tryRestorePlaybackProgress();
        updateFloatingPlaybackControls();
    });
    connect(m_audioPlayer, &AudioPlayer::seekableChanged, this, [this](bool seekable) {
        if (!seekable) clearKeyboardSeek();
        m_playbackControls->setSeekable(seekable);
        tryRestorePlaybackProgress();
        updateFloatingPlaybackControls();
    });
    connect(m_audioPlayer, &AudioPlayer::positionChanged, this,
            [this](qint64 position) {
        m_playbackControls->setPosition(position);
        if (m_keyboardSeekTarget
            && (m_keyboardSeekAudioGeneration != m_audioGeneration
                || m_keyboardSeekSource != m_audioPlayer->source()
                || qAbs(position - *m_keyboardSeekTarget) <= 250))
            clearKeyboardSeek();
        // The control may be previewing a drag; subtitles still follow media time.
        onProgressPosition(position);
        refreshSubtitles();
    });
    connect(m_audioPlayer, &AudioPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState state) {
        if (state != QMediaPlayer::PlayingState) clearKeyboardSeek();
        updatePlaybackStatus(state);
        onProgressState(state);
        refreshSubtitles();
    });
    connect(m_audioPlayer, &AudioPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::NoMedia || status == QMediaPlayer::LoadingMedia
            || status == QMediaPlayer::InvalidMedia || status == QMediaPlayer::EndOfMedia) {
            finishPlaybackDetailTransition();
            clearKeyboardSeek();
        }
        updatePlaybackStatus(m_audioPlayer->playbackState());
        onProgressMediaStatus(status);
        refreshSubtitles();
        onQueueMediaStatus(status);
    });
    connect(m_audioPlayer, &AudioPlayer::errorOccurred, this,
            [this](const QString &message) {
        if (m_closing || m_audioPlayer->error() == QMediaPlayer::NoError)
            return;
        finishPlaybackDetailTransition();
        clearKeyboardSeek();
        updatePlaybackStatus(m_audioPlayer->playbackState());
        onProgressMediaStatus(QMediaPlayer::InvalidMedia);
        m_queueAdvanceArmed = false;
        if (!m_progressSourceChanging && m_queueAudioGeneration == m_audioGeneration && m_playbackQueue.currentId()) {
            // An error in the old source must not discard appends belonging
            // to a newer replacement that is still being validated.
            m_playbackQueue.setIssue(m_playbackQueue.currentId(), message);
            updatePlaybackQueue();
        }
        showAudioOpenError(QStringLiteral("无法播放当前音频：%1\n%2")
            .arg(m_audioPlayer->source().toLocalFile(), message));
    });
}

void MainWindow::stopPlayback()
{
    if (m_closing) return;
    cancelQueueTransition();
    clearCurrentPlaybackProgress(true);
    cancelPlaybackRestore(false);
    m_audioPlayer->stop();
    refreshSubtitles();
}

void MainWindow::togglePlaybackFromControls()
{
    if (m_closing || m_progressQueryPending || m_progressRestorePending) return;
    m_subtitlePanel->cancelPendingSeek();
    cancelQueueTransition();
    m_queueAdvanceArmed = true;
    m_queueEndedGeneration = 0;
    m_audioPlayer->togglePlayback();
}

void MainWindow::seekPlaybackTo(qint64 position, bool subtitleClick)
{
    if (m_closing) return;
    m_subtitlePanel->cancelPendingSeek();
    // Both timeline and keyboard seeks preserve the same queue cancellation,
    // restoration override and subtitle synchronization behavior.
    cancelQueueTransition();
    m_queueAdvanceArmed = true;
    m_queueEndedGeneration = 0;
    const bool wasRestoring = m_progressQueryPending || m_progressRestorePending;
    cancelPlaybackRestore(false);
    m_floatingSubtitleWindow->resetAutoScroll();
    // The shared cancellation above must run before preparing click following.
    // Set the mode before setPosition can synchronously notify the transcript.
    if (subtitleClick) m_subtitlePanel->prepareForSubtitleSeek();
    m_audioPlayer->setPosition(position);
    if (wasRestoring) {
        m_audioPlayer->play(); // Seek is issued before playback is started.
    }
    refreshSubtitles();
}

bool MainWindow::subtitleSeekReady() const
{
    return playbackKeyboardReady(true) && !m_subtitleQueryInProgress
        && !m_subtitleQueryPending && !m_subtitleLoadInProgress && !m_subtitleLoadPending
        && !hasPendingSubtitlePreference(m_playingLibraryWorkId, m_playingLibraryFileId);
}

void MainWindow::seekToSubtitle(qint64 position)
{
    // A delayed single click must not override an in-flight source, subtitle
    // choice or progress restore, or act behind a subsequently opened dialog.
    if (!subtitleSeekReady() || QApplication::activePopupWidget()
        || QApplication::activeModalWidget())
        return;
    const bool paused = m_audioPlayer->playbackState() == QMediaPlayer::PausedState;
    const qint64 lastPosition = m_audioPlayer->duration() - (paused ? 1 : 0);
    clearKeyboardSeek();
    seekPlaybackTo(qBound<qint64>(0, position, lastPosition), true);
}

void MainWindow::updatePlaybackStatus(QMediaPlayer::PlaybackState state)
{
    updateFloatingPlaybackControls();
    const auto status = m_audioPlayer->mediaStatus();
    if (m_audioPlayer->error() != QMediaPlayer::NoError || status == QMediaPlayer::InvalidMedia) {
        m_playbackControls->setMediaAvailable(false);
        m_subtitlePanel->setImportEnabled(false);
        m_subtitlePanel->setSelectionEnabled(false);
        setSubtitleText(QString());
        return;
    }
    m_playbackControls->setPlaying(state == QMediaPlayer::PlayingState);
}

void MainWindow::showAudioOpenError(const QString &message)
{
    if (m_closing)
        return;
    if (!m_audioErrorDialog) {
        m_audioErrorDialog = new PlayerMessageBox(QMessageBox::Warning, QStringLiteral("音频打开失败"),
                                             QString(), QMessageBox::Ok, this);
        m_audioErrorDialog->setObjectName(QStringLiteral("audioOpenErrorDialog"));
        m_audioErrorDialog->setTextFormat(Qt::PlainText);
        m_audioErrorDialog->setWindowModality(Qt::WindowModal);
    }
    m_audioErrorDialog->setText(message);
    // Same warning UI, without a nested event loop during source changes/exit.
    if (!m_audioErrorDialog->isVisible())
        m_audioErrorDialog->open();
}

void MainWindow::openAudioFile()
{
    if (m_closing)
        return;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择音频文件"), QString(),
        QStringLiteral("音频文件 (*.mp3 *.wav);;所有文件 (*)"));
    if (!path.isEmpty())
        requestOpenAudio(path);
}

void MainWindow::playLibraryTrack(qint64 fileId)
{
    if (m_closing)
        return;
    // Even a rejected newer play intention supersedes pending filesystem work.
    cancelQueueTransition();
    if (fileId <= 0 || !m_libraryDatabaseReady || m_libraryQueryInProgress || m_libraryQueryPending
        || m_librarySnapshotWorkId != m_libraryPanel->selectedWorkId()) {
        showAudioOpenError(QStringLiteral("音轨列表尚未就绪或已更新，请等待作品读取完成后重试。"));
        return;
    }
    const auto file = std::find_if(m_libraryFilesSnapshot.cbegin(), m_libraryFilesSnapshot.cend(),
        [fileId](const auto &entry) { return entry.id == fileId; });
    if (file == m_libraryFilesSnapshot.cend() || file->workId != m_librarySnapshotWorkId
        || file->type != LibraryFileType::Audio) {
        showAudioOpenError(QStringLiteral("音轨不属于当前有效作品快照，请重新选择。"));
        return;
    }
    if (file->missing) {
        showAudioOpenError(QStringLiteral("此音轨已在最近一次完整扫描中标记为缺失：%1").arg(file->relativePath));
        return;
    }
    const auto work = std::find_if(m_libraryWorksSnapshot.cbegin(), m_libraryWorksSnapshot.cend(),
        [file](const auto &entry) { return entry.id == file->workId; });
    if (work == m_libraryWorksSnapshot.cend() || work->rootPath.isEmpty()) {
        showAudioOpenError(QStringLiteral("找不到音轨所属作品的根目录，请刷新后重试。"));
        return;
    }
    // Capture the browser layer and stable file identity together. A stale row
    // must never silently start a different track at index zero.
    m_queuePreparationRequest = {};
    m_queuePreparationRequest.kind = QueuePreparationKind::Directory;
    m_queuePreparationRequest.generation = m_audioOpenGeneration;
    m_queuePreparationRequest.workId = work->id;
    m_queuePreparationRequest.directory = m_libraryPanel->currentDirectory();
    m_queuePreparationRequest.startFileId = file->id;
    m_queuePreparationRequest.entry.relativePath = file->relativePath;
    m_queuePreparationRequest.replacement = m_playbackQueue;
    m_queueReplacementPending = true;
    m_queuePreparationPending = true;
    if (!m_queuePreparationInProgress) startQueuePreparation();
}

void MainWindow::requestOpenAudio(const QString &path, const QString &libraryRoot,
                                  qint64 workId, qint64 fileId, const QString &workName,
                                  quint64 queueEntryId, const std::optional<PlaybackQueue> &replacement,
                                  PlaybackQueueNavigation navigation, bool fromBeginning)
{
    if (m_closing)
        return;
    finishPlaybackDetailTransition();
    clearKeyboardSeek();
    ++m_audioOpenGeneration;
    m_queueAdvanceArmed = false;
    m_queueContinuationPending = false;
    m_queueNaturalRequest = navigation == PlaybackQueueNavigation::Natural;
    m_queueRestartingCurrent = false;
    if (!replacement) {
        m_queueAppendDuringReplacement.clear();
        m_queueReplacementPending = queueEntryId == 0;
    }
    m_queuePreparationPending = false;
    m_audioRequestedQueueEntryId = queueEntryId;
    m_audioRequestedQueueReplacement = replacement;
    m_audioRequestedQueueNavigation = navigation;
    m_audioRequestedFromBeginning = fromBeginning;
    m_audioRequestedGeneration = m_audioOpenGeneration;
    m_audioRequestedPath = path;
    m_audioRequestedRoot = libraryRoot;
    m_audioRequestedWorkId = workId;
    m_audioRequestedFileId = fileId;
    m_audioRequestedWorkName = workName;
    m_audioOpenPending = true;
    updateFloatingPlaybackControls();
    if (m_audioErrorDialog)
        m_audioErrorDialog->hide();
    // No playback, subtitle, or media identity changes until validation succeeds.
    if (!m_audioOpenInProgress)
        startAudioOpen();
}

void MainWindow::startAudioOpen()
{
    m_audioOpenInProgress = true;
    m_audioOpenPending = false;
    const quint64 generation = m_audioOpenGeneration;
    const QString path = m_audioRequestedPath;
    const QString root = m_audioRequestedRoot;
    const auto hook = m_audioValidationTestHook;
    const qint64 workId = m_audioRequestedWorkId;
    const qint64 fileId = m_audioRequestedFileId;
    const QString workName = m_audioRequestedWorkName;
    const quint64 queueEntryId = m_audioRequestedQueueEntryId;
    const auto replacement = m_audioRequestedQueueReplacement;
    const auto navigation = m_audioRequestedQueueNavigation;
    const bool fromBeginning = m_audioRequestedFromBeginning;
    auto promise = std::make_shared<QPromise<AudioFileValidationResult>>();
    auto *watcher = new QFutureWatcher<AudioFileValidationResult>(this);
    connect(watcher, &QFutureWatcher<AudioFileValidationResult>::finished, this,
            [this, watcher, generation, root, workId, fileId, workName, queueEntryId, replacement,
             navigation, fromBeginning]() {
        const auto result = watcher->result();
        watcher->deleteLater();
        m_audioOpenInProgress = false;
        if (!m_closing && generation == m_audioOpenGeneration)
            applyAudioOpenResult(result, generation, workId, fileId, workName, queueEntryId, replacement,
                                 root, navigation, fromBeginning);
        if (m_closing)
            finishCloseIfReady();
        else if (m_audioOpenPending && !m_audioOpenInProgress)
            startAudioOpen();
        updateFloatingPlaybackControls();
    });
    watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([promise, path, root, hook]() {
        promise->start();
        AudioFileValidationResult result;
        try {
            if (hook)
                (*hook)();
            result = root.isEmpty() ? validateAudioFile(path) : validateLibraryAudioFile(root, path);
        } catch (const std::exception &exception) {
            result.errorMessage = QStringLiteral("音频路径检查失败：%1").arg(QString::fromUtf8(exception.what()));
        } catch (...) {
            result.errorMessage = QStringLiteral("音频路径检查发生未知错误。");
        }
        promise->addResult(std::move(result));
        promise->finish();
    });
}

void MainWindow::applyAudioOpenResult(const AudioFileValidationResult &result, quint64 requestGeneration,
                                      qint64 workId, qint64 fileId, const QString &workName,
                                      quint64 queueEntryId, const std::optional<PlaybackQueue> &replacement,
                                      const QString &libraryRoot, PlaybackQueueNavigation navigation,
                                      bool fromBeginning)
{
    if (m_closing || requestGeneration != m_audioOpenGeneration)
        return;
    if (!result.success) {
        m_queueNaturalRequest = false;
        m_queueReplacementPending = false;
        m_queueAppendDuringReplacement.clear();
        if (queueEntryId)
            failQueueRequest(replacement ? 0 : queueEntryId, result.errorMessage);
        showAudioOpenError(result.errorMessage);
        return;
    }
    if (workId > 0 && m_removedLibraryWorkIds.contains(workId))
        return;
    if (queueEntryId && !replacement) {
        const auto *entry = m_playbackQueue.entry(queueEntryId);
        if (!entry || !entry->available) {
            failQueueRequest(queueEntryId, QStringLiteral("队列音轨在检查期间已失效。"), true);
            return;
        }
    }
    if (m_audioErrorDialog)
        m_audioErrorDialog->hide();
    finishPlaybackDetailTransition();
    // The previous session's final observation precedes the new session query.
    clearKeyboardSeek();
    saveCurrentPlaybackProgress(true);
    cancelPlaybackRestore(false);
    m_progressSessionActive = false;
    m_progressSourceChanging = true;
    // Both the file dialog and library requests install media through this path.
    // Subtitle generation changes only for a genuinely adopted new source.
    ++m_audioGeneration;
    if (m_subtitleErrorDialog)
        m_subtitleErrorDialog->hide();
    ++m_subtitleRequestGeneration;
    m_playingLibraryWorkId = workId;
    m_playingLibraryFileId = fileId;
    m_libraryPanel->setPlayingTrack(workId, fileId);
    resetBoundSubtitleContext();
    m_subtitlePanel->setSelectionEnabled(false);
    m_subtitlePreferenceFeedback.clear();
    m_subtitlePreferenceFeedbackError = false;
    m_manualSubtitleChoice = false;
    m_subtitleQueryPending = false;
    m_subtitleLoadPending = false;
    m_librarySubtitleResolution = {};
    m_lastLibrarySubtitleLoad = {};
    m_subtitlePanel->setLoading(false);
    m_subtitlePanel->setLibraryStatus({});
    clearSubtitles();
    m_playbackControls->setMediaAvailable(false);
    m_audioPlayer->stop();

    // Capture accepted playback metadata; browsing later must not retarget it.
    const auto acceptedWorkName = latestWorkTitle(workId, workName);
    updatePlaybackIdentity(QFileInfo(result.path).fileName(),
        workId > 0 && fileId > 0 ? acceptedWorkName : QString());
    m_audioPlayer->setLocalFile(result.path);
    m_progressSourceChanging = false;
    adoptPlaybackQueue(result.path, workId, fileId, acceptedWorkName, queueEntryId, replacement, navigation);
    adoptCoverArt(libraryRoot, result.path, workId);
    if (m_closing || requestGeneration != m_audioOpenGeneration)
        return;
    if (m_audioPlayer->error() != QMediaPlayer::NoError
        || m_audioPlayer->mediaStatus() == QMediaPlayer::InvalidMedia) {
        updatePlaybackStatus(m_audioPlayer->playbackState());
        return;
    }
    m_playbackControls->setMediaAvailable(true);
    m_playbackControls->setDuration(m_audioPlayer->duration());
    m_playbackControls->setSeekable(m_audioPlayer->isSeekable());
    m_playbackControls->setPosition(m_audioPlayer->position());
    m_subtitlePanel->setImportEnabled(true);
    m_subtitlePanel->setSelectionEnabled(workId > 0 && fileId > 0);
    updateSubtitleActions();
    beginPlaybackProgress(fromBeginning);
    if (m_playingLibraryWorkId && m_playingLibraryFileId)
        requestLibrarySubtitles();
}

void MainWindow::openSubtitleFile()
{
    if (m_closing || m_audioGeneration == 0 || !m_subtitlePanel->canOpenSubtitle()
        || !m_boundSubtitleDialog || !m_boundSubtitleDialog->isVisible()
        || m_subtitleChoiceAudioGeneration != m_audioGeneration
        || m_boundSubtitleDialog->workId() != m_playingLibraryWorkId
        || m_boundSubtitleDialog->audioFileId() != m_playingLibraryFileId)
        return;
    if (m_subtitleImportDialog) {
        m_subtitleImportDialog->raise();
        m_subtitleImportDialog->activateWindow();
        return;
    }
    const auto audioGeneration = m_audioGeneration;
    const auto subtitleGeneration = m_subtitleRequestGeneration;
    const auto source = m_audioPlayer->source();
    const auto workId = m_playingLibraryWorkId;
    const auto fileId = m_playingLibraryFileId;
    auto *dialog = new QFileDialog(m_boundSubtitleDialog, QStringLiteral("导入字幕"),
        source.isLocalFile() ? QFileInfo(source.toLocalFile()).absolutePath() : QString());
    dialog->setObjectName(QStringLiteral("subtitleImportDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setAcceptMode(QFileDialog::AcceptOpen);
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setNameFilters({QStringLiteral("字幕文件 (*.srt *.vtt *.lrc)"),
        QStringLiteral("SRT 字幕 (*.srt)"), QStringLiteral("WebVTT 字幕 (*.vtt)"),
        QStringLiteral("LRC 字幕 (*.lrc)"), QStringLiteral("所有文件 (*)")});
    m_subtitleImportDialog = dialog;
    updateSubtitleActions();
    connect(dialog, &QDialog::finished, this,
            [this, dialog, audioGeneration, subtitleGeneration, source, workId, fileId](int result) {
        if (m_subtitleImportDialog != dialog)
            return;
        m_subtitleImportDialog = nullptr;
        updateSubtitleActions();
        // Selecting a file must not retarget an audio session or a subtitle
        // decision that changed while the picker was open.
        if (result != QDialog::Accepted || m_closing || audioGeneration != m_audioGeneration
            || subtitleGeneration != m_subtitleRequestGeneration || source != m_audioPlayer->source()
            || workId != m_playingLibraryWorkId || fileId != m_playingLibraryFileId
            || !m_boundSubtitleDialog->isVisible() || !m_subtitlePanel->canOpenSubtitle()
            || m_subtitleChoiceAudioGeneration != audioGeneration)
            return;
        const auto paths = dialog->selectedFiles();
        if (!paths.isEmpty())
            requestManualSubtitle(paths.front());
    });
    // Parent-owned and non-nested: a source change or asynchronous close can
    // reject this picker without retaining a stack frame in MainWindow.
    dialog->open();
}

void MainWindow::updateSubtitleActions()
{
    if (m_chickenRiceTranslateDialog)
        m_chickenRiceTranslateDialog->setCurrentAudioPath(
            !m_closing && !m_audioOpenInProgress && !m_audioOpenPending && m_audioGeneration > 0
                ? m_audioPlayer->source().toLocalFile() : QString());
    const bool enabled = !m_closing && m_audioGeneration > 0;
    if (m_subtitleImportDialog && (!enabled || !m_subtitlePanel->canOpenSubtitle()))
        m_subtitleImportDialog->reject();
    m_playbackControls->setSubtitleSelectionAvailable(enabled
        && (m_subtitlePanel->canSelectSubtitle() || m_subtitlePanel->canOpenSubtitle()));
    if (m_boundSubtitleDialog)
        m_boundSubtitleDialog->setImportEnabled(enabled && m_subtitlePanel->canOpenSubtitle()
            && !m_subtitleImportDialog);
}

void MainWindow::showSubtitleLoadError(const QString &message, const QString &details)
{
    if (m_closing)
        return;
    if (!m_subtitleErrorDialog) {
        m_subtitleErrorDialog = new PlayerMessageBox(QMessageBox::Warning,
            QStringLiteral("字幕加载失败"), QString(), QMessageBox::Ok, this);
        m_subtitleErrorDialog->setObjectName(QStringLiteral("subtitleLoadErrorDialog"));
        m_subtitleErrorDialog->setTextFormat(Qt::PlainText);
        m_subtitleErrorDialog->setWindowModality(Qt::WindowModal);
    }
    m_subtitleErrorDialog->setText(message);
    m_subtitleErrorDialog->setDetailedText(details);
    // Parent-owned and non-nested: asynchronous close may destroy MainWindow.
    m_subtitleErrorDialog->open();
}

void MainWindow::applySubtitleLoad(const QString &path, const SubtitleFileLoadResult &result,
                                  quint64 audioGeneration)
{
    // Reject stale successes and failures before touching the binding or either display.
    if (m_closing || audioGeneration != m_audioGeneration)
        return;

    if (!result.succeeded()) {
        m_subtitlePanel->showLoadFailure();
        showSubtitleLoadError(result.errorMessage, diagnosticDetails(result.diagnostics));
        return;
    }

    m_subtitleSynchronizer.setEntries(result.entries);
    m_subtitlePanel->setSubtitleEntries(result.entries);
    // A reload starts a new reading session even when its cues are identical.
    m_floatingSubtitleWindow->resetAutoScroll();
    qsizetype warnings = 0;
    qsizetype errors = 0;
    for (const auto &diagnostic : result.diagnostics) {
        if (diagnostic.severity == SubtitleLoadDiagnosticSeverity::Error)
            ++errors;
        else
            ++warnings;
    }
    QString summary = QStringLiteral("字幕：%1（有效 %2 条）")
                          .arg(QFileInfo(path).fileName())
                          .arg(result.entries.size());
    if (warnings != 0 || errors != 0) {
        summary += QStringLiteral("；警告 %1，错误 %2").arg(warnings).arg(errors);
        if (errors != 0)
            summary += QStringLiteral("，无效条目已跳过");
    }
    m_subtitlePanel->setFileInfo(summary, diagnosticDetails(result.diagnostics));
    refreshSubtitles();
}

void MainWindow::clearSubtitles()
{
    m_subtitleSynchronizer.clear();
    m_subtitlePanel->setSubtitleEntries({});
    m_subtitlePanel->clearFileInfo();
    setSubtitleText(QString());
    m_floatingSubtitleWindow->setSubtitlePlaybackContext({}, 0, false);
}

void MainWindow::refreshSubtitles()
{
    updateFloatingPlaybackControls();
    refreshSubtitles(m_audioPlayer->position(),
                     m_audioPlayer->playbackState() == QMediaPlayer::StoppedState
                     || m_audioPlayer->error() != QMediaPlayer::NoError
                     || m_audioPlayer->mediaStatus() == QMediaPlayer::InvalidMedia);
}

void MainWindow::refreshSubtitles(qint64 positionMs, bool playbackStopped)
{
    QString text;
    QList<SubtitleEntry> entries;
    if (!playbackStopped) {
        QStringList parts;
        entries = m_subtitleSynchronizer.activeEntries(positionMs);
        for (const auto &entry : entries)
            parts.append(entry.text);
        text = parts.join(QChar('\n'));
    }
    setSubtitleText(text);
    m_subtitlePanel->setPlaybackPosition(positionMs, playbackStopped,
        !m_closing && m_audioPlayer->playbackState() == QMediaPlayer::PlayingState);
    m_floatingSubtitleWindow->setSubtitlePlaybackContext(entries, positionMs,
        !m_closing && !playbackStopped
            && m_audioPlayer->playbackState() == QMediaPlayer::PlayingState);
}

void MainWindow::setSubtitleText(const QString &text)
{
    m_subtitlePanel->setSubtitleText(text);
    // Hidden windows also receive the same query result, ready for reopening.
    m_floatingSubtitleWindow->setSubtitleText(text);
}

} // namespace asmr
