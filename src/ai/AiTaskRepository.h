#pragma once
#include <QJsonObject>
#include <QStringList>
#include <QVector>
#include <memory>

class QSettings;
class QLockFile;

namespace asmr {
struct AiTaskRecord {
    QString id;
    QString mode;
    int queueOrder = 0;
    QString state;
    QJsonObject data;
    QString createdAt;
    QString updatedAt;
};
struct AiTaskAttempt {
    QString id;
    QString taskId;
    QString state;
    QJsonObject parameters;
    QJsonObject result;
    QString startedAt;
    QString finishedAt;
};
struct AiTaskSnapshot {
    QVector<AiTaskRecord> tasks;
    QVector<AiTaskAttempt> attempts;
};
// Worker-only short transactions. No media, generated files, credentials or
// legacy QSettings/JSON task lists are read or modified by this repository.
class AiTaskRepository final {
public:
    explicit AiTaskRepository(QString databasePath);
    bool load(const QStringList &modes, AiTaskSnapshot *snapshot, QString *error = nullptr);
    // Updates individual rows and removes only absent tasks in the named modes.
    // Include retained attempt rows in snapshots; existing attempts are never
    // removed except by explicitly removing their parent task.
    bool save(const QStringList &modes, const AiTaskSnapshot &snapshot, QString *error = nullptr);
private:
    QString m_databasePath;
};
// Acquire on a worker before reading task state; retain through all queue I/O.
std::shared_ptr<QLockFile> acquireAiTaskQueueLease(const QString &databasePath,
    const QString &group, QString *error = nullptr);
// Call before moving to a worker. The explicit application database wins;
// otherwise settings-backed instances use a separate adjacent test database.
QString aiTaskDatabasePath(QSettings *settings, const QString &explicitPath = {});
} // namespace asmr
