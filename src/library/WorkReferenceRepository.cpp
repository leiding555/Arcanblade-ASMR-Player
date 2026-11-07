#include "library/WorkReferenceRepository.h"
#include "library/BusinessDatabase.h"
#include "library/SqliteWriteTransaction.h"
#include "library/WorkReferenceExtractor.h"
#include "library/WorkReferenceSelection.h"

#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <utility>

namespace asmr {
namespace {
constexpr qint64 MaximumJsonBytes = 64 * 1024 * 1024;
constexpr qint64 MaximumSnapshotJsonBytes = 128 * 1024 * 1024;

QString key(QString path)
{
    path = QDir::cleanPath(QDir::fromNativeSeparators(path));
    path = path.toCaseFolded();
    return path;
}
QString nonNull(const QString &value) { return value.isNull() ? QStringLiteral("") : value; }
bool safeRelative(const QString &path)
{
    if (path.isEmpty() || QDir::isAbsolutePath(path) || path.contains(QChar::Null)
        || path.contains(QLatin1Char('\\')) || path.contains(QLatin1Char(':'))
        || QDir::cleanPath(path) != path)
        return false;
    for (const auto &part : path.split(QLatin1Char('/'))) {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")
            || part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' '))) return false;
        for (const auto ch : part)
            if (ch.unicode() < 32 || QStringLiteral("<>:\"|?*").contains(ch)) return false;
    }
    return true;
}
bool identity(QSqlDatabase &db, const LibraryWork &expected, QString *error)
{
    if (expected.id <= 0 || expected.importedAtMs < 0 || !QDir::isAbsolutePath(expected.rootPath)) {
        *error = QStringLiteral("作品资料请求的作品身份无效。"); return false;
    }
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT root_path,root_key,imported_at_ms FROM works WHERE id=?"));
    query.addBindValue(expected.id);
    if (!query.exec()) { *error = query.lastError().text(); return false; }
    if (!query.next() || key(query.value(0).toString()) != key(expected.rootPath)
        || query.value(1).toString() != key(expected.rootPath)
        || query.value(2).toLongLong() != expected.importedAtMs) {
        *error = QStringLiteral("作品已删除或身份已变化，资料操作未执行。"); return false;
    }
    return true;
}
// Count actual body text, not JSON escaping, object names or timestamp metadata.
qint64 bodyBytes(const QJsonObject &content)
{
    qint64 count = 0;
    if (content.value(QStringLiteral("text")).isString())
        count += content.value(QStringLiteral("text")).toString().toUtf8().size();
    for (const auto &field : {QStringLiteral("pages"), QStringLiteral("cues")})
        for (const auto &entry : content.value(field).toArray())
            count += entry.toObject().value(QStringLiteral("text")).toString().toUtf8().size();
    return count;
}
void warn(WorkReferenceSyncResult &result, const QString &message)
{
    if (result.warnings.size() < 20) result.warnings.append(message.left(4096));
}

