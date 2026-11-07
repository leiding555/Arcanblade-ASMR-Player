#include "library/LibraryImportService.h"
#include "library/WorkDirectoryDiscovery.h"
#include "library/SubtitleTranslationCleanup.h"
#include "library/WorkReferenceRepository.h"
#include "subtitle/GeneratedSubtitleInfo.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QUuid>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <system_error>
#include <utility>

namespace asmr {

// run() deliberately has no event loop: cancellation is atomic, and exiting
// never depends on the owner thread processing a queued quit() request.
class LibraryImportThread final : public QThread {
    Q_OBJECT

public:
    LibraryImportThread(QString taskId, QString databasePath, QString directory,
                        DirectoryScanOptions options, QString displayName,
                        LibraryImportService::TaskKind kind, qint64 workId, QString scanRoot,
                        WorkReferenceRefreshPolicy references,
                        std::shared_ptr<LibraryImportService::TaskControl> control,
                        std::shared_ptr<const LibraryImportService::TestHooks> hooks,
                        QObject *parent)
        : QThread(parent), m_databasePath(std::move(databasePath)),
          m_directory(std::move(directory)), m_options(options),
          m_displayName(std::move(displayName)), m_kind(kind), m_workId(workId),
          m_scanRoot(std::move(scanRoot)), m_referencePolicy(references), m_control(std::move(control)),
          m_hooks(std::move(hooks))
    {
        m_result.taskId = std::move(taskId);
    }

    LibraryImportTaskResult takeResult() { return std::move(m_result); }

signals:
    void scanReady(QString taskId, asmr::DirectoryScanResult scan);
    void committing(QString taskId);
    void processingReferences(QString taskId);
    void importReady(QString taskId, asmr::LibraryImportResult result);

protected:
    void run() override
    {
        try {
            performTask();
        } catch (const std::exception &exception) {
            unexpectedFailure(QStringLiteral("Unexpected background failure: ")
                              + QString::fromUtf8(exception.what()));
        } catch (...) {
            unexpectedFailure(QStringLiteral("Unexpected background failure."));
        }
        const auto lastPhase = m_control->phase.exchange(LibraryImportService::TaskPhase::Finished);
        if (lastPhase == LibraryImportService::TaskPhase::CancelRequested
            && !m_result.databaseCommitted)
            m_result.state = LibraryImportState::Cancelled;
        // A private test observer is never installed by production callers.
        if (m_hooks && m_hooks->reached)
            m_hooks->reached(LibraryImportService::TestPoint::BeforeFinished);
    }

private:
    void reached(LibraryImportService::TestPoint point)
    {
        if (m_hooks && m_hooks->reached)
            m_hooks->reached(point);
    }

    bool cancellationRequested() const
    {
        return m_control->phase.load() == LibraryImportService::TaskPhase::CancelRequested;
    }

    void unexpectedFailure(const QString &message)
    {
        if (m_result.databaseCommitted) {
            // Import is already durable: report only the separate association
            // failure, never imply its successful transaction was rolled back.
            m_result.state = LibraryImportState::Completed;
            if (m_referencePhaseStarted) {
                m_result.references.status = WorkReferenceSyncStatus::Failed;
                m_result.references.error = message;
                return;
            }
            m_result.autoAssociation.status = LibraryAutoAssociationStatus::Failed;
            m_result.autoAssociation.workId = m_result.repositoryResult.workId;
            m_result.autoAssociation.diagnostics.push_back(
                {LibraryDiagnosticCode::AutoAssociationError, m_databasePath, message});
        } else {
            m_result.state = LibraryImportState::Failed;
            m_result.errorMessage = message;
        }
    }

    void processReferences(LibraryRepository &repository)
    {
        m_referencePhaseStarted = true;
        auto expected = LibraryImportService::TaskPhase::Committing;
        if (!m_control->phase.compare_exchange_strong(
                expected, LibraryImportService::TaskPhase::ProcessingReferences)) {
            m_result.references.status = WorkReferenceSyncStatus::Cancelled;
            m_result.references.error = QStringLiteral("作品已入库；参考资料处理已取消，上次资料快照保留。");
            return;
        }
        emit processingReferences(m_result.taskId);
        reached(LibraryImportService::TestPoint::BeforeReferences);
        const auto snapshot = repository.workSnapshot(m_result.repositoryResult.workId);
        if (!snapshot.success) {
            m_result.references.status = WorkReferenceSyncStatus::Failed;
            m_result.references.error = QStringLiteral("无法读取作品身份，参考资料未更新。");
            return;
        }
        WorkReferenceRepository references(m_databasePath);
        m_result.references = references.synchronize(snapshot.work, m_result.scan,
            m_control->referencesCancelled, m_referencePolicy, [this] {
                auto phase = LibraryImportService::TaskPhase::ProcessingReferences;
                if (!m_control->phase.compare_exchange_strong(
                        phase, LibraryImportService::TaskPhase::ReferenceCommitting)) return false;
                emit committing(m_result.taskId);
                reached(LibraryImportService::TestPoint::ReferencesCommitting);
                return true;
            });
    }

