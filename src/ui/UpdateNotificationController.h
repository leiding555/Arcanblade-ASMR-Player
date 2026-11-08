#pragma once

#include "update/UpdateCheckService.h"
#include "update/UpdateDownloadService.h"

#include <QDateTime>
#include <QObject>
#include <QPointer>

#include <functional>

class QAction;
class QMenu;
class QSettings;
class QTimer;
class QWidget;

namespace asmr {
class AboutSoftwareDialog;
class UpdateNoticeDialog;

struct UpdateNotificationOptions {
    // Empty uses the version compiled from the project's VERSION file.
    QString currentVersion;
    std::function<QDateTime()> nowUtc;
    std::function<bool(const QUrl &)> openUrl;
    // Optional injected downloader remains owned by its caller. Production
    // creates one lazily, after a release with download metadata is selected.
    QPointer<UpdateDownloadService> downloadService;
    // Empty means that AI/model tasks permit a restart. Checked again on click.
    std::function<QString()> restartBlockedReason;
    // The host prepares/starts the separate updater asynchronously and reports
    // errors through installPreparationFailed(); only the host closes itself.
    std::function<void(const UpdateDownloadResult &)> requestRestart;
};

// Coordinates update preferences and non-modal notifications. It does not own
// the main window or touch the media library. Installation is delegated to the
// host so its existing asynchronous shutdown protocol stays authoritative.
class UpdateNotificationController final : public QObject {
    Q_OBJECT
public:
    UpdateNotificationController(QWidget *window, QMenu *settingsMenu,
                                 QSettings *settings, QObject *parent = nullptr);
    UpdateNotificationController(QWidget *window, QMenu *settingsMenu,
                                 QSettings *settings, UpdateCheckService *service,
                                 UpdateNotificationOptions options,
                                 QObject *parent = nullptr);
    ~UpdateNotificationController() override;

    void startAutomaticCheck();
    void showAbout();
    void checkManually();
    void shutdown();
    void installPreparationFailed(const QString &message);
    [[nodiscard]] bool isShutdownFinished() const { return m_downloadShutdownFinished; }

signals:
    void shutdownFinished();

private:
    QDateTime nowUtc() const;
    QDateTime savedDate(const QString &key) const;
    bool isIgnored(const QString &version) const;
    bool isRateLimited() const;
    void beginCheck(bool manual);
    void finished(UpdateCheckResult result);
    void showChecking();
    void showStatus(const QString &message);
    void showRelease(const UpdateRelease &release);
    void showNotice(const UpdateRelease &release);
    AboutSoftwareDialog *aboutDialog();
    void refreshAbout();
    void cancelCheck();
    void setAutomaticCheckEnabled(bool enabled);
    void setIncludePrereleases(bool enabled);
    void openRepository();
    void openRelease();
    void downloadOrRestart();
    UpdateDownloadService *downloadService();
    void downloadChanged();
    void downloadFinished(UpdateDownloadResult result);
    void restorePreparedRelease();
    void savePreparedRelease(const UpdateRelease &release);
    void ignoreRelease();
    enum class ViewState { NotChecked, Checking, Status, Release };

    QPointer<QWidget> m_window;
    QPointer<QSettings> m_settings;
    QPointer<UpdateCheckService> m_service;
    QPointer<UpdateDownloadService> m_downloadService;
    UpdateNotificationOptions m_options;
    QString m_currentVersion;
    QPointer<QAction> m_aboutAction;
    QPointer<AboutSoftwareDialog> m_about;
    QPointer<UpdateNoticeDialog> m_notice;
    UpdateRelease m_latestRelease;
    ViewState m_viewState = ViewState::NotChecked;
    QString m_status;
    bool m_autoCheckEnabled = true;
    bool m_includePrereleases = false;
    bool m_manual = false;
    bool m_requestedPrereleases = false;
    bool m_discardResult = false;
    bool m_shuttingDown = false;
    bool m_preparingInstall = false;
    bool m_downloadShutdownFinished = true;
    bool m_savedRestoreAttempted = false;
    bool m_restoringSavedRelease = false;
    UpdateRelease m_preparedReleaseBeforeCheck;
    qint64 m_downloadedBytes = 0;
    qint64 m_downloadTotalBytes = 0;
    QTimer *m_restartAvailabilityTimer = nullptr;
};
} // namespace asmr
