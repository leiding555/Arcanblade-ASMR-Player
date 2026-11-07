#include "ui/MainWindow.h"

#include "playback/TrackDurationService.h"
#include "ui/LibraryPanel.h"

#include <algorithm>

namespace asmr {

void MainWindow::connectTrackDurations()
{
    m_trackDurationService = new TrackDurationService(this);
    connect(m_libraryPanel, &LibraryPanel::trackDurationDemandChanged,
            this, &MainWindow::requestVisibleTrackDurations);
    connect(m_trackDurationService, &TrackDurationService::durationReady, this,
        [this](const TrackDurationResult &result) {
            const auto &request = result.request;
            if (m_closing || m_libraryQueryInProgress || m_libraryQueryPending
                || request.workId != m_librarySnapshotWorkId
                || request.workId != m_libraryPanel->selectedWorkId()
                || m_removedLibraryWorkIds.contains(request.workId)) return;
            const auto work = std::find_if(m_libraryWorksSnapshot.cbegin(), m_libraryWorksSnapshot.cend(),
                [&request](const auto &entry) {
                    return entry.id == request.workId && entry.rootPath == request.rootPath;
                });
            const auto file = std::find_if(m_libraryFilesSnapshot.cbegin(), m_libraryFilesSnapshot.cend(),
                [&request](const auto &entry) {
                    return entry.id == request.fileId && entry.workId == request.workId
                        && entry.relativePath == request.relativePath
                        && entry.type == LibraryFileType::Audio && !entry.missing;
                });
            if (work != m_libraryWorksSnapshot.cend() && file != m_libraryFilesSnapshot.cend())
                m_libraryPanel->setTrackDuration(request.workId, request.fileId, result.durationMs);
        });
    connect(m_trackDurationService, &TrackDurationService::shutdownFinished, this, [this] {
        m_durationShutdownFinished = true;
        finishCloseIfReady();
    });
    m_libraryPanel->requestTrackDurationDemand();
}

void MainWindow::requestVisibleTrackDurations(qint64 workId, const QVector<qint64> &fileIds)
{
    if (m_closing) return;
    QVector<TrackDurationRequest> requests;
    if (!m_libraryQueryInProgress && !m_libraryQueryPending && workId > 0
        && workId == m_librarySnapshotWorkId && workId == m_libraryPanel->selectedWorkId()
        && !m_removedLibraryWorkIds.contains(workId)) {
        const auto work = std::find_if(m_libraryWorksSnapshot.cbegin(), m_libraryWorksSnapshot.cend(),
            [workId](const auto &entry) { return entry.id == workId; });
        if (work != m_libraryWorksSnapshot.cend()) {
            QSet<qint64> wanted(fileIds.cbegin(), fileIds.cend());
            QHash<qint64, const LibraryFile *> files;
            for (const auto &file : m_libraryFilesSnapshot)
                if (wanted.contains(file.id) && file.workId == workId
                    && file.type == LibraryFileType::Audio && !file.missing)
                    files.insert(file.id, &file);
            for (const auto fileId : fileIds)
                if (const auto *file = files.value(fileId))
                    requests.append({workId, fileId, work->rootPath, file->relativePath});
        }
    }
    m_trackDurationService->setDemand(std::move(requests));
}

void MainWindow::invalidateTrackDurations(qint64 workId)
{
    if (m_closing || workId <= 0) return;
    m_trackDurationService->invalidateWork(workId);
    m_libraryPanel->clearTrackDurations(workId);
    m_libraryPanel->requestTrackDurationDemand();
}

} // namespace asmr
