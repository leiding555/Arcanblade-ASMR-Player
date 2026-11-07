#pragma once

#include <QStringList>
#include <functional>
class QSettings;
namespace asmr {
struct ScanRootChangeResult { bool success = false; QString root; QString errorMessage; };
struct ScanRootSnapshot {
    QStringList roots;
    bool startupEnabled = true;
    QString errorMessage;
};
struct ScanRootPruneResult {
    bool success = false;
    QStringList removedRoots;
    QString errorMessage;
};
class ScanRootStoreTestAccess;
// SQLite configuration only; removing a root never changes library or media.
// Database work belongs on workers. The UI uses a no-I/O cache instance.
class ScanRootStore final {
public:
    explicit ScanRootStore(QString databasePath, QSettings *legacySettings = nullptr,
        bool loadImmediately = true);
    ~ScanRootStore();
    ScanRootStore(const ScanRootStore &) = delete;
    ScanRootStore &operator=(const ScanRootStore &) = delete;
    QStringList roots() const;
    bool startupEnabled() const;
    ScanRootSnapshot snapshot() const { return m_snapshot; }
    void adoptSnapshot(const ScanRootSnapshot &snapshot) { m_snapshot = snapshot; }
    bool reload();
    ScanRootChangeResult addRoot(const QString &path);
    ScanRootChangeResult removeRoot(const QString &path);
    // Worker-only: remove configuration for confirmed absent directories.
    // Inaccessible roots, volumes, links and uncertain paths are retained.
    // Filesystem probing precedes a short transaction and is rechecked before deletion.
    ScanRootPruneResult pruneMissingRoots();
    bool setStartupEnabled(bool enabled);
    static QString normalizePath(const QString &path);
    static QString pathKey(const QString &path);
private:
    friend class ScanRootStoreTestAccess;
    std::function<void()> m_beforePruneTransactionTestHook;
    QString m_databasePath;
    ScanRootSnapshot m_snapshot;
};
} // namespace asmr
