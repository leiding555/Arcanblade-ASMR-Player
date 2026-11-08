#include "ui/UpdateNotificationController.h"

#include "ui/ApplicationBrand.h"
#include "ui/DialogFrame.h"
#include "ui/PlayerTheme.h"
#include "ui/TitleBarWidget.h"
#include "update/ApplicationVersion.h"
#include "update/SemanticVersion.h"

#include <QAction>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>
#include <cmath>

namespace asmr {
namespace {
const QString Prefix = QStringLiteral("updates/");
constexpr qint64 AutomaticIntervalSeconds = 24 * 60 * 60;
constexpr qsizetype MaximumSavedReleaseBytes = 64 * 1024;

QLabel *plainLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}

QString normalizedVersion(const QString &value)
{
    const auto version = SemanticVersion::parse(value, true);
    return version ? version->toString() : value;
}

QString downloadSize(qint64 bytes)
{
    return QStringLiteral("%1 MB").arg(double(qMax(qint64(0), bytes)) / (1024.0 * 1024.0), 0, 'f', 1);
}

bool sameReleasePayload(const UpdateRelease &left, const UpdateRelease &right)
{
    return left.version == right.version && left.pageUrl == right.pageUrl
        && left.packageUrl == right.packageUrl && left.checksumUrl == right.checksumUrl
        && left.packageSize == right.packageSize
        && left.packageSha256.compare(right.packageSha256, Qt::CaseInsensitive) == 0;
}
} // namespace

class AboutSoftwareDialog final : public QDialog {
public:
    explicit AboutSoftwareDialog(const QString &currentVersion, QWidget *parent)
        : QDialog(parent)
    {
        setObjectName(QStringLiteral("aboutSoftwareDialog"));
        setWindowTitle(QStringLiteral("关于软件"));
        setModal(false);
        setWindowModality(Qt::NonModal);
        setStyleSheet(playerStyleSheet());
        resize(440, 260);
        auto *body = new QVBoxLayout;
        body->setContentsMargins(20, 20, 20, 16);
        body->setSpacing(8);
        // The frame's scroll host must follow the content minimum when update
        // notes appear after the first show, including on small screens.
        body->setSizeConstraint(QLayout::SetMinimumSize);

        auto *brand = new QHBoxLayout;
        brand->setSpacing(14);
        auto *icon = new QLabel(this);
        icon->setObjectName(QStringLiteral("aboutApplicationIcon"));
        icon->setFixedSize(48, 48);
        icon->setPixmap(applicationMarkIcon().pixmap(QSize(48, 48), devicePixelRatioF()));
        icon->setAlignment(Qt::AlignCenter);
        brand->addWidget(icon, 0, Qt::AlignVCenter);
        auto *identity = new QVBoxLayout;
        identity->setSpacing(4);
        auto *name = plainLabel(applicationDisplayName(), this);
        name->setObjectName(QStringLiteral("applicationTitle"));
        name->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        identity->addWidget(name);
        auto *current = plainLabel(QStringLiteral("当前版本：%1").arg(currentVersion), this);
        current->setObjectName(QStringLiteral("applicationVersionLabel"));
        current->setProperty("secondaryText", true);
        current->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        current->setTextInteractionFlags(Qt::TextSelectableByMouse);
        identity->addWidget(current);
        brand->addLayout(identity, 1);
        body->addLayout(brand);
        body->addSpacing(12);

        automatic = new QCheckBox(QStringLiteral("启动时自动检查更新"), this);
        automatic->setObjectName(QStringLiteral("automaticUpdateCheckBox"));
        automatic->setToolTip(QStringLiteral("每天最多自动检查一次。"));
        automatic->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        body->addWidget(automatic);
        prerelease = new QCheckBox(QStringLiteral("接收测试版更新"), this);
        prerelease->setObjectName(QStringLiteral("prereleaseUpdatesCheckBox"));
        prerelease->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        body->addWidget(prerelease);
        body->addSpacing(4);
        status = plainLabel(QStringLiteral("尚未检查更新"), this);
        status->setObjectName(QStringLiteral("updateStatus"));
        status->setProperty("secondaryText", true);
        status->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        body->addWidget(status);
        progress = new QProgressBar(this);
        progress->setObjectName(QStringLiteral("updateProgress"));
        progress->setRange(0, 0);
        progress->setTextVisible(false);
        progress->setMaximumHeight(6);
        body->addWidget(progress);
        notes = new QPlainTextEdit(this);
        notes->setObjectName(QStringLiteral("updateReleaseNotes"));
        notes->setReadOnly(true);
        // Keep the release text scrollable inside its editor. It must not push
        // the download/close actions outside the frame on a normal desktop.
        notes->setMinimumHeight(80);
        notes->setMaximumHeight(180);
        notes->setPlaceholderText(QStringLiteral("此版本未提供更新说明。"));
        body->addWidget(notes);
        auto *releaseLinks = new QHBoxLayout;
        releasePage = new QPushButton(QStringLiteral("发布页"), this);
        releasePage->setObjectName(QStringLiteral("updateReleasePage"));
        releasePage->setFlat(true);
        releasePage->setAutoDefault(false);
        releasePage->setCursor(Qt::PointingHandCursor);
        ignore = new QPushButton(QStringLiteral("忽略此版本"), this);
        ignore->setObjectName(QStringLiteral("updateIgnoreVersion"));
        ignore->setAutoDefault(false);
        releaseLinks->addWidget(releasePage);
        releaseLinks->addWidget(ignore);
        releaseLinks->addStretch();
        body->addLayout(releaseLinks);
        // Extra height belongs below the content, keeping the brand and update
        // preferences together instead of spreading their rows apart.
        body->addStretch(1);
        auto *actions = new QHBoxLayout;
        actions->setSpacing(8);
        repository = new QPushButton(QStringLiteral("GitHub"), this);
        repository->setObjectName(QStringLiteral("repositoryLinkButton"));
        repository->setAutoDefault(false);
        repository->setFlat(true);
        repository->setCursor(Qt::PointingHandCursor);
        repository->setStyleSheet(QStringLiteral(
            "QPushButton#repositoryLinkButton { background: transparent; color: #ADB5C1;"
            " border: 1px solid transparent; padding: 3px 2px; text-decoration: underline; }"
            "QPushButton#repositoryLinkButton:hover { color: #69C8A5; }"
            "QPushButton#repositoryLinkButton:focus { color: #69C8A5; border: 1px dotted #69C8A5; }"));
        cancel = new QPushButton(QStringLiteral("取消检查"), this);
        cancel->setObjectName(QStringLiteral("updateCancelCheck"));
        cancel->setAutoDefault(false);
        check = new QPushButton(QStringLiteral("检查更新"), this);
        check->setObjectName(QStringLiteral("checkForUpdatesButton"));
        check->setAutoDefault(false);
        later = new QPushButton(QStringLiteral("关闭"), this);
        later->setObjectName(QStringLiteral("updateLater"));
        download = new QPushButton(QStringLiteral("前往下载"), this);
        download->setObjectName(QStringLiteral("updateDownload"));
        download->setProperty("primaryAction", true);
        download->setAutoDefault(false);
        ignore->setAutoDefault(false);
        later->setAutoDefault(false);
        actions->addWidget(repository);
        actions->addStretch();
        actions->addWidget(cancel);
        actions->addWidget(check);
        actions->addWidget(later);
        actions->addWidget(download);
        body->addLayout(actions);
        progress->hide();
        cancel->hide();
        notes->hide();
        ignore->hide();
        releasePage->hide();
        download->hide();
        frame = new DialogFrame(this, body);
        setTabOrder(automatic, prerelease);
        setTabOrder(prerelease, notes);
        setTabOrder(notes, releasePage);
        setTabOrder(releasePage, ignore);
        setTabOrder(ignore, repository);
        setTabOrder(repository, cancel);
        setTabOrder(cancel, check);
        setTabOrder(check, later);
        setTabOrder(later, download);
        connect(later, &QPushButton::clicked, this, &QDialog::close);
    }