enum class SourceFilter { All, ReferenceFiles, Asr };
WorkReferenceSnapshot readSnapshot(QSqlDatabase &db, const LibraryWork &expected,
    bool repairing = false, SourceFilter filter = SourceFilter::All, bool contextRead = false)
{
    WorkReferenceSnapshot result;
    if (!identity(db, expected, &result.error)) return result;
    {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("SELECT root_key,imported_at_ms,synchronized_at FROM work_reference_scans WHERE work_id=?"));
        query.addBindValue(expected.id);
        if (!query.exec()) { result.error = query.lastError().text(); return result; }
        if (query.next()) {
            if (query.value(0).toString() != key(expected.rootPath)
                || query.value(1).toLongLong() != expected.importedAtMs) {
                result.error = QStringLiteral("作品资料快照身份不匹配。"); return result;
            }
            result.scanned = true;
            result.synchronizedAt = query.value(2).toString();
        }
    }
    QSqlQuery query(db);
    // Read only lengths and metadata first; fetch each body after both budgets have been checked.
    QString select = QStringLiteral(
        "SELECT id,relative_path,path_key,format,source,audio_relative_path,completed_at,source_hash,encoding,"
        "encoding_inferred,extractor_version,content_version,"
        "NULL,"
        "length(CAST(content_json AS BLOB)),state,error,warning,missing "
        "FROM work_references WHERE work_id=?");
    if (filter == SourceFilter::ReferenceFiles) select += QStringLiteral(" AND source IN ('file','external-subtitle')");
    if (filter == SourceFilter::Asr) select += QStringLiteral(" AND source='generated-asr'");
    select += QStringLiteral(" ORDER BY relative_path,id LIMIT ?");
    query.prepare(select);
    query.addBindValue(expected.id);
    query.addBindValue(WorkReferenceMaximumFiles + 1);
    if (!query.exec()) { result.error = query.lastError().text(); return result; }
    qint64 jsonBytes = 0, textBytes = 0;
    while (query.next()) {
        if (result.records.size() >= WorkReferenceMaximumFiles) {
            result.error = QStringLiteral("作品资料记录超过 1000 份限制。"); return result;
        }
        const auto length = query.value(13).toLongLong();
        const bool singleOversized = length < 0 || length > MaximumJsonBytes;
        const bool aggregateOversized = !singleOversized && length > MaximumSnapshotJsonBytes - jsonBytes;
        const bool oversized = singleOversized || aggregateOversized;
        if ((aggregateOversized && contextRead) || (!contextRead && (length < 0 || (oversized && !repairing)))) {
            result.error = QStringLiteral("作品资料缓存超过读取限制。"); return result;
        }
        if (!oversized) jsonBytes += length;
        WorkReferenceRecord record;
        record.id = query.value(0).toLongLong(); record.workId = expected.id;
        record.relativePath = query.value(1).toString(); record.format = query.value(3).toString();
        record.source = query.value(4).toString(); record.audioRelativePath = query.value(5).toString();
        record.completedAt = QDateTime::fromString(query.value(6).toString(), Qt::ISODateWithMs);
        record.sourceHash = query.value(7).toString(); record.encoding = query.value(8).toString();
        record.encodingInferred = query.value(9).toInt() != 0;
        record.extractorVersion = query.value(10).toString(); record.contentVersion = query.value(11).toString();
        record.state = query.value(14).toString(); record.error = query.value(15).toString();
        record.warning = query.value(16).toString(); record.missing = query.value(17).toInt() != 0;
        const bool validState = record.state == QStringLiteral("ready") || record.state == QStringLiteral("no-text")
            || record.state == QStringLiteral("failed") || record.state == QStringLiteral("missing");
        const bool validSource = record.source == QStringLiteral("file") || record.source == QStringLiteral("external-subtitle")
            || record.source == QStringLiteral("generated-asr");
        const bool invalidIdentity = record.id <= 0 || !safeRelative(record.relativePath) || !validState || !validSource
            || query.value(2).toString() != key(record.relativePath)
            || (!record.audioRelativePath.isEmpty() && !safeRelative(record.audioRelativePath))
            || record.missing != (record.state == QStringLiteral("missing"))
            || (record.source == QStringLiteral("generated-asr")
                && (record.audioRelativePath.isEmpty() || !record.completedAt.isValid()));
        if (invalidIdentity) {
            if (!contextRead) { result.error = QStringLiteral("作品资料缓存含有无效身份或路径。"); return result; }
            record.state = QStringLiteral("failed"); record.error = QStringLiteral("资料缓存身份或路径无效。");
            record.contentVersion.clear(); result.records.append(std::move(record)); continue;
        }
        QByteArray serialized;
        if (!oversized) {
            QSqlQuery bodyQuery(db);
            bodyQuery.prepare(QStringLiteral("SELECT content_json FROM work_references WHERE id=? AND work_id=?"));
            bodyQuery.addBindValue(record.id); bodyQuery.addBindValue(expected.id);
            if (!bodyQuery.exec() || !bodyQuery.next()) {
                result.error = bodyQuery.lastError().isValid() ? bodyQuery.lastError().text()
                    : QStringLiteral("作品资料正文不存在。"); return result;
            }
            serialized = bodyQuery.value(0).toByteArray();
            if (serialized.size() != length) {
                if (!contextRead) { result.error = QStringLiteral("作品资料缓存长度无效。"); return result; }
                record.state = QStringLiteral("failed"); record.error = QStringLiteral("资料缓存长度无效。");
                record.contentVersion.clear(); result.records.append(std::move(record)); continue;
            }
        }
        const auto document = QJsonDocument::fromJson(serialized);
        if (document.isObject()) record.content = document.object();
        if (oversized || ((record.state == QStringLiteral("ready") || record.state == QStringLiteral("no-text"))
            && (!document.isObject() || !validWorkReferenceCache(record)))) {
            // Keep this record discoverable so the next sync can repair it, but never expose invalid text as ready.
            record.state = QStringLiteral("failed");
            record.error = QStringLiteral("资料缓存损坏，请更新资料或重新提取。");
            record.content = {}; record.contentVersion.clear();
        }
        if (record.missing || record.state == QStringLiteral("failed") || record.state == QStringLiteral("missing"))
            record.content = {};
        if (contextRead && filter == SourceFilter::Asr) {
            QSqlQuery files(db);
            files.prepare(QStringLiteral("SELECT s.id,a.id FROM work_files s JOIN work_files a ON a.work_id=s.work_id "
                "WHERE s.work_id=? AND s.path_key=? AND s.type=1 AND s.missing=0 "
                "AND a.path_key=? AND a.type=0 AND a.missing=0"));
            files.addBindValue(expected.id); files.addBindValue(key(record.relativePath));
            files.addBindValue(key(record.audioRelativePath));
            if (!files.exec()) { result.error = files.lastError().text(); return result; }
            if (files.next()) {
                record.subtitleFileId = files.value(0).toLongLong(); record.audioFileId = files.value(1).toLongLong();
            } else {
                record.state = QStringLiteral("failed"); record.error = QStringLiteral("ASR 字幕或原音轨未入库或已缺失。");
                record.content = {}; record.contentVersion.clear();
            }
        }
        const auto size = bodyBytes(record.content);
        if (size > WorkReferenceWorkBytes - textBytes) {
            if (!repairing) { result.error = QStringLiteral("作品资料正文超过 32 MiB 限制。"); return result; }
            record.state = QStringLiteral("failed");
            record.error = QStringLiteral("资料缓存超过正文限制，请重新提取。");
            record.content = {}; record.contentVersion.clear();
        } else {
            textBytes += size;
        }
        result.records.append(std::move(record));
    }
    if (query.lastError().isValid()) { result.error = query.lastError().text(); return result; }
    result.success = true;
    return result;
}
}

