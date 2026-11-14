#include "ui/MainWindow.h"

#include "library/LibraryRepository.h"
#include "ui/LibraryPanel.h"
#include "ui/CategoryEditDialog.h"
#include "ui/PlayerMessageBox.h"
#include "ui/TitleBarWidget.h"
#include "ui/WindowFrameController.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPromise>
#include <QPushButton>
#include <QThreadPool>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>

namespace asmr {
namespace {
QString categoryError(const LibraryOperationResult &result)
{
    QStringList lines;
    for (const auto &diagnostic : result.diagnostics) lines.append(diagnostic.message);
    return lines.join(QLatin1Char('\n'));
}
}

bool MainWindow::canEditCategories() const
{
    return !m_closing && m_libraryDatabaseReady && !libraryScanBusy()
        && !m_libraryQueryInProgress && !m_libraryQueryPending && !m_categoryMutationInProgress;
}

void MainWindow::connectWorkCategories()
{
    connect(m_libraryPanel, &LibraryPanel::createCategoryRequested, this,
            [this](qint64 workId) { showCategoryNameEditor(0, workId); });
    connect(m_libraryPanel, &LibraryPanel::renameCategoryRequested, this,
            [this](qint64 categoryId) { showCategoryNameEditor(categoryId); });
    connect(m_libraryPanel, &LibraryPanel::removeCategoryRequested, this, &MainWindow::requestCategoryRemoval);
    connect(m_libraryPanel, &LibraryPanel::workCategoryRequested, this, &MainWindow::requestWorkCategory);
}

void MainWindow::showCategoryNameEditor(qint64 categoryId, qint64 addWorkId)
{
    if (!canEditCategories()) return;
    if (m_categoryNameDialog) { m_categoryNameDialog->raise(); m_categoryNameDialog->activateWindow(); return; }
    CategoryMutation request;
    if (categoryId) {
        const auto found = std::find_if(m_categoriesSnapshot.cbegin(), m_categoriesSnapshot.cend(),
            [categoryId](const auto &category) { return category.id == categoryId; });
        if (found == m_categoriesSnapshot.cend()) return;
        request.kind = CategoryMutationKind::Rename;
        request.category = *found;
    }
    if (addWorkId) {
        const auto found = std::find_if(m_libraryWorksSnapshot.cbegin(), m_libraryWorksSnapshot.cend(),
            [addWorkId](const auto &work) { return work.id == addWorkId; });
        if (found == m_libraryWorksSnapshot.cend() || m_removedLibraryWorkIds.contains(addWorkId)) return;
        request.work = *found;
    }
    QVector<WorkCategoryMembership> memberships;
    for (const auto &member : m_categoryMembershipsSnapshot)
        if (member.categoryId == categoryId) memberships.append(member);
    std::sort(memberships.begin(), memberships.end(), [](const auto &left, const auto &right) {
        return left.addedAtMs != right.addedAtMs ? left.addedAtMs > right.addedAtMs : left.workId > right.workId;
    });
    QVector<LibraryWork> members;
    for (const auto &membership : memberships) {
        const auto found = std::find_if(m_libraryWorksSnapshot.cbegin(), m_libraryWorksSnapshot.cend(),
            [&](const auto &work) { return work.id == membership.workId; });
        if (found != m_libraryWorksSnapshot.cend()) members.append(*found);
    }
    auto *dialog = new CategoryEditDialog(request.category, m_manualCoverSettings, members, this);
    m_categoryNameDialog = dialog;
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (m_categoryNameDialog == dialog) m_categoryNameDialog = nullptr;
    });
    connect(dialog, &CategoryEditDialog::shutdownFinished, this, &MainWindow::finishCloseIfReady);
    connect(dialog, &CategoryEditDialog::saveRequested, this, [this, dialog, request]() mutable {
        if (!canEditCategories()) return;
        request.name = dialog->name();
        if (request.name.isEmpty()) return;
        request.coverDraft.reset();
        request.automaticCover = dialog->coverChange() == CategoryEditDialog::CoverChange::Automatic;
        if (dialog->coverChange() == CategoryEditDialog::CoverChange::Replace) request.coverDraft = dialog->draft();
        dialog->setFeedback({});
        startCategoryMutation(request);
    });
    dialog->open();
}

void MainWindow::requestCategoryRemoval(qint64 categoryId)
{
    if (!canEditCategories() || m_categoryRemovalConfirmation) return;
    const auto found = std::find_if(m_categoriesSnapshot.cbegin(), m_categoriesSnapshot.cend(),
        [categoryId](const auto &category) { return category.id == categoryId; });
    if (found == m_categoriesSnapshot.cend()) return;
    CategoryMutation request;
    request.kind = CategoryMutationKind::Remove;
    request.category = *found;
    auto *dialog = new PlayerMessageBox(QMessageBox::Warning, QStringLiteral("删除分类"),
        QStringLiteral("删除分类“%1”及其中的分组关系？\n作品和本地文件将保留。")
            .arg(found->name), QMessageBox::NoButton, this);
    dialog->setObjectName(QStringLiteral("workCategoryRemovalConfirmation"));
    dialog->setTextFormat(Qt::PlainText);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto *remove = dialog->addButton(QStringLiteral("删除"), QMessageBox::DestructiveRole);
    remove->setObjectName(QStringLiteral("confirmWorkCategoryRemovalButton"));
    auto *cancel = dialog->addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    cancel->setObjectName(QStringLiteral("cancelWorkCategoryRemovalButton"));
    dialog->setDefaultButton(cancel);
    dialog->setEscapeButton(cancel);
    m_categoryRemovalConfirmation = dialog;
    connect(dialog, &QDialog::finished, this, [this, dialog, remove, request] {
        if (m_categoryRemovalConfirmation == dialog) m_categoryRemovalConfirmation = nullptr;
        if (dialog->clickedButton() == remove && canEditCategories()) startCategoryMutation(request);
    });
    dialog->open();
}

