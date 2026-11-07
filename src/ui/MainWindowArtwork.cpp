#include "ui/MainWindow.h"
#include "ui/PlayerMessageBox.h"

#include "artwork/CoverArtService.h"
#include "artwork/WorkCoverService.h"
#include "playback/AudioPlayer.h"
#include "ui/PlaybackControlsWidget.h"
#include "ui/SubtitlePanel.h"
#include "ui/PlaybackDetailWidget.h"
#include "ui/CoverSelectionDialog.h"
#include "ui/WorkEditDialog.h"
#include "ui/LibraryPanel.h"
#include "library/LibraryRepository.h"

#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPromise>
#include <QThreadPool>

#include <algorithm>
#include <exception>

namespace asmr {

void MainWindow::connectCoverArt()
{
    m_manualCoverSettings = {m_libraryDatabasePath, m_settings.fileName(), m_settings.format()};
    m_coverArtService = new CoverArtService(this);
    connect(m_coverArtService, &CoverArtService::resultReady,
            this, &MainWindow::applyCoverArtResult);
    connect(m_coverArtService, &CoverArtService::shutdownFinished, this, [this] {
        m_coverShutdownFinished = true;
        finishCloseIfReady();
    });
    m_workCoverService = new WorkCoverService(m_manualCoverSettings, this);
    connect(m_libraryPanel, &LibraryPanel::workCoverDemandChanged, this,
        [this](qint64 selectedWorkId, const QVector<qint64> &visibleWorkIds) {
            if (!m_closing)
                m_workCoverService->setDemand(selectedWorkId, visibleWorkIds);
        });
    connect(m_workCoverService, &WorkCoverService::resultReady, this,
        [this](qint64 workId, const CoverArtResult &result) {
            if (!m_closing && coverIdentity(workId).workId > 0)
                m_libraryPanel->setWorkCover(workId, result);
        });
    connect(m_workCoverService, &WorkCoverService::shutdownFinished, this, [this] {
        m_workCoverShutdownFinished = true;
        finishCloseIfReady();
    });
    connect(m_libraryPanel, &LibraryPanel::chooseCoverRequested, this, &MainWindow::chooseWorkCover);
    connect(m_libraryPanel, &LibraryPanel::restoreAutoCoverRequested, this, &MainWindow::restoreWorkAutoCover);
    connect(m_libraryPanel, &LibraryPanel::editWorkRequested, this, &MainWindow::showWorkEditor);
    m_libraryPanel->requestWorkCoverDemand();
}

void MainWindow::syncWorkCoverCatalog()
{
    QVector<LibraryWork> available;
    available.reserve(m_libraryWorksSnapshot.size());
    for (const auto &work : m_libraryWorksSnapshot)
        if (!m_removedLibraryWorkIds.contains(work.id))
            available.append(work);
    m_workCoverService->setWorks(available);
}

void MainWindow::adoptCoverArt(const QString &rootPath, const QString &audioPath, qint64 workId)
{
    if (m_closing) return;
    const bool libraryWork = workId > 0 && !rootPath.isEmpty();
    const QString root = libraryWork ? rootPath : QFileInfo(audioPath).absolutePath();
    const bool sameOwner = libraryWork
        ? m_coverContext.libraryWork && m_coverWorkId == workId && m_coverContext.rootPath == root
        : !m_coverContext.libraryWork && m_coverContext.audioPath == audioPath;
    // Ownership comes from the adopted source, never the current browser or a
    // newer open intention that can still fail its filesystem preflight.
    m_coverContext.rootPath = root;
    m_coverContext.audioPath = audioPath;
    m_coverContext.libraryWork = libraryWork;
    m_coverContext.forceReload = false;
    m_coverContext.manualLookup.reset();
    const auto identity = coverIdentity(workId);
    if (libraryWork && identity.workId > 0)
        m_coverContext.manualLookup = ManualCoverLookup{m_manualCoverSettings, identity};
    m_coverWorkId = workId;
    m_coverAudioGeneration = m_audioGeneration;
    m_coverSource = QUrl::fromLocalFile(audioPath);
    if (!sameOwner || !m_coverHasImage) {
        m_coverHasImage = false;
        m_playbackControls->setCoverPlaceholder(QStringLiteral("正在读取封面"));
        m_subtitlePanel->setCoverPlaceholder(QStringLiteral("正在读取封面"));
        m_playbackDetailPage->clearCoverImage();
    }
    m_coverRequestId = m_coverArtService->request(m_coverContext);
}

void MainWindow::refreshCoverArtForWork(qint64 workId)
{
    if (!m_closing && workId > 0 && !m_removedLibraryWorkIds.contains(workId))
        m_workCoverService->invalidate(workId);
    if (m_closing || workId <= 0 || workId != m_coverWorkId
        || workId != m_playingLibraryWorkId || !m_coverContext.libraryWork
        || m_coverAudioGeneration != m_audioGeneration || m_coverSource != m_audioPlayer->source())
        return;
    auto request = m_coverContext;
    const auto identity = coverIdentity(workId);
    if (identity.workId > 0)
        request.manualLookup = ManualCoverLookup{m_manualCoverSettings, identity};
    request.forceReload = true;
    if (!m_coverHasImage) {
        m_playbackControls->setCoverPlaceholder(QStringLiteral("正在读取封面"));
        m_subtitlePanel->setCoverPlaceholder(QStringLiteral("正在读取封面"));
        m_playbackDetailPage->clearCoverImage();
    }
    m_coverRequestId = m_coverArtService->request(request);
}

void MainWindow::applyCoverArtResult(quint64 requestId, const CoverArtResult &result)
{
    if (m_closing || !requestId || requestId != m_coverRequestId
        || m_coverAudioGeneration != m_audioGeneration || m_coverSource != m_audioPlayer->source())
        return;
    if (result.status == CoverArtStatus::Ready && !result.image.isNull()) {
        const QString detail = result.manualFallback
            ? result.manualRelativePath.isEmpty()
                ? QStringLiteral("手动封面设置暂不可用，当前使用自动封面。")
                : QStringLiteral("手动封面不可用，当前使用自动封面；已保留手动选择。")
            : result.manual ? QStringLiteral("手动指定封面") : QStringLiteral("自动查找封面");
        m_playbackControls->setCoverImage(result.image, result.sourcePath, detail);
        m_subtitlePanel->setCoverImage(result.image, result.sourcePath, detail);
        m_playbackDetailPage->setCoverImage(result.image);
        m_coverHasImage = true;
        return;
    }
    // A removed work loses its database identity while its loaded media stays.
    // Keep a previously decoded image even if a pending recheck now finds no file.
    if (m_coverHasImage && m_coverWorkId > 0 && m_removedLibraryWorkIds.contains(m_coverWorkId))
        return;
    if (result.status == CoverArtStatus::Cancelled)
        return;
    m_coverHasImage = false;
    const QString placeholder = result.manualFallback
        ? result.manualRelativePath.isEmpty()
            ? QStringLiteral("手动封面设置暂不可用，未找到可用自动封面。")
            : QStringLiteral("手动封面不可用，未找到可用自动封面；已保留手动选择。")
        : result.status == CoverArtStatus::NotFound
        ? QStringLiteral("暂无封面") : QStringLiteral("封面不可用");
    m_playbackControls->setCoverPlaceholder(placeholder);
    m_subtitlePanel->setCoverPlaceholder(placeholder);
    m_playbackDetailPage->clearCoverImage();
}

ManualCoverIdentity MainWindow::coverIdentity(qint64 workId) const
{
    if (workId <= 0 || m_removedLibraryWorkIds.contains(workId)) return {};
    const auto work = std::find_if(m_libraryWorksSnapshot.cbegin(), m_libraryWorksSnapshot.cend(),
        [workId](const auto &entry) { return entry.id == workId; });
    return work == m_libraryWorksSnapshot.cend() ? ManualCoverIdentity{}
        : ManualCoverIdentity{work->id, work->rootPath, work->importedAtMs};
}

void MainWindow::chooseWorkCover(qint64 workId)
{
    const auto identity = coverIdentity(workId);
    if (m_closing || !m_libraryDatabaseReady || identity.workId <= 0) return;
    if (m_coverSelectionDialog) {
        if (m_coverSelectionWorkId == workId && !m_coverSelectionDialog->isHidden()) {
            m_coverSelectionDialog->raise();
            m_coverSelectionDialog->activateWindow();
        }
        return;
    }
    const auto work = std::find_if(m_libraryWorksSnapshot.cbegin(), m_libraryWorksSnapshot.cend(),
        [workId](const auto &entry) { return entry.id == workId; });
    auto *dialog = new CoverSelectionDialog(work->displayName, identity.rootPath, this);
    m_coverSelectionDialog = dialog;
    m_coverSelectionWorkId = workId;
    dialog->setManualSelection(m_manualCoverPaths.value(workId));
    connect(dialog, &CoverSelectionDialog::coverChosen, this,
        [this, workId, identity](const QString &relativePath) {
            const auto current = coverIdentity(workId);
            if (current.workId != identity.workId || current.rootPath != identity.rootPath
                || current.importedAtMs != identity.importedAtMs) {
                if (m_coverSelectionDialog)
                    m_coverSelectionDialog->setFeedback(QStringLiteral("作品身份已变化，请关闭后重新选择作品。"));
                return;
            }
            setWorkManualCover(workId, relativePath);
        });
    connect(dialog, &CoverSelectionDialog::shutdownFinished, this, [this] { finishCloseIfReady(); });
    connect(dialog, &QDialog::finished, this, [this, dialog] {
        if (m_coverSelectionDialog == dialog) {
            m_coverSelectionDialog = nullptr;
            m_coverSelectionWorkId = 0;
        }
        dialog->deleteLater();
        finishCloseIfReady();
    });
    dialog->show();
    m_coverMutations.enqueue({CoverMutationKind::Inspect, identity, {},
                              m_coverMutationRevisions.value(workId), {}});
    startNextCoverMutation();
}

void MainWindow::setWorkManualCover(qint64 workId, const QString &relativePath)
{
    const auto identity = coverIdentity(workId);
    if (m_closing || identity.workId <= 0 || relativePath.isEmpty()) return;
    const auto revision = ++m_coverMutationRevisions[workId];
    if (m_coverMutationInProgress && m_activeCoverMutation.identity.workId == workId
        && m_activeCoverMutation.cancelled) m_activeCoverMutation.cancelled->store(true);
    for (auto &pending : m_coverMutations)
        if (pending.identity.workId == workId && pending.cancelled) pending.cancelled->store(true);
    m_coverMutations.enqueue({CoverMutationKind::Set, identity, relativePath, revision,
                              std::make_shared<std::atomic_bool>(false)});
    if (m_coverSelectionDialog && m_coverSelectionWorkId == workId)
        m_coverSelectionDialog->setSaving(true);
    startNextCoverMutation();
}

void MainWindow::restoreWorkAutoCover(qint64 workId)
{
    const auto identity = coverIdentity(workId);
    if (m_closing || identity.workId <= 0) return;
    const auto revision = ++m_coverMutationRevisions[workId];
    if (m_coverMutationInProgress && m_activeCoverMutation.identity.workId == workId
        && m_activeCoverMutation.cancelled) m_activeCoverMutation.cancelled->store(true);
    for (auto &pending : m_coverMutations)
        if (pending.identity.workId == workId && pending.cancelled) pending.cancelled->store(true);
    m_coverMutations.enqueue({CoverMutationKind::Restore, identity, {}, revision,
                              std::make_shared<std::atomic_bool>(false)});
    startNextCoverMutation();
}

void MainWindow::forgetCoverSelection(qint64 workId)
{
    // Retire the library artwork immediately, before the next SQL snapshot.
    // The separately adopted playback image keeps its existing lifetime.
    syncWorkCoverCatalog();
    m_libraryPanel->clearWorkCover(workId);
    ++m_coverMutationRevisions[workId];
    m_manualCoverPaths.remove(workId);
    if (m_coverMutationInProgress && m_activeCoverMutation.identity.workId == workId
        && m_activeCoverMutation.cancelled) m_activeCoverMutation.cancelled->store(true);
    for (auto &pending : m_coverMutations)
        if (pending.identity.workId == workId && pending.cancelled) pending.cancelled->store(true);
    // Artwork metadata was removed by the work deletion transaction; all images remain.
    if (m_coverSelectionDialog && m_coverSelectionWorkId == workId) {
        m_coverSelectionDialog->setSaving(false);
        m_coverSelectionDialog->reject();
    }
    startNextCoverMutation();
}

void MainWindow::updateCoverActions()
{
    const auto workId = m_libraryPanel->selectedWorkId();
    m_libraryPanel->setManualCoverAvailable(workId, !m_manualCoverPaths.value(workId).isEmpty());
    if (m_closing || !m_libraryDatabaseReady) return;
    // Inspect and orphan cleanup share the mutation FIFO, so an older snapshot
    // cannot undo a newer accepted selection. The worker queries live DB IDs.
    m_coverMutations.enqueue({CoverMutationKind::Inspect, coverIdentity(workId), {},
                              m_coverMutationRevisions.value(workId), {}});
    startNextCoverMutation();
}

MainWindow::CoverMutationResult MainWindow::executeCoverMutation(
    const CoverMutationRequest &request, const ManualCoverSettings &settings)
{
    if (request.kind == CoverMutationKind::Edit || request.kind == CoverMutationKind::EditorInspect)
        return executeWorkEdit(request, settings);
    if (request.cancelled && request.cancelled->load()) return {};
    LibraryRepository repository(settings.databasePath);
    if (!repository.open().success)
        return {false, {}, QStringLiteral("无法核对作品库，封面设置未修改。")};
    const auto works = repository.works();
    if (!works.success)
        return {false, {}, QStringLiteral("无法读取完整作品列表，封面设置未修改。")};
    if (request.kind == CoverMutationKind::Inspect) {
        QVector<ManualCoverIdentity> identities;
        for (const auto &work : works.works)
            identities.append({work.id, work.rootPath, work.importedAtMs});
        const auto pruned = ManualCoverStore::prune(settings, identities);
        if (!pruned.success) return {false, {}, pruned.errorMessage};
    }
    const auto work = std::find_if(works.works.cbegin(), works.works.cend(), [&](const auto &entry) {
        return entry.id == request.identity.workId && entry.rootPath == request.identity.rootPath
            && entry.importedAtMs == request.identity.importedAtMs;
    });
    if (work == works.works.cend())
        return request.kind == CoverMutationKind::Inspect ? CoverMutationResult{true, {}, {}}
            : CoverMutationResult{false, {}, QStringLiteral("作品已删除或身份已变化，封面设置未修改。")};
    if (request.kind == CoverMutationKind::Inspect) {
        const auto saved = ManualCoverStore::read(settings, request.identity);
        const bool selected = !saved.relativePath.isEmpty() || !saved.managedFile.isEmpty();
        CoverArtRequest coverRequest{request.identity.rootPath, {}, true};
        coverRequest.manualLookup = ManualCoverLookup{settings, request.identity};
        const bool unavailable = saved.success && selected && !CoverArtLoader::load(coverRequest).manual;
        return {saved.success, saved.relativePath, saved.errorMessage, unavailable, saved.managedFile};
    }
    if (request.kind == CoverMutationKind::Set) {
        const auto loaded = CoverArtLoader::loadSelected(request.identity.rootPath,
            request.relativePath, {}, request.cancelled);
        if (loaded.status != CoverArtStatus::Ready || loaded.image.isNull())
            return {false, {}, loaded.message.isEmpty() ? QStringLiteral("所选图片不可用，原封面设置保持。") : loaded.message};
    }
    if (request.cancelled && request.cancelled->load()) return {};
    // Decode can take time; do not commit a selection for a deleted/replaced ID.
    const auto current = repository.works();
    if (!current.success || std::none_of(current.works.cbegin(), current.works.cend(), [&](const auto &entry) {
        return entry.id == request.identity.workId && entry.rootPath == request.identity.rootPath
            && entry.importedAtMs == request.identity.importedAtMs;
    })) return {false, {}, QStringLiteral("作品已删除或身份已变化，封面设置未修改。")};
    const auto saved = ManualCoverStore::write(settings, request.identity,
        request.kind == CoverMutationKind::Restore ? QString{} : request.relativePath);
    return {saved.success, saved.relativePath, saved.errorMessage};
}

void MainWindow::startNextCoverMutation()
{
    if (m_coverMutationInProgress || m_coverMutations.isEmpty()) return;
    m_activeCoverMutation = m_coverMutations.dequeue();
    m_coverMutationInProgress = true;
    const auto request = m_activeCoverMutation;
    const auto settings = m_manualCoverSettings;
    const auto hook = request.kind == CoverMutationKind::Inspect || request.kind == CoverMutationKind::EditorInspect
        ? nullptr : m_beforeCoverMutationTestHook;
    auto promise = std::make_shared<QPromise<CoverMutationResult>>();
    auto *watcher = new QFutureWatcher<CoverMutationResult>(this);
    connect(watcher, &QFutureWatcher<CoverMutationResult>::finished, this, [this, watcher, request] {
        const auto result = watcher->result();
        watcher->deleteLater();
        m_coverMutationInProgress = false;
        m_activeCoverMutation = {};
        const auto workId = request.identity.workId;
        const bool current = request.revision == m_coverMutationRevisions.value(workId)
            && (!request.cancelled || !request.cancelled->load());
        if (request.kind == CoverMutationKind::Edit || request.kind == CoverMutationKind::EditorInspect) {
            const bool sameEditor = m_workEditDialog && m_workEditExpected.id == workId
                && request.editorSession == m_workEditSession;
            const auto identity = coverIdentity(workId);
            const bool sameWork = identity.workId > 0 && identity.rootPath == request.identity.rootPath
                && identity.importedAtMs == request.identity.importedAtMs;
            if (current && result.success && sameWork) {
                const auto selection = result.managedFile.isEmpty() ? result.relativePath : result.managedFile;
                if (selection.isEmpty()) m_manualCoverPaths.remove(workId);
                else m_manualCoverPaths.insert(workId, selection);
                if (!m_closing) {
                    if (request.kind == CoverMutationKind::Edit) {
                        if (result.titleChanged) applyWorkTitle(workId, result.displayName);
                        if (result.coverChanged) refreshCoverArtForWork(workId);
                        if (sameEditor) {
                            m_workEditDialog->setSaving(false);
                            m_workEditDialog->accept();
                        }
                    } else if (sameEditor) {
                        m_workEditDialog->setCoverState(result.cover, !selection.isEmpty());
                    }
                }
            } else if (!m_closing && sameEditor) {
                m_workEditDialog->setSaving(false);
                m_workEditDialog->setFeedback(result.errorMessage.isEmpty()
                    ? QStringLiteral("作品已变化，编辑未保存，请关闭后重新打开。") : result.errorMessage);
            } else if (current && !result.success && request.kind == CoverMutationKind::Edit) {
                showCoverError(result.errorMessage.isEmpty() ? QStringLiteral("保存作品编辑失败。") : result.errorMessage);
            }
            startNextCoverMutation();
            finishCloseIfReady();
            return;
        }
        if (current && result.success && !m_removedLibraryWorkIds.contains(workId)) {
            const auto selection = result.managedFile.isEmpty() ? result.relativePath : result.managedFile;
            if (selection.isEmpty()) m_manualCoverPaths.remove(workId);
            else m_manualCoverPaths.insert(workId, selection);
            if (!m_closing) {
                m_libraryPanel->setManualCoverAvailable(m_libraryPanel->selectedWorkId(),
                    !m_manualCoverPaths.value(m_libraryPanel->selectedWorkId()).isEmpty());
                if (m_coverSelectionDialog && m_coverSelectionWorkId == workId) {
                    m_coverSelectionDialog->setManualSelection(result.relativePath, result.manualUnavailable);
                    if (request.kind != CoverMutationKind::Inspect) {
                        m_coverSelectionDialog->setSaving(false);
                        m_coverSelectionDialog->accept();
                    }
                }
                if (request.kind == CoverMutationKind::Set || request.kind == CoverMutationKind::Restore)
                    refreshCoverArtForWork(workId);
            }
        } else if (current && !result.success && request.kind != CoverMutationKind::Inspect) {
            const auto message = result.errorMessage.isEmpty() ? QStringLiteral("保存封面设置失败，原设置保持。") : result.errorMessage;
            if (!m_closing && m_coverSelectionDialog && m_coverSelectionWorkId == workId) {
                m_coverSelectionDialog->setSaving(false);
                m_coverSelectionDialog->setFeedback(message);
            } else showCoverError(message);
        }
        startNextCoverMutation();
        finishCloseIfReady();
    });
    m_coverMutationLifetime = promise->future();
    watcher->setFuture(m_coverMutationLifetime);
    QThreadPool::globalInstance()->start([promise, request, settings, hook] {
        promise->start();
        CoverMutationResult result;
        try {
            if (hook) (*hook)();
            result = executeCoverMutation(request, settings);
        } catch (const std::exception &) {
            result.errorMessage = QStringLiteral("封面设置处理失败，原设置保持。");
        } catch (...) {
            result.errorMessage = QStringLiteral("封面设置处理失败，原设置保持。");
        }
        promise->addResult(std::move(result));
        promise->finish();
    });
}

void MainWindow::finishCoverMutations()
{
    // Normal close drains asynchronously. Direct destruction still completes
    // accepted value-only writes without callbacks into the dying widget.
    if (m_coverMutationInProgress) {
        m_coverMutationLifetime.waitForFinished();
        const auto result = m_coverMutationLifetime.result();
        if (!result.success && !result.errorMessage.isEmpty())
            qWarning().noquote() << result.errorMessage;
    }
    if (m_coverMutations.isEmpty()) return;
    auto pending = std::move(m_coverMutations);
    const auto settings = m_manualCoverSettings;
    auto promise = std::make_shared<QPromise<void>>();
    auto future = promise->future();
    QThreadPool::globalInstance()->start([promise, settings, pending = std::move(pending)] {
        promise->start();
        for (const auto &request : pending) {
            try {
                const auto result = executeCoverMutation(request, settings);
                if (!result.success && !result.errorMessage.isEmpty())
                    qWarning().noquote() << result.errorMessage;
            }
            catch (...) { qWarning("Failed to finish an accepted cover settings change."); }
        }
        promise->finish();
    });
    future.waitForFinished();
}

void MainWindow::showCoverError(const QString &message)
{
    if (m_coverErrorDialog) {
        m_coverErrorDialog->setText(m_coverErrorDialog->text() + u'\n' + message);
        return;
    }
    auto *dialog = new PlayerMessageBox(QMessageBox::Warning, QStringLiteral("封面设置失败"),
                                   message, QMessageBox::Ok, this);
    m_coverErrorDialog = dialog;
    dialog->setObjectName(QStringLiteral("coverSettingsErrorDialog"));
    dialog->setTextFormat(Qt::PlainText);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QDialog::finished, this, [this] {
        m_coverErrorDialog = nullptr;
        finishCloseIfReady();
    });
    dialog->open();
}

} // namespace asmr