    void associateSubtitles(LibraryRepository &repository)
    {
        auto &result = m_result.autoAssociation;
        result.status = LibraryAutoAssociationStatus::Failed;
        result.workId = m_result.repositoryResult.workId;
        const auto snapshot = repository.workSnapshot(result.workId);
        if (!snapshot.success) {
            result.diagnostics = snapshot.diagnostics;
            return;
        }
        QElapsedTimer timer;
        timer.start();
        result.matches = SubtitleMatcher::match(snapshot.work, snapshot.files);
        result.matchingElapsedNs = timer.nsecsElapsed();
        QVector<AutoSubtitleBinding> bindings;
        const auto partners = SubtitleMatcher::sharedAudioPartners(snapshot.work, snapshot.files);
        QSet<QPair<qint64, qint64>> bindingPairs;
        const auto addBinding = [&](qint64 audioId, qint64 subtitleId) {
            const QPair<qint64, qint64> pair{audioId, subtitleId};
            if (bindingPairs.contains(pair)) return;
            bindingPairs.insert(pair);
            bindings.push_back({audioId, subtitleId});
            ++result.eligibleBindings;
        };
        const auto addSharedBindings = [&](qint64 audioId, qint64 subtitleId) {
            addBinding(audioId, subtitleId);
            if (const auto partner = partners.value(audioId)) addBinding(partner, subtitleId);
        };
        // Generated provenance still resolves the exact original audio first.
        // Only the playback relationship expands to a unique WAV/MP3 partner;
        // a missing original must never borrow the other format's identity.
        QHash<QString, qint64> audioIds;
        const auto pathKey = [](const QString &path) {
            auto key = QDir::cleanPath(QDir::fromNativeSeparators(path));
#ifdef Q_OS_WIN
            key = key.toCaseFolded();
#endif
            return key;
        };
        for (const auto &file : snapshot.files)
            if (!file.missing && file.type == LibraryFileType::Audio)
                audioIds.insert(pathKey(file.relativePath), file.id);
        QHash<qint64, qint64> generatedAudio;
        for (const auto &file : snapshot.files) {
            if (file.missing || file.type != LibraryFileType::Subtitle)
                continue;
            const auto info = readGeneratedSubtitleInfo(QDir(snapshot.work.rootPath).filePath(file.relativePath));
            if (!info)
                continue;
            const auto relativeAudio = QDir(QFileInfo(file.relativePath).path())
                .filePath(QStringLiteral("../") + info->audioFileName);
            const auto audioId = audioIds.value(pathKey(relativeAudio));
            generatedAudio.insert(file.id, audioId);
            if (audioId > 0) {
                addSharedBindings(audioId, file.id);
            }
        }
        for (const auto &track : result.matches.tracks) {
            for (const auto &candidate : track.candidates) {
                ++result.candidateCount;
                if (generatedAudio.contains(candidate.subtitleFileId))
                    continue;
                if (candidate.autoAssociationAllowed) {
                    addSharedBindings(track.audioFileId, candidate.subtitleFileId);
                } else if (candidate.rule == SubtitleMatchRule::TrackNumberOnly
                           || candidate.numberOnly || !candidate.recommended) {
                    ++result.skippedWeakCandidates;
                } else {
                    ++result.skippedAmbiguousCandidates;
                    if (!candidate.competingAudioFileIds.isEmpty())
                        ++result.skippedCompetingCandidates;
                    if (candidate.ambiguousVersion)
                        ++result.skippedVersionAmbiguityCandidates;
                }
            }
        }
        reached(LibraryImportService::TestPoint::AfterMatching);
        // Matching holds no SQL transaction. The batch locks and compares the
        // whole snapshot again before its first write, including empty batches.
        timer.start();
        const auto applied = repository.addAutoSubtitleBindings(snapshot, bindings);
        result.bindingElapsedNs = timer.nsecsElapsed();
        result.diagnostics = applied.diagnostics;
        if (applied.success) {
            result.status = LibraryAutoAssociationStatus::Succeeded;
            result.addedBindings = applied.insertedBindings;
            result.existingBindings = applied.existingBindings;
            result.manualBindingsPreserved = applied.manualBindingsPreserved;
        } else {
            const bool stale = std::any_of(applied.diagnostics.cbegin(), applied.diagnostics.cend(),
                                           [](const auto &diagnostic) {
                return diagnostic.code == LibraryDiagnosticCode::StaleWorkSnapshot;
            });
            result.status = stale ? LibraryAutoAssociationStatus::StaleSnapshot
                                  : LibraryAutoAssociationStatus::Failed;
        }
    }