void MainWindow::requestWorkCategory(qint64 workId, qint64 categoryId, bool present)
{
    if (!canEditCategories() || m_removedLibraryWorkIds.contains(workId)) return;
    const auto work = std::find_if(m_libraryWorksSnapshot.cbegin(), m_libraryWorksSnapshot.cend(),
        [workId](const auto &entry) { return entry.id == workId; });
    const auto category = std::find_if(m_categoriesSnapshot.cbegin(), m_categoriesSnapshot.cend(),
        [categoryId](const auto &entry) { return entry.id == categoryId; });
    if (work == m_libraryWorksSnapshot.cend() || category == m_categoriesSnapshot.cend()) return;
    CategoryMutation request;
    request.kind = CategoryMutationKind::Membership;
    request.category = *category;
    request.work = *work;
    request.present = present;
    startCategoryMutation(request);
}

void MainWindow::startCategoryMutation(const CategoryMutation &request)
{
    if (!canEditCategories()) return;
    m_categoryMutationInProgress = true;
    m_categoryMutationCancelled = std::make_shared<std::atomic_bool>(false);
    m_libraryPanel->setCategoryMutationBusy(true);
    const QPointer<CategoryEditDialog> editor = m_categoryNameDialog;
    if (editor) editor->setSaving(true);
    auto promise = std::make_shared<QPromise<CategoryMutationResult>>();
    auto *watcher = new QFutureWatcher<CategoryMutationResult>(this);
    connect(watcher, &QFutureWatcher<CategoryMutationResult>::finished, this, [this, watcher, editor] {
        const auto result = watcher->result();
        watcher->deleteLater();
        m_categoryMutationInProgress = false;
        m_libraryPanel->setCategoryMutationBusy(false);
        const bool activeEditor = editor && editor == m_categoryNameDialog && editor->isVisible();
        if (!m_closing) {
            if (result.success) {
                if (activeEditor) { editor->setSaving(false); editor->accept(); }
                refreshLibrary();
            } else if (activeEditor) {
                editor->setSaving(false);
                editor->setFeedback(result.error);
            } else {
                m_libraryPanel->setImportError(result.error);
            }
        }
        finishCloseIfReady();
    });
    m_categoryMutationLifetime = promise->future();
    watcher->setFuture(m_categoryMutationLifetime);
    const auto cancelled = m_categoryMutationCancelled;
    const auto hook = m_beforeCategoryMutationTestHook;
    QThreadPool::globalInstance()->start([promise, request, path = m_libraryDatabasePath, settings = m_manualCoverSettings, cancelled, hook] {
        promise->start();
        CategoryMutationResult result;
        try {
            if (hook) (*hook)();
            if (cancelled->load()) {
                result.error = QStringLiteral("分类操作已取消。");
            } else {
                LibraryRepository repository(path);
                auto written = repository.open();
                if (written.success && !cancelled->load()) {
                    switch (request.kind) {
                    case CategoryMutationKind::Create: written = repository.createCategory(request.name, request.work); break;
                    case CategoryMutationKind::Rename: {
                        std::optional<QString> token;
                        QString staged;
                        if (request.coverDraft) {
                            const auto file = ManagedCoverFile::stageCategory(settings, request.category.id, *request.coverDraft);
                            if (!file.success) { written = {false, {{LibraryDiagnosticCode::InvalidCategory, {}, file.errorMessage}}}; break; }
                            staged = file.token;
                            token = staged;
                        } else if (request.automaticCover) token = QString();
                        if (cancelled->load()) written = {false, {{LibraryDiagnosticCode::StaleCategory, {}, QStringLiteral("分类操作已取消。")}}};
                        else written = repository.updateCategory(request.category, request.name, token);
                        if (!written.success && !staged.isEmpty())
                            static_cast<void>(ManagedCoverFile::discardCategory(settings, request.category.id, staged));
                        // Remove only this edit's previous copy; never sweep a
                        // different process's staged or newly committed image.
                        if (written.success && token && *token != request.category.managedCoverFile
                            && !request.category.managedCoverFile.isEmpty())
                            static_cast<void>(ManagedCoverFile::discardCategory(settings, request.category.id, request.category.managedCoverFile));
                        break;
                    }
                    case CategoryMutationKind::Remove:
                        written = repository.removeCategory(request.category);
                        if (written.success) static_cast<void>(ManagedCoverFile::cleanupCategory(settings, request.category.id));
                        break;
                    case CategoryMutationKind::Membership:
                        written = request.work ? repository.setWorkCategory(*request.work, request.category, request.present)
                            : LibraryOperationResult{false, {}};
                        break;
                    }
                    result.success = written.success;
                    result.error = categoryError(written);
                } else {
                    result.error = categoryError(written);
                }
                const auto closed = repository.close();
                if (!closed.success) { result.success = false; result.error = categoryError(closed); }
            }
        } catch (...) { result.error = QStringLiteral("分类操作失败，请重试。"); }
        if (!result.success && result.error.isEmpty()) result.error = QStringLiteral("分类操作未完成，请重试。");
        promise->addResult(std::move(result));
        promise->finish();
    });
}
} // namespace asmr
