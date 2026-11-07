#include "ui/MainWindow.h"

#include "artwork/ManualCoverStore.h"
#include "ui/LibraryPanel.h"
#include "ui/PlaybackControlsWidget.h"
#include "ui/WorkEditDialog.h"

#include <QScopeGuard>
#include <QStringList>

#include <algorithm>

namespace asmr {
namespace {
QString diagnosticText(const LibraryOperationResult &result)
{
    QStringList messages;
    for (const auto &diagnostic : result.diagnostics) messages.append(diagnostic.message);
    return messages.join(QLatin1Char('\n'));
}
}

void MainWindow::showWorkEditor(qint64 workId)
{
    const auto identity = coverIdentity(workId);
    if (m_closing || !m_libraryDatabaseReady || identity.workId <= 0
        || workId == m_libraryManualRemovalWorkId) return;
    if (m_workEditDialog) {
        m_workEditDialog->raise();
        m_workEditDialog->activateWindow();
        return;
    }
    const auto found = std::find_if(m_libraryWorksSnapshot.cbegin(), m_libraryWorksSnapshot.cend(),
        [workId](const auto &work) { return work.id == workId; });
    if (found == m_libraryWorksSnapshot.cend()) return;
    m_workEditExpected = *found;
    ++m_workEditSession;
    auto *dialog = new WorkEditDialog(found->displayName, found->rootPath, this);
    m_workEditDialog = dialog;
    connect(dialog, &WorkEditDialog::saveRequested, this, &MainWindow::saveWorkEditor);
    connect(dialog, &WorkEditDialog::shutdownFinished, this, [this] { finishCloseIfReady(); });
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (m_workEditDialog == dialog) {
            m_workEditDialog = nullptr;
            m_workEditExpected = {};
        }
        dialog->deleteLater();
        finishCloseIfReady();
    });
    CoverMutationRequest request;
    request.kind = CoverMutationKind::EditorInspect;
    request.identity = identity;
    request.revision = m_coverMutationRevisions.value(workId);
    request.editorSession = m_workEditSession;
    m_coverMutations.enqueue(std::move(request));
    dialog->open();
    startNextCoverMutation();
}

void MainWindow::saveWorkEditor()
{
    if (m_closing || !m_workEditDialog) return;
    const auto identity = coverIdentity(m_workEditExpected.id);
    if (identity.workId <= 0 || identity.rootPath != m_workEditExpected.rootPath
        || identity.importedAtMs != m_workEditExpected.importedAtMs) {
        m_workEditDialog->setFeedback(QStringLiteral("作品已删除或身份已变化，请关闭后重新打开编辑。"));
        return;
    }
    CoverMutationRequest request;
    request.kind = CoverMutationKind::Edit;
    request.identity = identity;
    request.expectedWork = m_workEditExpected;
    request.displayName = m_workEditDialog->title();
    switch (m_workEditDialog->coverChange()) {
    case WorkEditDialog::CoverChange::Keep: request.coverChange = WorkCoverChange::Keep; break;
    case WorkEditDialog::CoverChange::Replace: request.coverChange = WorkCoverChange::Replace; break;
    case WorkEditDialog::CoverChange::Automatic: request.coverChange = WorkCoverChange::Automatic; break;
    }
    request.draft = m_workEditDialog->draft();
    request.editorSession = m_workEditSession;
    request.revision = ++m_coverMutationRevisions[identity.workId];
    request.cancelled = std::make_shared<std::atomic_bool>(false);
    m_coverMutations.enqueue(std::move(request));
    m_workEditDialog->setSaving(true);
    startNextCoverMutation();
}

