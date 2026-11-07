#include "library/SubtitleTranslationCleanup.h"
#include "library/BusinessDatabase.h"
#include "library/SqliteWriteTransaction.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <filesystem>
#include <system_error>
#include <utility>

namespace asmr {
namespace {
namespace fs = std::filesystem;
QString normalized(const QString &value) {
    auto path = QDir::cleanPath(QDir::fromNativeSeparators(value));
#ifdef Q_OS_WIN
    path = path.toCaseFolded();
#endif
    return path;
}
bool absolute(const QString &value) {
    return !value.isEmpty() && value == value.trimmed() && !value.contains(QChar::Null)
        && QDir::isAbsolutePath(value);
}
bool within(const QString &root, const QString &path) {
    const auto base = normalized(root), candidate = normalized(path);
    return candidate == base || candidate.startsWith(base.endsWith(QLatin1Char('/'))
        ? base : base + QLatin1Char('/'));
}
bool validHash(const QString &value) {
    static const QRegularExpression expression(QStringLiteral("^[0-9a-f]{64}$"));
    return expression.match(value).hasMatch();
}
fs::path nativePath(const QString &value) {
#ifdef Q_OS_WIN
    return fs::path(value.toStdWString());
#else
    return fs::path(QFile::encodeName(value).constData());
#endif
}
bool readableDirectory(const QString &path) {
    std::error_code error;
    const auto status = fs::symlink_status(nativePath(path), error);
    if (error || !fs::is_directory(status) || fs::is_symlink(status)) return false;
    const QFileInfo info(path);
    if (info.isSymbolicLink() || info.isJunction()) return false;
    fs::directory_iterator iterator(nativePath(path), error);
    (void)iterator;
    return !error;
}
bool safeAnchor(const QString &anchor) {
    if (!absolute(anchor) || !readableDirectory(anchor)) return false;
    auto current = QDir::cleanPath(anchor);
    while (true) {
        std::error_code error;
        const auto status = fs::symlink_status(nativePath(current), error);
        const QFileInfo info(current);
        if (error || !fs::is_directory(status) || fs::is_symlink(status)
            || info.isSymbolicLink() || info.isJunction()) return false;
        const auto parent = QDir::cleanPath(info.absolutePath());
        if (normalized(parent) == normalized(current)) return true;
        current = parent;
    }
}
struct Candidate {
    QString fingerprint, manifest, path, hash, anchor;
    QString contextGroup, phase;
    QJsonObject identity;
};
bool validCandidate(Candidate *value) {
    if (!validHash(value->fingerprint) || !validHash(value->hash) || !absolute(value->path)
        || value->manifest.toUtf8().size() > 16 * 1024 * 1024) return false;
    const auto document = QJsonDocument::fromJson(value->manifest.toUtf8());
    if (!document.isObject()) return false;
    const auto manifest = document.object();
    value->identity = manifest;
    if (manifest.contains(QStringLiteral("contextKind"))
        && manifest.value(QStringLiteral("contextKind")).toString() != QStringLiteral("asr")) return false;
    if (manifest.contains(QStringLiteral("contextGroup"))) {
        value->contextGroup = manifest.value(QStringLiteral("contextGroup")).toString();
        value->phase = manifest.value(QStringLiteral("phase")).toString();
        if (!validHash(value->contextGroup)
            || (value->phase != QStringLiteral("correction") && value->phase != QStringLiteral("translation"))) return false;
    }
    value->anchor = manifest.value(QStringLiteral("outputDirectory")).toString();
    const QFileInfo file(value->path), directory(file.absolutePath());
    return manifest.value(QStringLiteral("fingerprint")).toString() == value->fingerprint
        && absolute(value->anchor) && normalized(directory.absolutePath()) == normalized(value->anchor)
        && directory.fileName().startsWith(QStringLiteral("deepseek_translate_"))
        && file.fileName() == manifest.value(QStringLiteral("outputFileName")).toString();
}
bool confirmedMissing(const Candidate &value) {
    if (!safeAnchor(value.anchor)) return false;
    auto current = QDir::cleanPath(value.anchor);
    const auto relative = QDir(current).relativeFilePath(value.path);
    if (relative.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(relative)) return false;
    const auto parts = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (qsizetype index = 0; index < parts.size(); ++index) {
        current = QDir(current).filePath(parts[index]);
        std::error_code error;
        const auto status = fs::symlink_status(nativePath(current), error);
        if (status.type() == fs::file_type::not_found
            && (!error || error == std::errc::no_such_file_or_directory))
            return safeAnchor(value.anchor);
        const QFileInfo info(current);
        if (error || fs::is_symlink(status) || info.isSymbolicLink() || info.isJunction()) return false;
        if (index + 1 < parts.size() && !readableDirectory(current)) return false;
    }
    return false;
}
bool execute(QSqlQuery &query, QString *error) {
    if (query.exec()) return true;
    *error = query.lastError().text(); return false;
}
struct TaskReset { QString id, original, updated; QJsonObject data; };
bool findTaskResets(QSqlDatabase &database, const Candidate &candidate,
    QList<TaskReset> *resets, bool *runningReference, QString *error) {
    *runningReference = false;
    QSqlQuery tasks(database);
    if (!tasks.exec(QStringLiteral("SELECT task_id,data_json,updated_at,state FROM ai_tasks WHERE mode='cloud-translation'"))) {
        *error = tasks.lastError().text(); return false;
    }
    while (tasks.next()) {
        const auto json = tasks.value(1).toString();
        if (json.size() > 1024 * 1024) continue;
        const auto document = QJsonDocument::fromJson(json.toUtf8());
        if (!document.isObject()) continue;
        auto data = document.object();
        const auto resultPath = data.value(QStringLiteral("resultPath")).toString();
        const auto correctedPath = data.value(QStringLiteral("correctedJapanesePath")).toString();
        bool matches = (!resultPath.isEmpty() && normalized(resultPath) == normalized(candidate.path))
            || (!correctedPath.isEmpty() && normalized(correctedPath) == normalized(candidate.path))
            || (!candidate.contextGroup.isEmpty()
                && (data.value(QStringLiteral("contextGroup")).toString() == candidate.contextGroup
                    || data.value(QStringLiteral("correctionFingerprint")).toString() == candidate.contextGroup));
        if (!matches && resultPath.isEmpty()) {
            QSqlQuery latest(database);
            latest.prepare(QStringLiteral("SELECT parameters_json FROM ai_task_attempts WHERE task_id=? ORDER BY started_at DESC,attempt_id DESC LIMIT 1"));
            latest.addBindValue(tasks.value(0));
            if (!execute(latest, error)) return false;
            if (latest.next()) {
                const auto parameters = QJsonDocument::fromJson(latest.value(0).toString().toUtf8()).object();
                matches = parameters.value(QStringLiteral("fingerprint")).toString() == candidate.fingerprint
                    || (!candidate.contextGroup.isEmpty()
                        && (parameters.value(QStringLiteral("contextGroup")).toString() == candidate.contextGroup
                            || parameters.value(QStringLiteral("fingerprint")).toString() == candidate.contextGroup));
            }
        }
        if (!matches) continue;
        if (tasks.value(3).toString() == QStringLiteral("running")) {
            *runningReference = true;
            return true;
        }
        data.insert(QStringLiteral("resultPath"), QString());
        if (!candidate.contextGroup.isEmpty()) {
            for (const auto &field : {"correctedJapanesePath", "contextGroup", "correctionFingerprint",
                     "translationFingerprint", "referenceDigest"}) data.insert(QString::fromLatin1(field), QString());
        }
        data.insert(QStringLiteral("taskDirectory"), QString());
        data.insert(QStringLiteral("error"), QString());
        data.insert(QStringLiteral("warnings"), QJsonArray());
        data.insert(QStringLiteral("resultMissing"), true);
        resets->append({tasks.value(0).toString(), json, tasks.value(2).toString(), data});
    }
    if (tasks.lastError().isValid()) { *error = tasks.lastError().text(); return false; }
    return true;
}
bool matchingContextGroup(const QList<Candidate> &group)
{
    if (group.size() != 2) return false;
    const Candidate *correction = nullptr, *translation = nullptr;
    for (const auto &candidate : group) {
        if (candidate.phase == QStringLiteral("correction")) correction = &candidate;
        else if (candidate.phase == QStringLiteral("translation")) translation = &candidate;
    }
    if (!correction || !translation || correction->fingerprint != correction->contextGroup
        || correction->contextGroup != translation->contextGroup
        || correction->fingerprint == translation->fingerprint
        || normalized(QFileInfo(correction->path).absolutePath()) != normalized(QFileInfo(translation->path).absolutePath())
        || !validHash(translation->identity.value(QStringLiteral("correctedSourceHash")).toString())) return false;
    for (const auto &field : {"source", "sourceHash", "taskId", "referenceDigest", "contextKind", "workId", "audioFileId",
             "subtitleFileId", "workRoot", "workImportedAtMs", "scope", "model", "thinking"}) {
        const auto key = QString::fromLatin1(field);
        if (correction->identity.value(key) != translation->identity.value(key)) return false;
    }
    return validHash(correction->identity.value(QStringLiteral("referenceDigest")).toString())
        && validHash(correction->identity.value(QStringLiteral("sourceHash")).toString());
}
} // namespace

SubtitleTranslationCleanupResult cleanupMissingSubtitleTranslationResults(
    const QString &databasePath, const QString &scopeDirectory, const QString &fingerprint) {
    SubtitleTranslationCleanupResult result;
    if (!absolute(databasePath) || (!scopeDirectory.isEmpty() && !absolute(scopeDirectory))
        || (!fingerprint.isEmpty() && !validHash(fingerprint))) {
        result.error = QStringLiteral("字幕译文清理需要有效的数据库、扫描范围及翻译标识。");
        return result;
    }
    if (!QFileInfo::exists(databasePath)) return result;
    BusinessDatabase owner(databasePath);
    if (!owner.open(&result.error)) return result;
    auto &database = owner.database();
    QList<Candidate> candidates;
    QSqlQuery query(database);
    // Context siblings must be available even when a caller scopes this check to one fingerprint.
    query.prepare(QStringLiteral("SELECT fingerprint,manifest_json,result_path,result_sha256 FROM translation_checkpoints WHERE state='published'"));
    if (!execute(query, &result.error)) return result;
    while (query.next()) {
        Candidate candidate{query.value(0).toString(), query.value(1).toString(),
            query.value(2).toString(), query.value(3).toString(), {}};
        if (validCandidate(&candidate) && (scopeDirectory.isEmpty() || within(scopeDirectory, candidate.path)))
            candidates.append(std::move(candidate));
    }
    if (query.lastError().isValid()) { result.error = query.lastError().text(); return result; }
    query.finish();
    const QFileInfo databaseInfo(databasePath);
    const auto canonical = databaseInfo.canonicalFilePath();
    const auto lockBase = canonical.isEmpty() ? databaseInfo.absoluteFilePath() : canonical;
    QSet<QString> checked;
    for (const auto &candidate : candidates) {
        const auto groupKey = candidate.contextGroup.isEmpty() ? candidate.fingerprint : candidate.contextGroup;
        if (checked.contains(groupKey)) continue;
        QList<Candidate> group;
        if (candidate.contextGroup.isEmpty()) group.append(candidate);
        else for (const auto &sibling : candidates)
            if (sibling.contextGroup == candidate.contextGroup) group.append(sibling);
        checked.insert(groupKey);
        bool selected = fingerprint.isEmpty();
        for (const auto &member : group) selected = selected || member.fingerprint == fingerprint;
        if (!selected || (!candidate.contextGroup.isEmpty() && !matchingContextGroup(group))) continue;
        QLockFile lease(lockBase + QStringLiteral(".translation-") + groupKey + QStringLiteral(".lock"));
        lease.setStaleLockTime(0);
        const auto hasMissing = [&] {
            for (const auto &member : group) if (confirmedMissing(member)) return true;
            return false;
        };
        if (!lease.tryLock(0) || !hasMissing()) continue;
        // Filesystem checks run before the short CAS transaction. The same
        // translation lease prevents publication from racing this deletion.
        if (!hasMissing()) continue;
        SqliteWriteTransaction transaction(database, QStringLiteral("Remove missing published subtitle translation"));
        if (!transaction.begin()) { result.error = transaction.errorMessage(); return result; }
        QList<TaskReset> resets;
        bool runningReference = false;
        for (const auto &member : group) {
            QList<TaskReset> memberResets;
            bool memberRunning = false;
            if (!findTaskResets(database, member, &memberResets, &memberRunning, &result.error)) return result;
            runningReference = runningReference || memberRunning;
            for (const auto &reset : memberResets) {
                bool exists = false;
                for (const auto &previous : resets) if (previous.id == reset.id) { exists = true; break; }
                if (!exists) resets.append(reset);
            }
        }
        // The service can have released its translation lease before its owner
        // durably saves the final task state. Keep the entire checkpoint until
        // that running row settles; otherwise a late succeeded save would leave
        // a missing artifact with no publication record left to reconcile.
        if (runningReference) continue;
        bool removed = true;
        for (const auto &member : group) {
            QSqlQuery remove(database);
            remove.prepare(QStringLiteral("DELETE FROM translation_checkpoints WHERE fingerprint=? AND state='published' AND result_path=? AND result_sha256=? AND manifest_json=?"));
            remove.addBindValue(member.fingerprint); remove.addBindValue(member.path);
            remove.addBindValue(member.hash); remove.addBindValue(member.manifest);
            if (!execute(remove, &result.error)) return result;
            if (remove.numRowsAffected() != 1) { removed = false; break; }
        }
        if (!removed) continue; // RAII rolls back any earlier sibling deletion.
        QStringList resetIds;
        for (const auto &reset : resets) {
            QSqlQuery update(database);
            update.prepare(QStringLiteral("UPDATE ai_tasks SET state='pending',data_json=?,updated_at=? WHERE task_id=? AND mode='cloud-translation' AND state!='running' AND data_json=? AND updated_at=?"));
            update.addBindValue(QString::fromUtf8(QJsonDocument(reset.data).toJson(QJsonDocument::Compact)));
            update.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            update.addBindValue(reset.id); update.addBindValue(reset.original); update.addBindValue(reset.updated);
            if (!execute(update, &result.error)) return result;
            if (update.numRowsAffected() == 1) resetIds.append(reset.id);
        }
        if (!transaction.commit()) { result.error = transaction.errorMessage(); return result; }
        for (const auto &member : group) result.removedFingerprints.append(member.fingerprint);
        for (const auto &id : resetIds) if (!result.resetTaskIds.contains(id)) result.resetTaskIds.append(id);
    }
    return result;
}
} // namespace asmr
