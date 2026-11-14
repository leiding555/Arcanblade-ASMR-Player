#include "ui/MainWindow.h"
#include "ui/PlayerMessageBox.h"

#include "playback/AudioPlayer.h"
#include "ui/SubtitlePanel.h"

#include <QFileInfo>
#include <QFutureWatcher>
#include <QPromise>
#include <QPointer>
#include <QThreadPool>

#include <exception>

namespace asmr {
namespace {
QString issueText(const LibrarySubtitleResolution &result)
{
    QStringList lines;
    for (const auto &issue : result.issues)
        lines.append(QStringLiteral("[字幕 ID %1] %2：%3")
                         .arg(issue.subtitleFileId).arg(issue.path, issue.message));
    return lines.join(QLatin1Char('\n'));
}
SubtitleFormat subtitleFormat(const QString &path, bool &supported)
{
    const auto suffix = QFileInfo(path).suffix().toLower();
    supported = true;
    if (suffix == QStringLiteral("srt")) return SubtitleFormat::Srt;
    if (suffix == QStringLiteral("vtt")) return SubtitleFormat::Vtt;
    if (suffix == QStringLiteral("lrc")) return SubtitleFormat::Lrc;
    supported = false;
    return SubtitleFormat::Srt;
}
}

bool MainWindow::isCurrentSubtitleRequest(quint64 audioGeneration, quint64 requestGeneration,
                                         qint64 workId, qint64 fileId) const
{
    return !m_closing && audioGeneration == m_audioGeneration
        && requestGeneration == m_subtitleRequestGeneration
        && workId == m_playingLibraryWorkId && fileId == m_playingLibraryFileId;
}

void MainWindow::requestLibrarySubtitles()
{
    if (m_closing || m_manualSubtitleChoice || !m_playingLibraryWorkId || !m_playingLibraryFileId)
        return;
    m_rootSubtitleWaitAudioGeneration = 0;
    m_subtitlePanel->setSeekEnabled(false);
    ++m_subtitleRequestGeneration;
    m_subtitleLoadPending = false;
    m_subtitleQueryPending = false;
    m_subtitlePanel->setLoading(false);
    if (hasPendingSubtitlePreference(m_playingLibraryWorkId, m_playingLibraryFileId)) {
        setLibrarySubtitleStatus(QStringLiteral("此音轨的字幕设置尚未保存完成，等待后台写入。"));
        return;
    }
    if (m_libraryAssociationWorkId == m_playingLibraryWorkId) {
        if (!m_libraryRootChildTaskId.isEmpty()) {
            m_rootSubtitleWaitAudioGeneration = m_audioGeneration;
            m_rootSubtitleWaitRequestGeneration = m_subtitleRequestGeneration;
            m_rootSubtitleWaitWorkId = m_playingLibraryWorkId;
            m_rootSubtitleWaitFileId = m_playingLibraryFileId;
        }
        setLibrarySubtitleStatus(QStringLiteral("作品已导入，字幕关联尚未就绪；等待关联完成。"));
        return;
    }
    setLibrarySubtitleStatus(QStringLiteral("正在查询当前音轨的字幕设置…"));
    m_subtitleQueryPending = true;
    if (!m_subtitleQueryInProgress)
        startLibrarySubtitleQuery();
}

void MainWindow::startLibrarySubtitleQuery()
{
    m_subtitleQueryInProgress = true;
    m_subtitleQueryPending = false;
    const auto audio = m_audioGeneration;
    const auto request = m_subtitleRequestGeneration;
    const auto workId = m_playingLibraryWorkId;
    const auto fileId = m_playingLibraryFileId;
    const auto path = m_libraryDatabasePath;
    const auto hook = m_subtitleTaskTestHook;
    auto promise = std::make_shared<QPromise<LibrarySubtitleResolution>>();
    auto *watcher = new QFutureWatcher<LibrarySubtitleResolution>(this);
    connect(watcher, &QFutureWatcher<LibrarySubtitleResolution>::finished, this,
            [this, watcher, audio, request, workId, fileId] {
        auto result = watcher->result();
        watcher->deleteLater();
        m_subtitleQueryInProgress = false;
        if (!m_closing && result.pruned)
            refreshLibrary();
        if (isCurrentSubtitleRequest(audio, request, workId, fileId) && !m_manualSubtitleChoice) {
            m_librarySubtitleResolution = std::move(result);
            const auto &resolved = m_librarySubtitleResolution;
            switch (resolved.decision) {
            case LibrarySubtitleDecision::Selected:
                m_requestedSubtitleLoad = {resolved.selectedFile.relativePath, resolved.snapshot.work.rootPath,
                    resolved.selectedFile, audio, request, workId, fileId, true};
                m_subtitleLoadPending = true;
                setLibrarySubtitleStatus(QStringLiteral("正在自动加载已关联字幕…"));
                if (!m_subtitleLoadInProgress)
                    startSubtitleLoad();
                break;
            case LibrarySubtitleDecision::NeedsSelection:
                clearSubtitles();
                setLibrarySubtitleStatus(QStringLiteral("当前音轨有 %1 份可用字幕，等待选择；可使用“选择字幕”。")
                    .arg(resolved.availableSubtitleIds.size()));
                break;
            case LibrarySubtitleDecision::Disabled:
                clearSubtitles();
                setLibrarySubtitleStatus(QStringLiteral("此音轨已明确设置为不使用字幕。"));
                break;
            case LibrarySubtitleDecision::NoAvailable:
                clearSubtitles();
                setLibrarySubtitleStatus(QStringLiteral("此音轨没有可用的已绑定字幕。")
                    + (resolved.issues.isEmpty() ? QString() : QLatin1Char('\n') + issueText(resolved)));
                break;
            case LibrarySubtitleDecision::Error:
                clearSubtitles();
                m_subtitlePanel->showLoadFailure();
                setLibrarySubtitleStatus(QStringLiteral("字幕设置查询或首选字幕检查失败，不改选其他字幕。\n")
                                                  + issueText(resolved));
                break;
            }
        }
        if (m_closing)
            finishCloseIfReady();
        else if (m_subtitleQueryPending)
            startLibrarySubtitleQuery();
        updateFloatingPlaybackControls();
    });
    watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([promise, path, workId, fileId, hook] {
        promise->start();
        LibrarySubtitleResolution result;
        try {
            if (hook) (*hook)(SubtitleTestPoint::BeforeQuery);
            result = resolveLibrarySubtitles(path, workId, fileId);
            if (hook) (*hook)(SubtitleTestPoint::AfterQuery);
        } catch (const std::exception &exception) {
            result.decision = LibrarySubtitleDecision::Error;
            result.issues.push_back({LibrarySubtitleIssueCode::Database, 0, path,
                                     QString::fromUtf8(exception.what())});
        } catch (...) {
            result.decision = LibrarySubtitleDecision::Error;
            result.issues.push_back({LibrarySubtitleIssueCode::Database, 0, path,
                                     QStringLiteral("字幕设置查询发生未知错误。")});
        }
        promise->addResult(std::move(result));
        promise->finish();
    });
}

void MainWindow::requestManualSubtitle(const QString &path)
{
    if (m_closing || !m_audioGeneration || path.isEmpty())
        return;
    m_manualSubtitleChoice = true;
    m_subtitlePreferenceFeedback.clear();
    m_subtitlePreferenceFeedbackError = false;
    if (m_subtitleErrorDialog)
        m_subtitleErrorDialog->hide();
    m_rootSubtitleWaitAudioGeneration = 0;
    m_subtitlePanel->setSeekEnabled(false);
    ++m_subtitleRequestGeneration;
    m_subtitleQueryPending = false;
    m_subtitleLoadPending = false;
    setLibrarySubtitleStatus({});
    bool supported = false;
    subtitleFormat(path, supported);
    if (!supported) {
        m_subtitlePanel->showLoadFailure();
        updateFloatingPlaybackControls();
        showSubtitleLoadError(QStringLiteral("不支持此字幕文件扩展名，请选择 .srt、.vtt 或 .lrc 文件。"));
        return;
    }
    m_requestedSubtitleLoad = {path, {}, {}, m_audioGeneration, m_subtitleRequestGeneration,
                              m_playingLibraryWorkId, m_playingLibraryFileId, false};
    m_subtitleLoadPending = true;
    m_subtitlePanel->setLoading(true);
    if (!m_subtitleLoadInProgress)
        startSubtitleLoad();
}

void MainWindow::startSubtitleLoad()
{
    m_subtitleLoadInProgress = true;
    m_subtitleLoadPending = false;
    const auto load = m_requestedSubtitleLoad;
    const auto hook = m_subtitleTaskTestHook;
    auto promise = std::make_shared<QPromise<SubtitleFileLoadResult>>();
    auto *watcher = new QFutureWatcher<SubtitleFileLoadResult>(this);
    connect(watcher, &QFutureWatcher<SubtitleFileLoadResult>::finished, this, [this, watcher, load] {
        const auto result = watcher->result();
        watcher->deleteLater();
        m_subtitleLoadInProgress = false;
        QPointer<MainWindow> guard(this);
        const bool currentLoad = load.automatic
            ? isCurrentSubtitleRequest(load.audioGeneration, load.requestGeneration, load.workId, load.audioFileId)
            : !m_closing && load.audioGeneration == m_audioGeneration
                && load.requestGeneration == m_subtitleRequestGeneration;
        if (currentLoad && (!load.automatic || !m_manualSubtitleChoice)) {
            m_subtitlePanel->setLoading(false);
            m_subtitlePanel->setImportEnabled(m_audioPlayer->error() == QMediaPlayer::NoError);
            if (load.automatic) {
                m_lastLibrarySubtitleLoad = result;
                if (result.succeeded()) {
                    setLibrarySubtitleStatus(QStringLiteral("已自动加载绑定字幕（文件 ID %1）。").arg(load.file.id));
                    applySubtitleLoad(load.path, result, load.audioGeneration);
                } else {
                    clearSubtitles();
                    m_subtitlePanel->showLoadFailure();
                    setLibrarySubtitleStatus(QStringLiteral("自动字幕加载失败，不改选其他字幕：%1").arg(result.errorMessage));
                }
            } else {
                applySubtitleLoad(load.path, result, load.audioGeneration);
            }
        }
        if (!guard)
            return;
        if (m_closing)
            finishCloseIfReady();
        else if (m_subtitleLoadPending)
            startSubtitleLoad();
        updateFloatingPlaybackControls();
    });
    watcher->setFuture(promise->future());
    QThreadPool::globalInstance()->start([promise, load, hook] {
        promise->start();
        SubtitleFileLoadResult result;
        try {
            if (hook) (*hook)(SubtitleTestPoint::BeforeLoad);
            bool supported = false;
            const auto format = subtitleFormat(load.path, supported);
            result = load.automatic ? loadLibrarySubtitle(load.root, load.file)
                                   : SubtitleFileLoader::load(load.path, format);
            if (hook) (*hook)(SubtitleTestPoint::AfterLoad);
        } catch (const std::exception &exception) {
            result = {SubtitleFileError::ReadFailed, QString::fromUtf8(exception.what()), {}, {}};
        } catch (...) {
            result = {SubtitleFileError::ReadFailed, QStringLiteral("字幕加载发生未知错误。"), {}, {}};
        }
        promise->addResult(std::move(result));
        promise->finish();
    });
}
} // namespace asmr