    static std::filesystem::path nativePath(const QString &path)
    {
#ifdef Q_OS_WIN
        return std::filesystem::path(path.toStdWString());
#else
        return std::filesystem::path(QFile::encodeName(path).constData());
#endif
    }

    static bool sameDirectory(const QString &first, const QString &second)
    {
        std::error_code error;
        return std::filesystem::equivalent(nativePath(first), nativePath(second), error) && !error;
    }

    bool missingPathStillConfirmed() const
    {
        namespace fs = std::filesystem;
        std::error_code error;
        const auto root = fs::symlink_status(nativePath(m_scanRoot), error);
        const QFileInfo rootInfo(m_scanRoot);
        if (error || !fs::is_directory(root) || fs::is_symlink(root)
            || rootInfo.isSymbolicLink() || rootInfo.isJunction())
            return false;
        const auto target = fs::symlink_status(nativePath(m_directory), error);
        // MSVC reports ENOENT with file_type::not_found. Every other error,
        // existing file, symlink and junction is inconclusive, never deletion.
        return target.type() == fs::file_type::not_found
            && (!error || error == std::errc::no_such_file_or_directory);
    }

    bool verifyMissingDirectory()
    {
        const auto discovery = WorkDirectoryDiscovery::discover(
            m_scanRoot, {}, {}, [this] { return cancellationRequested(); });
        if (discovery.cancelled || cancellationRequested()) {
            m_result.state = LibraryImportState::Cancelled;
            return false;
        }
        if (!discovery.success || !QDir::isAbsolutePath(m_directory)
            || !sameDirectory(QDir::cleanPath(QFileInfo(m_directory).absolutePath()), discovery.rootDirectory)
            || !missingPathStillConfirmed()) {
            m_result.errorMessage = QStringLiteral(
                "Deletion was not confirmed under a fully accessible scan root; historical records were retained.");
            m_result.repositoryResult.diagnostics.push_back(
                {LibraryDiagnosticCode::RejectedScan, m_directory, m_result.errorMessage});
            return false;
        }
        return true;
    }

    bool readExpectedSnapshot()
    {
        LibraryRepository repository(m_databasePath);
        const auto opened = repository.open();
        if (!opened.success) {
            m_result.repositoryResult.diagnostics = opened.diagnostics;
            m_result.errorMessage = QStringLiteral("Could not open the library database before the work operation.");
            return false;
        }
        m_expected = repository.workSnapshot(m_workId);
        if (!m_expected.success) {
            m_result.repositoryResult.diagnostics = m_expected.diagnostics;
            m_result.errorMessage = QStringLiteral("The existing work is unavailable for this operation.");
            return false;
        }
        const QString requested = QDir::cleanPath(QDir::fromNativeSeparators(m_directory));
        // Manual removal is intentionally independent of disk availability.
        // Its UI passes the persisted path; never resolve aliases against disk.
        if (m_expected.work.rootPath != requested
            && (m_kind == LibraryImportService::TaskKind::ManualRemoval
                || !sameDirectory(m_expected.work.rootPath, requested))) {
            m_result.errorMessage = QStringLiteral("The work operation must retain the original root path.");
            m_result.repositoryResult.diagnostics.push_back(
                {LibraryDiagnosticCode::PathConflict, m_directory, m_result.errorMessage});
            return false;
        }
        m_directory = m_expected.work.rootPath;
        return true;
    }