    void checking()
    {
        status->setText(QStringLiteral("正在检查更新…"));
        progress->setRange(0, 0);
        progress->show();
        notes->hide();
        download->hide();
        ignore->hide();
        releasePage->hide();
        scheduleContentFit();
    }

    void resultStatus(const QString &message)
    {
        status->setText(message);
        progress->hide();
        notes->hide();
        download->hide();
        ignore->hide();
        releasePage->hide();
        scheduleContentFit();
    }

    void release(const UpdateRelease &value)
    {
        status->setText(QStringLiteral("发现新版本：%1\n%2").arg(value.version, value.title));
        progress->hide();
        // Release notes are always plain text; no links or remote images are loaded.
        // Availability polling must preserve the reader's selection/scroll.
        if (notes->toPlainText() != value.notes) notes->setPlainText(value.notes);
        notes->show();
        download->show();
        ignore->show();
        releasePage->show();
        scheduleContentFit();
    }

    void transfer(const QString &message, qint64 done, qint64 total, bool indeterminate)
    {
        status->setText(message);
        progress->show();
        if (indeterminate || total <= 0) progress->setRange(0, 0);
        else {
            progress->setRange(0, 1000);
            progress->setValue(int(qBound(0.0, double(done) / double(total), 1.0) * 1000.0));
        }
        scheduleContentFit();
    }

    QPushButton *repository = nullptr;
    QPushButton *releasePage = nullptr;
    QPushButton *check = nullptr;
    QPushButton *cancel = nullptr;
    QCheckBox *automatic = nullptr;
    QCheckBox *prerelease = nullptr;
    QLabel *status = nullptr;
    QProgressBar *progress = nullptr;
    QPlainTextEdit *notes = nullptr;
    QPushButton *download = nullptr;
    QPushButton *ignore = nullptr;
    QPushButton *later = nullptr;

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QDialog::resizeEvent(event);
        // A narrower window can wrap the status onto another line. Height-only
        // adjustments from fitting must not schedule an endless resize cycle.
        if (frame && event->oldSize().width() != event->size().width())
            scheduleContentFit();
    }

    bool nativeEvent(const QByteArray &type, void *message, qintptr *result) override
    {
        return frame && frame->handleNativeEvent(type, message, result)
            ? true : QDialog::nativeEvent(type, message, result);
    }

