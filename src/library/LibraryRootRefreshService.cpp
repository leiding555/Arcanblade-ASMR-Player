#include "library/LibraryRootRefreshService.h"

#include "library/ScanRootStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMetaObject>
#include <QPromise>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <QUuid>

#include <exception>
#include <filesystem>
#include <utility>

namespace asmr {
namespace {
void diagnose(QStringList &messages, qsizetype &suppressed, qsizetype limit, const QString &message)
{
    if (messages.size() < qMax(qsizetype(0), limit)) messages.append(message);
    else ++suppressed;
}
std::filesystem::path nativePath(const QString &path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(path).constData());
#endif
}
void summarize(LibraryRootRefreshItemResult &item, const LibraryImportTaskResult &child, qsizetype limit)
{
    item.errorMessage = child.errorMessage;
    if (child.databaseCommitted) {
        item.workId = child.repositoryResult.workId;
        item.insertedFiles = child.repositoryResult.insertedFiles;
        item.updatedFiles = child.repositoryResult.updatedFiles;
        item.missingFiles = child.repositoryResult.missingFiles;
        item.associationStatus = child.autoAssociation.status;
        item.addedBindings = child.autoAssociation.addedBindings;
        item.existingBindings = child.autoAssociation.existingBindings;
        item.manualBindingsPreserved = child.autoAssociation.manualBindingsPreserved;
        item.outcome = child.repositoryResult.removed ? LibraryRootRefreshItemOutcome::Removed
            : child.repositoryResult.created ? LibraryRootRefreshItemOutcome::Added : LibraryRootRefreshItemOutcome::Updated;
    } else if (child.state == LibraryImportState::Cancelled)
        item.outcome = LibraryRootRefreshItemOutcome::Cancelled;
    else if (!item.workId && child.scan.isComplete() && child.scan.audioFiles.isEmpty()) {
        item.outcome = LibraryRootRefreshItemOutcome::Skipped;
        item.errorMessage = QStringLiteral("无受支持音频，未创建作品。");
    } else item.outcome = LibraryRootRefreshItemOutcome::Failed;
    for (const auto &diagnostic : child.scan.diagnostics)
        diagnose(item.diagnostics, item.suppressedDiagnostics, limit, diagnostic.message);
    item.suppressedDiagnostics += child.scan.suppressedDiagnostics;
    for (const auto &diagnostic : child.repositoryResult.diagnostics)
        diagnose(item.diagnostics, item.suppressedDiagnostics, limit, diagnostic.message);
    for (const auto &diagnostic : child.autoAssociation.diagnostics)
        diagnose(item.diagnostics, item.suppressedDiagnostics, limit, diagnostic.message);
    if (!child.references.error.isEmpty())
        diagnose(item.diagnostics, item.suppressedDiagnostics, limit, child.references.error);
    for (const auto &warning : child.references.warnings)
        diagnose(item.diagnostics, item.suppressedDiagnostics, limit, warning);
    if (child.references.failedCount > 0)
        diagnose(item.diagnostics, item.suppressedDiagnostics, limit,
            QStringLiteral("参考资料中有 %1 份无法提取，请在参考资料窗口查看。").arg(child.references.failedCount));
}
}

