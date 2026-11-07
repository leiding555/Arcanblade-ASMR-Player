#include "ui/MainWindow.h"

#include "playback/AudioPlayer.h"
#include "library/SubtitleMatcher.h"
#include "ui/BoundSubtitleDialog.h"
#include "ui/ChickenRiceTranslateDialog.h"
#include "ui/SubtitlePanel.h"

#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QHash>
#include <QPromise>
#include <QSet>
#include <QThreadPool>

#include <algorithm>
#include <exception>
#include <utility>

namespace asmr {
namespace {
struct SubtitleChoices {
    LibraryTrackSubtitlesResult snapshot;
    QVector<BoundSubtitleChoiceItem> items;
    QVector<qint64> availableIds;
    QHash<qint64, QString> directories;
    bool pruned = false;
};
QString diagnosticText(const LibraryOperationResult &result)
{
    QStringList lines;
    for (const auto &diagnostic : result.diagnostics)
        lines.append(diagnostic.message);
    return lines.join(QLatin1Char('\n'));
}
SubtitleChoices readChoices(const QString &path, qint64 workId, qint64 audioId)
{
    SubtitleChoices result;
    LibraryRepository repository(path);
    const auto opened = repository.open();
    if (opened.success) {
        result.snapshot = repository.trackSubtitles(workId, audioId);
        if (result.snapshot.success) {
            const auto previousFiles = result.snapshot.files;
            LibraryWorkSnapshotResult expected;
            expected.success = true; expected.work = result.snapshot.work; expected.files = result.snapshot.files;
            const auto pruned = repository.pruneMissingSubtitles(expected);
            if (pruned.success) {
                result.snapshot = repository.trackSubtitles(workId, audioId);
                if (result.snapshot.success) {
                    QSet<qint64> currentIds;
                    for (const auto &file : result.snapshot.files) currentIds.insert(file.id);
                    result.pruned = std::any_of(previousFiles.cbegin(), previousFiles.cend(), [&](const auto &file) {
                        return file.type == LibraryFileType::Subtitle && !currentIds.contains(file.id);
                    });
                }
            } else {
                result.snapshot.success = false;
                result.snapshot.diagnostics += pruned.diagnostics;
            }
        }
    } else
        result.snapshot.diagnostics = opened.diagnostics;
    const auto closed = repository.close();
    if (!closed.success) {
        result.snapshot.success = false;
        result.snapshot.diagnostics += closed.diagnostics;
    }
    if (!result.snapshot.success)
        return result;
    // Matching needs the complete committed work, including competing audio IDs.
    // No media/subtitle content is read by the matcher.
    const auto matches = SubtitleMatcher::match(result.snapshot.work, result.snapshot.files);
    QHash<qint64, SubtitleMatchCandidate> candidates;
    QHash<qint64, qsizetype> candidateOrder;
    for (const auto &track : matches.tracks) {
        if (track.audioFileId == audioId) {
            for (const auto &candidate : track.candidates) {
                candidateOrder.insert(candidate.subtitleFileId, candidateOrder.size());
                candidates.insert(candidate.subtitleFileId, candidate);
            }
            break;
        }
    }
    QHash<qint64, SubtitleBindingSource> bound;
    for (const auto &binding : result.snapshot.bindings) {
        if (binding.workId != workId || binding.audioFileId != audioId) {
            result.snapshot.success = false;
            result.snapshot.diagnostics.push_back({LibraryDiagnosticCode::InvalidSubtitleBinding, path,
                QStringLiteral("已绑定字幕的身份与当前音轨不一致。")});
            return result;
        }
        bound.insert(binding.subtitleFileId, binding.source);
    }
    for (const auto &file : result.snapshot.files) {
        if (file.workId != workId || file.type != LibraryFileType::Subtitle)
            continue;
        const auto checked = validateLibrarySubtitlePath(result.snapshot.work.rootPath, file);
        const bool available = checked.success && !result.snapshot.audio.missing;
        BoundSubtitleChoiceItem item{file, bound.value(file.id), available,
            result.snapshot.audio.missing ? QStringLiteral("音轨已标记缺失")
                                          : checked.success ? QString() : checked.issue.message};
        if (checked.success) {
            item.generation = readGeneratedSubtitleInfo(checked.path);
            result.directories.insert(file.id, QFileInfo(checked.path).absolutePath());
        }
        item.order = librarySubtitleOrderKey(checked.path, file, item.generation);
        item.kind = bound.contains(file.id) ? BoundSubtitleChoiceKind::Bound : BoundSubtitleChoiceKind::Other;
        const auto candidate = candidates.constFind(file.id);
        if (candidate != candidates.cend()) {
            QString rule;
            switch (candidate->rule) {
            case SubtitleMatchRule::ExactStem: rule = QStringLiteral("完整主文件名一致"); break;
            case SubtitleMatchRule::NormalizedStem: rule = QStringLiteral("Unicode/大小写规范化主文件名一致"); break;
            case SubtitleMatchRule::LanguageSuffixStem: rule = QStringLiteral("去除明确文件名语言后缀后主文件名一致"); break;
            case SubtitleMatchRule::MediaExtensionStem:
                rule = QStringLiteral("去除保留的音频扩展名及可选语言后缀后主文件名一致"); break;
            case SubtitleMatchRule::TrackNumberOnly: rule = QStringLiteral("仅编号相同（弱依据，需人工核实）"); break;
            }
            item.matchReason = rule + QStringLiteral("；规则分 %1；目录距离 %2")
                .arg(candidate->ruleScore).arg(candidate->directoryDistance);
            if (candidate->numberOnly)
                item.matchReason += QStringLiteral("；纯编号，需人工核实");
            if (candidate->ambiguousVersion)
                item.matchReason += QStringLiteral("；版本歧义");
            if (!candidate->competingAudioFileIds.isEmpty())
                item.matchReason += QStringLiteral("；另有 %1 首竞争音轨").arg(candidate->competingAudioFileIds.size());
            if (!candidate->language.isEmpty())
                item.matchReason += QStringLiteral("；文件名标记 %1（实际语言未验证）").arg(candidate->language);
            if (!bound.contains(file.id) && candidate->recommended)
                item.kind = BoundSubtitleChoiceKind::Recommended;
        } else {
            item.matchReason = QStringLiteral("无规则匹配；可按内容自行确认关联");
        }
        result.items.push_back(std::move(item));
    }
    // The bound group shares the playback ranking. Recommendations retain
    // the matcher's priority/distance ordering.
    std::stable_sort(result.items.begin(), result.items.end(), [&candidateOrder](const auto &left, const auto &right) {
        if (left.kind != right.kind)
            return static_cast<int>(left.kind) < static_cast<int>(right.kind);
        if (left.kind == BoundSubtitleChoiceKind::Bound)
            return librarySubtitleNewer(left.order, right.order);
        return left.kind == BoundSubtitleChoiceKind::Recommended
            && candidateOrder.value(left.file.id) < candidateOrder.value(right.file.id);
    });
    for (const auto &item : result.items)
        if (item.available)
            result.availableIds.push_back(item.file.id);
    return result;
}
}

void MainWindow::setLibrarySubtitleStatus(const QString &message)
{
    m_subtitlePanel->setLibraryStatus(m_subtitlePreferenceFeedback.isEmpty() ? message
        : m_subtitlePreferenceFeedback + (message.isEmpty() ? QString() : QLatin1Char('\n') + message));
}

void MainWindow::resetBoundSubtitleContext()
{
    m_pendingSubtitleTranslationId = 0;
    m_pendingSubtitleFolderId = 0;
    ++m_subtitleChoiceQueryGeneration;
    m_subtitleChoiceQueryPending = false;
    m_subtitleChoiceSnapshot = {};
    m_availableSubtitleChoiceIds.clear();
    m_subtitleChoiceAudioGeneration = 0;
    if (m_subtitleImportDialog)
        m_subtitleImportDialog->reject();
    if (m_boundSubtitleDialog) {
        m_boundSubtitleDialog->hide();
        m_boundSubtitleDialog->setTrack(0, 0, {}, {}, {});
        m_boundSubtitleDialog->setQuerying(true);
        m_boundSubtitleDialog->setImportEnabled(false);
    }
}

void MainWindow::showBoundSubtitleDialog()
{
    if (m_closing || !m_audioGeneration
        || (!m_subtitlePanel->canSelectSubtitle() && !m_subtitlePanel->canOpenSubtitle()))
        return;
    if (m_boundSubtitleDialog && m_boundSubtitleDialog->isVisible()) {
        m_boundSubtitleDialog->raise();
        m_boundSubtitleDialog->activateWindow();
        updateSubtitleActions();
        return;
    }
    // A new dialog session must not replay actions requested before it was hidden.
    m_pendingSubtitleFolderId = 0;
    m_pendingSubtitleTranslationId = 0;
    if (!m_boundSubtitleDialog) {
        m_boundSubtitleDialog = new BoundSubtitleDialog(this);
        connect(m_boundSubtitleDialog, &BoundSubtitleDialog::importRequested,
                this, &MainWindow::openSubtitleFile);
        connect(m_boundSubtitleDialog, &BoundSubtitleDialog::preferenceRequested, this,
            [this](SubtitlePreferenceState state, qint64 fileId) {
                if (m_subtitleChoiceAudioGeneration != m_audioGeneration
                    || m_boundSubtitleDialog->workId() != m_playingLibraryWorkId
                    || m_boundSubtitleDialog->audioFileId() != m_playingLibraryFileId)
                    return;
                requestSubtitlePreference(state, fileId);
            });
        connect(m_boundSubtitleDialog, &BoundSubtitleDialog::bindingRequested, this,
            [this](qint64 fileId) {
                if (m_subtitleChoiceAudioGeneration == m_audioGeneration
                    && m_boundSubtitleDialog->workId() == m_playingLibraryWorkId
                    && m_boundSubtitleDialog->audioFileId() == m_playingLibraryFileId)
                    requestSubtitleBinding(fileId);
            });
        connect(m_boundSubtitleDialog, &BoundSubtitleDialog::unbindRequested, this,
            [this](qint64 fileId) {
                if (m_subtitleChoiceAudioGeneration == m_audioGeneration
                    && m_boundSubtitleDialog->workId() == m_playingLibraryWorkId
                    && m_boundSubtitleDialog->audioFileId() == m_playingLibraryFileId)
                    requestSubtitleBinding(fileId, true);
            });
        connect(m_boundSubtitleDialog, &BoundSubtitleDialog::translationRequested, this,
            [this](qint64 fileId) {
                if (m_closing || m_subtitleChoiceAudioGeneration != m_audioGeneration
                    || m_boundSubtitleDialog->workId() != m_playingLibraryWorkId
                    || m_boundSubtitleDialog->audioFileId() != m_playingLibraryFileId) return;
                m_pendingSubtitleTranslationId = fileId;
                refreshBoundSubtitleChoices();
            });
        connect(m_boundSubtitleDialog, &BoundSubtitleDialog::openFolderRequested, this,
            [this](qint64 fileId) {
                if (m_closing || fileId <= 0 || m_subtitleChoiceAudioGeneration != m_audioGeneration
                    || m_boundSubtitleDialog->workId() != m_playingLibraryWorkId
                    || m_boundSubtitleDialog->audioFileId() != m_playingLibraryFileId) return;
                m_pendingSubtitleFolderId = fileId;
                refreshBoundSubtitleChoices(); // Recheck database identity and canonical path on the worker.
            });
    }
    m_subtitleChoiceAudioGeneration = m_audioGeneration;
    m_boundSubtitleDialog->setTrack(m_playingLibraryWorkId, m_playingLibraryFileId,
        QFileInfo(m_audioPlayer->source().toLocalFile()).fileName(), {}, {});
    const bool libraryTrack = m_playingLibraryWorkId > 0 && m_playingLibraryFileId > 0;
    m_boundSubtitleDialog->setQuerying(libraryTrack);
    updateSubtitleActions();
    m_boundSubtitleDialog->show();
    m_boundSubtitleDialog->raise();
    m_boundSubtitleDialog->activateWindow();
    if (libraryTrack)
        refreshBoundSubtitleChoices();
}

void MainWindow::refreshBoundSubtitleChoices()
{
    if (m_closing || !m_boundSubtitleDialog || !m_boundSubtitleDialog->isVisible()
        || !m_playingLibraryWorkId || !m_playingLibraryFileId)
        return;
    ++m_subtitleChoiceQueryGeneration;
    m_subtitleChoiceQueryPending = true;
    m_boundSubtitleDialog->setQuerying(true);
    if (!m_subtitleChoiceQueryInProgress)
        startBoundSubtitleQuery();
}

void MainWindow::startBoundSubtitleQuery()
{
    m_subtitleChoiceQueryInProgress = true;
    m_subtitleChoiceQueryPending = false;
    const auto generation = m_subtitleChoiceQueryGeneration;
    const auto audio = m_audioGeneration;
    const auto workId = m_playingLibraryWorkId;
    const auto fileId = m_playingLibraryFileId;
    const auto path = m_libraryDatabasePath;
    const auto hook = m_preferenceTaskTestHook;
    auto promise = std::make_shared<QPromise<SubtitleChoices>>();
    auto *watcher = new QFutureWatcher<SubtitleChoices>(this);
    connect(watcher, &QFutureWatcher<SubtitleChoices>::finished, this,
            [this, watcher, generation, audio, workId, fileId] {
        auto result = watcher->result();
        watcher->deleteLater();
        m_subtitleChoiceQueryInProgress = false;
        if (!m_closing && result.pruned)
            refreshLibrary();
        if (!m_closing && generation == m_subtitleChoiceQueryGeneration && audio == m_audioGeneration
            && workId == m_playingLibraryWorkId && fileId == m_playingLibraryFileId) {
            m_subtitleChoiceSnapshot = result.snapshot;
            m_availableSubtitleChoiceIds = result.availableIds;
            if (result.snapshot.success) {
                m_boundSubtitleDialog->setTrack(workId, fileId, result.snapshot.audio.fileName,
                    result.snapshot.preference, result.items);
                if (!m_subtitlePreferenceFeedback.isEmpty())
                    m_boundSubtitleDialog->setFeedback(m_subtitlePreferenceFeedback,
                                                       m_subtitlePreferenceFeedbackError);
                if (result.pruned) {
                    if (!m_manualSubtitleChoice)
                        requestLibrarySubtitles();
                }
                if (m_pendingSubtitleTranslationId > 0) {
                    const auto subtitleId = std::exchange(m_pendingSubtitleTranslationId, qint64(0));
                    if (m_boundSubtitleDialog->isVisible()) {
                        const auto selected = std::find_if(result.items.cbegin(), result.items.cend(), [subtitleId](const auto &entry) {
                            return entry.file.id == subtitleId;
                        });
                        if (selected == result.items.cend() || !selected->available || selected->file.missing
                            || m_removedLibraryWorkIds.contains(workId)) {
                            m_boundSubtitleDialog->setFeedback(QStringLiteral("所选字幕已不可用，未加入翻译列表。"), true);
                        } else {
                            showChickenRiceTranslation();
                            const auto &work = result.snapshot.work;
                            const auto &track = result.snapshot.audio;
                            if (!m_chickenRiceTranslateDialog || !m_chickenRiceTranslateDialog->addSubtitleForTranslation(
                                QDir(work.rootPath).filePath(selected->file.relativePath),
                                QDir(work.rootPath).filePath(track.relativePath), work.rootPath, track.relativePath,
                                work.id, track.id, selected->file.id))
                                m_boundSubtitleDialog->setFeedback(QStringLiteral("AI 字幕窗口正忙，请稍后重试。"), true);
                        }
                    }
                }
                if (m_pendingSubtitleFolderId > 0) {
                    const auto subtitleId = std::exchange(m_pendingSubtitleFolderId, qint64(0));
                    const auto directory = result.directories.value(subtitleId);
                    if (m_boundSubtitleDialog->isVisible()) {
                        if (!result.availableIds.contains(subtitleId) || directory.isEmpty()
                            || m_removedLibraryWorkIds.contains(workId))
                            m_boundSubtitleDialog->setFeedback(QStringLiteral("所选字幕已不可用，无法打开所在文件夹。"), true);
                        else if (!QDesktopServices::openUrl(QUrl::fromLocalFile(directory)))
                            m_boundSubtitleDialog->setFeedback(QStringLiteral("无法打开字幕所在文件夹。"), true);
                    }
                }
            } else {
                m_pendingSubtitleTranslationId = 0;
                m_pendingSubtitleFolderId = 0;
                m_boundSubtitleDialog->setQuerying(true); // Keep invalid choices disabled.
                m_boundSubtitleDialog->setFeedback(QStringLiteral("读取失败：") + diagnosticText(result.snapshot), true);
            }
        }
        if (m_closing)
            finishCloseIfReady();
        else if (m_subtitleChoiceQueryPending)
            startBoundSubtitleQuery();
    });
    watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([promise, path, workId, fileId, hook] {
        promise->start();
        SubtitleChoices result;
        try {
            if (hook) (*hook)(PreferenceTestPoint::BeforeChoices);
            result = readChoices(path, workId, fileId);
            if (hook) (*hook)(PreferenceTestPoint::AfterChoices);
        } catch (const std::exception &exception) {
            result.snapshot.success = false;
            result.snapshot.diagnostics.push_back({LibraryDiagnosticCode::DatabaseError, path,
                                                   QString::fromUtf8(exception.what())});
        } catch (...) {
            result.snapshot.success = false;
            result.snapshot.diagnostics.push_back({LibraryDiagnosticCode::DatabaseError, path,
                                                   QStringLiteral("字幕选择查询发生未知错误。")});
        }
        promise->addResult(std::move(result));
        promise->finish();
    });
}

bool MainWindow::hasPendingSubtitlePreference(qint64 workId, qint64 audioFileId) const
{
    const auto matches = [workId, audioFileId](const SubtitlePreferenceRequest &request) {
        return request.workId == workId && request.audioFileId == audioFileId;
    };
    if (m_preferenceWriteInProgress && matches(m_activeSubtitlePreference))
        return true;
    return std::any_of(m_preferenceRequests.cbegin(), m_preferenceRequests.cend(), matches);
}

void MainWindow::requestSubtitlePreference(SubtitlePreferenceState state, qint64 subtitleFileId)
{
    const bool validState = state == SubtitlePreferenceState::Unset || state == SubtitlePreferenceState::None
        || state == SubtitlePreferenceState::Subtitle;
    if (m_closing || !m_playingLibraryFileId || !m_playingLibraryWorkId)
        return;
    if (!validState || !m_subtitleChoiceSnapshot.success
        || m_subtitleChoiceSnapshot.work.id != m_playingLibraryWorkId
        || m_subtitleChoiceSnapshot.audio.id != m_playingLibraryFileId
        || m_subtitleChoiceAudioGeneration != m_audioGeneration
        || (state == SubtitlePreferenceState::Subtitle
            && !m_availableSubtitleChoiceIds.contains(subtitleFileId))
        || (state != SubtitlePreferenceState::Subtitle && subtitleFileId != 0)) {
        m_subtitlePreferenceFeedback = QStringLiteral("选择无效：仅可使用当前作品中可用的字幕。");
        m_subtitlePreferenceFeedbackError = true;
        if (m_boundSubtitleDialog)
            m_boundSubtitleDialog->setFeedback(m_subtitlePreferenceFeedback, true);
        setLibrarySubtitleStatus({});
        return;
    }
    enqueueSubtitleChange(state, subtitleFileId, SubtitleChangeKind::Preference);
}

void MainWindow::requestSubtitleBinding(qint64 subtitleFileId, bool remove)
{
    if (m_closing || !m_playingLibraryFileId || !m_playingLibraryWorkId)
        return;
    const auto &snapshot = m_subtitleChoiceSnapshot;
    const auto file = std::find_if(snapshot.files.cbegin(), snapshot.files.cend(),
        [subtitleFileId](const auto &entry) { return entry.id == subtitleFileId; });
    const bool bound = std::any_of(snapshot.bindings.cbegin(), snapshot.bindings.cend(),
        [subtitleFileId](const auto &entry) { return entry.subtitleFileId == subtitleFileId; });
    if (!snapshot.success || snapshot.work.id != m_playingLibraryWorkId
        || snapshot.audio.id != m_playingLibraryFileId || m_subtitleChoiceAudioGeneration != m_audioGeneration
        || file == snapshot.files.cend() || file->workId != m_playingLibraryWorkId
        || file->type != LibraryFileType::Subtitle || (remove ? !bound : file->missing || snapshot.audio.missing)) {
        m_subtitlePreferenceFeedback = QStringLiteral("操作无效：仅可关联本作品中标记存在的字幕，或解除当前音轨的已有绑定。");
        m_subtitlePreferenceFeedbackError = true;
        if (m_boundSubtitleDialog)
            m_boundSubtitleDialog->setFeedback(m_subtitlePreferenceFeedback, true);
        setLibrarySubtitleStatus({});
        return;
    }
    enqueueSubtitleChange(SubtitlePreferenceState::Unset, subtitleFileId,
                          remove ? SubtitleChangeKind::Unbind : SubtitleChangeKind::BindManual);
}

void MainWindow::enqueueSubtitleChange(SubtitlePreferenceState state, qint64 subtitleFileId,
                                       SubtitleChangeKind kind)
{
    // A binding edit preserves a newer temporary subtitle. An owned pending
    // preference/edit carries its refresh intent through later FIFO operations.
    bool refreshPlayback = kind == SubtitleChangeKind::Preference || !m_manualSubtitleChoice;
    const auto ownsCurrent = [this](const SubtitlePreferenceRequest &pending) {
        return pending.workId == m_playingLibraryWorkId && pending.audioFileId == m_playingLibraryFileId
            && pending.audioGeneration == m_audioGeneration
            && pending.subtitleGeneration == m_subtitleRequestGeneration && pending.refreshPlayback;
    };
    if (!refreshPlayback) {
        refreshPlayback = (m_preferenceWriteInProgress && ownsCurrent(m_activeSubtitlePreference))
            || std::any_of(m_preferenceRequests.cbegin(), m_preferenceRequests.cend(), ownsCurrent);
    }
    ++m_preferenceOperationGeneration;
    if (refreshPlayback) {
        m_subtitlePanel->setSeekEnabled(false);
        ++m_subtitleRequestGeneration;
        m_subtitleQueryPending = false;
        m_subtitleLoadPending = false;
        m_manualSubtitleChoice = true;
        m_subtitlePanel->setLoading(false);
    }
    ++m_subtitleChoiceQueryGeneration;
    m_subtitleChoiceQueryPending = false;
    m_subtitlePreferenceFeedback = kind == SubtitleChangeKind::Preference ? QStringLiteral("正在保存首选决定…")
        : kind == SubtitleChangeKind::BindManual ? QStringLiteral("正在保存人工绑定…") : QStringLiteral("正在解除绑定…");
    m_subtitlePreferenceFeedbackError = false;
    if (m_boundSubtitleDialog)
        m_boundSubtitleDialog->setFeedback(m_subtitlePreferenceFeedback);
    if (kind == SubtitleChangeKind::Preference && state == SubtitlePreferenceState::None)
        clearSubtitles();
    setLibrarySubtitleStatus({});
    m_preferenceRequests.enqueue({m_playingLibraryWorkId, m_playingLibraryFileId, state, subtitleFileId,
        m_audioGeneration, m_subtitleRequestGeneration, m_preferenceOperationGeneration, kind, refreshPlayback});
    if (!m_preferenceWriteInProgress)
        startSubtitlePreferenceWrite();
}

LibraryOperationResult MainWindow::writeSubtitlePreference(
    const QString &path, const SubtitlePreferenceRequest &request)
{
    LibraryRepository repository(path);
    const auto opened = repository.open();
    if (!opened.success)
        return opened;
    const auto snapshot = repository.trackSubtitles(request.workId, request.audioFileId);
    if (!snapshot.success)
        return {false, snapshot.diagnostics};
    if (request.kind != SubtitleChangeKind::Preference) {
        const auto file = std::find_if(snapshot.files.cbegin(), snapshot.files.cend(),
            [&request](const auto &entry) { return entry.id == request.subtitleFileId; });
        if (file == snapshot.files.cend() || file->workId != request.workId
            || file->type != LibraryFileType::Subtitle)
            return {false, {{LibraryDiagnosticCode::InvalidSubtitleBinding, path,
                             QStringLiteral("字幕身份或作品归属无效，未修改绑定。")}}};
        const auto written = request.kind == SubtitleChangeKind::BindManual
            ? repository.bindSubtitle(request.audioFileId, request.subtitleFileId,
                SubtitleBindingSource::Manual, SubtitleBindingAvailabilityPolicy::RequirePresent)
            : repository.removeSubtitleBinding(request.audioFileId, request.subtitleFileId);
        auto result = written;
        result.diagnostics += repository.close().diagnostics;
        return result;
    }
    if (request.state == SubtitlePreferenceState::Subtitle) {
        const auto file = std::find_if(snapshot.files.cbegin(), snapshot.files.cend(),
            [&request](const auto &entry) { return entry.id == request.subtitleFileId; });
        if (file == snapshot.files.cend() || file->workId != request.workId
            || file->type != LibraryFileType::Subtitle
            || snapshot.audio.missing || !validateLibrarySubtitlePath(snapshot.work.rootPath, *file).success)
            return {false, {{LibraryDiagnosticCode::InvalidSubtitleBinding, path,
                             QStringLiteral("字幕已不可用或不属于当前作品，未保存首选。")}}};
    }
    LibraryOperationResult written;
    if (request.state == SubtitlePreferenceState::Subtitle)
        written = repository.useSubtitle(request.audioFileId, request.subtitleFileId);
    else if (request.state == SubtitlePreferenceState::None)
        written = repository.setNoSubtitlePreference(request.audioFileId);
    else
        written = repository.clearSubtitlePreference(request.audioFileId);
    const auto closed = repository.close();
    // A close error cannot undo or misreport a successfully committed transaction.
    written.diagnostics += closed.diagnostics;
    return written;
}

void MainWindow::startSubtitlePreferenceWrite()
{
    if (m_preferenceRequests.isEmpty())
        return;
    m_preferenceWriteInProgress = true;
    const auto request = m_preferenceRequests.dequeue();
    m_activeSubtitlePreference = request;
    const auto path = m_libraryDatabasePath;
    const auto hook = m_preferenceTaskTestHook;
    auto promise = std::make_shared<QPromise<LibraryOperationResult>>();
    auto *watcher = new QFutureWatcher<LibraryOperationResult>(this);
    connect(watcher, &QFutureWatcher<LibraryOperationResult>::finished, this,
            [this, watcher, request] {
        const auto result = watcher->result();
        watcher->deleteLater();
        m_preferenceWriteInProgress = false;
        m_lastSubtitlePreferenceWrite = result;
        if (result.success && (request.kind != SubtitleChangeKind::Preference
                              || request.state == SubtitlePreferenceState::Subtitle))
            refreshSubtitleBadges(request.workId);
        if (!result.success && !m_removedLibraryWorkIds.contains(request.workId)) {
            const auto message = QStringLiteral("字幕设置操作失败（作品 ID %1，音轨文件 ID %2，字幕文件 ID %3）：%4")
                .arg(request.workId).arg(request.audioFileId).arg(request.subtitleFileId)
                .arg(diagnosticText(result));
            qWarning().noquote() << message;
            if (m_closing) {
                m_subtitleCloseFailuresByWork[request.workId].append(message);
                m_subtitlePreferenceCloseFailure += (m_subtitlePreferenceCloseFailure.isEmpty()
                    ? QString() : QStringLiteral("\n")) + message;
            }
        }
        const bool samePlayback = !m_closing && request.audioGeneration == m_audioGeneration
            && request.workId == m_playingLibraryWorkId && request.audioFileId == m_playingLibraryFileId;
        if (samePlayback && request.operationGeneration == m_preferenceOperationGeneration) {
            const bool ownsRequest = request.subtitleGeneration == m_subtitleRequestGeneration;
            const auto failureText = request.kind == SubtitleChangeKind::Preference ? QStringLiteral("首选保存失败，数据库原设置保留：")
                : QStringLiteral("绑定操作失败，数据库原设置保留：");
            m_subtitlePreferenceFeedback = result.success ? QString() : failureText + diagnosticText(result);
            if (!result.success && request.kind == SubtitleChangeKind::Preference
                && request.state == SubtitlePreferenceState::None && ownsRequest)
                m_subtitlePreferenceFeedback += QStringLiteral("当前会话暂不显示字幕。");
            m_subtitlePreferenceFeedbackError = !result.success;
            setLibrarySubtitleStatus({});
            refreshBoundSubtitleChoices();
            if (ownsRequest && request.refreshPlayback
                && (result.success || request.kind != SubtitleChangeKind::Preference)) {
                m_manualSubtitleChoice = false;
                requestLibrarySubtitles();
            }
        } else if (!m_closing && !m_manualSubtitleChoice
                   && request.workId == m_playingLibraryWorkId && request.audioFileId == m_playingLibraryFileId) {
            // A prior A-session save completed after A->B->A. Success and
            // failure both release the new session's wait. A fresh read uses
            // the latest committed preference, never a prior session's load.
            if (result.success) {
                m_subtitlePreferenceFeedback = hasPendingSubtitlePreference(request.workId, request.audioFileId)
                    ? QStringLiteral("此音轨仍有字幕设置待保存…")
                    : QString();
                m_subtitlePreferenceFeedbackError = false;
            } else {
                m_subtitlePreferenceFeedback = QStringLiteral("此音轨先前的字幕设置保存失败，数据库原设置保留：")
                    + diagnosticText(result);
                m_subtitlePreferenceFeedbackError = true;
            }
            refreshBoundSubtitleChoices();
            requestLibrarySubtitles();
        }
        if (!m_preferenceRequests.isEmpty())
            startSubtitlePreferenceWrite(); // Even during close: drain every accepted edit in FIFO order.
        updateFloatingPlaybackControls();
        if (m_closing)
            finishCloseIfReady();
    });
    const auto future = promise->future();
    m_preferenceTaskLifetime = future;
    watcher->setFuture(future);
    QThreadPool::globalInstance()->start([promise, path, request, hook] {
        promise->start();
        LibraryOperationResult result;
        try {
            if (hook) (*hook)(PreferenceTestPoint::BeforeWrite);
            result = writeSubtitlePreference(path, request);
            if (hook) (*hook)(PreferenceTestPoint::AfterWrite);
        } catch (const std::exception &exception) {
            result.diagnostics.push_back({LibraryDiagnosticCode::DatabaseError, path,
                                          QString::fromUtf8(exception.what())});
        } catch (...) {
            result.diagnostics.push_back({LibraryDiagnosticCode::DatabaseError, path,
                                          QStringLiteral("字幕设置操作发生未知错误。")});
        }
        promise->addResult(std::move(result));
        promise->finish();
    });
}

void MainWindow::finishSubtitlePreferenceWrites()
{
    // Normal close drains asynchronously. A direct destructor is the last
    // safety net: wait for the running value-only task, then drain the queue
    // on a worker without relying on callbacks into this dying QWidget.
    if (m_preferenceWriteInProgress) {
        m_preferenceTaskLifetime.waitForFinished();
        const auto result = m_preferenceTaskLifetime.result();
        if (!result.success && !m_removedLibraryWorkIds.contains(m_activeSubtitlePreference.workId))
            qWarning().noquote() << QStringLiteral("Subtitle settings change failed during destruction: workId=%1 audioFileId=%2 subtitleFileId=%3; %4")
                .arg(m_activeSubtitlePreference.workId).arg(m_activeSubtitlePreference.audioFileId)
                .arg(m_activeSubtitlePreference.subtitleFileId).arg(diagnosticText(result));
    }
    if (m_preferenceRequests.isEmpty())
        return;
    auto pending = std::move(m_preferenceRequests);
    const auto path = m_libraryDatabasePath;
    auto promise = std::make_shared<QPromise<void>>();
    auto future = promise->future();
    QThreadPool::globalInstance()->start([promise, path, pending = std::move(pending)] {
        promise->start();
        for (const auto &request : pending) {
            try {
                const auto result = writeSubtitlePreference(path, request);
                if (!result.success)
                    qWarning().noquote() << QStringLiteral("Subtitle settings change failed during destruction: workId=%1 audioFileId=%2 subtitleFileId=%3; %4")
                        .arg(request.workId).arg(request.audioFileId).arg(request.subtitleFileId)
                        .arg(diagnosticText(result));
            } catch (...) {
                qWarning().noquote() << QStringLiteral("Subtitle settings change failed during destruction: workId=%1 audioFileId=%2 subtitleFileId=%3; unexpected failure.")
                    .arg(request.workId).arg(request.audioFileId).arg(request.subtitleFileId);
            }
        }
        promise->finish();
    });
    future.waitForFinished();
}
} // namespace asmr