WorkReferenceRepository::WorkReferenceRepository(QString databasePath)
    : m_databasePath(std::move(databasePath)) {}

WorkReferenceSnapshot WorkReferenceRepository::load(const LibraryWork &expected, bool referenceFilesOnly)
{
    BusinessDatabase connection(m_databasePath);
    WorkReferenceSnapshot result;
    if (!connection.open(&result.error)) return result;
    auto &db = connection.database();
    if (!db.transaction()) { result.error = db.lastError().text(); return result; }
    result = readSnapshot(db, expected, false, referenceFilesOnly ? SourceFilter::ReferenceFiles : SourceFilter::All);
    if (!result.success) { db.rollback(); return result; }
    if (!db.commit()) { result.success = false; result.error = db.lastError().text(); }
    return result;
}

WorkReferenceSnapshot WorkReferenceRepository::loadTranslationContext(const LibraryWork &expected,
    const QString &excludedReferencePath)
{
    BusinessDatabase connection(m_databasePath);
    WorkReferenceSnapshot result;
    if (!connection.open(&result.error)) return result;
    auto &db = connection.database();
    if (!db.transaction()) { result.error = db.lastError().text(); return result; }
    result = readSnapshot(db, expected, false, SourceFilter::ReferenceFiles, true);
    if (!result.success) { db.rollback(); return result; }
    bool ready = false;
    for (const auto &record : result.records) {
        if (key(record.relativePath) == key(excludedReferencePath)) continue;
        if (!record.missing && record.state == QStringLiteral("ready")) ready = true;
        else result.warnings.append(record.relativePath + QStringLiteral(": ")
            + (record.error.isEmpty() ? QStringLiteral("没有可用正文，未用作上下文。") : record.error));
    }
    if (!ready && result.scanned) {
        const auto warnings = result.warnings;
        result = readSnapshot(db, expected, false, SourceFilter::Asr, true);
        result.asrContext = true;
        result.warnings = warnings;
    }
    if (!result.success) { db.rollback(); return result; }
    if (!db.commit()) { result.success = false; result.error = db.lastError().text(); }
    return result;
}