private:
    void scheduleContentFit()
    {
        if (m_fitScheduled) return;
        m_fitScheduled = true;
        // Let visibility, native metrics and the frame's first-show polish
        // settle before measuring; repeated status refreshes share one fit.
        QTimer::singleShot(0, this, [this] {
            m_fitScheduled = false;
            fitContents();
        });
    }

    void fitContents()
    {
        if (!frame) return;
        auto *content = frame->contentWidget();
        content->ensurePolished();
        auto *body = content->layout();
        body->invalidate();
        body->activate();
        QSize bodyMinimum = body->totalMinimumSize();
        const QMargins border = layout()->contentsMargins();
        const QSize decoration(border.left() + border.right(),
                               border.top() + border.bottom() + frame->titleBar()->height());
        const QSize available = screen() ? screen()->availableGeometry().size() : QSize(800, 800);
        const QSize limit = (available - QSize(32, 32)).expandedTo(QSize(1, 1));
        // A long message may have a very wide size hint. Keep the current
        // width unless the controls need more, and measure wrapping at the
        // target width without subtracting a temporary vertical scrollbar.
        const int targetWidth = qMin(qMax(width(), bodyMinimum.width() + decoration.width()), limit.width());
        const int contentWidth = qMax(bodyMinimum.width(), targetWidth - decoration.width());
        const auto margins = body->contentsMargins();
        const int statusWidth = qMax(1, contentWidth - margins.left() - margins.right());
        // Qt can otherwise squeeze a word-wrapped, Fixed-policy QLabel back
        // to its earlier short-message height when notes/buttons share the
        // layout. Reserve its full wrapped height before fitting the frame.
        status->setMinimumHeight(qMax(status->fontMetrics().height(), status->heightForWidth(statusWidth)));
        body->invalidate();
        body->activate();
        bodyMinimum = body->totalMinimumSize();
        content->setMinimumSize(bodyMinimum);
        const int wrappedHeight = body->hasHeightForWidth()
            ? body->totalHeightForWidth(contentWidth) : body->totalSizeHint().height();
        const int requiredHeight = qMax(bodyMinimum.height(), wrappedHeight) + decoration.height();
        const QSize target(targetWidth, qMin(qMax(height(), requiredHeight), limit.height()));
        if (target != size()) resize(target);
    }

    DialogFrame *frame = nullptr;
    bool m_fitScheduled = false;
};

class UpdateNoticeDialog final : public QDialog {
public:
    explicit UpdateNoticeDialog(QWidget *parent) : QDialog(parent, Qt::Tool)
    {
        setObjectName(QStringLiteral("updateNoticeDialog"));
        setWindowTitle(QStringLiteral("发现新版本"));
        setWindowModality(Qt::NonModal);
        setModal(false);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setStyleSheet(playerStyleSheet());
        setMinimumWidth(300);
        auto *body = new QVBoxLayout;
        body->setContentsMargins(16, 12, 16, 12);
        label = plainLabel({}, this);
        label->setObjectName(QStringLiteral("updateNoticeText"));
        body->addWidget(label);
        auto *buttons = new QHBoxLayout;
        view = new QPushButton(QStringLiteral("查看更新"), this);
        view->setObjectName(QStringLiteral("updateViewDetails"));
        auto *later = new QPushButton(QStringLiteral("稍后"), this);
        later->setObjectName(QStringLiteral("updateNoticeLater"));
        view->setAutoDefault(false);
        later->setAutoDefault(false);
        buttons->addStretch();
        buttons->addWidget(later);
        buttons->addWidget(view);
        body->addLayout(buttons);
        frame = new DialogFrame(this, body, false);
        connect(later, &QPushButton::clicked, this, &QDialog::close);
    }

    QLabel *label = nullptr;
    QPushButton *view = nullptr;

protected:
    bool nativeEvent(const QByteArray &type, void *message, qintptr *result) override
    {
        return frame && frame->handleNativeEvent(type, message, result)
            ? true : QDialog::nativeEvent(type, message, result);
    }

private:
    DialogFrame *frame = nullptr;
};

UpdateNotificationController::UpdateNotificationController(QWidget *window, QMenu *settingsMenu,
    QSettings *settings, QObject *parent)
    : UpdateNotificationController(window, settingsMenu, settings, nullptr, {}, parent)
{}

