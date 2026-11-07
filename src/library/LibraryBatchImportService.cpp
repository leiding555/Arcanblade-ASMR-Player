#include "library/LibraryBatchImportService.h"

#include <QFileInfo>
#include <QFutureWatcher>
#include <QMetaObject>
#include <QPromise>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <QUuid>

#include <exception>
#include <utility>

namespace asmr {
namespace {
void appendDiagnostic(QStringList &messages, qsizetype &suppressed,
                      qsizetype limit, const QString &message)
{
    if (messages.size() < limit)
        messages.append(message);
    else
        ++suppressed;
}

WorkDirectoryDiscoveryResult discoverWithRepository(
    const QString &databasePath, const QString &root, const LibraryBatchImportOptions &options,
    const std::shared_ptr<std::atomic<bool>> &cancelled,
    const std::shared_ptr<const std::function<void()>> &beforeDiscovery)
{
    WorkDirectoryDiscoveryResult result;
    result.rootDirectory = root;
    try {
        if (beforeDiscovery)
            (*beforeDiscovery)();
        if (cancelled->load()) {
            result.cancelled = true;
            return result;
        }
        if (options.maximumItemDiagnostics < 0) {
            result.errorMessage = QStringLiteral("每项诊断限额不能为负数。");
            return result;
        }
        LibraryRepository repository(databasePath);
        const auto opened = repository.open();
        LibraryWorksResult known;
        if (opened.success)
            known = repository.works();
        const auto closed = repository.close();
        if (!opened.success || !known.success || !closed.success) {
            result.errorMessage = QStringLiteral("读取已有作品失败，未生成可导入列表。");
            const qsizetype limit = qMax(qsizetype(0), options.discovery.maximumDiagnostics);
            const auto copy = [&](const auto &operation) {
                for (const auto &diagnostic : operation.diagnostics)
                    appendDiagnostic(result.diagnostics, result.suppressedDiagnostics, limit, diagnostic.message);
            };
            copy(opened); copy(known); copy(closed);
            return result;
        }
        result = WorkDirectoryDiscovery::discover(root, known.works, options.discovery,
            [cancelled] { return cancelled->load(); });
    } catch (const std::exception &exception) {
        result.success = false;
        result.candidates.clear();
        result.errorMessage = QString::fromUtf8(exception.what());
    } catch (...) {
        result.success = false;
        result.candidates.clear();
        result.errorMessage = QStringLiteral("枚举作品总目录发生未知错误。");
    }
    return result;
}

LibraryBatchImportItemResult summarize(const QString &directory,
                                        const LibraryImportTaskResult &child,
                                        qsizetype diagnosticLimit)
{
    LibraryBatchImportItemResult item;
    item.directory = directory;
    item.workId = child.repositoryResult.workId;
    item.errorMessage = child.errorMessage;
    if (child.databaseCommitted) {
        item.created = child.repositoryResult.created;
        item.insertedFiles = child.repositoryResult.insertedFiles;
        item.updatedFiles = child.repositoryResult.updatedFiles;
        item.missingFiles = child.repositoryResult.missingFiles;
        item.associationStatus = child.autoAssociation.status;
        item.addedBindings = child.autoAssociation.addedBindings;
        item.existingBindings = child.autoAssociation.existingBindings;
        item.manualBindingsPreserved = child.autoAssociation.manualBindingsPreserved;
        item.outcome = child.autoAssociation.status == LibraryAutoAssociationStatus::Succeeded
            ? LibraryBatchItemOutcome::Imported : LibraryBatchItemOutcome::ImportedAssociationFailed;
    } else if (child.state == LibraryImportState::Cancelled)
        item.outcome = LibraryBatchItemOutcome::Cancelled;
    else if (child.scan.isComplete() && child.scan.audioFiles.isEmpty()) {
        item.outcome = LibraryBatchItemOutcome::Skipped;
        item.errorMessage = QStringLiteral("无受支持音频，未创建作品。");
    } else
        item.outcome = LibraryBatchItemOutcome::Failed;
    const auto add = [&](const QString &message) {
        appendDiagnostic(item.diagnostics, item.suppressedDiagnostics, diagnosticLimit, message);
    };
    for (const auto &diagnostic : child.scan.diagnostics)
        add(diagnostic.message);
    item.suppressedDiagnostics += child.scan.suppressedDiagnostics;
    for (const auto &diagnostic : child.repositoryResult.diagnostics)
        add(diagnostic.message);
    for (const auto &diagnostic : child.autoAssociation.diagnostics)
        add(diagnostic.message);
    if (!child.references.error.isEmpty()) add(child.references.error);
    for (const auto &warning : child.references.warnings) add(warning);
    if (child.references.failedCount > 0)
        add(QStringLiteral("参考资料中有 %1 份无法提取，请在参考资料窗口查看。").arg(child.references.failedCount));
    return item;
}
} // namespace

LibraryBatchImportService::LibraryBatchImportService(
    QString databasePath, LibraryImportService *single, QObject *parent)
    : QObject(parent), m_databasePath(std::move(databasePath)), m_single(single)
{
    qRegisterMetaType<LibraryImportSubmission>();
    qRegisterMetaType<WorkDirectoryDiscoveryResult>();
    qRegisterMetaType<LibraryBatchImportState>();
    qRegisterMetaType<LibraryBatchImportResult>();
    if (single) {
        connect(single, &LibraryImportService::taskFinished,
                this, &LibraryBatchImportService::consumeChild, Qt::QueuedConnection);
        connect(single, &LibraryImportService::stateChanged, this,
            [this](const QString &taskId, LibraryImportState state) {
                if (!m_active || taskId != m_childTaskId)
                    return;
                switch (state) {
                case LibraryImportState::Scanning:
                    publishChildStage(QStringLiteral("正在递归扫描作品目录…")); break;
                case LibraryImportState::Committing:
                    publishChildStage(QStringLiteral("正在提交作品；提交和关联阶段将等待完成。")); break;
                case LibraryImportState::ProcessingReferences:
                    publishChildStage(QStringLiteral("作品已入库，正在提取并缓存参考资料…")); break;
                case LibraryImportState::Cancelling:
                    publishChildStage(QStringLiteral("正在等待当前作品扫描停止…")); break;
                default: break; // Only taskFinished advances or finalizes.
                }
            }, Qt::QueuedConnection);
        connect(single, &LibraryImportService::importCommitted, this,
            [this](const QString &taskId, const LibraryImportResult &) {
                if (m_active && taskId == m_childTaskId)
                    publishChildStage(QStringLiteral("作品已入库，正在处理字幕自动关联…"));
            }, Qt::QueuedConnection);
        connect(single, &QObject::destroyed, this, &LibraryBatchImportService::singleDestroyed);
    }
}

LibraryBatchImportService::~LibraryBatchImportService()
{
    if (m_cancelDiscovery)
        m_cancelDiscovery->store(true);
    if (m_single && (m_submitting || (!m_childTaskId.isEmpty()
        && m_single->activeTaskId() == m_childTaskId)))
        m_single->cancelCurrent();
    if (m_discoveryLifetime.isValid())
        m_discoveryLifetime.waitForFinished();
}

LibraryImportSubmission LibraryBatchImportService::discover(
    const QString &root, const LibraryBatchImportOptions &options)
{
    if (QThread::currentThread() != thread())
        return {LibraryImportSubmissionStatus::WrongThread, {}};
    if (m_shuttingDown || !m_single || m_single->isShuttingDown())
        return {LibraryImportSubmissionStatus::ShuttingDown, {}};
    if (m_active || m_finalizing || m_single->isBusy())
        return {LibraryImportSubmissionStatus::Busy, {}};
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_result = {};
    m_result.batchId = id;
    m_result.rootDirectory = root;
    m_preview = {};
    m_options = options;
    m_queue.clear();
    m_next = 0;
    m_currentItem = -1;
    m_childTaskId.clear();
    m_deferredChildResult.reset();
    m_active = true;
    m_stopRequested = false;
    m_discovering = true;
    m_cancelDiscovery = std::make_shared<std::atomic<bool>>(false);
    auto promise = std::make_shared<QPromise<WorkDirectoryDiscoveryResult>>();
    auto *watcher = new QFutureWatcher<WorkDirectoryDiscoveryResult>(this);
    connect(watcher, &QFutureWatcher<WorkDirectoryDiscoveryResult>::finished, this,
        [this, watcher, id] {
            auto result = watcher->result();
            watcher->deleteLater();
            if (!m_active || m_result.batchId != id)
                return;
            m_discovering = false;
            m_result.rootDirectory = result.rootDirectory;
            m_result.diagnostics = result.diagnostics;
            m_result.suppressedDiagnostics = result.suppressedDiagnostics;
            m_result.rootFilesSkipped = result.rootFilesSkipped;
            m_result.linksSkipped = result.linksSkipped;
            const qsizetype limit = qMax(qsizetype(0), m_options.discovery.maximumDiagnostics);
            if (result.rootFilesSkipped > 0)
                appendDiagnostic(m_result.diagnostics, m_result.suppressedDiagnostics, limit,
                    QStringLiteral("总根目录文件未纳入作品：%1 个。").arg(result.rootFilesSkipped));
            if (result.linksSkipped > 0)
                appendDiagnostic(m_result.diagnostics, m_result.suppressedDiagnostics, limit,
                    QStringLiteral("跳过链接或 junction：%1 个。").arg(result.linksSkipped));
            if (m_stopRequested || result.cancelled) {
                m_result.cancelled = true;
                finishBatch();
                return;
            }
            if (!result.success) {
                m_result.errorMessage = result.errorMessage;
                finishBatch();
                return;
            }
            m_preview = result;
            m_result.items.reserve(result.candidates.size());
            for (const auto &candidate : result.candidates) {
                LibraryBatchImportItemResult item;
                item.directory = candidate.directory;
                item.workId = candidate.existingWorkId;
                m_result.items.push_back(std::move(item));
            }
            QPointer<LibraryBatchImportService> guard(this);
            publishState(LibraryBatchImportState::AwaitingSelection, QStringLiteral("请选择要导入的一级子目录。"));
            if (guard && m_active && !m_stopRequested && !m_shuttingDown
                && m_result.batchId == id && m_state == LibraryBatchImportState::AwaitingSelection)
                emit previewReady(id, result);
        });
    m_discoveryLifetime = promise->future();
    watcher->setFuture(m_discoveryLifetime);
    const QString databasePath = m_databasePath;
    const auto cancelFlag = m_cancelDiscovery;
    const auto beforeDiscovery = m_beforeDiscovery;
    QThreadPool::globalInstance()->start([promise, databasePath, root, options, cancelFlag, beforeDiscovery] {
        promise->start();
        auto result = discoverWithRepository(databasePath, root, options, cancelFlag, beforeDiscovery);
        promise->addResult(std::move(result));
        promise->finish();
    });
    publishState(LibraryBatchImportState::Discovering, QStringLiteral("正在枚举作品总目录的一级子目录…"));
    return {LibraryImportSubmissionStatus::Accepted, id};
}

bool LibraryBatchImportService::startSelected(const QString &id, const QStringList &directories)
{
    if (QThread::currentThread() != thread() || m_shuttingDown || !m_active
        || m_state != LibraryBatchImportState::AwaitingSelection || id != m_result.batchId
        || m_stopRequested)
        return false;
    QSet<QString> candidates;
    candidates.reserve(m_preview.candidates.size());
    for (const auto &candidate : m_preview.candidates)
        candidates.insert(candidate.directory);
    QSet<QString> selected;
    selected.reserve(directories.size());
    for (const auto &directory : directories) {
        if (selected.contains(directory) || !candidates.contains(directory))
            return false;
        selected.insert(directory);
    }
    for (qsizetype index = 0; index < m_result.items.size(); ++index) {
        auto &item = m_result.items[index];
        if (selected.contains(item.directory))
            m_queue.push_back(index);
        else {
            item.outcome = LibraryBatchItemOutcome::Skipped;
            item.errorMessage = QStringLiteral("未选择此目录。");
        }
    }
    m_result.selectedCount = m_queue.size();
    scheduleNext();
    publishState(LibraryBatchImportState::Importing, QStringLiteral("已确认导入列表，正在准备逐作品导入。"));
    return true;
}

void LibraryBatchImportService::scheduleNext()
{
    const QString id = m_result.batchId;
    QMetaObject::invokeMethod(this, [this, id] { startNext(id); }, Qt::QueuedConnection);
}

void LibraryBatchImportService::startNext(const QString &id)
{
    if (!m_active || id != m_result.batchId || m_discovering || m_submitting || !m_childTaskId.isEmpty())
        return;
    if (m_stopRequested || m_shuttingDown || m_next >= m_queue.size()) {
        finishBatch();
        return;
    }
    if (!m_single || m_single->isShuttingDown()) {
        m_result.errorMessage = QStringLiteral("单作品导入服务不可用，后续项目未开始。");
        finishBatch();
        return;
    }
    m_currentItem = m_queue[m_next++];
    auto &item = m_result.items[m_currentItem];
    const QString directory = item.directory;
    QPointer<LibraryBatchImportService> guard(this);
    m_submitting = true;
    m_deferredChildResult.reset();
    const auto submission = m_single->submit(directory, m_options.scan);
    if (!guard)
        return;
    m_submitting = false;
    if (!m_active || m_result.batchId != id)
        return;
    if (!m_single) {
        finishBatch();
        return;
    }
    if (submission.status != LibraryImportSubmissionStatus::Accepted) {
        item.outcome = LibraryBatchItemOutcome::Failed;
        item.errorMessage = QStringLiteral("单作品导入服务拒绝了此项目，批次已停止。");
        m_currentItem = -1;
        m_result.errorMessage = item.errorMessage;
        finishBatch();
        return;
    }
    m_childTaskId = submission.taskId;
    // submit() synchronously emits Scanning. A direct observer may cancel or
    // close the batch before this ID can be installed. Apply the latched
    // intent now; the single service's atomic commit boundary still decides
    // whether the current child can cancel or must finish its DB stages.
    if (m_stopRequested || m_shuttingDown)
        m_single->cancelCurrent();
    if (!guard || !m_active || m_result.batchId != id || m_childTaskId != submission.taskId)
        return;
    auto deferred = std::move(m_deferredChildResult);
    m_deferredChildResult.reset();
    emit childStarted(id, submission, directory, static_cast<int>(m_next),
                      static_cast<int>(m_queue.size()));
    if (guard && deferred && deferred->taskId == submission.taskId) {
        QMetaObject::invokeMethod(this, [this, child = std::move(*deferred)] {
            consumeChild(child);
        }, Qt::QueuedConnection);
    }
}

void LibraryBatchImportService::consumeChild(const LibraryImportTaskResult &result)
{
    if (m_active && m_submitting && m_childTaskId.isEmpty()) {
        // A direct Scanning observer can run a nested event loop. Retain the
        // one result until submit returns, so childStarted still precedes its
        // consumption and neither completion nor cancellation is lost.
        m_deferredChildResult = result;
        return;
    }
    if (!m_active || m_childTaskId.isEmpty() || result.taskId != m_childTaskId)
        return;
    const QString directory = m_result.items[m_currentItem].directory;
    m_result.items[m_currentItem] = summarize(directory, result, m_options.maximumItemDiagnostics);
    m_currentItem = -1;
    m_childTaskId.clear();
    // The single service has joined/deleted its worker before this queued slot.
    // Queue advancement after all observers have received this child's result.
    scheduleNext();
    publishState(m_stopRequested ? LibraryBatchImportState::Stopping : LibraryBatchImportState::Importing,
                 QStringLiteral("当前作品处理完成，正在准备下一项或汇总结果。"));
}

LibraryImportCancelResult LibraryBatchImportService::cancel()
{
    if (QThread::currentThread() != thread())
        return LibraryImportCancelResult::WrongThread;
    if (!m_active)
        return LibraryImportCancelResult::NoActiveTask;
    if (m_stopRequested)
        return LibraryImportCancelResult::AlreadyRequested;
    m_stopRequested = true;
    m_result.cancelled = true;
    if (m_cancelDiscovery)
        m_cancelDiscovery->store(true);
    QPointer<LibraryBatchImportService> guard(this);
    if (m_single && !m_childTaskId.isEmpty() && m_single->activeTaskId() == m_childTaskId)
        m_single->cancelCurrent();
    if (!guard)
        return LibraryImportCancelResult::Requested;
    if (!m_discovering && !m_submitting && m_childTaskId.isEmpty())
        scheduleNext();
    publishState(LibraryBatchImportState::Stopping,
        QStringLiteral("已请求停止批次；当前提交或关联若已开始，将等待完成，后续作品不再启动。"));
    return LibraryImportCancelResult::Requested;
}

bool LibraryBatchImportService::shutdown()
{
    if (QThread::currentThread() != thread())
        return false;
    m_shuttingDown = true;
    QPointer<LibraryBatchImportService> guard(this);
    if (m_active)
        cancel();
    if (guard)
        QMetaObject::invokeMethod(this, &LibraryBatchImportService::finishShutdownIfReady, Qt::QueuedConnection);
    return true;
}

void LibraryBatchImportService::finishBatch()
{
    if (!m_active || m_discovering || m_submitting || !m_childTaskId.isEmpty())
        return;
    m_active = false;
    m_finalizing = true;
    m_result.imported = m_result.associationFailed = m_result.skipped = m_result.failed = 0;
    m_result.cancelledItems = m_result.notStarted = 0;
    for (const auto &item : m_result.items) {
        switch (item.outcome) {
        case LibraryBatchItemOutcome::Imported: ++m_result.imported; break;
        case LibraryBatchItemOutcome::ImportedAssociationFailed:
            ++m_result.imported; ++m_result.associationFailed; break;
        case LibraryBatchItemOutcome::Skipped: ++m_result.skipped; break;
        case LibraryBatchItemOutcome::Failed: ++m_result.failed; break;
        case LibraryBatchItemOutcome::Cancelled: ++m_result.cancelledItems; break;
        case LibraryBatchItemOutcome::NotStarted: ++m_result.notStarted; break;
        }
    }
    const auto result = m_result;
    QPointer<LibraryBatchImportService> guard(this);
    publishState(LibraryBatchImportState::Finished,
                 result.cancelled ? QStringLiteral("批次已停止，已提交的作品保留。")
                                  : QStringLiteral("批次处理结束，请查看各作品结果。"));
    if (!guard)
        return;
    m_finalizing = false;
    emit finished(result);
    if (guard)
        finishShutdownIfReady();
}

void LibraryBatchImportService::finishShutdownIfReady()
{
    if (!m_shuttingDown || m_shutdownNotified || m_active || m_finalizing
        || m_discovering || m_submitting || !m_childTaskId.isEmpty())
        return;
    m_shutdownNotified = true;
    emit shutdownFinished();
}

void LibraryBatchImportService::singleDestroyed()
{
    m_single = nullptr;
    if (!m_active)
        return;
    m_stopRequested = true;
    if (m_cancelDiscovery)
        m_cancelDiscovery->store(true);
    m_result.errorMessage = QStringLiteral("单作品导入服务已销毁，后续项目未开始。");
    if (m_currentItem >= 0) {
        auto &item = m_result.items[m_currentItem];
        item.outcome = LibraryBatchItemOutcome::Failed;
        item.errorMessage = m_result.errorMessage;
    }
    m_currentItem = -1;
    m_childTaskId.clear();
    if (!m_discovering)
        finishBatch();
}

void LibraryBatchImportService::publishChildStage(const QString &text)
{
    if (!m_active || m_currentItem < 0 || m_childTaskId.isEmpty())
        return;
    publishState(m_stopRequested ? LibraryBatchImportState::Stopping : LibraryBatchImportState::Importing,
        QStringLiteral("第 %1/%2 项：%3\n%4").arg(m_next).arg(m_queue.size())
            .arg(QFileInfo(m_result.items[m_currentItem].directory).fileName(), text));
}

void LibraryBatchImportService::publishState(LibraryBatchImportState state, const QString &text)
{
    m_state = state;
    emit stateChanged(m_result.batchId, state, text);
}

} // namespace asmr