WorkReferenceSyncResult WorkReferenceRepository::synchronize(
    const LibraryWork &expected, const DirectoryScanResult &scan, const std::atomic_bool &cancelled,
    WorkReferenceRefreshPolicy policy, const std::function<bool()> &beginCommit)
{
    WorkReferenceSyncResult result;
    result.status = WorkReferenceSyncStatus::Failed;
    const auto stop = [&]() {
        if (!cancelled.load()) return false;
        result.status = WorkReferenceSyncStatus::Cancelled;
        result.error = QStringLiteral("资料同步已取消，保留上次快照。"); return true;
    };
    if (stop()) return result;
    if (!scan.isComplete() || key(scan.rootDirectory) != key(expected.rootPath)) {
        result.error = QStringLiteral("只有当前作品的完整扫描结果才能同步资料。"); return result;
    }
    const auto selected = selectWorkReferences(scan, [&] { return cancelled.load(); });
    if (!selected.success) {
        result.status = selected.cancelled ? WorkReferenceSyncStatus::Cancelled : WorkReferenceSyncStatus::Failed;
        result.error = selected.error; return result;
    }
    result.selectedCount = selected.files.size();
    for (const auto &warning : selected.warnings) warn(result, warning);
    if (selected.files.size() > WorkReferenceMaximumFiles) {
        result.error = QStringLiteral("作品参考资料超过 1000 份限制，保留上次快照。"); return result;
    }
    BusinessDatabase connection(m_databasePath);
    if (!connection.open(&result.error)) return result;
    auto &db = connection.database();
    // This read transaction ends before any file/PDF work starts.
    if (!db.transaction()) { result.error = db.lastError().text(); return result; }
    const auto previous = readSnapshot(db, expected, true);
    if (!previous.success) { db.rollback(); result.error = previous.error; return result; }
    if (!db.commit()) { result.error = db.lastError().text(); return result; }
    QHash<QString, WorkReferenceRecord> previousByPath;
    for (const auto &record : previous.records) previousByPath.insert(key(record.relativePath), record);
    QSet<QString> selectedKeys, excludedKeys;
    for (const auto &path : selected.excludedPaths) {
        if (!safeRelative(path)) { result.error = QStringLiteral("资料筛选包含无效路径。"); return result; }
        excludedKeys.insert(key(path));
    }
    QVector<WorkReferenceRecord> prepared;
    qint64 bytes = 0, jsonBytes = 0;
    qsizetype readyCount = 0, failedCount = 0, reusedCount = 0, removedCount = 0;
    for (const auto &candidate : selected.files) {
        if (stop()) return result;
        const auto pathKey = key(candidate.relativePath);
        if (!safeRelative(candidate.relativePath) || selectedKeys.contains(pathKey) || excludedKeys.contains(pathKey)
            || key(candidate.absolutePath) != key(QDir(expected.rootPath).filePath(candidate.relativePath))) {
            result.error = QStringLiteral("资料筛选包含重复、冲突或越界路径。"); return result;
        }
        selectedKeys.insert(pathKey);
        const auto found = previousByPath.constFind(pathKey);
        auto record = extractWorkReference(candidate, cancelled,
            found == previousByPath.cend() ? nullptr : &found.value(),
            policy == WorkReferenceRefreshPolicy::ForceReextract);
        if (stop()) return result;
        record.workId = expected.id;
        record.relativePath = candidate.relativePath; record.format = candidate.format;
        record.source = candidate.source; record.audioRelativePath = candidate.audioRelativePath;
        record.completedAt = candidate.completedAt; record.warning = candidate.warning;
        record.missing = false;
        if (record.state != QStringLiteral("ready") && record.state != QStringLiteral("no-text")
            && record.state != QStringLiteral("failed")) {
            result.error = QStringLiteral("资料提取器返回了无效状态。"); return result;
        }
        if (record.state == QStringLiteral("failed")) {
            record.content = {}; record.contentVersion.clear();
            ++failedCount; warn(result, record.relativePath + QStringLiteral(": ") + record.error);
        } else {
            if (!validWorkReferenceCache(record)) { result.error = QStringLiteral("资料提取结果未通过完整性校验。"); return result; }
            if (record.state == QStringLiteral("ready")) ++readyCount;
        }
        if (record.cacheReused) ++reusedCount;
        const auto size = bodyBytes(record.content);
        const auto serializedBytes = QJsonDocument(record.content).toJson(QJsonDocument::Compact).size();
        if (size > WorkReferenceWorkBytes - bytes || serializedBytes > MaximumJsonBytes
            || serializedBytes > MaximumSnapshotJsonBytes - jsonBytes) {
            result.error = QStringLiteral("作品参考资料正文超过 32 MiB 或缓存资源限制，保留上次快照。"); return result;
        }
        bytes += size; jsonBytes += serializedBytes;
        prepared.append(std::move(record));
    }
    if (stop()) return result;
    if (beginCommit && !beginCommit()) {
        result.status = WorkReferenceSyncStatus::Cancelled;
        result.error = QStringLiteral("资料同步已取消，保留上次快照。"); return result;
    }
    SqliteWriteTransaction transaction(db, QStringLiteral("Synchronize work references"));
    if (!transaction.begin()) { result.error = transaction.errorMessage(); return result; }
    if (!identity(db, expected, &result.error)) return result;
    // Compare the committed scan generation too: another writer cannot silently replace a newer reference snapshot.
    {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("SELECT synchronized_at FROM work_reference_scans WHERE work_id=?"));
        query.addBindValue(expected.id);
        if (!query.exec()) { result.error = query.lastError().text(); return result; }
        const bool exists = query.next();
        if (exists != previous.scanned || (exists && query.value(0).toString() != previous.synchronizedAt)) {
            result.error = QStringLiteral("作品资料快照已更新，本次结果未覆盖较新资料。"); return result;
        }
    }
    // A complete scan replaces the reference list: remove absent or excluded rows,
    // including legacy missing records. Keep selected rows so their IDs remain stable.
    for (const auto &record : previous.records) {
        if (selectedKeys.contains(key(record.relativePath))) continue;
        QSqlQuery query(db);
        query.prepare(QStringLiteral("DELETE FROM work_references WHERE work_id=? AND id=?"));
        query.addBindValue(expected.id); query.addBindValue(record.id);
        if (!query.exec()) { result.error = query.lastError().text(); return result; }
        ++removedCount;
    }
    for (const auto &record : prepared) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral(
            "INSERT INTO work_references(work_id,relative_path,path_key,format,source,audio_relative_path,completed_at,"
            "source_hash,encoding,encoding_inferred,extractor_version,content_version,content_json,state,error,warning,missing) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0) ON CONFLICT(work_id,path_key) DO UPDATE SET "
            "relative_path=excluded.relative_path,format=excluded.format,source=excluded.source,audio_relative_path=excluded.audio_relative_path,"
            "completed_at=excluded.completed_at,source_hash=excluded.source_hash,encoding=excluded.encoding,encoding_inferred=excluded.encoding_inferred,"
            "extractor_version=excluded.extractor_version,content_version=excluded.content_version,content_json=excluded.content_json,"
            "state=excluded.state,error=excluded.error,warning=excluded.warning,missing=0"));
        query.addBindValue(expected.id);
        for (const auto &value : {record.relativePath, key(record.relativePath), record.format, record.source,
             record.audioRelativePath, record.completedAt.isValid() ? record.completedAt.toUTC().toString(Qt::ISODateWithMs) : QString(),
             record.sourceHash, record.encoding}) query.addBindValue(nonNull(value));
        query.addBindValue(record.encodingInferred ? 1 : 0);
        for (const auto &value : {record.extractorVersion, record.contentVersion,
             QString::fromUtf8(QJsonDocument(record.content).toJson(QJsonDocument::Compact)), record.state,
             record.error.left(4096), record.warning.left(4096)}) query.addBindValue(nonNull(value));
        if (!query.exec()) { result.error = query.lastError().text(); return result; }
    }
    {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("INSERT INTO work_reference_scans(work_id,root_key,imported_at_ms,synchronized_at) VALUES(?,?,?,?) "
            "ON CONFLICT(work_id) DO UPDATE SET root_key=excluded.root_key,imported_at_ms=excluded.imported_at_ms,synchronized_at=excluded.synchronized_at"));
        query.addBindValue(expected.id); query.addBindValue(key(expected.rootPath)); query.addBindValue(expected.importedAtMs);
        auto synchronizedAt = QDateTime::currentDateTimeUtc();
        const auto previousTime = QDateTime::fromString(previous.synchronizedAt, Qt::ISODateWithMs);
        if (previousTime.isValid() && synchronizedAt <= previousTime) synchronizedAt = previousTime.addMSecs(1);
        query.addBindValue(synchronizedAt.toString(Qt::ISODateWithMs));
        if (!query.exec()) { result.error = query.lastError().text(); return result; }
    }
    if (stop()) return result;
    if (!transaction.commit()) { result.error = transaction.errorMessage(); return result; }
    result.readyCount = readyCount; result.failedCount = failedCount;
    result.reusedCount = reusedCount; result.removedCount = removedCount;
    result.status = WorkReferenceSyncStatus::Succeeded;
    return result;
}

} // namespace asmr
