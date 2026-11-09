#include "ui/CategoryEditDialog.h"

#include "ui/CoverArtWidget.h"
#include "ui/TitleBarWidget.h"
#include "ui/WindowFrameController.h"

#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPromise>
#include <QPushButton>
#include <QThreadPool>
#include <QToolButton>
#include <QVBoxLayout>

namespace asmr {
namespace {
CoverArtResult automaticCover(const ManualCoverSettings &settings,
    const QVector<LibraryWork> &members, const std::shared_ptr<std::atomic_bool> &cancelled)
{
    for (const auto &work : members) {
        if (cancelled->load()) { CoverArtResult result; result.status = CoverArtStatus::Cancelled; return result; }
        CoverArtRequest request;
        request.rootPath = work.rootPath;
        request.libraryWork = true;
        request.forceReload = true;
        request.manualLookup = ManualCoverLookup{settings, {work.id, work.rootPath, work.importedAtMs}};
        auto result = CoverArtLoader::load(request, {}, cancelled);
        if (result.status == CoverArtStatus::Cancelled
            || (result.status == CoverArtStatus::Ready && !result.image.isNull())) return result;
    }
    return {};
}
}

CategoryEditDialog::CategoryEditDialog(const WorkCategory &category, const ManualCoverSettings &settings,
    const QVector<LibraryWork> &orderedMembers, QWidget *parent)
    : QDialog(parent), m_category(category), m_settings(settings), m_members(orderedMembers)
{
    setObjectName(QStringLiteral("workCategoryNameDialog"));
    setWindowTitle(category.id ? QStringLiteral("编辑分类") : QStringLiteral("新建分类"));
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint);
    setWindowModality(Qt::WindowModal);
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(QStringLiteral("QDialog#workCategoryNameDialog { background: #1F232A; border: 1px solid #39414D; border-radius: 10px; }"));
    setMinimumWidth(360);
    resize(440, category.id ? 510 : 210);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    m_titleBar = new TitleBarWidget(this, TitleBarWidget::Mode::Dialog);
    m_titleBar->setTitle(windowTitle());
    m_frame = new WindowFrameController(this, m_titleBar);
    connect(m_titleBar, &TitleBarWidget::closeRequested, this, &QDialog::reject);
    connect(m_titleBar, &TitleBarWidget::systemMoveRequested, m_frame, &WindowFrameController::startSystemMove);
    connect(m_titleBar, &TitleBarWidget::systemMenuRequested, m_frame, &WindowFrameController::showSystemMenu);
    layout->addWidget(m_titleBar);
    auto *body = new QWidget(this);
    auto *form = new QVBoxLayout(body);
    form->setContentsMargins(20, 12, 20, 20);
    form->setSpacing(10);
    auto *label = new QLabel(QStringLiteral("分类名称"), body);
    m_name = new QLineEdit(category.name, body);
    m_name->setObjectName(QStringLiteral("workCategoryNameEdit"));
    m_name->setAccessibleName(QStringLiteral("分类名称"));
    label->setBuddy(m_name);
    form->addWidget(label);
    form->addWidget(m_name);
    m_preview = new CoverArtWidget(body);
    m_preview->setObjectName(QStringLiteral("categoryEditCoverPreview"));
    m_preview->setContextName(QStringLiteral("分类封面预览"));
    m_preview->setFixedSize(200, 200);
    form->addWidget(m_preview, 0, Qt::AlignHCenter);
    auto *coverButtons = new QWidget(body);
    auto *coverLayout = new QHBoxLayout(coverButtons);
    coverLayout->setContentsMargins(0, 0, 0, 0);
    m_choose = new QPushButton(QStringLiteral("选择图片…"), coverButtons);
    m_choose->setObjectName(QStringLiteral("categoryEditChooseCoverButton"));
    m_automatic = new QPushButton(QStringLiteral("恢复自动封面"), coverButtons);
    m_automatic->setObjectName(QStringLiteral("categoryEditAutomaticCoverButton"));
    m_automatic->setToolTip(QStringLiteral("保存后，使用最近加入且有真实封面的作品；分类名称保持不变。"));
    coverLayout->addWidget(m_choose);
    coverLayout->addWidget(m_automatic);
    form->addWidget(coverButtons);
    auto *hint = new QLabel(QStringLiteral("支持 JPG、PNG、WebP、BMP、GIF，最大 32 MiB。保存图片副本，不修改原图。"), body);
    hint->setProperty("subtleLabel", true);
    hint->setWordWrap(true);
    form->addWidget(hint);
    m_preview->setVisible(category.id > 0);
    coverButtons->setVisible(category.id > 0);
    hint->setVisible(category.id > 0);
    m_feedback = new QLabel(body);
    m_feedback->setObjectName(QStringLiteral("workCategoryNameFeedback"));
    m_feedback->setTextFormat(Qt::PlainText);
    m_feedback->setWordWrap(true);
    m_feedback->setProperty("errorLabel", true);
    form->addWidget(m_feedback);
    form->addStretch();
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    m_save = new QPushButton(QStringLiteral("保存"), body);
    m_save->setObjectName(QStringLiteral("saveWorkCategoryNameButton"));
    m_save->setProperty("primary", true);
    m_save->setDefault(true);
    m_cancel = new QPushButton(QStringLiteral("取消"), body);
    m_cancel->setObjectName(QStringLiteral("cancelWorkCategoryNameButton"));
    buttons->addWidget(m_save);
    buttons->addWidget(m_cancel);
    form->addLayout(buttons);
    layout->addWidget(body);
    for (auto *button : {m_choose, m_automatic, m_cancel}) { button->setAutoDefault(false); button->setDefault(false); }
    connect(m_name, &QLineEdit::textChanged, this, &CategoryEditDialog::updateControls);
    connect(m_save, &QPushButton::clicked, this, [this] { if (m_save->isEnabled()) emit saveRequested(); });
    connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_choose, &QPushButton::clicked, this, &CategoryEditDialog::chooseLocalFile);
    connect(m_automatic, &QPushButton::clicked, this, [this] {
        if (m_active || m_saving || m_shuttingDown) return;
        m_draft = {};
        m_coverChange = m_category.managedCoverFile.isEmpty() ? CoverChange::Keep : CoverChange::Automatic;
        startPreview(PreviewKind::Automatic);
    });
    updateControls();
    m_name->selectAll();
    m_name->setFocus();
    if (category.id > 0) startPreview(PreviewKind::Initial);
}

