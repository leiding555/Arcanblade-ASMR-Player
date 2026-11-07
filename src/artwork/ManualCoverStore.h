#pragma once

#include <QSettings>
#include <QString>
#include <QVector>

namespace asmr {

struct ManualCoverSettings {
    QString databasePath;
    QString fileName;
    QSettings::Format format = QSettings::NativeFormat;
    QString managedDirectory;
};

struct ManualCoverIdentity {
    qint64 workId = 0;
    QString rootPath;
    qint64 importedAtMs = 0;
};

struct ManualCoverLookup {
    ManualCoverSettings settings;
    ManualCoverIdentity identity;
};

struct ManualCoverStoreResult {
    bool success = false;
    QString relativePath;
    QString errorMessage;
    QString managedFile;
};

// SQLite business records only. Call from a worker; each operation owns its connection.
// Legacy QSettings location fields are ignored and never imported or removed.
class ManualCoverStore final {
public:
    [[nodiscard]] static ManualCoverStoreResult read(
        const ManualCoverSettings &settings, const ManualCoverIdentity &identity);
    [[nodiscard]] static ManualCoverStoreResult write(
        const ManualCoverSettings &settings, const ManualCoverIdentity &identity,
        const QString &relativePath);
    [[nodiscard]] static ManualCoverStoreResult writeManaged(
        const ManualCoverSettings &settings, const ManualCoverIdentity &identity,
        const QString &token);
    [[nodiscard]] static ManualCoverStoreResult remove(
        const ManualCoverSettings &settings, qint64 workId);
    // Only pass a complete, successfully loaded works snapshot.
    [[nodiscard]] static ManualCoverStoreResult prune(
        const ManualCoverSettings &settings, const QVector<ManualCoverIdentity> &works);
    [[nodiscard]] static QString normalizeRelativePath(const QString &relativePath);
};

} // namespace asmr
