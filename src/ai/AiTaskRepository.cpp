#include "ai/AiTaskRepository.h"
#include "library/BusinessDatabase.h"
#include "library/SqliteWriteTransaction.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QHash>
#include <QLockFile>
#include <QSet>
#include <QSettings>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>

namespace asmr {
namespace {
const QStringList Modes{QStringLiteral("offline-direct"), QStringLiteral("offline-asr-ja"), QStringLiteral("cloud-translation")};
const QStringList States{QStringLiteral("pending"), QStringLiteral("running"), QStringLiteral("interrupted"),
    QStringLiteral("succeeded"), QStringLiteral("failed"), QStringLiteral("cancelled")};
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
bool validModes(const QStringList &modes) {
    return !modes.isEmpty() && QSet<QString>(modes.cbegin(), modes.cend()).size() == modes.size()
        && std::all_of(modes.cbegin(), modes.cend(), [](const QString &mode) { return Modes.contains(mode); });
}
QString placeholders(int count) { return QStringList(count, QStringLiteral("?")).join(QLatin1Char(',')); }
QString json(const QJsonObject &value) { return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)); }
bool parse(const QString &value, QJsonObject *object) {
    if (value.size() > 1024 * 1024) return false;
    const auto document = QJsonDocument::fromJson(value.toUtf8());
    if (!document.isObject()) return false;
    *object = document.object(); return true;
}
bool execute(QSqlQuery &query, QString *error) {
    return query.exec() || fail(error, query.lastError().text());
}
}
AiTaskRepository::AiTaskRepository(QString databasePath) : m_databasePath(std::move(databasePath)) {}
std::shared_ptr<QLockFile> acquireAiTaskQueueLease(const QString &databasePath,const QString &group,QString *error) {
    if (error) error->clear();
    if ((group!=QStringLiteral("offline") && group!=QStringLiteral("cloud"))
        || databasePath.isEmpty() || databasePath.contains(QChar::Null) || !QFileInfo(databasePath).isAbsolute()) {
        fail(error,QStringLiteral("任务队列锁需要有效的数据库绝对路径及队列组。")); return {};
    }
    const auto info=QFileInfo(databasePath);
    if (!QDir().mkpath(info.absolutePath())) {
        fail(error,QStringLiteral("无法创建任务队列锁目录。")); return {};
    }
    const auto canonical=info.canonicalFilePath();
    const auto path=(canonical.isEmpty()?info.absoluteFilePath():canonical)
        +QLatin1Char('.')+group+QStringLiteral(".queue.lock");
    auto lease=std::make_shared<QLockFile>(path);
    lease->setStaleLockTime(0);
    if (!lease->tryLock(0)) {
        fail(error,QStringLiteral("任务列表已由另一个窗口或进程使用，或队列锁无法写入；请关闭其他窗口后重新打开。\n%1").arg(path));
        return {};
    }
    return lease;
}
QString aiTaskDatabasePath(QSettings *settings, const QString &explicitPath) {
    if (!explicitPath.isEmpty()) return QDir::cleanPath(QDir::fromNativeSeparators(explicitPath));
    if (settings && !settings->fileName().isEmpty()) {
        const auto identity = settings->fileName();
        const auto hash = QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
        const QFileInfo info(identity);
        const auto directory = settings->format() == QSettings::IniFormat ? info.absolutePath()
            : QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        return QDir(directory).filePath(QStringLiteral("arcanblade-tasks-%1.sqlite").arg(hash));
    }
    return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
        .filePath(QStringLiteral("arcanblade-tasks-%1.sqlite").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
}
bool AiTaskRepository::load(const QStringList &modes, AiTaskSnapshot *snapshot, QString *error) {
    if (error) error->clear();
    if (!snapshot || !validModes(modes)) return fail(error, QStringLiteral("Invalid AI task load request."));
    BusinessDatabase owner(m_databasePath);
    if (!owner.open(error)) return false;
    auto &db = owner.database();
    SqliteWriteTransaction transaction(db, QStringLiteral("load AI tasks"));
    if (!transaction.begin()) return fail(error, transaction.errorMessage());
    const auto modePlaceholders = placeholders(modes.size());
    QSqlQuery recover(db);
    recover.prepare(QStringLiteral("UPDATE ai_tasks SET state='interrupted',updated_at=? WHERE state='running' AND mode IN (%1)").arg(modePlaceholders));
    recover.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    for (const auto &mode : modes) recover.addBindValue(mode);
    if (!execute(recover,error)) return false;
    QSqlQuery attempts(db);
    attempts.prepare(QStringLiteral("UPDATE ai_task_attempts SET state='interrupted',finished_at=? WHERE state='running' AND task_id IN (SELECT task_id FROM ai_tasks WHERE mode IN (%1))").arg(modePlaceholders));
    attempts.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    for (const auto &mode : modes) attempts.addBindValue(mode);
    if (!execute(attempts,error)) return false;
    AiTaskSnapshot loaded;
    QSet<QString> loadedIds;
    qsizetype totalPayload=0;
    QSqlQuery tasks(db);
    tasks.prepare(QStringLiteral("SELECT task_id,mode,queue_order,state,data_json,created_at,updated_at FROM ai_tasks WHERE mode IN (%1) ORDER BY mode,queue_order,task_id").arg(modePlaceholders));
    for (const auto &mode : modes) tasks.addBindValue(mode);
    if (!execute(tasks,error)) return false;
    while (tasks.next()) {
        AiTaskRecord record{tasks.value(0).toString(),tasks.value(1).toString(),tasks.value(2).toInt(),tasks.value(3).toString(),{},tasks.value(5).toString(),tasks.value(6).toString()};
        totalPayload+=tasks.value(4).toString().size();
        if (QUuid(record.id).isNull() || loadedIds.contains(record.id) || !modes.contains(record.mode)
            || !States.contains(record.state) || record.queueOrder<0 || totalPayload>64*1024*1024
            || !parse(tasks.value(4).toString(),&record.data) || record.data.value(QStringLiteral("version")).toInt()!=1
            || loaded.tasks.size() >= 5000)
            return fail(error,QStringLiteral("Invalid or excessive AI task rows; original rows were retained."));
        loadedIds.insert(record.id);
        loaded.tasks.append(std::move(record));
    }
    if (tasks.lastError().isValid()) return fail(error,tasks.lastError().text());
    QSqlQuery rows(db);
    rows.prepare(QStringLiteral("SELECT a.attempt_id,a.task_id,a.state,a.parameters_json,a.result_json,a.started_at,a.finished_at FROM ai_task_attempts a JOIN ai_tasks t ON t.task_id=a.task_id WHERE t.mode IN (%1) ORDER BY a.started_at,a.attempt_id").arg(modePlaceholders));
    for (const auto &mode : modes) rows.addBindValue(mode);
    if (!execute(rows,error)) return false;
    while (rows.next()) {
        AiTaskAttempt record{rows.value(0).toString(),rows.value(1).toString(),rows.value(2).toString(),{},{},rows.value(5).toString(),rows.value(6).toString()};
        totalPayload+=rows.value(3).toString().size()+rows.value(4).toString().size();
        if (QUuid(record.id).isNull() || !loadedIds.contains(record.taskId) || !States.contains(record.state)
            || loaded.attempts.size()>=50000 || totalPayload>64*1024*1024
            || !parse(rows.value(3).toString(),&record.parameters) || !parse(rows.value(4).toString(),&record.result))
            return fail(error,QStringLiteral("Invalid AI task attempt; original rows were retained."));
        loaded.attempts.append(std::move(record));
    }
    if (rows.lastError().isValid()) return fail(error,rows.lastError().text());
    if (!transaction.commit()) return fail(error,transaction.errorMessage());
    *snapshot = std::move(loaded); return true;
}
bool AiTaskRepository::save(const QStringList &modes, const AiTaskSnapshot &snapshot, QString *error) {
    if (error) error->clear();
    if (!validModes(modes) || snapshot.tasks.size() > 5000 || snapshot.attempts.size()>50000) return fail(error,QStringLiteral("Invalid AI task snapshot."));
    QSet<QString> ids,attemptIds;
    qsizetype totalPayload=0;
    for (const auto &task : snapshot.tasks) {
        totalPayload+=json(task.data).size();
        if (totalPayload>64*1024*1024 || QUuid(task.id).isNull() || ids.contains(task.id) || !modes.contains(task.mode)
            || !States.contains(task.state) || task.queueOrder < 0 || task.data.value(QStringLiteral("version")).toInt() != 1
            || json(task.data).size() > 1024 * 1024)
            return fail(error,QStringLiteral("Invalid AI task row."));
        ids.insert(task.id);
    }
    for (const auto &attempt : snapshot.attempts) {
        totalPayload+=json(attempt.parameters).size()+json(attempt.result).size();
        if (totalPayload>64*1024*1024 || QUuid(attempt.id).isNull() || attemptIds.contains(attempt.id) || !ids.contains(attempt.taskId)
            || !States.contains(attempt.state) || json(attempt.parameters).size() > 1024 * 1024
            || json(attempt.result).size() > 1024 * 1024)
            return fail(error,QStringLiteral("Invalid AI task attempt."));
        attemptIds.insert(attempt.id);
    }
    BusinessDatabase owner(m_databasePath);
    if (!owner.open(error)) return false;
    auto &db = owner.database();
    SqliteWriteTransaction transaction(db,QStringLiteral("save AI tasks"));
    if (!transaction.begin()) return fail(error,transaction.errorMessage());
    // Never let a record from a different queue take over another task ID.
    QSqlQuery existing(db);
    existing.prepare(QStringLiteral("SELECT mode,data_json,updated_at FROM ai_tasks WHERE task_id=?"));
    QHash<QString, QPair<QJsonObject, QString>> missingResults;
    for (const auto &task : snapshot.tasks) {
        existing.bindValue(0,task.id);
        if (!execute(existing,error)) return false;
        if (existing.next()) {
            if (existing.value(0).toString() != task.mode)
                return fail(error,QStringLiteral("AI task identity belongs to another queue."));
            QJsonObject stored;
            if (task.mode == QStringLiteral("cloud-translation")
                && parse(existing.value(1).toString(), &stored)
                && stored.value(QStringLiteral("resultMissing")).toBool()
                && !task.data.value(QStringLiteral("resultMissing")).toBool()
                && task.state != QStringLiteral("running")) {
                // Scanning can invalidate a published artifact while a window
                // still holds an older completed/failed snapshot. Only a new
                // explicit run clears this marker; stale saves cannot revive it.
                missingResults.insert(task.id, {stored, existing.value(2).toString()});
            }
        }
        existing.finish();
    }
    QSqlQuery upsert(db);
    upsert.prepare(QStringLiteral("INSERT INTO ai_tasks(task_id,mode,queue_order,state,data_json,created_at,updated_at) VALUES(?,?,?,?,?,?,?) ON CONFLICT(task_id) DO UPDATE SET queue_order=excluded.queue_order,state=excluded.state,data_json=excluded.data_json,updated_at=excluded.updated_at"));
    const auto now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    for (const auto &task : snapshot.tasks) {
        upsert.bindValue(0,task.id); upsert.bindValue(1,task.mode); upsert.bindValue(2,task.queueOrder);
        const auto missing = missingResults.constFind(task.id);
        upsert.bindValue(3,missing == missingResults.cend() ? task.state : QStringLiteral("pending"));
        upsert.bindValue(4,json(missing == missingResults.cend() ? task.data : missing->first));
        upsert.bindValue(5,task.createdAt.isEmpty()?now:task.createdAt);
        upsert.bindValue(6,missing == missingResults.cend()
            ? (task.updatedAt.isEmpty()?now:task.updatedAt) : missing->second);
        if (!execute(upsert,error)) return false;
    }
    QSqlQuery attemptQuery(db);
    attemptQuery.prepare(QStringLiteral("INSERT INTO ai_task_attempts(attempt_id,task_id,state,parameters_json,result_json,started_at,finished_at) VALUES(?,?,?,?,?,?,?) ON CONFLICT(attempt_id) DO UPDATE SET state=excluded.state,result_json=excluded.result_json,finished_at=excluded.finished_at WHERE ai_task_attempts.task_id=excluded.task_id AND ai_task_attempts.parameters_json=excluded.parameters_json"));
    for (const auto &attempt : snapshot.attempts) {
        attemptQuery.bindValue(0,attempt.id); attemptQuery.bindValue(1,attempt.taskId); attemptQuery.bindValue(2,attempt.state);
        attemptQuery.bindValue(3,json(attempt.parameters)); attemptQuery.bindValue(4,json(attempt.result));
        attemptQuery.bindValue(5,attempt.startedAt.isEmpty()?now:attempt.startedAt);
        attemptQuery.bindValue(6,attempt.finishedAt.isNull()?QStringLiteral(""):attempt.finishedAt);
        if (!execute(attemptQuery,error)) return false;
        if (attemptQuery.numRowsAffected() != 1) return fail(error,QStringLiteral("AI task attempt identity or parameters changed."));
    }
    QSqlQuery candidates(db);
    candidates.prepare(QStringLiteral("SELECT task_id FROM ai_tasks WHERE mode IN (%1)").arg(placeholders(modes.size())));
    for (const auto &mode : modes) candidates.addBindValue(mode);
    if (!execute(candidates,error)) return false;
    QStringList removed;
    while (candidates.next()) if (!ids.contains(candidates.value(0).toString())) removed.append(candidates.value(0).toString());
    if (candidates.lastError().isValid()) return fail(error,candidates.lastError().text());
    candidates.finish();
    QSqlQuery remove(db); remove.prepare(QStringLiteral("DELETE FROM ai_tasks WHERE task_id=?"));
    for (const auto &id : removed) { remove.bindValue(0,id); if (!execute(remove,error)) return false; }
    return transaction.commit() || fail(error,transaction.errorMessage());
}
} // namespace asmr
