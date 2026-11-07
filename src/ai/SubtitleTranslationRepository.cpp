#include "ai/SubtitleTranslationRepository.h"
#include "library/BusinessDatabase.h"
#include "library/SqliteWriteTransaction.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <utility>

namespace asmr {
namespace {
constexpr qsizetype PayloadLimit = 16 * 1024 * 1024;
const QStringList CheckpointStates{QStringLiteral("pending"), QStringLiteral("running"),
    QStringLiteral("interrupted"), QStringLiteral("cancelled"), QStringLiteral("failed"),
    QStringLiteral("translated"), QStringLiteral("publishing"), QStringLiteral("published")};
const QStringList BatchStates{QStringLiteral("pending"), QStringLiteral("running"),
    QStringLiteral("interrupted"), QStringLiteral("cancelled"), QStringLiteral("failed"),
    QStringLiteral("completed")};
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
bool validDatabasePath(const QString &path, QString *error) {
    return (!path.isEmpty() && path == path.trimmed() && !path.contains(QChar::Null)
        && QFileInfo(path).isAbsolute())
        || fail(error, QStringLiteral("翻译断点需要有效的数据库绝对路径。"));
}
bool execute(QSqlQuery &query, QString *error) {
    return query.exec() || fail(error, query.lastError().text());
}
bool cancelled(const std::atomic_bool &flag, QString *error) {
    if (!flag.load()) return false;
    fail(error, QStringLiteral("已取消字幕翻译，已提交的完整批次保留。"));
    return true;
}
QString now() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
QString hash(const QByteArray &value) {
    return QString::fromLatin1(QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex());
}
bool validHash(const QString &value) {
    static const QRegularExpression expression(QStringLiteral("^[0-9a-f]{64}$"));
    return expression.match(value).hasMatch();
}
bool validResultPath(const QString &value) {
    if (value.isEmpty() || value != value.trimmed() || !QDir::isAbsolutePath(value)) return false;
    for (const auto character : value)
        if (character.category() == QChar::Other_Control) return false;
    return true;
}
QByteArray batchJson(const SubtitleTranslations &translations, const SubtitleTranslationBatch &batch) {
    QJsonArray array;
    for (int id = batch.firstId; id < batch.firstId + batch.count; ++id)
        array.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("text"), translations.value(id)}});
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}
struct Snapshot {
    bool exists = false;
    QString state, path, sha256;
    SubtitleTranslations translations;
    QVector<QString> batchStates;
    qsizetype payloadSize = 0;
};
bool validManifest(const SubtitleTranslationPlan &plan, QByteArray *manifest, QString *error) {
    *manifest = subtitleTranslationCheckpointManifest(plan);
    return (!manifest->isEmpty() && manifest->size() <= PayloadLimit)
        || fail(error, QStringLiteral("字幕翻译计划或断点清单无效。"));
}
// Called inside one read or write transaction, so the manifest and all batch rows
// are read from the same snapshot. Invalid data is never repaired or overwritten.
bool readSnapshot(QSqlDatabase &database, const SubtitleTranslationPlan &plan,
    const QByteArray &manifest, Snapshot *snapshot, QString *error) {
    *snapshot = {};
    QSqlQuery checkpoint(database);
    checkpoint.prepare(QStringLiteral("SELECT length(CAST(manifest_json AS BLOB)),manifest_json,state,result_path,result_sha256,typeof(manifest_json) FROM translation_checkpoints WHERE fingerprint=?"));
    checkpoint.addBindValue(plan.fingerprint);
    if (!execute(checkpoint, error)) return false;
    if (!checkpoint.next()) {
        if (checkpoint.lastError().isValid()) return fail(error, checkpoint.lastError().text());
        return true;
    }
    const qint64 manifestSize = checkpoint.value(0).toLongLong();
    if (checkpoint.value(5).toString() != QStringLiteral("text") || manifestSize < 1 || manifestSize > PayloadLimit
        || checkpoint.value(1).toString().toUtf8() != manifest)
        return fail(error, QStringLiteral("数据库中的翻译断点清单与当前计划不匹配，原记录已保留。"));
    snapshot->exists = true;
    snapshot->payloadSize = manifest.size();
    snapshot->state = checkpoint.value(2).toString();
    snapshot->path = checkpoint.value(3).toString();
    snapshot->sha256 = checkpoint.value(4).toString();
    const bool publishing = snapshot->state == QStringLiteral("publishing")
        || snapshot->state == QStringLiteral("published");
    if (!CheckpointStates.contains(snapshot->state)
        || (publishing && (!validResultPath(snapshot->path) || !validHash(snapshot->sha256)))
        || (!publishing && (!snapshot->path.isEmpty() || !snapshot->sha256.isEmpty())))
        return fail(error, QStringLiteral("数据库中的翻译发布状态无效，原记录已保留。"));
    if (checkpoint.next() || checkpoint.lastError().isValid())
        return fail(error, QStringLiteral("数据库中的翻译断点不唯一或读取失败。"));
    QSqlQuery rows(database);
    rows.prepare(QStringLiteral("SELECT batch_index,first_id,entry_count,state,length(CAST(items_json AS BLOB)),items_json,items_sha256,typeof(batch_index),typeof(first_id),typeof(entry_count),typeof(items_json),typeof(items_sha256),typeof(state) FROM translation_batches WHERE fingerprint=? ORDER BY batch_index"));
    rows.addBindValue(plan.fingerprint);
    if (!execute(rows, error)) return false;
    const bool tools = plan.mode == SubtitleTranslationMode::ToolConversation;
    int index = 0;
    int nextId = 1;
    while (rows.next()) {
        if (!tools && index >= plan.batches.size()) return fail(error, QStringLiteral("翻译断点包含多余批次。"));
        const qint64 storedFirst = rows.value(1).toLongLong();
        const qint64 storedCount = rows.value(2).toLongLong();
        if (tools && (storedFirst != nextId || storedCount < 1 || storedCount > 40
            || storedCount > plan.entries.size() - nextId + 1))
            return fail(error, QStringLiteral("工具翻译断点包含不连续或越界的译文。"));
        const auto batch = tools ? SubtitleTranslationBatch{nextId, static_cast<int>(storedCount)}
            : plan.batches.at(index);
        const QString state = rows.value(3).toString();
        const qint64 size = rows.value(4).toLongLong();
        if (rows.value(7).toString() != QStringLiteral("integer") || rows.value(8).toString() != QStringLiteral("integer")
            || rows.value(9).toString() != QStringLiteral("integer")
            || rows.value(10).toString() != QStringLiteral("text") || rows.value(11).toString() != QStringLiteral("text")
            || rows.value(12).toString() != QStringLiteral("text")
            || rows.value(0).toLongLong() != index || rows.value(1).toLongLong() != batch.firstId
            || rows.value(2).toLongLong() != batch.count || !BatchStates.contains(state)
            || (tools && state != QStringLiteral("completed"))
            || size < 0 || size > PayloadLimit - snapshot->payloadSize)
            return fail(error, QStringLiteral("翻译断点的批次结构或大小无效。"));
        snapshot->payloadSize += static_cast<qsizetype>(size);
        const QByteArray bytes = rows.value(5).toString().toUtf8();
        const QString sha256 = rows.value(6).toString();
        if (bytes.size() != size) return fail(error, QStringLiteral("翻译断点批次的编码无效。"));
        snapshot->batchStates.append(state);
        if (state != QStringLiteral("completed")) {
            if (!bytes.isEmpty() || !sha256.isEmpty())
                return fail(error, QStringLiteral("未完成翻译批次包含译文，原记录已保留。"));
        } else {
            if (!validHash(sha256) || hash(bytes) != sha256)
                return fail(error, QStringLiteral("翻译断点批次的校验和无效。"));
            const QJsonDocument document = QJsonDocument::fromJson(bytes);
            if (!document.isArray() || document.array().size() != batch.count
                || document.toJson(QJsonDocument::Compact) != bytes)
                return fail(error, QStringLiteral("翻译断点批次的 JSON 结构无效。"));
            int id = batch.firstId;
            for (const auto &item : document.array()) {
                const QJsonObject object = item.toObject();
                const auto identifier = object.value(QStringLiteral("id"));
                const auto text = object.value(QStringLiteral("text"));
                if (!item.isObject() || object.size() != 2 || !identifier.isDouble()
                    || identifier.toDouble() != id || !text.isString())
                    return fail(error, QStringLiteral("翻译断点批次包含缺失、重复或无效 ID。"));
                snapshot->translations.insert(id++, text.toString());
            }
        }
        nextId = batch.firstId + batch.count;
        ++index;
    }
    if (rows.lastError().isValid()) return fail(error, rows.lastError().text());
    if ((!tools && index != plan.batches.size())
        || !validateSubtitleTranslationData(plan, snapshot->translations, publishing || snapshot->state == QStringLiteral("translated"), error))
        return fail(error, QStringLiteral("翻译断点缺少批次或包含无效译文，原记录已保留。"));
    return true;
}
bool insertCheckpoint(QSqlDatabase &database, const SubtitleTranslationPlan &plan,
    const QByteArray &manifest, QString *error) {
    QSqlQuery checkpoint(database);
    checkpoint.prepare(QStringLiteral("INSERT INTO translation_checkpoints(fingerprint,manifest_json,state,result_path,result_sha256,updated_at) VALUES(?,?,'pending','','',?)"));
    checkpoint.addBindValue(plan.fingerprint);
    checkpoint.addBindValue(QString::fromUtf8(manifest));
    checkpoint.addBindValue(now());
    if (!execute(checkpoint, error)) return false;
    if (plan.mode == SubtitleTranslationMode::ToolConversation) return true;
    for (int index = 0; index < plan.batches.size(); ++index) {
        const auto &batch = plan.batches.at(index);
        QSqlQuery row(database);
        row.prepare(QStringLiteral("INSERT INTO translation_batches(fingerprint,batch_index,first_id,entry_count,state,items_json,items_sha256,updated_at) VALUES(?,?,?,?,'pending','','',?)"));
        row.addBindValue(plan.fingerprint); row.addBindValue(index);
        row.addBindValue(batch.firstId); row.addBindValue(batch.count); row.addBindValue(now());
        if (!execute(row, error)) return false;
    }
    return true;
}
bool finish(SqliteWriteTransaction &transaction, const std::atomic_bool &flag, QString *error) {
    if (cancelled(flag, error)) return false;
    return transaction.commit() || fail(error, transaction.errorMessage());
}
bool matchingPair(const SubtitleTranslationPlan &correction, const SubtitleTranslationPlan &translation,
    QString *error)
{
    return (correction.request.useWorkContext && translation.request.useWorkContext
        && correction.mode == SubtitleTranslationMode::ToolConversation
        && translation.mode == SubtitleTranslationMode::ToolConversation
        && correction.phase == SubtitleTranslationPhase::Correction
        && translation.phase == SubtitleTranslationPhase::Translation
        && correction.contextKind == translation.contextKind
        && correction.contextGroup == correction.fingerprint
        && translation.contextGroup == correction.contextGroup
        && correction.fingerprint != translation.fingerprint
        && validHash(correction.referenceDigest) && correction.referenceDigest == translation.referenceDigest
        && validHash(translation.correctedSourceHash)
        && correction.sourceSha256 == translation.sourceSha256
        && correction.request.sourcePath == translation.request.sourcePath
        && correction.request.taskId == translation.request.taskId
        && correction.request.databasePath == translation.request.databasePath
        && correction.request.settings.provider == translation.request.settings.provider
        && correction.request.settings.model == translation.request.settings.model
        && translationApiThinkingEnabled(correction.request.settings) == translationApiThinkingEnabled(translation.request.settings)
        && translationCredentialScope(correction.request.settings) == translationCredentialScope(translation.request.settings)
        && correction.request.audioPath == translation.request.audioPath
        && correction.contextWork == translation.contextWork
        && correction.request.workId == translation.request.workId
        && correction.request.audioFileId == translation.request.audioFileId
        && correction.request.subtitleFileId == translation.request.subtitleFileId
        && correction.entries.size() == translation.entries.size()
        && correction.outputBaseDirectory == translation.outputBaseDirectory
        && correction.outputFileName != translation.outputFileName)
        || fail(error, QStringLiteral("日语校正与中文翻译不属于同一完整上下文任务。"));
}
}
SubtitleTranslationRepository::SubtitleTranslationRepository(QString databasePath)
    : m_databasePath(std::move(databasePath)) {}