    void performTask()
    {
        const bool removal = m_kind == LibraryImportService::TaskKind::Removal
            || m_kind == LibraryImportService::TaskKind::ManualRemoval;
        reached(LibraryImportService::TestPoint::BeforeScan);
        if (m_kind != LibraryImportService::TaskKind::Import && !readExpectedSnapshot())
            return;
        if (removal) {
            if (m_kind == LibraryImportService::TaskKind::Removal && !verifyMissingDirectory())
                return;
            // Neither removal kind fabricates a recursive scan result.
            // scan.status stays Failed (not run); no scanCompleted is emitted.
            m_result.scan.rootDirectory = m_directory;
        } else {
            m_result.scan = DirectoryScanner::scan(m_directory, m_options,
                                                  [this] { return cancellationRequested(); });
        }
        if (!removal && m_hooks && m_hooks->afterScan)
            m_hooks->afterScan(m_result.scan);
        reached(LibraryImportService::TestPoint::AfterScan);
        if (!removal)
            emit scanReady(m_result.taskId, m_result.scan);

        if (cancellationRequested() || m_result.scan.status == DirectoryScanStatus::Cancelled) {
            m_result.state = LibraryImportState::Cancelled;
            return;
        }
        if (!removal) {
            const auto validation = LibraryRepository::validateScan(m_result.scan, m_displayName);
            if (!validation.success && (m_kind == LibraryImportService::TaskKind::Import
                || !m_result.scan.isComplete())) {
                m_result.repositoryResult.diagnostics = validation.diagnostics;
                m_result.errorMessage = QStringLiteral("The directory scan is not eligible for import.");
                return;
            }
        }

        reached(LibraryImportService::TestPoint::BeforeCommit);
        if (m_kind == LibraryImportService::TaskKind::Removal && !verifyMissingDirectory())
            return;
        auto expected = LibraryImportService::TaskPhase::Scanning;
        // Media commit and association finish together. A remembered stop can
        // skip the subsequent, separately cancellable reference preparation.
        if (!m_control->phase.compare_exchange_strong(
                expected, LibraryImportService::TaskPhase::Committing)) {
            m_result.state = LibraryImportState::Cancelled;
            return;
        }
        emit committing(m_result.taskId);
        reached(LibraryImportService::TestPoint::Committing);
        {
            // Construction, SQL connection creation, import, close and
            // destruction all occur here, on this worker thread.
            LibraryRepository repository(m_databasePath);
            const auto opened = repository.open();
            if (!opened.success) {
                m_result.repositoryResult.diagnostics = opened.diagnostics;
                m_result.errorMessage = QStringLiteral("Could not open the library database.");
            } else {
                reached(LibraryImportService::TestPoint::RepositoryOpened);
                if (m_kind == LibraryImportService::TaskKind::Import) {
                    m_result.repositoryResult = repository.importScan(m_result.scan, m_displayName);
                } else if (m_kind == LibraryImportService::TaskKind::Refresh) {
                    m_result.repositoryResult = repository.refreshExistingWork(m_expected, m_result.scan);
                } else if (m_kind == LibraryImportService::TaskKind::Removal) {
                    reached(LibraryImportService::TestPoint::RemovalBeforeLock);
                    m_result.repositoryResult = repository.removeMissingWork(m_expected, [this]() {
                        reached(LibraryImportService::TestPoint::RemovalLocked);
                        if (verifyMissingDirectory() && missingPathStillConfirmed())
                            return LibraryOperationResult{true, {}};
                        return LibraryOperationResult{false, {{LibraryDiagnosticCode::RejectedScan,
                            m_directory, QStringLiteral("Directory availability changed before committing work removal.")}}};
                    });
                } else {
                    reached(LibraryImportService::TestPoint::RemovalBeforeLock);
                    m_result.repositoryResult = repository.removeWork(m_expected, [this]() {
                        reached(LibraryImportService::TestPoint::RemovalLocked);
                        return LibraryOperationResult{true, {}};
                    });
                }
                m_result.databaseCommitted = m_result.repositoryResult.success;
                m_result.state = m_result.databaseCommitted ? LibraryImportState::Completed
                                                           : LibraryImportState::Failed;
                if (!m_result.databaseCommitted) {
                    m_result.errorMessage = QStringLiteral("The library import transaction failed.");
                } else {
                    emit importReady(m_result.taskId, m_result.repositoryResult);
                    reached(LibraryImportService::TestPoint::ImportCommitted);
                    if (!removal) {
                        // A committed, complete scan is the only scan result
                        // allowed to invalidate a previously published translation.
                        // Cleanup uses the recorded result path, not a missing
                        // scan entry, and retains unfinished translation work.
                        if (m_result.scan.isComplete()) {
                            const auto cleanup = cleanupMissingSubtitleTranslationResults(m_databasePath, m_directory);
                            if (!cleanup.succeeded())
                                m_result.repositoryResult.diagnostics.push_back(
                                    {LibraryDiagnosticCode::DatabaseError, m_databasePath,
                                     QStringLiteral("Translation result cleanup failed after the library scan committed: ")
                                         + cleanup.error});
                        }
                        associateSubtitles(repository);
                        processReferences(repository);
                    }
                }
            }
            const auto closed = repository.close();
            m_result.repositoryResult.diagnostics += closed.diagnostics;
        }
    }