UpdateNotificationController::UpdateNotificationController(QWidget *window, QMenu *settingsMenu,
    QSettings *settings, UpdateCheckService *service, UpdateNotificationOptions options, QObject *parent)
    : QObject(parent ? parent : window), m_window(window), m_settings(settings),
      m_service(service ? service : new UpdateCheckService(this)), m_options(std::move(options)),
      m_currentVersion(m_options.currentVersion.isEmpty() ? applicationBuildVersion() : m_options.currentVersion)
{
    if (!m_options.nowUtc) m_options.nowUtc = [] { return QDateTime::currentDateTimeUtc(); };
    if (!m_options.openUrl) m_options.openUrl = [](const QUrl &url) { return QDesktopServices::openUrl(url); };
    m_autoCheckEnabled = !m_settings || m_settings->value(Prefix + QStringLiteral("autoCheckEnabled"), true).toBool();
    const auto parsed = SemanticVersion::parse(m_currentVersion);
    const bool prerelease = parsed && parsed->isPrerelease();
    m_includePrereleases = m_settings && m_settings->contains(Prefix + QStringLiteral("includePrereleases"))
        ? m_settings->value(Prefix + QStringLiteral("includePrereleases")).toBool() : prerelease;
    if (settingsMenu) {
        settingsMenu->addSeparator();
        m_aboutAction = settingsMenu->addAction(QStringLiteral("关于软件…"));
        m_aboutAction->setObjectName(QStringLiteral("aboutSoftwareAction"));
        connect(m_aboutAction, &QAction::triggered, this, &UpdateNotificationController::showAbout);
    }
    connect(m_service, &UpdateCheckService::busyChanged, this,
            [this](bool) { if (!m_shuttingDown) refreshAbout(); });
    connect(m_service, &UpdateCheckService::finished, this, &UpdateNotificationController::finished);
    if (m_options.downloadService) downloadService();
    m_restartAvailabilityTimer = new QTimer(this);
    m_restartAvailabilityTimer->setInterval(500);
    connect(m_restartAvailabilityTimer, &QTimer::timeout, this, [this] {
        if (m_about && m_about->isVisible() && m_viewState == ViewState::Release
            && m_downloadService && m_downloadService->state() == UpdateDownloadState::Ready)
            refreshAbout();
    });
    m_restartAvailabilityTimer->start();
}

UpdateNotificationController::~UpdateNotificationController() { shutdown(); }

QDateTime UpdateNotificationController::nowUtc() const { return m_options.nowUtc().toUTC(); }

QDateTime UpdateNotificationController::savedDate(const QString &key) const
{
    if (!m_settings) return {};
    const auto value = m_settings->value(Prefix + key).toString();
    auto date = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!date.isValid()) date = QDateTime::fromString(value, Qt::ISODate);
    return date.toUTC();
}

bool UpdateNotificationController::isIgnored(const QString &version) const
{
    return m_settings && normalizedVersion(m_settings->value(Prefix + QStringLiteral("ignoredVersion")).toString())
        == normalizedVersion(version);
}

bool UpdateNotificationController::isRateLimited() const
{
    const auto until = savedDate(QStringLiteral("retryAfterUtc"));
    return until.isValid() && until > nowUtc();
}

void UpdateNotificationController::startAutomaticCheck()
{
    if (m_shuttingDown || !m_window || !m_autoCheckEnabled
        || !m_service || m_service->isBusy() || isRateLimited() || m_preparingInstall
        || (m_downloadService && m_downloadService->isBusy())) return;
    const auto now = nowUtc();
    const auto last = savedDate(QStringLiteral("lastAutomaticAttemptUtc"));
    // Clock rollback or a corrupt future timestamp must not disable checks forever.
    if (last.isValid() && last <= now && last.secsTo(now) < AutomaticIntervalSeconds) return;
    if (m_settings) m_settings->setValue(Prefix + QStringLiteral("lastAutomaticAttemptUtc"), now.toString(Qt::ISODateWithMs));
    beginCheck(false);
}

