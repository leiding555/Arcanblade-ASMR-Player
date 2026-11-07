#pragma once

#include "library/LibraryRepository.h"

#include <QStringList>

#include <functional>

namespace asmr {

struct WorkDirectoryCandidate {
    QString directory;
    QString name;
    qint64 existingWorkId = 0;
    QVector<LibraryWork> overlappingWorks;

    [[nodiscard]] bool defaultSelected() const { return overlappingWorks.isEmpty(); }
};

struct WorkDirectoryDiscoveryResult {
    QString rootDirectory;
    bool success = false;
    bool cancelled = false;
    QVector<WorkDirectoryCandidate> candidates;
    qsizetype rootFilesSkipped = 0;
    qsizetype linksSkipped = 0;
    QStringList diagnostics;
    qsizetype suppressedDiagnostics = 0;
    QString errorMessage;
};

struct WorkDirectoryDiscoveryOptions {
    qsizetype maximumEntries = 100'000;
    qsizetype maximumDiagnostics = 200;
};

class WorkDirectoryDiscovery final {
public:
    // Enumerates only direct real child directories. Call on a worker thread;
    // the callback must not throw or access GUI objects. Failed/cancelled
    // enumerations never expose a partial importable candidate list.
    static WorkDirectoryDiscoveryResult discover(
        const QString &root, const QVector<LibraryWork> &knownWorks,
        const WorkDirectoryDiscoveryOptions &options = {},
        const std::function<bool()> &cancellationRequested = {});
};

} // namespace asmr