    QString m_databasePath;
    QString m_directory;
    DirectoryScanOptions m_options;
    QString m_displayName;
    LibraryImportService::TaskKind m_kind;
    qint64 m_workId = 0;
    QString m_scanRoot;
    WorkReferenceRefreshPolicy m_referencePolicy;
    bool m_referencePhaseStarted = false;
    LibraryWorkSnapshotResult m_expected;
    std::shared_ptr<LibraryImportService::TaskControl> m_control;
    std::shared_ptr<const LibraryImportService::TestHooks> m_hooks;
    LibraryImportTaskResult m_result;
};

LibraryImportService::LibraryImportService(QString databasePath, QObject *parent)
    : QObject(parent), m_databasePath(std::move(databasePath))
{
    qRegisterMetaType<DirectoryScanResult>();
    qRegisterMetaType<LibraryImportState>();
    qRegisterMetaType<LibraryImportResult>();
    qRegisterMetaType<LibraryImportTaskResult>();
}

LibraryImportService::~LibraryImportService()
{
    // Like every QObject owner, destruction must occur on the object's thread.
    // The worker holds values/control only, never a pointer back to this object.
    if (m_thread) {
        requestCancellation();
        m_thread->wait();
        delete m_thread;
    }
}

LibraryImportSubmission LibraryImportService::submit(
    const QString &directory, const DirectoryScanOptions &options, const QString &displayName)
{
    return submitTask(TaskKind::Import, 0, directory, options, displayName, {});
}

LibraryImportSubmission LibraryImportService::submitRefresh(
    qint64 workId, const QString &directory, const DirectoryScanOptions &options,
    WorkReferenceRefreshPolicy references)
{
    return submitTask(TaskKind::Refresh, workId, directory, options, {}, {}, references);
}

LibraryImportSubmission LibraryImportService::submitRemoval(
    qint64 workId, const QString &directory, const QString &scanRoot)
{
    return submitTask(TaskKind::Removal, workId, directory, {}, {}, scanRoot);
}

LibraryImportSubmission LibraryImportService::submitManualRemoval(qint64 workId, const QString &directory)
{
    return submitTask(TaskKind::ManualRemoval, workId, directory, {}, {}, {});
}

LibraryImportSubmission LibraryImportService::submitTask(
    TaskKind kind, qint64 workId, const QString &directory,
    const DirectoryScanOptions &options, const QString &displayName, const QString &scanRoot,
    WorkReferenceRefreshPolicy references)
{
    if (QThread::currentThread() != thread())
        return {LibraryImportSubmissionStatus::WrongThread, {}};
    if (m_shuttingDown)
        return {LibraryImportSubmissionStatus::ShuttingDown, {}};
    if (isBusy())
        return {LibraryImportSubmissionStatus::Busy, {}};

    const LibraryImportSubmission accepted{
        LibraryImportSubmissionStatus::Accepted,
        QUuid::createUuid().toString(QUuid::WithoutBraces)};
    m_activeTaskId = accepted.taskId;
    m_control = std::make_shared<TaskControl>();
    m_thread = new LibraryImportThread(accepted.taskId, m_databasePath, directory,
                                      options, displayName, kind, workId, scanRoot, references,
                                      m_control, m_testHooks, this);
    connect(m_thread, &LibraryImportThread::scanReady, this,
            &LibraryImportService::applyScanCompleted, Qt::QueuedConnection);
    connect(m_thread, &LibraryImportThread::committing, this,
            &LibraryImportService::applyCommitting, Qt::QueuedConnection);
    connect(m_thread, &LibraryImportThread::processingReferences, this,
            &LibraryImportService::applyProcessingReferences, Qt::QueuedConnection);
    connect(m_thread, &LibraryImportThread::importReady, this,
            &LibraryImportService::applyImportCommitted, Qt::QueuedConnection);
    const auto worker = m_thread;
    connect(worker, &QThread::finished, this,
            [this, id = accepted.taskId, worker] { finishTask(id, worker); },
            Qt::QueuedConnection);
    m_state = LibraryImportState::Scanning;
    m_thread->start();
    emit stateChanged(accepted.taskId, LibraryImportState::Scanning);
    // An observer may cancel, shut down or even delete the service in the
    // signal handler. Do not access members after notifying it.
    return accepted;
}

LibraryImportCancelResult LibraryImportService::cancelCurrent()
{
    if (QThread::currentThread() != thread())
        return LibraryImportCancelResult::WrongThread;
    if (!m_thread)
        return LibraryImportCancelResult::NoActiveTask;

    const auto result = requestCancellation();
    if (result == LibraryImportCancelResult::Requested) {
        m_state = LibraryImportState::Cancelling;
        const QString taskId = m_activeTaskId;
        emit stateChanged(taskId, LibraryImportState::Cancelling);
    }
    return result;
}

LibraryImportCancelResult LibraryImportService::requestCancellation()
{
    auto phase = m_control->phase.load();
    for (;;) {
        if (phase == TaskPhase::Scanning || phase == TaskPhase::ProcessingReferences) {
            if (!m_control->phase.compare_exchange_weak(phase, TaskPhase::CancelRequested)) continue;
            m_control->referencesCancelled.store(true);
            return LibraryImportCancelResult::Requested;
        }
        if (phase == TaskPhase::Committing) {
            // Media commit/association is indivisible. Remember the stop request
            // so the later optional reference phase is not started.
            if (!m_control->phase.compare_exchange_weak(phase, TaskPhase::CommittingStopRequested)) continue;
            return LibraryImportCancelResult::TooLate;
        }
        if (phase == TaskPhase::CancelRequested) return LibraryImportCancelResult::AlreadyRequested;
        return LibraryImportCancelResult::TooLate;
    }
}

bool LibraryImportService::shutdown()
{
    if (QThread::currentThread() != thread())
        return false;
    m_shuttingDown = true;
    if (m_thread) {
        cancelCurrent();
    } else {
        QMetaObject::invokeMethod(this, &LibraryImportService::finishShutdownIfReady,
                                  Qt::QueuedConnection);
    }
    return true;
}

void LibraryImportService::applyScanCompleted(
    const QString &taskId, const DirectoryScanResult &scan)
{
    if (taskId != m_activeTaskId || !m_thread || m_finalizing)
        return;
    emit scanCompleted(taskId, scan);
}

void LibraryImportService::applyCommitting(const QString &taskId)
{
    if (taskId != m_activeTaskId || !m_thread || m_finalizing
        || (m_state != LibraryImportState::Scanning && m_state != LibraryImportState::ProcessingReferences))
        return;
    m_state = LibraryImportState::Committing;
    emit stateChanged(taskId, LibraryImportState::Committing);
}

void LibraryImportService::applyProcessingReferences(const QString &taskId)
{
    if (taskId != m_activeTaskId || !m_thread || m_finalizing
        || m_state == LibraryImportState::Cancelling) return;
    m_state = LibraryImportState::ProcessingReferences;
    emit stateChanged(taskId, LibraryImportState::ProcessingReferences);
}

void LibraryImportService::applyImportCommitted(const QString &taskId, const LibraryImportResult &result)
{
    if (taskId != m_activeTaskId || !m_thread || m_finalizing || !result.success)
        return;
    emit importCommitted(taskId, result);
}

void LibraryImportService::finishTask(const QString &taskId, LibraryImportThread *worker)
{
    if (taskId != m_activeTaskId || !worker || worker != m_thread)
        return;
    // finished() precedes possible thread-local cleanup. Join that final cleanup
    // before releasing the thread or permitting the next task; run() is over
    // and no owner event-loop action is needed to make this wait complete.
    worker->wait();
    const auto result = worker->takeResult();
    m_thread = nullptr;
    m_control.reset();
    m_activeTaskId.clear();
    delete worker;
    m_finalizing = true;
    m_state = result.state;
    QPointer<LibraryImportService> guard(this);
    emit stateChanged(taskId, result.state);
    if (!guard)
        return;
    m_finalizing = false;
    emit taskFinished(result);
    if (!guard)
        return;
    // taskFinished may have submitted a new task. Never rewrite its state or
    // publish an old idle notification after that callback.
    finishShutdownIfReady();
}

void LibraryImportService::finishShutdownIfReady()
{
    if (!m_shuttingDown || m_shutdownNotified || isBusy())
        return;
    m_shutdownNotified = true;
    emit shutdownFinished();
}

} // namespace asmr

#include "LibraryImportService.moc"