void UpdateNotificationController::checkManually()
{
    if (m_shuttingDown || !m_window) return;
    if (m_preparingInstall || (m_downloadService && m_downloadService->isBusy())) {
        showAbout();
        return;
    }
    if (isRateLimited()) {
        showStatus(QStringLiteral("检查更新暂时受到限流，请在 %1 后重试。")
            .arg(savedDate(QStringLiteral("retryAfterUtc")).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
        return;
    }
    if (m_service && m_service->isBusy()) {
        if (m_discardResult) {
            showStatus(QStringLiteral("上次检查正在取消，请稍后重新检查。"));
            return;
        }
        if (m_requestedPrereleases != m_includePrereleases) {
            showStatus(QStringLiteral("更新通道已变更，请稍后重新检查。"));
            return;
        }
        m_manual = true;
        showChecking();
        return;
    }
    beginCheck(true);
}

void UpdateNotificationController::beginCheck(bool manual)
{
    if (m_shuttingDown || !m_window || !m_service) {
        if (manual && !m_shuttingDown) showStatus(QStringLiteral("更新检查服务不可用，请稍后重试。"));
        return;
    }
    m_manual = manual;
    m_discardResult = false;
    m_requestedPrereleases = m_includePrereleases;
    m_preparedReleaseBeforeCheck = {};
    if (m_downloadService && m_downloadService->state() == UpdateDownloadState::Ready
        && sameReleasePayload(m_downloadService->result().release, m_latestRelease))
        m_preparedReleaseBeforeCheck = m_latestRelease;
    m_latestRelease = {};
    m_viewState = ViewState::Checking;
    m_status.clear();
    if (m_notice) m_notice->hide();
    if (manual) showChecking();
    else refreshAbout();
    m_service->start(m_currentVersion, m_requestedPrereleases);
}

void UpdateNotificationController::setAutomaticCheckEnabled(bool enabled)
{
    if (m_shuttingDown || enabled == m_autoCheckEnabled) return;
    m_autoCheckEnabled = enabled;
    if (m_settings) m_settings->setValue(Prefix + QStringLiteral("autoCheckEnabled"), enabled);
    if (!enabled && !m_manual && m_service && m_service->isBusy()) {
        m_discardResult = true;
        m_viewState = ViewState::Status;
        m_status = QStringLiteral("已取消自动检查更新。");
        m_service->cancel();
    }
    if (!enabled && m_notice) m_notice->hide();
    refreshAbout();
}

void UpdateNotificationController::setIncludePrereleases(bool enabled)
{
    if (m_shuttingDown || m_preparingInstall || enabled == m_includePrereleases) return;
    m_includePrereleases = enabled;
    if (m_settings) {
        m_settings->setValue(Prefix + QStringLiteral("includePrereleases"), enabled);
        m_settings->remove(Prefix + QStringLiteral("lastAutomaticAttemptUtc"));
    }
    if (m_notice) m_notice->hide();
    m_latestRelease = {};
    m_viewState = ViewState::NotChecked;
    m_status.clear();
    if (m_downloadService && m_downloadService->isBusy()) m_downloadService->cancel();
    m_preparedReleaseBeforeCheck = {};
    m_savedRestoreAttempted = false;
    m_restoringSavedRelease = false;
    if (m_service && m_service->isBusy()) {
        m_manual = false;
        m_discardResult = true;
        m_service->cancel();
    }
    refreshAbout();
}

void UpdateNotificationController::cancelCheck()
{
    if (!m_shuttingDown && !m_preparingInstall && m_downloadService && m_downloadService->isBusy()) {
        m_downloadService->cancel();
        return;
    }
    if (m_shuttingDown || !m_service || !m_service->isBusy()) return;
    // A transport result may already be queued, so cancellation also discards
    // that result instead of allowing it to turn into a new notification.
    m_manual = false;
    m_discardResult = true;
    m_latestRelease = {};
    m_viewState = ViewState::Status;
    m_status = QStringLiteral("已取消检查更新。");
    m_service->cancel();
    refreshAbout();
}

void UpdateNotificationController::finished(UpdateCheckResult result)
{
    if (m_shuttingDown || !m_window) return;
    const bool manual = std::exchange(m_manual, false);
    if (m_settings) {
        if (result.retryAfterUtc.isValid() && result.retryAfterUtc > nowUtc())
            m_settings->setValue(Prefix + QStringLiteral("retryAfterUtc"), result.retryAfterUtc.toUTC().toString(Qt::ISODateWithMs));
        else m_settings->remove(Prefix + QStringLiteral("retryAfterUtc"));
    }
    // Explicit cancellation can race with an already queued completion.
    if (std::exchange(m_discardResult, false)
        || m_requestedPrereleases != m_includePrereleases) return;
    if (result.state == UpdateCheckState::UpdateAvailable) {
        m_latestRelease = result.release;
        m_viewState = ViewState::Release;
        m_status.clear();
        if (isDownloadableUpdateRelease(m_latestRelease)) {
            auto *downloader = downloadService();
            if (!downloader->isBusy()
                && (downloader->state() != UpdateDownloadState::Ready
                    || !sameReleasePayload(downloader->result().release, m_latestRelease)))
                downloader->restore(m_latestRelease);
        }
        refreshAbout();
        // Closing About never reopens it when a request finishes. A manual
        // request also stays manual after closing, so it cannot become a toast.
        if (!manual && m_autoCheckEnabled && !isIgnored(result.release.version)
            && (!m_about || !m_about->isVisible())) showNotice(result.release);
    } else {
        if (result.state == UpdateCheckState::Failed && !m_preparedReleaseBeforeCheck.version.isEmpty()
            && m_downloadService && m_downloadService->state() == UpdateDownloadState::Ready
            && sameReleasePayload(m_downloadService->result().release, m_preparedReleaseBeforeCheck)) {
            m_latestRelease = std::exchange(m_preparedReleaseBeforeCheck, {});
            m_viewState = ViewState::Release;
            m_status = (result.message.isEmpty() ? QStringLiteral("检查更新失败，请稍后重试。") : result.message)
                + QStringLiteral("\n已下载并校验的更新仍可重启安装。");
            refreshAbout();
            return;
        }
        m_preparedReleaseBeforeCheck = {};
        m_latestRelease = {};
        m_viewState = ViewState::Status;
        if (result.state == UpdateCheckState::UpToDate) {
            if (m_settings) m_settings->remove(Prefix + QStringLiteral("preparedRelease"));
            m_status = QStringLiteral("你使用的已是最新版本。");
        } else if (result.state == UpdateCheckState::Cancelled) m_status = QStringLiteral("已取消检查更新。");
        else m_status = result.message.isEmpty() ? QStringLiteral("检查更新失败，请检查网络或代理后重试。") : result.message;
        if (m_notice) m_notice->hide();
        refreshAbout();
    }
}

AboutSoftwareDialog *UpdateNotificationController::aboutDialog()
{
    if (!m_about && m_window && !m_shuttingDown) {
        m_about = new AboutSoftwareDialog(m_currentVersion, m_window);
        connect(m_about->check, &QPushButton::clicked, this, &UpdateNotificationController::checkManually);
        connect(m_about->cancel, &QPushButton::clicked, this, &UpdateNotificationController::cancelCheck);
        connect(m_about->repository, &QPushButton::clicked, this, &UpdateNotificationController::openRepository);
        connect(m_about->download, &QPushButton::clicked, this, &UpdateNotificationController::downloadOrRestart);
        connect(m_about->releasePage, &QPushButton::clicked, this, &UpdateNotificationController::openRelease);
        connect(m_about->ignore, &QPushButton::clicked, this, &UpdateNotificationController::ignoreRelease);
        connect(m_about->automatic, &QCheckBox::toggled, this, &UpdateNotificationController::setAutomaticCheckEnabled);
        connect(m_about->prerelease, &QCheckBox::toggled, this, &UpdateNotificationController::setIncludePrereleases);
    }
    return m_about;
}

void UpdateNotificationController::refreshAbout()
{
    if (!m_about || m_shuttingDown) return;
    const bool busy = m_service && m_service->isBusy();
    const bool transferring = m_downloadService && m_downloadService->isBusy();
    const QSignalBlocker automaticBlocker(m_about->automatic);
    const QSignalBlocker prereleaseBlocker(m_about->prerelease);
    m_about->automatic->setChecked(m_autoCheckEnabled);
    m_about->prerelease->setChecked(m_includePrereleases);
    m_about->prerelease->setEnabled(!busy && !transferring && !m_preparingInstall);
    m_about->check->setEnabled(!busy && !transferring && !m_preparingInstall);
    m_about->check->setText(busy ? QStringLiteral("正在检查更新…") : QStringLiteral("检查更新"));
    m_about->cancel->setVisible((busy && !m_discardResult) || (transferring && !m_preparingInstall));
    m_about->cancel->setText(transferring ? QStringLiteral("取消下载") : QStringLiteral("取消检查"));
    m_about->download->setEnabled(!m_preparingInstall && !transferring);
    m_about->download->setToolTip({});
    m_about->ignore->setEnabled(!m_preparingInstall && !transferring);
    m_about->later->setEnabled(!m_preparingInstall);
    switch (m_viewState) {
    case ViewState::NotChecked:
        m_about->resultStatus(QStringLiteral("尚未检查更新"));
        break;
    case ViewState::Checking:
        m_about->checking();
        break;
    case ViewState::Status:
        m_about->resultStatus(m_status);
        break;
    case ViewState::Release:
        m_about->release(m_latestRelease);
        m_about->download->setText(isDownloadableUpdateRelease(m_latestRelease)
            ? QStringLiteral("下载更新") : QStringLiteral("前往下载"));
        if (!m_status.isEmpty()) m_about->status->setText(m_status);
        if (m_downloadService && sameReleasePayload(m_downloadService->result().release, m_latestRelease)) {
            const auto result = m_downloadService->result();
            switch (result.state) {
            case UpdateDownloadState::Downloading:
                m_about->transfer(QStringLiteral("正在下载更新：%1 / %2")
                    .arg(downloadSize(m_downloadedBytes), downloadSize(m_downloadTotalBytes)),
                    m_downloadedBytes, m_downloadTotalBytes, false);
                break;
            case UpdateDownloadState::Preparing:
                m_about->transfer(QStringLiteral("正在校验并准备更新文件…"), 0, 0, true);
                break;
            case UpdateDownloadState::Ready: {
                m_about->download->setText(QStringLiteral("重启并更新"));
                const auto blocked = m_options.restartBlockedReason ? m_options.restartBlockedReason() : QString();
                m_about->download->setEnabled(!m_preparingInstall && blocked.isEmpty());
                m_about->download->setToolTip(blocked);
                if (m_status.isEmpty()) m_about->status->setText(blocked.isEmpty()
                    ? QStringLiteral("新版本 %1 已准备就绪，重启后安装。").arg(m_latestRelease.version) : blocked);
                break;
            }
            case UpdateDownloadState::Failed:
            case UpdateDownloadState::Cancelled:
                if (m_status.isEmpty()) m_about->status->setText(result.message);
                m_about->download->setText(QStringLiteral("重新下载"));
                break;
            case UpdateDownloadState::Idle: break;
            }
        }
        if (m_preparingInstall) m_about->transfer(QStringLiteral("正在准备重启更新…"), 0, 0, true);
        break;
    }
}

UpdateDownloadService *UpdateNotificationController::downloadService()
{
    if (m_downloadService) return m_downloadService;
    m_downloadService = m_options.downloadService ? m_options.downloadService.data() : new UpdateDownloadService(this);
    connect(m_downloadService, &UpdateDownloadService::stateChanged, this,
            [this](UpdateDownloadState) { downloadChanged(); });
    connect(m_downloadService, &UpdateDownloadService::progressChanged, this, [this](qint64 done, qint64 total) {
        m_downloadedBytes = done;
        m_downloadTotalBytes = total;
        downloadChanged();
    });
    connect(m_downloadService, &UpdateDownloadService::finished, this, &UpdateNotificationController::downloadFinished);
    connect(m_downloadService, &UpdateDownloadService::shutdownFinished, this, [this] {
        m_downloadShutdownFinished = true;
        emit shutdownFinished();
    });
    return m_downloadService;
}

void UpdateNotificationController::downloadChanged()
{
    if (!m_shuttingDown) refreshAbout();
}

void UpdateNotificationController::downloadFinished(UpdateDownloadResult result)
{
    if (m_shuttingDown || !sameReleasePayload(result.release, m_latestRelease)) return;
    if (result.state == UpdateDownloadState::Ready) savePreparedRelease(result.release);
    else if (m_restoringSavedRelease && m_settings)
        m_settings->remove(Prefix + QStringLiteral("preparedRelease"));
    m_restoringSavedRelease = false;
    m_status.clear();
    refreshAbout();
}

void UpdateNotificationController::savePreparedRelease(const UpdateRelease &release)
{
    if (!m_settings || !isDownloadableUpdateRelease(release)) return;
    const auto bytes = QJsonDocument(QJsonObject{
        {QStringLiteral("schema"), 1}, {QStringLiteral("version"), release.version},
        {QStringLiteral("title"), release.title.left(1024)},
        {QStringLiteral("notes"), release.notes.left(16 * 1024)},
        {QStringLiteral("prerelease"), release.prerelease},
        {QStringLiteral("pageUrl"), release.pageUrl.toString(QUrl::FullyEncoded)},
        {QStringLiteral("packageUrl"), release.packageUrl.toString(QUrl::FullyEncoded)},
        {QStringLiteral("checksumUrl"), release.checksumUrl.toString(QUrl::FullyEncoded)},
        {QStringLiteral("packageSize"), double(release.packageSize)},
        {QStringLiteral("sha256"), release.packageSha256}
    }).toJson(QJsonDocument::Compact);
    if (bytes.size() <= MaximumSavedReleaseBytes)
        m_settings->setValue(Prefix + QStringLiteral("preparedRelease"), QString::fromUtf8(bytes));
}

void UpdateNotificationController::restorePreparedRelease()
{
    if (m_savedRestoreAttempted || !m_settings || m_preparingInstall
        || (m_service && m_service->isBusy()) || (m_downloadService && m_downloadService->isBusy())
        || !m_latestRelease.version.isEmpty() || m_viewState == ViewState::Checking) return;
    m_savedRestoreAttempted = true;
    const auto key = Prefix + QStringLiteral("preparedRelease");
    const auto saved = m_settings->value(key).toString();
    if (saved.isEmpty()) return;
    const auto invalid = [&] { m_settings->remove(key); };
    if (saved.size() > MaximumSavedReleaseBytes) { invalid(); return; }
    const auto bytes = saved.toUtf8();
    if (bytes.size() > MaximumSavedReleaseBytes) { invalid(); return; }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    const auto object = document.object();
    UpdateRelease release;
    release.version = object.value(QStringLiteral("version")).toString();
    release.title = object.value(QStringLiteral("title")).toString();
    release.notes = object.value(QStringLiteral("notes")).toString();
    release.prerelease = object.value(QStringLiteral("prerelease")).toBool();
    release.pageUrl = QUrl(object.value(QStringLiteral("pageUrl")).toString(), QUrl::StrictMode);
    release.packageUrl = QUrl(object.value(QStringLiteral("packageUrl")).toString(), QUrl::StrictMode);
    release.checksumUrl = QUrl(object.value(QStringLiteral("checksumUrl")).toString(), QUrl::StrictMode);
    release.packageSha256 = object.value(QStringLiteral("sha256")).toString();
    const auto size = object.value(QStringLiteral("packageSize")).toDouble();
    const auto current = SemanticVersion::parse(m_currentVersion);
    const auto version = SemanticVersion::parse(release.version);
    if (parse.error != QJsonParseError::NoError || !document.isObject()
        || object.value(QStringLiteral("schema")).toInt() != 1
        || !object.value(QStringLiteral("prerelease")).isBool()
        || release.title.size() > 1024 || release.notes.size() > 16 * 1024
        || !std::isfinite(size) || size <= 0 || size > 2.0 * 1024 * 1024 * 1024 || size != std::floor(size)
        || !current || !version || SemanticVersion::compare(*version, *current) <= 0) {
        invalid();
        return;
    }
    release.packageSize = qint64(size);
    if (!isDownloadableUpdateRelease(release)) { invalid(); return; }
    if (!m_includePrereleases && (release.prerelease || version->isPrerelease())) return;
    m_latestRelease = release;
    m_viewState = ViewState::Release;
    m_status.clear();
    m_restoringSavedRelease = true;
    if (!downloadService()->restore(release)) {
        m_restoringSavedRelease = false;
        invalid();
        m_status = QStringLiteral("上次下载的更新缓存已失效，请重新下载。");
    }
}

void UpdateNotificationController::downloadOrRestart()
{
    if (m_shuttingDown || m_preparingInstall || m_viewState != ViewState::Release) return;
    if (!isDownloadableUpdateRelease(m_latestRelease)) { openRelease(); return; }
    auto *downloader = downloadService();
    if (downloader->isBusy()) return;
    const auto result = downloader->result();
    if (result.state == UpdateDownloadState::Ready && sameReleasePayload(result.release, m_latestRelease)) {
        const auto blocked = m_options.restartBlockedReason ? m_options.restartBlockedReason() : QString();
        if (!blocked.isEmpty()) { m_status = blocked; refreshAbout(); return; }
        if (!m_options.requestRestart) {
            installPreparationFailed(QStringLiteral("当前运行方式不支持自动安装，请从发布页手动下载。"));
            return;
        }
        m_preparingInstall = true;
        m_status.clear();
        refreshAbout();
        m_options.requestRestart(result);
        return;
    }
    m_status.clear();
    m_downloadedBytes = 0;
    m_downloadTotalBytes = m_latestRelease.packageSize;
    downloader->start(m_latestRelease);
    refreshAbout();
}

void UpdateNotificationController::installPreparationFailed(const QString &message)
{
    if (m_shuttingDown) return;
    m_preparingInstall = false;
    m_status = message;
    refreshAbout();
}

void UpdateNotificationController::showAbout()
{
    if (m_shuttingDown || !m_window) return;
    restorePreparedRelease();
    if (auto *dialog = aboutDialog()) {
        if (m_notice) m_notice->hide();
        refreshAbout();
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
    }
}

void UpdateNotificationController::showChecking()
{
    m_viewState = ViewState::Checking;
    m_status.clear();
    showAbout();
}

void UpdateNotificationController::showStatus(const QString &message)
{
    const bool prepared = m_downloadService && m_downloadService->state() == UpdateDownloadState::Ready
        && sameReleasePayload(m_downloadService->result().release, m_latestRelease);
    m_viewState = prepared ? ViewState::Release : ViewState::Status;
    m_status = message;
    showAbout();
}

void UpdateNotificationController::showRelease(const UpdateRelease &release)
{
    if (m_shuttingDown || !isAllowedReleasePageUrl(release.pageUrl)) return;
    m_latestRelease = release;
    m_viewState = ViewState::Release;
    m_status.clear();
    if (m_notice) m_notice->hide();
    showAbout();
}

void UpdateNotificationController::showNotice(const UpdateRelease &release)
{
    if (m_shuttingDown || !m_window || !m_window->isVisible() || m_window->isMinimized()) return;
    if (!m_notice) {
        m_notice = new UpdateNoticeDialog(m_window);
        connect(m_notice->view, &QPushButton::clicked, this, [this] { showRelease(m_latestRelease); });
    }
    m_notice->label->setText(QStringLiteral("Arcanblade %1 已发布，可查看更新说明并下载。").arg(release.version));
    m_notice->adjustSize();
    const QPoint desired = m_window->mapToGlobal(QPoint(
        qMax(0, m_window->width() - m_notice->width() - 16),
        qMax(0, m_window->height() - m_notice->height() - 16)));
    const QRect available = m_window->screen() ? m_window->screen()->availableGeometry() : QRect(desired, m_notice->size());
    m_notice->move(qBound(available.left(), desired.x(), qMax(available.left(), available.right()-m_notice->width()+1)),
                   qBound(available.top(), desired.y(), qMax(available.top(), available.bottom()-m_notice->height()+1)));
    m_notice->show();
}

void UpdateNotificationController::openRelease()
{
    if (m_shuttingDown || !isAllowedReleasePageUrl(m_latestRelease.pageUrl)) return;
    QPointer<UpdateNotificationController> guard(this);
    const bool opened = m_options.openUrl(m_latestRelease.pageUrl);
    if (!guard || m_shuttingDown) return;
    if (opened) {
        if (m_notice) m_notice->hide();
    } else {
        m_status = QStringLiteral("无法打开浏览器，请稍后重试。");
        refreshAbout();
    }
}

void UpdateNotificationController::openRepository()
{
    if (m_shuttingDown || !m_window) return;
    QPointer<UpdateNotificationController> guard(this);
    const bool opened = m_options.openUrl(QUrl(QStringLiteral("https://github.com/leiding555/AsmrPlayer-Arcanblade")));
    if (!guard || m_shuttingDown) return;
    if (!opened) {
        if (m_viewState != ViewState::Release) m_viewState = ViewState::Status;
        m_status = QStringLiteral("无法打开浏览器，请稍后重试。");
        refreshAbout();
    }
}

void UpdateNotificationController::ignoreRelease()
{
    if (m_shuttingDown || m_preparingInstall || m_latestRelease.version.isEmpty()
        || (m_downloadService && m_downloadService->isBusy())) return;
    if (m_settings) m_settings->setValue(Prefix + QStringLiteral("ignoredVersion"), normalizedVersion(m_latestRelease.version));
    if (m_about) m_about->hide();
    if (m_notice) m_notice->hide();
}

void UpdateNotificationController::shutdown()
{
    if (m_shuttingDown) return;
    m_shuttingDown = true;
    if (m_restartAvailabilityTimer) m_restartAvailabilityTimer->stop();
    if (m_downloadService) {
        m_downloadShutdownFinished = false;
        m_downloadService->shutdown();
    }
    if (m_service) {
        disconnect(m_service, nullptr, this, nullptr);
        m_service->cancel();
    }
    if (m_aboutAction) m_aboutAction->setEnabled(false);
    if (m_notice) { m_notice->hide(); m_notice->deleteLater(); }
    if (m_about) {
        m_about->setEnabled(false);
        m_about->hide();
        m_about->deleteLater();
    }
}
} // namespace asmr
