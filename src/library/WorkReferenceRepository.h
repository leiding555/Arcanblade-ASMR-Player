#pragma once

#include "library/LibraryRepository.h"
#include "library/WorkReferenceTypes.h"

#include <atomic>
#include <functional>

namespace asmr {

// Worker-owned operations. File/PDF reads and extraction never hold a SQL write transaction.
class WorkReferenceRepository final {
public:
    explicit WorkReferenceRepository(QString databasePath);
    // Translation context can exclude ASR rows before fetching any body bytes.
    WorkReferenceSnapshot load(const LibraryWork &expected, bool referenceFilesOnly = false);
    // Chooses ready reference files first, otherwise cached latest ASR, within one read snapshot.
    // Individual unavailable entries are reported without weakening normal load/sync validation.
    WorkReferenceSnapshot loadTranslationContext(const LibraryWork &expected,
        const QString &excludedReferencePath = {});
    WorkReferenceSyncResult synchronize(
        const LibraryWork &expected, const DirectoryScanResult &scan,
        const std::atomic_bool &cancelled,
        WorkReferenceRefreshPolicy policy = WorkReferenceRefreshPolicy::ReuseCache,
        const std::function<bool()> &beginCommit = {});
private:
    QString m_databasePath;
};

} // namespace asmr