MainWindow::CoverMutationResult MainWindow::executeWorkEdit(
    const CoverMutationRequest &request, const ManualCoverSettings &settings)
{
    const auto cancelled = [&] { return request.cancelled && request.cancelled->load(); };
    if (cancelled()) return {false, {}, QStringLiteral("作品编辑已取消。")};
    LibraryRepository repository(settings.databasePath);
    const auto opened = repository.open();
    if (!opened.success) return {false, {}, diagnosticText(opened)};
    const auto snapshot = repository.workSnapshot(request.identity.workId);
    if (!snapshot.success || snapshot.work.rootPath != request.identity.rootPath
        || snapshot.work.importedAtMs != request.identity.importedAtMs)
        return {false, {}, QStringLiteral("作品已删除或身份已变化，编辑未保存。")};
    if (request.kind == CoverMutationKind::Edit && snapshot.work.displayName != request.expectedWork.displayName)
        return {false, {}, QStringLiteral("作品名称已变化，编辑未保存。")};
    const auto previous = ManualCoverStore::read(settings, request.identity);
    if (!previous.success) return {false, {}, previous.errorMessage};
    if (request.kind == CoverMutationKind::EditorInspect) {
        CoverMutationResult result;
        result.success = true;
        result.relativePath = previous.relativePath;
        result.managedFile = previous.managedFile;
        CoverArtRequest coverRequest{request.identity.rootPath, {}, true};
        coverRequest.manualLookup = ManualCoverLookup{settings, request.identity};
        result.cover = CoverArtLoader::load(coverRequest);
        return result;
    }
    QString staged;
    if (request.coverChange == WorkCoverChange::Replace) {
        const auto file = ManagedCoverFile::stage(settings, request.identity, request.draft);
        if (!file.success) return {false, {}, file.errorMessage};
        staged = file.token;
    }
    auto selected = previous;
    if (request.coverChange == WorkCoverChange::Replace) {
        selected.relativePath.clear(); selected.managedFile = staged;
    } else if (request.coverChange == WorkCoverChange::Automatic) {
        selected.relativePath.clear(); selected.managedFile.clear();
    }
    const auto checkCancellation = [&]() -> LibraryOperationResult {
        return cancelled() ? LibraryOperationResult{false, {{LibraryDiagnosticCode::StaleWorkSnapshot, {},
            QStringLiteral("作品编辑已失效，未保存。")}}} : LibraryOperationResult{true, {}};
    };
    const auto updated = request.coverChange == WorkCoverChange::Keep
        ? repository.updateWorkTitle(request.expectedWork, request.displayName, checkCancellation,
            [] { return LibraryOperationResult{true, {}}; })
        : repository.updateWorkTitleAndArtwork(request.expectedWork, request.displayName,
            selected.relativePath, selected.managedFile, checkCancellation);
    if (!updated.success) return {false, {}, diagnosticText(updated)};
    const bool coverChanged = request.coverChange != WorkCoverChange::Keep;
    CoverMutationResult result;
    result.success = true;
    result.relativePath = selected.relativePath;
    result.managedFile = selected.managedFile;
    result.displayName = request.displayName.trimmed();
    result.coverChanged = coverChanged;
    result.titleChanged = result.displayName != request.expectedWork.displayName;
    return result;
}

QString MainWindow::latestWorkTitle(qint64 workId, const QString &fallback) const
{
    const auto found = std::find_if(m_libraryWorksSnapshot.cbegin(), m_libraryWorksSnapshot.cend(),
        [workId](const auto &work) { return work.id == workId; });
    return found == m_libraryWorksSnapshot.cend() ? fallback : found->displayName;
}

void MainWindow::syncQueueWorkNames(PlaybackQueue &queue) const
{
    for (const auto &work : m_libraryWorksSnapshot) queue.updateWorkName(work.id, work.displayName);
}

void MainWindow::applyWorkTitle(qint64 workId, const QString &title)
{
    for (auto &work : m_libraryWorksSnapshot)
        if (work.id == workId) work.displayName = title;
    m_playbackQueue.updateWorkName(workId, title);
    if (m_audioRequestedWorkId == workId) m_audioRequestedWorkName = title;
    if (m_audioRequestedQueueReplacement) m_audioRequestedQueueReplacement->updateWorkName(workId, title);
    if (m_queuePreparationRequest.entry.workId == workId)
        m_queuePreparationRequest.entry.workName = title;
    if (m_queuePreparationRequest.replacement)
        m_queuePreparationRequest.replacement->updateWorkName(workId, title);
    for (auto &entry : m_queueAppendDuringReplacement)
        if (entry.workId == workId) entry.workName = title;
    if (m_playingLibraryWorkId == workId) updatePlaybackIdentity(m_playingAudioName, title);
    updatePlaybackQueue();
    // Retire any library read that started before the commit. The new query
    // preserves the current browser page and accepts the persisted metadata.
    refreshLibrary(m_libraryPanel->selectedWorkId());
}
} // namespace asmr