LibraryRootRefreshService::Preparation LibraryRootRefreshService::prepare(
    const QString &databasePath, const QString &root, const LibraryRootRefreshOptions &options,
    const std::shared_ptr<std::atomic<bool>> &cancelled,
    const std::shared_ptr<const std::function<void()>> &beforeDiscovery)
{
    Preparation prepared;
    auto &result = prepared.discovery;
    result.rootDirectory = root;
    try {
        if (beforeDiscovery) (*beforeDiscovery)();
        if (cancelled->load()) { result.cancelled = true; return prepared; }
        if (options.maximumItemDiagnostics < 0) {
            result.errorMessage = QStringLiteral("每项诊断限额不能为负数。");
            return prepared;
        }
        LibraryRepository repository(databasePath);
        const auto opened = repository.open();
        LibraryWorksResult known;
        if (opened.success) known = repository.works();
        const auto closed = repository.close();
        if (!opened.success || !known.success || !closed.success) {
            result.errorMessage = QStringLiteral("读取已有作品失败，本根未刷新。");
            const auto copy = [&](const auto &operation) {
                for (const auto &diagnostic : operation.diagnostics)
                    diagnose(result.diagnostics, result.suppressedDiagnostics, options.discovery.maximumDiagnostics, diagnostic.message);
            };
            copy(opened); copy(known); copy(closed);
            return prepared;
        }
        result = WorkDirectoryDiscovery::discover(root, known.works, options.discovery,
            [cancelled] { return cancelled->load(); });
        if (!result.success || result.cancelled) return prepared;
        QSet<QString> found;
        for (const auto &candidate : result.candidates) found.insert(ScanRootStore::pathKey(candidate.directory));
        const QString rootKey = ScanRootStore::pathKey(result.rootDirectory);
        for (const auto &work : known.works) {
            if (cancelled->load()) {
                result.cancelled = true; result.success = false;
                prepared.missingWorks.clear(); result.candidates.clear(); return prepared;
            }
            const QString path = ScanRootStore::normalizePath(work.rootPath);
            if (work.id <= 0 || path.isEmpty() || found.contains(path.toCaseFolded())
                || ScanRootStore::pathKey(QFileInfo(path).absolutePath()) != rootKey)
                continue;
            // A link or unreadable path is never absence. The child worker
            // independently re-enumerates the entire root before any write.
            std::error_code error;
            const auto status = std::filesystem::symlink_status(nativePath(path), error);
            if (status.type() == std::filesystem::file_type::not_found
                && (!error || error == std::errc::no_such_file_or_directory))
                prepared.missingWorks.append(work);
            else if (error)
                diagnose(result.diagnostics, result.suppressedDiagnostics, options.discovery.maximumDiagnostics,
                    QStringLiteral("无法确定作品目录是否消失，保留作品记录：%1").arg(path));
        }
    } catch (const std::exception &exception) {
        result.success = false; result.candidates.clear(); prepared.missingWorks.clear();
        result.errorMessage = QString::fromUtf8(exception.what());
    } catch (...) {
        result.success = false; result.candidates.clear(); prepared.missingWorks.clear();
        result.errorMessage = QStringLiteral("刷新根目录时发生未知错误。");
    }
    return prepared;
}

