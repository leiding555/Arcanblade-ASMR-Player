#include "ui/CoverSelectionDialog.h"

#include "artwork/CoverArtService.h"
#include "ui/CoverArtWidget.h"
#include "ui/DialogFrame.h"

#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFrame>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QPointer>
#include <QPromise>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QStringList>
#include <QStyle>
#include <QTableView>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <exception>
#include <utility>

namespace asmr {
namespace {
QLabel *label(const QString &name, QWidget *parent)
{
    auto *result = new QLabel(parent);
    result->setObjectName(name);
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    result->setMinimumWidth(0);
    result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return result;
}
QString tooltip(const QString &text) { return QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped()); }
QString parentPath(const QString &path)
{
    const auto separator = path.lastIndexOf(QLatin1Char('/'));
    return separator < 0 ? QString() : path.left(separator);
}
}

CoverSelectionDialog::CoverSelectionDialog(const QString &workName, const QString &rootPath,
                                         QWidget *parent)
    : QDialog(parent), m_rootPath(rootPath), m_previewService(new CoverArtService(this))
{
    setObjectName(QStringLiteral("coverSelectionDialog"));
    setWindowTitle(QStringLiteral("选择作品封面"));
    setWindowModality(Qt::WindowModal);
    resize(700, 600);
    setMinimumSize(460, 440);
    auto *title = label(QStringLiteral("coverSelectionWorkLabel"), this);
    title->setText(workName);
    auto *root = label(QStringLiteral("coverSelectionRootLabel"), this);
    root->setText(rootPath);
    root->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->setProperty("subtleLabel", true);
    m_manualLabel = label(QStringLiteral("coverSelectionManualLabel"), this);
    setManualSelection({});

    m_upButton = new QPushButton(QStringLiteral("返回上级"), this);
    m_upButton->setObjectName(QStringLiteral("coverSelectionUpButton"));
    m_reloadButton = new QPushButton(QStringLiteral("刷新"), this);
    m_reloadButton->setObjectName(QStringLiteral("coverSelectionRefreshButton"));
    m_breadcrumbScroll = new QScrollArea(this);
    m_breadcrumbScroll->setObjectName(QStringLiteral("coverSelectionBreadcrumbScroll"));
    m_breadcrumbScroll->setFrameShape(QFrame::NoFrame);
    m_breadcrumbScroll->setWidgetResizable(true);
    m_breadcrumbScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_breadcrumbScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_breadcrumbBar = new QWidget;
    m_breadcrumbBar->setObjectName(QStringLiteral("coverSelectionBreadcrumbBar"));
    m_breadcrumbLayout = new QHBoxLayout(m_breadcrumbBar);
    m_breadcrumbLayout->setContentsMargins(0, 0, 0, 0);
    m_breadcrumbLayout->setSpacing(4);
    m_breadcrumbLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);
    m_breadcrumbScroll->setWidget(m_breadcrumbBar);
    m_breadcrumbBar->setAutoFillBackground(false);
    m_breadcrumbScroll->viewport()->setAutoFillBackground(false);
    auto *navigation = new QHBoxLayout;
    navigation->addWidget(m_upButton);
    navigation->addWidget(m_breadcrumbScroll, 1);
    navigation->addWidget(m_reloadButton);
    rebuildBreadcrumbs();

    m_model = new QStandardItemModel(this);
    m_model->setHorizontalHeaderLabels({QStringLiteral("文件夹／图片"), QStringLiteral("类型")});
    m_view = new QTableView(this);
    m_view->setObjectName(QStringLiteral("coverSelectionEntriesView"));
    m_view->setAccessibleName(QStringLiteral("作品目录内的文件夹和图片"));
    m_view->setModel(m_model);
    m_view->verticalHeader()->hide();
    m_view->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_view->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setSortingEnabled(false);
    m_view->setWordWrap(false);
    m_view->setShowGrid(false);
    m_view->setMinimumSize(0, 80);
    m_view->installEventFilter(this);
    m_preview = new CoverArtWidget(this);
    m_preview->setObjectName(QStringLiteral("coverSelectionPreview"));
    m_preview->setFixedSize(112, 112);
    m_selectionLabel = label(QStringLiteral("coverSelectionSelectedLabel"), this);
    m_selectionLabel->setText(QStringLiteral("选择图片预览，再点击“设为封面”。"));
    auto *previewLayout = new QHBoxLayout;
    previewLayout->addWidget(m_preview);
    previewLayout->addWidget(m_selectionLabel, 1);
    m_feedbackLabel = label(QStringLiteral("coverSelectionFeedbackLabel"), this);
    auto *buttons = new QDialogButtonBox(this);
    m_chooseButton = buttons->addButton(QStringLiteral("设为封面"), QDialogButtonBox::ActionRole);
    m_chooseButton->setObjectName(QStringLiteral("coverSelectionChooseButton"));
    m_chooseButton->setProperty("primary", true);
    m_chooseButton->setDefault(false);
    m_chooseButton->setAutoDefault(false);
    m_cancelButton = buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
    m_cancelButton->setObjectName(QStringLiteral("coverSelectionCancelButton"));
    m_cancelButton->setAutoDefault(false);
    auto *layout = new QVBoxLayout;
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);
    layout->addWidget(title);
    layout->addWidget(root);
    layout->addWidget(m_manualLabel);
    layout->addLayout(navigation);
    layout->addWidget(m_view, 1);
    layout->addLayout(previewLayout);
    layout->addWidget(m_feedbackLabel);
    layout->addWidget(buttons);
    m_dialogFrame = new DialogFrame(this, layout);
    connect(m_upButton, &QPushButton::clicked, this, &CoverSelectionDialog::navigateUp);
    connect(m_reloadButton, &QPushButton::clicked, this, [this] { navigate(m_currentDirectory); });
    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex &current) { selectEntry(current); });
    connect(m_view, &QTableView::doubleClicked, this, &CoverSelectionDialog::activateEntry);
    connect(m_chooseButton, &QPushButton::clicked, this, [this] {
        if (!m_shuttingDown && !m_saving && !m_loading && m_previewReady && !m_selectedPath.isEmpty())
            emit coverChosen(m_selectedPath);
    });
    connect(m_previewService, &CoverArtService::resultReady, this,
            [this](quint64 id, const CoverArtResult &result) {
        if (m_shuttingDown || id != m_previewRequest || m_selectedPath.isEmpty()) return;
        m_previewReady = result.status == CoverArtStatus::Ready && !result.image.isNull();
        if (m_previewReady) {
            m_preview->setImage(result.image, result.sourcePath);
            setFeedback(QStringLiteral("预览已就绪。"));
        } else {
            m_preview->clearImage(QStringLiteral("图片不可用"));
            setFeedback(result.message.isEmpty() ? QStringLiteral("无法读取这张图片，请选择其他图片。") : result.message);
        }
        updateControls();
    });
    connect(m_previewService, &CoverArtService::shutdownFinished, this, [this] {
        m_previewShutdownFinished = true;
        finishShutdownIfReady();
    });
    updateControls();
    navigate({});
}

