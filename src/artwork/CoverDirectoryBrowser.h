#pragma once

#include <QString>
#include <QVector>

#include <atomic>
#include <memory>

namespace asmr {

enum class CoverDirectoryStatus { Ready, Error, LimitExceeded, Cancelled };
struct CoverDirectoryEntry {
    QString relativePath;
    QString name;
    bool directory = false;
};
struct CoverDirectoryPage {
    CoverDirectoryStatus status = CoverDirectoryStatus::Error;
    QString relativeDirectory;
    QVector<CoverDirectoryEntry> entries;
    QString message;
};

// Synchronous filesystem work: callers must run this outside the GUI thread.
// Each component is checked before traversal, including pure-image directories.
class CoverDirectoryBrowser final {
public:
    [[nodiscard]] static CoverDirectoryPage list(const QString &rootPath,
        const QString &relativeDirectory,
        const std::shared_ptr<std::atomic_bool> &cancelled = {},
        qsizetype maxEntries = 10000);
};

} // namespace asmr