bool SubtitleTranslationRepository::load(const SubtitleTranslationPlan &plan,
    SubtitleTranslations *translations, QString *error) {
    if (error) error->clear();
    if (!translations) return fail(error, QStringLiteral("缺少翻译断点接收对象。"));
    QByteArray manifest;
    if (!validManifest(plan, &manifest, error)) return false;
    if (!validDatabasePath(m_databasePath, error)) return false;
    if (!QFileInfo::exists(m_databasePath)) {
        translations->clear();
        return true;
    }
    BusinessDatabase owner(m_databasePath);
    if (!owner.open(error)) return false;
    auto &database = owner.database();
    if (!database.transaction()) return fail(error, database.lastError().text());
    Snapshot snapshot;
    if (!readSnapshot(database, plan, manifest, &snapshot, error)) {
        database.rollback(); return false;
    }
    if (!database.commit()) return fail(error, database.lastError().text());
    *translations = std::move(snapshot.translations);
    return true;
}
bool SubtitleTranslationRepository::save(const SubtitleTranslationPlan &plan,
    const SubtitleTranslations &translations, const std::atomic_bool &flag, QString *error) {
    if (error) error->clear();
    QByteArray manifest;
    if (cancelled(flag, error) || !validManifest(plan, &manifest, error)
        || !validateSubtitleTranslationData(plan, translations, false, error)) return false;
    if (!validDatabasePath(m_databasePath, error)) return false;
    BusinessDatabase owner(m_databasePath);
    if (!owner.open(error)) return false;
    auto &database = owner.database();
    SqliteWriteTransaction transaction(database, QStringLiteral("Save subtitle translation batches"));
    if (!transaction.begin()) return fail(error, transaction.errorMessage());
    if (cancelled(flag, error)) return false;
    Snapshot snapshot;
    if (!readSnapshot(database, plan, manifest, &snapshot, error)) return false;
    if (!snapshot.exists) {
        if (!insertCheckpoint(database, plan, manifest, error)) return false;
        snapshot.payloadSize = manifest.size();
        if (plan.mode == SubtitleTranslationMode::IndependentBatches)
            snapshot.batchStates.fill(QStringLiteral("pending"), plan.batches.size());
    }
    for (auto item = translations.cbegin(); item != translations.cend(); ++item)
        if (snapshot.translations.contains(item.key()) && snapshot.translations.value(item.key()) != item.value())
            return fail(error, QStringLiteral("完整翻译批次与已有译文冲突，原记录已保留。"));
    if (plan.mode == SubtitleTranslationMode::ToolConversation) {
        int index = static_cast<int>(snapshot.batchStates.size());
        const int total = static_cast<int>(translations.size());
        for (int first = static_cast<int>(snapshot.translations.size()) + 1; first <= total;) {
            if (cancelled(flag, error)) return false;
            // Physical rows retain the v5 limit; a model write can span any number of rows.
            const SubtitleTranslationBatch batch{first, std::min(40, total - first + 1)};
            const QByteArray bytes = batchJson(translations, batch);
            if (bytes.size() > PayloadLimit - snapshot.payloadSize)
                return fail(error, QStringLiteral("翻译断点超过 16 MiB，未覆盖原记录。"));
            snapshot.payloadSize += bytes.size();
            QSqlQuery insert(database);
            insert.prepare(QStringLiteral("INSERT INTO translation_batches(fingerprint,batch_index,first_id,entry_count,state,items_json,items_sha256,updated_at) VALUES(?,?,?,?,'completed',?,?,?)"));
            insert.addBindValue(plan.fingerprint); insert.addBindValue(index++);
            insert.addBindValue(batch.firstId); insert.addBindValue(batch.count);
            insert.addBindValue(QString::fromUtf8(bytes)); insert.addBindValue(hash(bytes));
            insert.addBindValue(now());
            if (!execute(insert, error) || insert.numRowsAffected() != 1)
                return fail(error, QStringLiteral("无法保存工具翻译的连续译文。"));
            first += batch.count;
        }
    } else {
        for (int index = 0; index < plan.batches.size(); ++index) {
            const auto &batch = plan.batches.at(index);
            if (!translations.contains(batch.firstId) || snapshot.batchStates.at(index) == QStringLiteral("completed")) continue;
            const QByteArray bytes = batchJson(translations, batch);
            if (bytes.size() > PayloadLimit - snapshot.payloadSize)
                return fail(error, QStringLiteral("翻译断点超过 16 MiB，未覆盖原记录。"));
            snapshot.payloadSize += bytes.size();
            QSqlQuery update(database);
            update.prepare(QStringLiteral("UPDATE translation_batches SET state='completed',items_json=?,items_sha256=?,updated_at=? WHERE fingerprint=? AND batch_index=?"));
            update.addBindValue(QString::fromUtf8(bytes)); update.addBindValue(hash(bytes));
            update.addBindValue(now()); update.addBindValue(plan.fingerprint); update.addBindValue(index);
            if (!execute(update, error) || update.numRowsAffected() != 1)
                return fail(error, QStringLiteral("无法保存完整翻译批次。"));
        }
    }
    SubtitleTranslations merged = snapshot.translations;
    for (auto item = translations.cbegin(); item != translations.cend(); ++item) merged.insert(item.key(), item.value());
    const bool complete = merged.size() == plan.entries.size();
    QSqlQuery touch(database);
    touch.prepare(QStringLiteral("UPDATE translation_checkpoints SET state=CASE WHEN ? AND state NOT IN ('publishing','published') THEN 'translated' ELSE state END,updated_at=? WHERE fingerprint=?"));
    touch.addBindValue(complete); touch.addBindValue(now()); touch.addBindValue(plan.fingerprint);
    if (!execute(touch, error)) return false;
    return finish(transaction, flag, error);
}
bool SubtitleTranslationRepository::setState(const SubtitleTranslationPlan &plan,
    const QString &state, int batchIndex, QString *error) {
    if (error) error->clear();
    if ((state != QStringLiteral("running") && state != QStringLiteral("cancelled")
        && state != QStringLiteral("failed") && state != QStringLiteral("interrupted"))
        || (plan.mode == SubtitleTranslationMode::ToolConversation ? batchIndex != -1
            : batchIndex < -1 || batchIndex >= plan.batches.size()))
        return fail(error, QStringLiteral("无效的翻译执行状态。"));
    QByteArray manifest;
    if (!validManifest(plan, &manifest, error)) return false;
    if (!validDatabasePath(m_databasePath, error)) return false;
    BusinessDatabase owner(m_databasePath);
    if (!owner.open(error)) return false;
    auto &database = owner.database();
    SqliteWriteTransaction transaction(database, QStringLiteral("Update subtitle translation state"));
    if (!transaction.begin()) return fail(error, transaction.errorMessage());
    Snapshot snapshot;
    if (!readSnapshot(database, plan, manifest, &snapshot, error)) return false;
    if (!snapshot.exists) return fail(error, QStringLiteral("必须先保存翻译断点，才能更新执行状态。"));
    if (snapshot.state == QStringLiteral("publishing") || snapshot.state == QStringLiteral("published"))
        return transaction.commit() || fail(error, transaction.errorMessage());
    if (plan.mode == SubtitleTranslationMode::IndependentBatches) {
        if (state == QStringLiteral("running") && batchIndex >= 0
            && snapshot.batchStates.at(batchIndex) == QStringLiteral("completed"))
            return fail(error, QStringLiteral("已完成的翻译批次不能重新进入执行状态。"));
        QSqlQuery batches(database);
        batches.prepare(QStringLiteral("UPDATE translation_batches SET state=?,updated_at=? WHERE fingerprint=? AND state='running'"));
        batches.addBindValue(state == QStringLiteral("running") ? QStringLiteral("interrupted") : state);
        batches.addBindValue(now()); batches.addBindValue(plan.fingerprint);
        if (!execute(batches, error)) return false;
        if (state == QStringLiteral("running") && batchIndex >= 0) {
            QSqlQuery active(database);
            active.prepare(QStringLiteral("UPDATE translation_batches SET state='running',updated_at=? WHERE fingerprint=? AND batch_index=? AND state!='completed'"));
            active.addBindValue(now()); active.addBindValue(plan.fingerprint); active.addBindValue(batchIndex);
            if (!execute(active, error)) return false;
        }
    }
    QSqlQuery checkpoint(database);
    checkpoint.prepare(QStringLiteral("UPDATE translation_checkpoints SET state=?,updated_at=? WHERE fingerprint=?"));
    // Recovery only interrupts a formerly running checkpoint; fresh pending
    // or fully translated data must retain its accurate durable state.
    checkpoint.addBindValue(state == QStringLiteral("interrupted") && snapshot.state != QStringLiteral("running")
        ? snapshot.state : state);
    checkpoint.addBindValue(now()); checkpoint.addBindValue(plan.fingerprint);
    if (!execute(checkpoint, error)) return false;
    return transaction.commit() || fail(error, transaction.errorMessage());
}
bool SubtitleTranslationRepository::publication(const SubtitleTranslationPlan &plan,
    QString *path, QString *sha256, QString *state, QString *error) {
    if (error) error->clear();
    if (!path || !sha256 || !state) return fail(error, QStringLiteral("缺少翻译发布状态接收对象。"));
    QByteArray manifest;
    if (!validManifest(plan, &manifest, error)) return false;
    if (!validDatabasePath(m_databasePath, error)) return false;
    if (!QFileInfo::exists(m_databasePath)) {
        path->clear(); sha256->clear(); state->clear();
        return true;
    }
    BusinessDatabase owner(m_databasePath);
    if (!owner.open(error)) return false;
    auto &database = owner.database();
    if (!database.transaction()) return fail(error, database.lastError().text());
    Snapshot snapshot;
    if (!readSnapshot(database, plan, manifest, &snapshot, error)) {
        database.rollback(); return false;
    }
    if (!database.commit()) return fail(error, database.lastError().text());
    *path = snapshot.path; *sha256 = snapshot.sha256; *state = snapshot.state;
    return true;
}
bool SubtitleTranslationRepository::reservePublication(const SubtitleTranslationPlan &plan,
    const QString &path, const QString &sha256, const std::atomic_bool &flag, QString *error) {
    if (error) error->clear();
    QByteArray manifest;
    if (cancelled(flag, error) || !validManifest(plan, &manifest, error)) return false;
    if (!validResultPath(path) || !validHash(sha256))
        return fail(error, QStringLiteral("翻译发布路径或校验和无效。"));
    if (!validDatabasePath(m_databasePath, error)) return false;
    BusinessDatabase owner(m_databasePath);
    if (!owner.open(error)) return false;
    auto &database = owner.database();
    SqliteWriteTransaction transaction(database, QStringLiteral("Reserve subtitle translation publication"));
    if (!transaction.begin()) return fail(error, transaction.errorMessage());
    Snapshot snapshot;
    if (!readSnapshot(database, plan, manifest, &snapshot, error)) return false;
    if (!snapshot.exists || !validateSubtitleTranslationData(plan, snapshot.translations, true, error))
        return fail(error, QStringLiteral("译文未完整保存到数据库，不能发布。"));
    if (snapshot.state == QStringLiteral("publishing") || snapshot.state == QStringLiteral("published")) {
        if (snapshot.path != path || snapshot.sha256 != sha256)
            return fail(error, QStringLiteral("不能覆盖已有翻译发布记录。"));
        return finish(transaction, flag, error);
    }
    QSqlQuery update(database);
    update.prepare(QStringLiteral("UPDATE translation_checkpoints SET state='publishing',result_path=?,result_sha256=?,updated_at=? WHERE fingerprint=?"));
    update.addBindValue(path); update.addBindValue(sha256); update.addBindValue(now()); update.addBindValue(plan.fingerprint);
    if (!execute(update, error)) return false;
    return finish(transaction, flag, error);
}
bool SubtitleTranslationRepository::clearPublicationReservation(const SubtitleTranslationPlan &plan,
    const QString &expectedPath, const QString &expectedSha256, QString *error) {
    if (error) error->clear();
    QByteArray manifest;
    if (!validManifest(plan, &manifest, error)) return false;
    if (!validDatabasePath(m_databasePath, error)) return false;
    BusinessDatabase owner(m_databasePath);
    if (!owner.open(error)) return false;
    auto &database = owner.database();
    SqliteWriteTransaction transaction(database, QStringLiteral("Clear absent subtitle translation publication"));
    if (!transaction.begin()) return fail(error, transaction.errorMessage());
    Snapshot snapshot;
    if (!readSnapshot(database, plan, manifest, &snapshot, error)) return false;
    if (!snapshot.exists || snapshot.state != QStringLiteral("publishing")
        || snapshot.path != expectedPath || snapshot.sha256 != expectedSha256)
        return fail(error, QStringLiteral("翻译发布预留记录已变化或已确认，不能清除。"));
    QSqlQuery update(database);
    update.prepare(QStringLiteral("UPDATE translation_checkpoints SET state='translated',result_path='',result_sha256='',updated_at=? WHERE fingerprint=? AND state='publishing' AND result_path=? AND result_sha256=?"));
    update.addBindValue(now()); update.addBindValue(plan.fingerprint);
    update.addBindValue(expectedPath); update.addBindValue(expectedSha256);
    if (!execute(update, error) || update.numRowsAffected() != 1)
        return fail(error, QStringLiteral("无法清除翻译发布预留记录。"));
    return transaction.commit() || fail(error, transaction.errorMessage());
}
bool SubtitleTranslationRepository::confirmPublication(const SubtitleTranslationPlan &plan,
    const std::atomic_bool &flag, QString *error) {
    if (error) error->clear();
    QByteArray manifest;
    if (cancelled(flag, error) || !validManifest(plan, &manifest, error)) return false;
    if (!validDatabasePath(m_databasePath, error)) return false;
    BusinessDatabase owner(m_databasePath);
    if (!owner.open(error)) return false;
    auto &database = owner.database();
    SqliteWriteTransaction transaction(database, QStringLiteral("Confirm subtitle translation publication"));
    if (!transaction.begin()) return fail(error, transaction.errorMessage());
    Snapshot snapshot;
    if (!readSnapshot(database, plan, manifest, &snapshot, error)) return false;
    if (!snapshot.exists || (snapshot.state != QStringLiteral("publishing") && snapshot.state != QStringLiteral("published")))
        return fail(error, QStringLiteral("翻译发布尚未预留，不能确认。"));
    QSqlQuery update(database);
    update.prepare(QStringLiteral("UPDATE translation_checkpoints SET state='published',updated_at=? WHERE fingerprint=?"));
    update.addBindValue(now()); update.addBindValue(plan.fingerprint);
    if (!execute(update, error)) return false;
    return finish(transaction, flag, error);
}
bool SubtitleTranslationRepository::reservePublicationPair(const SubtitleTranslationPlan &correction,
    const QString &japanesePath, const QString &japaneseSha256,
    const SubtitleTranslationPlan &translation, const QString &chinesePath,
    const QString &chineseSha256, const std::atomic_bool &flag, QString *error)
{
    if (error) error->clear();
    QByteArray japaneseManifest, chineseManifest;
    if (cancelled(flag, error) || !matchingPair(correction, translation, error)
        || !validManifest(correction, &japaneseManifest, error)
        || !validManifest(translation, &chineseManifest, error)
        || !validDatabasePath(m_databasePath, error)) return false;
    if (!validResultPath(japanesePath) || !validResultPath(chinesePath)
        || !validHash(japaneseSha256) || !validHash(chineseSha256)
        || QFileInfo(japanesePath).absolutePath() != QFileInfo(chinesePath).absolutePath()
        || QFileInfo(japanesePath).fileName() != correction.outputFileName
        || QFileInfo(chinesePath).fileName() != translation.outputFileName)
        return fail(error, QStringLiteral("成对字幕发布路径或校验和无效。"));
    BusinessDatabase owner(m_databasePath);
    if (!owner.open(error)) return false;
    auto &database = owner.database();
    SqliteWriteTransaction transaction(database, QStringLiteral("Reserve context subtitle pair publication"));
    if (!transaction.begin()) return fail(error, transaction.errorMessage());
    Snapshot japanese, chinese;
    if (!readSnapshot(database, correction, japaneseManifest, &japanese, error)
        || !readSnapshot(database, translation, chineseManifest, &chinese, error)) return false;
    if (!japanese.exists || !chinese.exists
        || !validateSubtitleTranslationData(correction, japanese.translations, true, error)
        || !validateSubtitleTranslationData(translation, chinese.translations, true, error))
        return fail(error, QStringLiteral("两阶段字幕尚未完整保存，不能成对发布。"));
    const auto reserved = [](const Snapshot &value) {
        return value.state == QStringLiteral("publishing") || value.state == QStringLiteral("published");
    };
    if (reserved(japanese) || reserved(chinese)) {
        if (!reserved(japanese) || !reserved(chinese) || japanese.state != chinese.state
            || japanese.path != japanesePath || japanese.sha256 != japaneseSha256
            || chinese.path != chinesePath || chinese.sha256 != chineseSha256)
            return fail(error, QStringLiteral("成对发布记录不一致或与现有文件冲突，未覆盖记录。"));
        return finish(transaction, flag, error);
    }
    const auto reserve = [&](const SubtitleTranslationPlan &plan, const QString &path, const QString &sha256) {
        QSqlQuery update(database);
        update.prepare(QStringLiteral("UPDATE translation_checkpoints SET state='publishing',result_path=?,result_sha256=?,updated_at=? WHERE fingerprint=?"));
        update.addBindValue(path); update.addBindValue(sha256); update.addBindValue(now()); update.addBindValue(plan.fingerprint);
        return execute(update, error) && update.numRowsAffected() == 1;
    };
    if (!reserve(correction, japanesePath, japaneseSha256) || !reserve(translation, chinesePath, chineseSha256))
        return fail(error, error && !error->isEmpty() ? *error : QStringLiteral("无法同时登记两份字幕发布记录。"));
    return finish(transaction, flag, error);
}
bool SubtitleTranslationRepository::confirmPublicationPair(const SubtitleTranslationPlan &correction,
    const SubtitleTranslationPlan &translation, const std::atomic_bool &flag, QString *error)
{
    if (error) error->clear();
    QByteArray japaneseManifest, chineseManifest;
    if (cancelled(flag, error) || !matchingPair(correction, translation, error)
        || !validManifest(correction, &japaneseManifest, error)
        || !validManifest(translation, &chineseManifest, error)
        || !validDatabasePath(m_databasePath, error)) return false;
    BusinessDatabase owner(m_databasePath);
    if (!owner.open(error)) return false;
    auto &database = owner.database();
    SqliteWriteTransaction transaction(database, QStringLiteral("Confirm context subtitle pair publication"));
    if (!transaction.begin()) return fail(error, transaction.errorMessage());
    Snapshot japanese, chinese;
    if (!readSnapshot(database, correction, japaneseManifest, &japanese, error)
        || !readSnapshot(database, translation, chineseManifest, &chinese, error)) return false;
    if (!japanese.exists || !chinese.exists || japanese.state != chinese.state
        || (japanese.state != QStringLiteral("publishing") && japanese.state != QStringLiteral("published")))
        return fail(error, QStringLiteral("两份字幕尚未同时预留发布，不能确认。"));
    QSqlQuery update(database);
    update.prepare(QStringLiteral("UPDATE translation_checkpoints SET state='published',updated_at=? WHERE fingerprint IN (?,?)"));
    update.addBindValue(now()); update.addBindValue(correction.fingerprint); update.addBindValue(translation.fingerprint);
    if (!execute(update, error) || update.numRowsAffected() != 2)
        return fail(error, error && !error->isEmpty() ? *error : QStringLiteral("无法同时确认两份字幕发布记录。"));
    return finish(transaction, flag, error);
}
} // namespace asmr