LibraryRootRefreshService::LibraryRootRefreshService(QString databasePath, LibraryImportService *single, QObject *parent)
    : QObject(parent), m_databasePath(std::move(databasePath)), m_single(single)
{
    qRegisterMetaType<LibraryImportSubmission>();
    qRegisterMetaType<LibraryRootRefreshResult>();
    if (single) {
        connect(single, &LibraryImportService::taskFinished, this, &LibraryRootRefreshService::consumeChild, Qt::QueuedConnection);
        connect(single, &LibraryImportService::stateChanged, this,
            [this](const QString &id, LibraryImportState state) {
                if (!m_active || id != m_childTaskId) return;
                if (state == LibraryImportState::Scanning) publishChildStage(m_result.roots[m_rootIndex].items[m_currentItem].requestedRemoval ? QStringLiteral("正在核实作品目录是否已删除…") : QStringLiteral("正在扫描作品…"));
                else if (state == LibraryImportState::Committing) publishChildStage(m_result.roots[m_rootIndex].items[m_currentItem].requestedRemoval ? QStringLiteral("正在清理作品及其关联记录…") : QStringLiteral("正在提交作品，等待提交和字幕关联完成…"));
                else if (state == LibraryImportState::ProcessingReferences) publishChildStage(QStringLiteral("作品已入库，正在提取并缓存参考资料…"));
                else if (state == LibraryImportState::Cancelling) publishChildStage(QStringLiteral("正在停止当前扫描…"));
            }, Qt::QueuedConnection);
        connect(single, &LibraryImportService::importCommitted, this,
            [this](const QString &id, const LibraryImportResult &result) {
                if (m_active && id == m_childTaskId) publishChildStage(result.removed ? QStringLiteral("作品及其关联记录已清理。") : QStringLiteral("作品已提交，正在处理字幕关联…"));
            }, Qt::QueuedConnection);
        connect(single, &QObject::destroyed, this, &LibraryRootRefreshService::singleDestroyed);
    }
}
LibraryRootRefreshService::~LibraryRootRefreshService()
{
    if (m_cancelDiscovery) m_cancelDiscovery->store(true);
    if (m_single && (m_submitting || (!m_childTaskId.isEmpty() && m_single->activeTaskId() == m_childTaskId)))
        m_single->cancelCurrent();
    if (m_discoveryLifetime.isValid()) m_discoveryLifetime.waitForFinished();
}
LibraryImportSubmission LibraryRootRefreshService::start(const QStringList &roots, const LibraryRootRefreshOptions &options)
{
    if (QThread::currentThread() != thread()) return {LibraryImportSubmissionStatus::WrongThread, {}};
    if (m_shuttingDown || !m_single || m_single->isShuttingDown()) return {LibraryImportSubmissionStatus::ShuttingDown, {}};
    if (m_active || m_finalizing || m_single->isBusy()) return {LibraryImportSubmissionStatus::Busy, {}};
    m_result = {};
    m_result.taskId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSet<QString> keys;
    for (const auto &input : roots) {
        const QString normalized = ScanRootStore::normalizePath(input);
        const QString path = normalized.isEmpty() ? input : normalized;
        const QString key = path.toCaseFolded();
        if (keys.contains(key)) continue;
        keys.insert(key);
        LibraryRootRefreshRootResult item; item.rootDirectory = path; m_result.roots.append(item);
    }
    m_options = options; m_rootIndex = 0; m_queue.clear(); m_next = 0; m_currentItem = -1;
    m_childTaskId.clear(); m_deferredChildResult.reset(); m_rootPrepared = false;
    m_active = true; m_stopRequested = false;
    m_cancelDiscovery = std::make_shared<std::atomic<bool>>(false);
    const auto id = m_result.taskId;
    scheduleNext();
    emit stateChanged(QStringLiteral("正在准备刷新扫描目录…"));
    return {LibraryImportSubmissionStatus::Accepted, id};
}
void LibraryRootRefreshService::scheduleNext()
{
    const auto id = m_result.taskId;
    QMetaObject::invokeMethod(this, [this, id] { advance(id); }, Qt::QueuedConnection);
}
void LibraryRootRefreshService::advance(const QString &id)
{
    if (!m_active || id != m_result.taskId || m_discovering || m_submitting || !m_childTaskId.isEmpty()) return;
    if (m_stopRequested || m_shuttingDown || m_rootIndex >= m_result.roots.size()) { finishRefresh(); return; }
    if (!m_single || m_single->isShuttingDown()) {
        m_result.errorMessage = QStringLiteral("单作品导入服务不可用，刷新已停止。"); finishRefresh(); return;
    }
    if (!m_rootPrepared) { beginRoot(); return; }
    if (m_next >= m_queue.size()) { finishRoot(); scheduleNext(); return; }
    m_currentItem = m_queue[m_next++];
    auto &item = m_result.roots[m_rootIndex].items[m_currentItem];
    const auto directory = item.directory;
    QPointer<LibraryRootRefreshService> guard(this);
    m_submitting = true; m_deferredChildResult.reset();
    const auto submission = item.requestedRemoval ? m_single->submitRemoval(item.workId, directory, m_result.roots[m_rootIndex].rootDirectory)
        : item.workId ? m_single->submitRefresh(item.workId, directory, m_options.scan) : m_single->submit(directory, m_options.scan);
    if (!guard) return;
    m_submitting = false;
    if (!m_active || id != m_result.taskId) return;
    if (!m_single) { scheduleNext(); return; }
    if (submission.status != LibraryImportSubmissionStatus::Accepted) {
        item.outcome = LibraryRootRefreshItemOutcome::Failed;
        item.errorMessage = QStringLiteral("单作品服务拒绝当前任务，后续刷新已停止。");
        m_currentItem = -1; m_result.errorMessage = item.errorMessage;
        finishRefresh(); return;
    }
    m_childTaskId = submission.taskId;
    if (m_stopRequested || m_shuttingDown) m_single->cancelCurrent();
    if (!guard || !m_active || id != m_result.taskId || m_childTaskId != submission.taskId) return;
    auto deferred = std::move(m_deferredChildResult); m_deferredChildResult.reset();
    emit childStarted(submission, directory, static_cast<int>(m_next), static_cast<int>(m_queue.size()));
    if (guard && deferred && deferred->taskId == submission.taskId)
        QMetaObject::invokeMethod(this, [this, child = std::move(*deferred)] { consumeChild(child); }, Qt::QueuedConnection);
}
void LibraryRootRefreshService::beginRoot()
{
    const auto id = m_result.taskId;
    const auto index = m_rootIndex;
    const auto root = m_result.roots[index].rootDirectory;
    m_discovering = true;
    auto promise = std::make_shared<QPromise<Preparation>>();
    auto *watcher = new QFutureWatcher<Preparation>(this);
    connect(watcher, &QFutureWatcher<Preparation>::finished, this, [this, watcher, id, index] {
        const auto prepared = watcher->result(); watcher->deleteLater();
        if (!m_active || id != m_result.taskId || index != m_rootIndex) return;
        m_discovering = false;
        auto &rootResult = m_result.roots[index];
        const auto &found = prepared.discovery;
        rootResult.rootDirectory = found.rootDirectory; rootResult.diagnostics = found.diagnostics;
        rootResult.suppressedDiagnostics = found.suppressedDiagnostics;
        rootResult.rootFilesSkipped = found.rootFilesSkipped; rootResult.linksSkipped = found.linksSkipped;
        if (m_stopRequested || found.cancelled) { rootResult.cancelled = true; m_result.cancelled = true; finishRefresh(); return; }
        if (!found.success) {
            rootResult.errorMessage = found.errorMessage;
            ++m_rootIndex; m_rootPrepared = false; scheduleNext(); return;
        }
        rootResult.success = true; rootResult.discovered = found.candidates.size();
        m_queue.clear(); m_next = 0; m_currentItem = -1;
        for (const auto &candidate : found.candidates) {
            LibraryRootRefreshItemResult item;
            item.directory = candidate.directory; item.workId = candidate.existingWorkId;
            if (!candidate.existingWorkId && !candidate.overlappingWorks.isEmpty()) {
                item.outcome = LibraryRootRefreshItemOutcome::Overlap;
                item.errorMessage = QStringLiteral("与已有作品路径重叠，等待手动确认导入。");
                rootResult.overlapDirectories.append(candidate.directory);
            } else m_queue.append(rootResult.items.size());
            rootResult.items.append(std::move(item));
        }
        for (const auto &work : prepared.missingWorks) {
            LibraryRootRefreshItemResult item; item.directory = work.rootPath;
            item.workId = work.id; item.requestedRemoval = true;
            m_queue.append(rootResult.items.size()); rootResult.items.append(std::move(item));
        }
        m_rootPrepared = true; scheduleNext();
        emit stateChanged(QStringLiteral("目录枚举完成，正在逐作品刷新：%1").arg(rootResult.rootDirectory));
    });
    m_discoveryLifetime = promise->future(); watcher->setFuture(m_discoveryLifetime);
    const auto databasePath = m_databasePath;
    const auto options = m_options;
    const auto cancelled = m_cancelDiscovery;
    const auto beforeDiscovery = m_beforeDiscovery;
    QThreadPool::globalInstance()->start([promise, databasePath, root, options, cancelled, beforeDiscovery] {
        promise->start(); promise->addResult(prepare(databasePath, root, options, cancelled, beforeDiscovery)); promise->finish();
    });
    emit stateChanged(QStringLiteral("正在枚举扫描目录 %1/%2：%3").arg(index + 1).arg(m_result.roots.size()).arg(root));
}
void LibraryRootRefreshService::consumeChild(const LibraryImportTaskResult &child)
{
    if (m_active && m_submitting && m_childTaskId.isEmpty()) { m_deferredChildResult = child; return; }
    if (!m_active || m_childTaskId.isEmpty() || child.taskId != m_childTaskId) return;
    summarize(m_result.roots[m_rootIndex].items[m_currentItem], child, m_options.maximumItemDiagnostics);
    m_childTaskId.clear(); m_currentItem = -1; scheduleNext();
    emit stateChanged(QStringLiteral("当前作品处理完成，正在准备后续项目…"));
}
void LibraryRootRefreshService::finishRoot()
{
    m_rootPrepared = false; m_queue.clear(); m_next = 0; m_currentItem = -1; ++m_rootIndex;
}
void LibraryRootRefreshService::finishRefresh()
{
    if (!m_active || m_discovering || m_submitting || !m_childTaskId.isEmpty()) return;
    m_active = false; m_finalizing = true;
    for (auto &root : m_result.roots) {
        root.imported = root.updated = root.removed = root.skipped = root.failed = root.associationFailed = root.notStarted = 0;
        for (const auto &item : root.items) {
            switch (item.outcome) {
            case LibraryRootRefreshItemOutcome::Added: ++root.imported; break;
            case LibraryRootRefreshItemOutcome::Updated: ++root.updated; break;
            case LibraryRootRefreshItemOutcome::Removed: ++root.removed; break;
            case LibraryRootRefreshItemOutcome::Skipped:
            case LibraryRootRefreshItemOutcome::Overlap: ++root.skipped; break;
            case LibraryRootRefreshItemOutcome::Failed: ++root.failed; break;
            case LibraryRootRefreshItemOutcome::Cancelled: root.cancelled = true; break;
            case LibraryRootRefreshItemOutcome::NotStarted: ++root.notStarted; break;
            }
            if (item.associationStatus == LibraryAutoAssociationStatus::Failed
                || item.associationStatus == LibraryAutoAssociationStatus::StaleSnapshot) ++root.associationFailed;
        }
        if (m_result.cancelled && (&root - m_result.roots.data()) >= m_rootIndex) root.cancelled = true;
        root.success = root.success && !root.cancelled && !root.failed && !root.associationFailed && !root.notStarted;
    }
    const auto result = m_result;
    QPointer<LibraryRootRefreshService> guard(this);
    emit stateChanged(result.cancelled ? QStringLiteral("刷新已停止，已提交结果保留。") : QStringLiteral("扫描目录刷新完成。"));
    if (!guard) return;
    m_finalizing = false; emit finished(result);
    if (guard) finishShutdownIfReady();
}
LibraryImportCancelResult LibraryRootRefreshService::cancel()
{
    if (QThread::currentThread() != thread()) return LibraryImportCancelResult::WrongThread;
    if (!m_active) return LibraryImportCancelResult::NoActiveTask;
    if (m_stopRequested) return LibraryImportCancelResult::AlreadyRequested;
    m_stopRequested = true; m_result.cancelled = true;
    if (m_cancelDiscovery) m_cancelDiscovery->store(true);
    QPointer<LibraryRootRefreshService> guard(this);
    if (m_single && !m_childTaskId.isEmpty() && m_single->activeTaskId() == m_childTaskId) m_single->cancelCurrent();
    if (!guard) return LibraryImportCancelResult::Requested;
    if (!m_discovering && !m_submitting && m_childTaskId.isEmpty()) scheduleNext();
    emit stateChanged(QStringLiteral("已请求停止；当前提交或关联若已开始，将等待完成，后续项目不再启动。"));
    return LibraryImportCancelResult::Requested;
}
bool LibraryRootRefreshService::shutdown()
{
    if (QThread::currentThread() != thread()) return false;
    m_shuttingDown = true;
    QPointer<LibraryRootRefreshService> guard(this);
    if (m_active) cancel();
    if (guard) QMetaObject::invokeMethod(this, &LibraryRootRefreshService::finishShutdownIfReady, Qt::QueuedConnection);
    return true;
}
void LibraryRootRefreshService::finishShutdownIfReady()
{
    if (!m_shuttingDown || m_shutdownNotified || m_active || m_finalizing || m_discovering || m_submitting || !m_childTaskId.isEmpty()) return;
    m_shutdownNotified = true; emit shutdownFinished();
}
void LibraryRootRefreshService::singleDestroyed()
{
    m_single = nullptr;
    if (!m_active) return;
    m_stopRequested = true;
    if (m_cancelDiscovery) m_cancelDiscovery->store(true);
    m_result.errorMessage = QStringLiteral("单作品导入服务已销毁，后续刷新未开始。");
    if (m_currentItem >= 0) {
        auto &item = m_result.roots[m_rootIndex].items[m_currentItem];
        item.outcome = LibraryRootRefreshItemOutcome::Failed; item.errorMessage = m_result.errorMessage;
    }
    m_currentItem = -1; m_childTaskId.clear();
    if (!m_discovering && !m_submitting) finishRefresh();
}
void LibraryRootRefreshService::publishChildStage(const QString &text)
{
    if (!m_active || m_currentItem < 0 || m_childTaskId.isEmpty()) return;
    emit stateChanged(QStringLiteral("根 %1/%2，作品 %3/%4：%5\n%6")
        .arg(m_rootIndex + 1).arg(m_result.roots.size()).arg(m_next).arg(m_queue.size())
        .arg(QFileInfo(m_result.roots[m_rootIndex].items[m_currentItem].directory).fileName(), text));
}

} // namespace asmr
