#pragma once
#include <QStringList>

namespace asmr {
struct SubtitleTranslationCleanupResult {
    QStringList removedFingerprints;
    QStringList resetTaskIds;
    QString error;
    [[nodiscard]] bool succeeded() const { return error.isEmpty(); }
};
// Synchronous worker-only reconciliation. Only confirmed missing, published
// artifacts are removed from storage; no files or attempt history are deleted.
// An empty scope/fingerprint checks all published results in this database.
[[nodiscard]] SubtitleTranslationCleanupResult cleanupMissingSubtitleTranslationResults(
    const QString &databasePath, const QString &scopeDirectory = {}, const QString &fingerprint = {});
} // namespace asmr