CategoryEditDialog::~CategoryEditDialog()
{
    if (m_cancelled) m_cancelled->store(true);
    if (m_active) m_lifetime.waitForFinished();
}

QString CategoryEditDialog::name() const { return m_name->text().trimmed(); }
void CategoryEditDialog::setSaving(bool saving) { m_saving = saving; updateControls(); }
void CategoryEditDialog::setFeedback(const QString &message)
{
    m_feedback->setText(message);
    m_feedback->setVisible(!message.isEmpty());
}

void CategoryEditDialog::chooseLocalFile()
{
    if (m_shuttingDown || m_saving || m_active || m_fileDialog) return;
    auto *dialog = new QFileDialog(this, QStringLiteral("选择分类封面"));
    dialog->setObjectName(QStringLiteral("categoryEditCoverFileDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setAcceptMode(QFileDialog::AcceptOpen);
    dialog->setNameFilter(QStringLiteral("图片 (*.jpg *.jpeg *.png *.webp *.bmp *.gif)"));
    m_fileDialog = dialog;
    connect(dialog, &QFileDialog::fileSelected, this, &CategoryEditDialog::selectLocalFile);
    connect(dialog, &QFileDialog::finished, this, [this] { m_fileDialog = nullptr; updateControls(); });
    dialog->open();
    updateControls();
}

void CategoryEditDialog::selectLocalFile(const QString &path)
{
    if (m_shuttingDown || m_saving || m_active || m_category.id <= 0 || path.isEmpty()) return;
    m_coverChange = CoverChange::Replace;
    m_draft = {};
    startPreview(PreviewKind::External, path);
}

void CategoryEditDialog::startPreview(PreviewKind kind, const QString &path)
{
    m_active = true;
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    m_preview->clearImage(QStringLiteral("正在读取封面…"));
    setFeedback({});
    updateControls();
    auto promise = std::make_shared<QPromise<PreviewResult>>();
    auto *watcher = new QFutureWatcher<PreviewResult>(this);
    connect(watcher, &QFutureWatcher<PreviewResult>::finished, this, [this, watcher, kind, path] {
        auto result = watcher->result();
        watcher->deleteLater();
        m_active = false;
        m_cancelled.reset();
        if (!m_shuttingDown) {
            if (kind == PreviewKind::External) {
                m_draft = std::move(result.draft);
                if (m_draft.success) m_preview->setImage(m_draft.image, path);
                else { m_preview->clearImage(QStringLiteral("图片不可用")); setFeedback(m_draft.errorMessage); }
            } else if (result.cover.status == CoverArtStatus::Ready && !result.cover.image.isNull()) {
                m_preview->setImage(result.cover.image, result.cover.sourcePath);
                if (result.cover.manualFallback) setFeedback(QStringLiteral("自定义封面不可用，当前预览为自动封面。可重新选择图片或恢复自动封面。"));
            } else { m_preview->clearImage(QStringLiteral("暂无封面")); }
            updateControls();
        }
        finishShutdownIfReady();
    });
    m_lifetime = promise->future();
    watcher->setFuture(m_lifetime);
    const auto cancelled = m_cancelled;
    QThreadPool::globalInstance()->start([promise, kind, path, category = m_category,
        settings = m_settings, members = m_members, cancelled] {
        promise->start();
        PreviewResult result;
        try {
            if (kind == PreviewKind::External) result.draft = ManagedCoverFile::readExternal(path, cancelled);
            else {
                if (kind == PreviewKind::Initial && !category.managedCoverFile.isEmpty()) {
                    const auto stored = ManagedCoverFile::categoryPath(settings, category.id, category.managedCoverFile);
                    if (!stored.isEmpty()) result.draft = ManagedCoverFile::readExternal(stored, cancelled);
                    if (result.draft.success) {
                        result.cover.status = CoverArtStatus::Ready;
                        result.cover.image = result.draft.image;
                        result.cover.sourcePath = stored;
                        result.cover.manual = true;
                    }
                }
                if (result.cover.status != CoverArtStatus::Ready) {
                    result.cover = automaticCover(settings, members, cancelled);
                    result.cover.manualFallback = kind == PreviewKind::Initial && !category.managedCoverFile.isEmpty();
                }
            }
        } catch (...) { result.draft.errorMessage = QStringLiteral("封面预览失败，请重新选择图片。"); }
        promise->addResult(std::move(result));
        promise->finish();
    });
}

void CategoryEditDialog::updateControls()
{
    const bool enabled = !m_shuttingDown && !m_saving;
    m_name->setEnabled(enabled);
    m_choose->setEnabled(enabled && !m_active && !m_fileDialog);
    m_automatic->setEnabled(enabled && !m_active && !m_fileDialog
        && (!m_category.managedCoverFile.isEmpty() || m_coverChange == CoverChange::Replace));
    const bool changed = m_category.id <= 0 || name() != m_category.name || m_coverChange != CoverChange::Keep;
    m_save->setEnabled(enabled && !m_active && !m_fileDialog && changed && !name().isEmpty()
        && (m_coverChange != CoverChange::Replace || m_draft.success));
    m_cancel->setEnabled(!m_shuttingDown);
    m_titleBar->setClosing(m_shuttingDown);
    m_titleBar->closeButton()->setEnabled(!m_shuttingDown);
}

bool CategoryEditDialog::nativeEvent(const QByteArray &type, void *message, qintptr *result)
{ return m_frame->handleNativeEvent(type, message, result) || QDialog::nativeEvent(type, message, result); }

void CategoryEditDialog::closeEvent(QCloseEvent *event)
{
    QDialog::closeEvent(event);
}

void CategoryEditDialog::done(int result)
{
    m_requestedResult = result;
    hide();
    shutdown();
}

void CategoryEditDialog::shutdown()
{
    if (!m_shuttingDown) {
        m_shuttingDown = true;
        if (m_cancelled) m_cancelled->store(true);
        if (m_fileDialog) m_fileDialog->reject();
        updateControls();
    }
    finishShutdownIfReady();
}

void CategoryEditDialog::finishShutdownIfReady()
{
    if (!m_shuttingDown || m_active || m_shutdownNotified) return;
    m_shutdownNotified = true;
    QPointer<CategoryEditDialog> guard(this);
    emit shutdownFinished();
    if (guard && m_requestedResult) QDialog::done(*m_requestedResult);
}
} // namespace asmr