CoverSelectionDialog::~CoverSelectionDialog()
{
    if (m_cancelled) m_cancelled->store(true, std::memory_order_relaxed);
    if (m_active) m_lifetime.waitForFinished();
    delete m_previewService; // Its direct-destruction fallback waits for its own worker.
}
void CoverSelectionDialog::setManualSelection(const QString &relativePath, bool unavailable)
{
    m_manualPath = relativePath;
    m_manualLabel->setText(relativePath.isEmpty() ? QStringLiteral("当前设置：自动查找")
        : QStringLiteral("手动选择：") + relativePath + (unavailable
            ? QStringLiteral("\n手动图片当前不可用，已保留选择；临时使用自动查找。")
            : QStringLiteral("\n图片失效时保留此选择，并临时使用自动查找。")));
    m_manualLabel->setToolTip(tooltip(m_manualLabel->text()));
}
void CoverSelectionDialog::setSaving(bool saving)
{
    m_saving = saving;
    if (saving) setFeedback(QStringLiteral("正在保存封面…"));
    updateControls();
}
void CoverSelectionDialog::setFeedback(const QString &message)
{
    m_feedbackLabel->setText(message);
    m_feedbackLabel->setToolTip(tooltip(message));
}
bool CoverSelectionDialog::isIdle() const
{
    return !m_active && !m_pending && m_previewService->isIdle();
}
void CoverSelectionDialog::navigate(const QString &directory)
{
    if (m_shuttingDown || m_saving) return;
    const auto id = ++m_browseGeneration;
    m_loading = true;
    m_previewReady = false;
    m_selectedPath.clear();
    m_previewRequest = 0;
    m_preview->clearImage(QStringLiteral("选择图片预览"));
    m_selectionLabel->setText(QStringLiteral("选择图片预览，再点击“设为封面”。"));
    setFeedback(QStringLiteral("正在读取目录…"));
    updateControls();
    if (m_active) {
        m_cancelled->store(true, std::memory_order_relaxed);
        m_pending = Pending{id, directory};
    } else startListing({id, directory});
}
void CoverSelectionDialog::startListing(Pending pending)
{
    m_active = true;
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    auto promise = std::make_shared<QPromise<CoverDirectoryPage>>();
    auto *watcher = new QFutureWatcher<CoverDirectoryPage>(this);
    connect(watcher, &QFutureWatcher<CoverDirectoryPage>::finished, this,
            [this, watcher, id = pending.id] {
        const auto page = watcher->result();
        watcher->deleteLater();
        m_active = false;
        m_cancelled.reset();
        if (!m_shuttingDown && id == m_browseGeneration) {
            m_loading = false;
            if (page.status == CoverDirectoryStatus::Ready) applyPage(page);
            else if (page.status != CoverDirectoryStatus::Cancelled) setFeedback(page.message);
            updateControls();
        }
        if (!m_shuttingDown && m_pending) {
            auto next = std::move(*m_pending);
            m_pending.reset();
            startListing(std::move(next));
        }
        finishShutdownIfReady();
    });
    m_lifetime = promise->future();
    watcher->setFuture(m_lifetime);
    const auto cancelled = m_cancelled;
    QThreadPool::globalInstance()->start([promise, root = m_rootPath,
                                         directory = std::move(pending.directory), cancelled] {
        promise->start();
        CoverDirectoryPage page;
        try { page = CoverDirectoryBrowser::list(root, directory, cancelled); }
        catch (const std::exception &) { page.message = QStringLiteral("目录读取失败，请重试。"); }
        catch (...) { page.message = QStringLiteral("目录读取失败，请重试。"); }
        promise->addResult(std::move(page));
        promise->finish();
    });
}
void CoverSelectionDialog::applyPage(const CoverDirectoryPage &page)
{
    m_currentDirectory = page.relativeDirectory;
    m_model->removeRows(0, m_model->rowCount());
    for (const auto &entry : page.entries) {
        auto *name = new QStandardItem(entry.name);
        name->setData(entry.relativePath, RelativePathRole);
        name->setData(entry.directory, DirectoryRole);
        name->setToolTip(tooltip(entry.relativePath));
        name->setIcon(style()->standardIcon(entry.directory ? QStyle::SP_DirIcon : QStyle::SP_FileIcon));
        auto *type = new QStandardItem(entry.directory ? QStringLiteral("文件夹") : QStringLiteral("图片"));
        m_model->appendRow({name, type});
    }
    m_view->clearSelection();
    m_view->setCurrentIndex({});
    rebuildBreadcrumbs();
    setFeedback(page.entries.isEmpty() ? QStringLiteral("此目录没有可选择的图片或文件夹。")
                                      : QStringLiteral("双击文件夹或按 Enter 进入；选择图片后设为封面。"));
}
void CoverSelectionDialog::rebuildBreadcrumbs()
{
    while (auto *item = m_breadcrumbLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const auto add = [this](const QString &text, const QString &path) {
        auto *button = new QToolButton(m_breadcrumbBar);
        button->setText(text);
        button->setAutoRaise(true);
        button->setProperty("relativePath", path);
        button->setToolTip(tooltip(path.isEmpty() ? m_rootPath : path));
        button->setEnabled(!m_saving && !m_shuttingDown && path != m_currentDirectory);
        connect(button, &QToolButton::clicked, this, [this, path] { navigate(path); });
        m_breadcrumbLayout->addWidget(button);
        return button;
    };
    const auto *root = add(QStringLiteral("作品根"), {});
    m_breadcrumbScroll->setFixedHeight(root->sizeHint().height()
        + style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 4);
    QString path;
    for (const auto &part : m_currentDirectory.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        path += (path.isEmpty() ? QString() : QStringLiteral("/")) + part;
        add(part, path);
    }
    m_breadcrumbLayout->addStretch();
    QTimer::singleShot(0, this, [this] {
        const auto buttons = m_breadcrumbBar->findChildren<QToolButton *>();
        if (!buttons.isEmpty()) m_breadcrumbScroll->ensureWidgetVisible(buttons.last());
    });
}
void CoverSelectionDialog::selectEntry(const QModelIndex &index)
{
    m_selectedPath.clear();
    m_previewReady = false;
    m_previewRequest = 0;
    m_preview->clearImage(QStringLiteral("选择图片预览"));
    if (!m_shuttingDown && !m_loading && !m_saving && index.isValid()) {
        const auto name = index.siblingAtColumn(0);
        const QString path = name.data(RelativePathRole).toString();
        m_selectionLabel->setText(path);
        m_selectionLabel->setToolTip(tooltip(path));
        if (!name.data(DirectoryRole).toBool()) {
            m_selectedPath = path;
            CoverArtRequest request;
            request.rootPath = m_rootPath;
            request.libraryWork = true;
            request.forceReload = true;
            request.manualRelativePath = path;
            request.manualOnly = true;
            m_previewRequest = m_previewService->request(request);
            setFeedback(QStringLiteral("正在读取预览…"));
        }
    }
    updateControls();
}
void CoverSelectionDialog::activateEntry(const QModelIndex &index)
{
    if (m_shuttingDown || m_loading || m_saving || !index.isValid()) return;
    const auto name = index.siblingAtColumn(0);
    if (name.data(DirectoryRole).toBool()) navigate(name.data(RelativePathRole).toString());
}
void CoverSelectionDialog::navigateUp()
{
    if (!m_currentDirectory.isEmpty()) navigate(parentPath(m_currentDirectory));
}
bool CoverSelectionDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_view && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            if (!key->isAutoRepeat()) activateEntry(m_view->currentIndex());
            return true;
        }
        if ((key->key() == Qt::Key_Backspace && key->modifiers() == Qt::NoModifier)
            || (key->key() == Qt::Key_Up && key->modifiers() == Qt::AltModifier)) {
            if (!key->isAutoRepeat()) navigateUp();
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}
void CoverSelectionDialog::updateControls()
{
    const bool enabled = !m_shuttingDown && !m_saving;
    m_view->setEnabled(enabled && !m_loading);
    m_upButton->setEnabled(enabled && !m_currentDirectory.isEmpty());
    m_reloadButton->setEnabled(enabled);
    m_chooseButton->setEnabled(enabled && !m_loading && m_previewReady && !m_selectedPath.isEmpty());
    m_cancelButton->setEnabled(enabled);
    if (m_dialogFrame) m_dialogFrame->setCloseEnabled(enabled);
    for (auto *button : m_breadcrumbBar->findChildren<QToolButton *>())
        button->setEnabled(enabled && button->property("relativePath").toString() != m_currentDirectory);
}
bool CoverSelectionDialog::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    if (m_dialogFrame && m_dialogFrame->handleNativeEvent(eventType, message, result))
        return true;
    return QDialog::nativeEvent(eventType, message, result);
}
void CoverSelectionDialog::closeEvent(QCloseEvent *event)
{
    if (m_saving && !m_shuttingDown) {
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}
void CoverSelectionDialog::done(int result)
{
    if (m_saving && !m_shuttingDown) return;
    m_requestedResult = result;
    hide();
    shutdown();
}
void CoverSelectionDialog::shutdown()
{
    if (!m_shuttingDown) {
        m_shuttingDown = true;
        ++m_browseGeneration;
        m_pending.reset();
        m_previewRequest = 0;
        if (m_cancelled) m_cancelled->store(true, std::memory_order_relaxed);
        m_previewService->shutdown();
        updateControls();
    }
    QMetaObject::invokeMethod(this, &CoverSelectionDialog::finishShutdownIfReady, Qt::QueuedConnection);
}
void CoverSelectionDialog::finishShutdownIfReady()
{
    if (!m_shuttingDown || m_shutdownNotified || !isIdle() || !m_previewShutdownFinished) return;
    m_shutdownNotified = true;
    QPointer<CoverSelectionDialog> guard(this);
    emit shutdownFinished();
    if (guard && m_requestedResult) QDialog::done(*m_requestedResult);
}
} // namespace asmr
