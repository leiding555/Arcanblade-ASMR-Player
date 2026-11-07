#include "ai/SubtitleWorkContext.h"
#include "ai/SubtitleTranslationDocument.h"
#include "library/BusinessDatabase.h"
#include "library/WorkReferenceRepository.h"
#include "library/SubtitleMatcher.h"
#include "playback/AudioFileValidation.h"
#include "subtitle/GeneratedSubtitleInfo.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QSet>
#include <algorithm>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace asmr {
namespace {
QString key(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded();
}
bool relative(const QString &path)
{
    if (path.isEmpty() || QDir::isAbsolutePath(path) || path.contains(QChar::Null)
        || path.contains(QLatin1Char('\\')) || path.contains(QLatin1Char(':'))
        || QDir::cleanPath(path) != path) return false;
    for (const auto &part : path.split(QLatin1Char('/')))
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")) return false;
    return true;
}
bool ordinary(QString path)
{
    if (!QDir::isAbsolutePath(path) || path.contains(QChar::Null)
        || path.startsWith(QStringLiteral("//?/")) || path.startsWith(QStringLiteral("//./"))) return false;
    bool leaf = true;
    for (;;) {
        const QFileInfo info(path);
        if (!info.exists() || (leaf ? !info.isFile() : !info.isDir()) || info.isSymLink() || info.isJunction()) return false;
#ifdef Q_OS_WIN
        const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
#endif
        const auto parent = info.absolutePath();
        if (parent == path) return true;
        path = parent; leaf = false;
    }
}
bool stop(const std::atomic_bool &cancelled, SubtitleWorkContext &result)
{
    if (!cancelled.load()) return false;
    result.error = QStringLiteral("作品上下文准备已取消。"); return true;
}
SubtitleWorkContext identity(const SubtitleTranslationRequest &request, const std::atomic_bool &cancelled,
    bool sharedSelection = false)
{
    SubtitleWorkContext result;
    if (stop(cancelled, result)) return result;
    if (request.workId < 0 || request.audioFileId < 0 || request.subtitleFileId < 0
        || request.databasePath.isEmpty() || !QDir::isAbsolutePath(request.databasePath)
        || (!request.sourcePath.isEmpty() && !QDir::isAbsolutePath(request.sourcePath))
        || (request.sourcePath.isEmpty() && (request.workId == 0 || request.subtitleFileId == 0))) {
        result.error = QStringLiteral("作品上下文需要有效的在库字幕身份和数据库路径。"); return result;
    }
    BusinessDatabase connection(request.databasePath);
    if (!connection.open(&result.error)) return result;
    auto &db = connection.database();
    if (!db.transaction()) { result.error = db.lastError().text(); return result; }
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT w.id,w.root_path,w.display_name,w.imported_at_ms,f.id,f.relative_path,f.file_name,f.extension "
        "FROM works w JOIN work_files f ON f.work_id=w.id WHERE f.type=1 AND f.missing=0 "
        "AND (?=0 OR w.id=?) AND (?=0 OR f.id=?) AND (?='' OR "
        "w.root_key || CASE WHEN substr(w.root_key,-1)='/' THEN '' ELSE '/' END || f.path_key=?) LIMIT 2"));
    query.addBindValue(request.workId); query.addBindValue(request.workId);
    query.addBindValue(request.subtitleFileId); query.addBindValue(request.subtitleFileId);
    const QString sourceKey = request.sourcePath.isEmpty() ? QStringLiteral("") : key(request.sourcePath);
    query.addBindValue(sourceKey); query.addBindValue(sourceKey);
    if (!query.exec()) { result.error = query.lastError().text(); return result; }
    if (!query.next()) { result.error = QStringLiteral("字幕未入库、已缺失或作品身份已变化，不能使用作品上下文。"); return result; }
    result.work = {query.value(0).toLongLong(), query.value(1).toString(), query.value(2).toString(), query.value(3).toLongLong()};
    result.subtitle = {query.value(4).toLongLong(), result.work.id, query.value(5).toString(), query.value(6).toString(),
        query.value(7).toString(), LibraryFileType::Subtitle, false};
    if (query.next()) { result.error = QStringLiteral("字幕同时属于多个在库作品，请从字幕管理明确选择所属作品。"); return result; }
    if (stop(cancelled, result)) return result;
    if (!relative(result.subtitle.relativePath)) { result.error = QStringLiteral("在库字幕路径无效。"); return result; }
    const auto subtitlePath = QDir(result.work.rootPath).filePath(result.subtitle.relativePath);
    if (!ordinary(subtitlePath)) { result.error = QStringLiteral("在库字幕不存在、不可访问或包含链接。"); return result; }
    const auto generated = readGeneratedSubtitleInfo(subtitlePath);
    if (!generated || generated->mode != QStringLiteral("offline-asr-ja") || generated->language != QStringLiteral("ja")) {
        result.error = QStringLiteral("只有生成记录确认为日语 ASR 的在库字幕可使用作品上下文。"); return result;
    }
    const auto audioRelative = QDir::cleanPath(QDir(QFileInfo(result.subtitle.relativePath).path())
        .filePath(QStringLiteral("../") + generated->audioFileName));
    if (!relative(audioRelative)) { result.error = QStringLiteral("ASR 生成记录中的原音轨路径无效。"); return result; }
    query.prepare(QStringLiteral("SELECT id,relative_path,file_name,extension FROM work_files WHERE work_id=? AND path_key=? AND type=0 AND missing=0"));
    query.addBindValue(result.work.id); query.addBindValue(key(audioRelative));
    if (!query.exec()) { result.error = query.lastError().text(); return result; }
    if (!query.next()) { result.error = QStringLiteral("ASR 生成记录对应的原音轨未入库或已缺失。"); return result; }
    result.audio = {query.value(0).toLongLong(), result.work.id, query.value(1).toString(), query.value(2).toString(),
        query.value(3).toString(), LibraryFileType::Audio, false};
    const auto matchesSelection = [&](const LibraryFile &audio) {
        return (request.audioFileId == 0 || request.audioFileId == audio.id)
            && (request.audioPath.isEmpty() || key(request.audioPath) == key(QDir(result.work.rootPath).filePath(audio.relativePath)))
            && (request.libraryRelativePath.isEmpty() || key(request.libraryRelativePath) == key(audio.relativePath));
    };
    bool validSelection = matchesSelection(result.audio);
    if (!validSelection && sharedSelection) {
        QVector<LibraryFile> files;
        query.prepare(QStringLiteral("SELECT id,relative_path,file_name,extension,type,missing FROM work_files WHERE work_id=?"));
        query.addBindValue(result.work.id);
        if (!query.exec()) { result.error = query.lastError().text(); return result; }
        while (query.next()) files.append({query.value(0).toLongLong(), result.work.id, query.value(1).toString(),
            query.value(2).toString(), query.value(3).toString(), static_cast<LibraryFileType>(query.value(4).toInt()),
            query.value(5).toBool()});
        if (query.lastError().isValid()) { result.error = query.lastError().text(); return result; }
        const auto partner = SubtitleMatcher::sharedAudioPartners(result.work, files).value(result.audio.id);
        const auto selected = std::find_if(files.cbegin(), files.cend(), [&](const auto &file) {
            return file.id == partner && matchesSelection(file);
        });
        if (partner > 0 && selected != files.cend()) {
            const auto checked = validateLibraryAudioFile(result.work.rootPath, selected->relativePath);
            validSelection = checked.success;
        }
    }
    if (!validSelection || (!request.libraryRoot.isEmpty() && key(request.libraryRoot) != key(result.work.rootPath))) {
        result.error = QStringLiteral("所选音轨与 ASR 生成记录不一致，绑定关系不能代替来源身份。"); return result;
    }
    const auto audio = validateLibraryAudioFile(result.work.rootPath, result.audio.relativePath);
    if (!audio.success) { result.error = audio.errorMessage; return result; }
    if (!db.commit()) { result.error = db.lastError().text(); return result; }
    stop(cancelled, result);
    return result;
}
QJsonArray lines(const QString &text, const std::atomic_bool &cancelled)
{
    QJsonArray result;
    int line = 1;
    for (const auto &value : text.split(QLatin1Char('\n'), Qt::KeepEmptyParts)) {
        if (cancelled.load()) return {};
        result.append(QJsonObject{{QStringLiteral("line"), line++}, {QStringLiteral("text"), value}});
    }
    return result;
}
QJsonObject located(const QJsonObject &content, const std::atomic_bool &cancelled)
{
    if (content.contains(QStringLiteral("text")))
        return {{QStringLiteral("lines"), lines(content.value(QStringLiteral("text")).toString(), cancelled)}};
    QJsonArray pages;
    for (const auto &value : content.value(QStringLiteral("pages")).toArray()) {
        const auto page = value.toObject();
        pages.append(QJsonObject{{QStringLiteral("page"), page.value(QStringLiteral("page"))},
            {QStringLiteral("lines"), lines(page.value(QStringLiteral("text")).toString(), cancelled)}});
        if (cancelled.load()) return {};
    }
    return pages.isEmpty() ? QJsonObject{{QStringLiteral("cues"), content.value(QStringLiteral("cues"))}}
        : QJsonObject{{QStringLiteral("pages"), pages}};
}
}

static SubtitleWorkContext resolveContext(const SubtitleTranslationRequest &request,
    const std::atomic_bool &cancelled, bool includeReferences, bool sharedSelection)
{
    auto result = identity(request, cancelled, sharedSelection);
    if (!result.succeeded()) return result;
    const auto snapshot = WorkReferenceRepository(request.databasePath).loadTranslationContext(result.work, result.subtitle.relativePath);
    if (!snapshot.success) { result.error = snapshot.error; return result; }
    if (!snapshot.scanned) { result.error = QStringLiteral("作品尚未扫描参考资料，请先更新作品。"); return result; }
    result.warnings = snapshot.warnings;
    result.contextKind = snapshot.asrContext ? SubtitleWorkContextKind::Asr : SubtitleWorkContextKind::ReferenceFiles;
    auto records = snapshot.records;
    std::sort(records.begin(), records.end(), [](const auto &left, const auto &right) { return left.relativePath < right.relativePath; });
    QJsonArray versions;
    if (snapshot.asrContext) {
        // Cache sync normally stores one version per track. Rank again to preserve
        // the no-fallback rule for legacy or independently restored snapshots.
        QHash<QString, QString> latest;
        QSet<QString> unsortableTracks;
        for (const auto &record : records) {
            if (record.audioRelativePath.isEmpty()) continue;
            const auto audioKey = key(record.audioRelativePath);
            if (!record.completedAt.isValid()) { unsortableTracks.insert(audioKey); continue; }
            const auto previousPath = latest.value(audioKey);
            const auto previous = std::find_if(records.cbegin(), records.cend(), [&](const auto &value) { return value.relativePath == previousPath; });
            if (previous == records.cend() || record.completedAt > previous->completedAt
                || (record.completedAt == previous->completedAt && record.relativePath < previous->relativePath))
                latest.insert(audioKey, record.relativePath);
        }
        for (const auto &record : records) {
            if (stop(cancelled, result)) return result;
            const bool target = key(record.relativePath) == key(result.subtitle.relativePath);
            const bool blocked = unsortableTracks.contains(key(record.audioRelativePath));
            const bool selected = !blocked && latest.value(key(record.audioRelativePath)) == record.relativePath;
            const bool available = !record.missing && record.state == QStringLiteral("ready")
                && record.subtitleFileId > 0 && record.audioFileId > 0;
            const auto disposition = blocked ? QStringLiteral("unsortable-track") : !selected ? QStringLiteral("superseded") : target
                ? QStringLiteral("target-source") : available ? QStringLiteral("included") : QStringLiteral("excluded");
            versions.append(QJsonObject{{QStringLiteral("id"), record.id},
                {QStringLiteral("subtitle_file_id"), record.subtitleFileId}, {QStringLiteral("audio_file_id"), record.audioFileId},
                {QStringLiteral("relative_path"), record.relativePath}, {QStringLiteral("audio_relative_path"), record.audioRelativePath},
                {QStringLiteral("completed_at"), record.completedAt.toUTC().toString(Qt::ISODateWithMs)},
                {QStringLiteral("source_hash"), record.sourceHash}, {QStringLiteral("content_version"), record.contentVersion},
                {QStringLiteral("state"), record.state}, {QStringLiteral("disposition"), disposition}});
            if (!selected) {
                if (blocked && !record.completedAt.isValid())
                    result.warnings.append(record.relativePath + QStringLiteral(": ASR 完成时间无效，已排除该音轨的辅助字幕且不回退旧版。"));
                else if (record.audioRelativePath.isEmpty())
                    result.warnings.append(record.relativePath + QStringLiteral(": ")
                        + (record.error.isEmpty() ? QStringLiteral("ASR 缓存音轨身份或完成时间无效，已排除。") : record.error));
                continue;
            }
            if (target) continue;
            if (!available) {
                result.warnings.append(record.relativePath + QStringLiteral(": ")
                    + (record.error.isEmpty() ? QStringLiteral("ASR 没有可用正文，已排除且不回退旧版。") : record.error));
                continue;
            }
            if (!record.warning.isEmpty()) result.warnings.append(record.warning);
            else if (std::any_of(records.cbegin(), records.cend(), [&](const auto &other) {
                return other.relativePath != record.relativePath && key(other.audioRelativePath) == key(record.audioRelativePath)
                    && other.completedAt == record.completedAt;
            })) result.warnings.append(QStringLiteral("ASR 完成时间并列，按相对路径固定选择：%1").arg(record.relativePath));
            if (includeReferences) result.asrSubtitles.append(QJsonObject{{QStringLiteral("id"), record.id},
                {QStringLiteral("subtitle_file_id"), record.subtitleFileId}, {QStringLiteral("audio_file_id"), record.audioFileId},
                {QStringLiteral("relative_path"), record.relativePath}, {QStringLiteral("audio_relative_path"), record.audioRelativePath},
                {QStringLiteral("completed_at"), record.completedAt.toUTC().toString(Qt::ISODateWithMs)},
                {QStringLiteral("source_hash"), record.sourceHash}, {QStringLiteral("content_version"), record.contentVersion},
                {QStringLiteral("format"), record.format}, {QStringLiteral("content"), located(record.content, cancelled)}});
        }
        if (stop(cancelled, result)) return result;
        const QJsonObject snapshotIdentity{{QStringLiteral("version"), QStringLiteral("work-asr-context-v1")},
            {QStringLiteral("contextKind"), QStringLiteral("asr")}, {QStringLiteral("workId"), result.work.id},
            {QStringLiteral("root"), key(result.work.rootPath)}, {QStringLiteral("importedAtMs"), result.work.importedAtMs},
            {QStringLiteral("title"), result.work.displayName}, {QStringLiteral("targetSubtitleFileId"), result.subtitle.id},
            {QStringLiteral("asr"), versions}};
        result.referenceDigest = QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(snapshotIdentity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
        return result;
    }
    for (const auto &record : records) {
        if (stop(cancelled, result)) return result;
        if (record.missing || record.state != QStringLiteral("ready")
            || (record.source != QStringLiteral("file") && record.source != QStringLiteral("external-subtitle"))
            || key(record.relativePath) == key(result.subtitle.relativePath)) continue;
        versions.append(QJsonObject{{QStringLiteral("id"), record.id}, {QStringLiteral("relative_path"), record.relativePath},
            {QStringLiteral("source_hash"), record.sourceHash}, {QStringLiteral("content_version"), record.contentVersion},
            {QStringLiteral("source"), record.source}, {QStringLiteral("format"), record.format}});
        if (includeReferences) result.references.append(QJsonObject{{QStringLiteral("id"), record.id},
            {QStringLiteral("relative_path"), record.relativePath}, {QStringLiteral("format"), record.format},
            {QStringLiteral("source"), record.source}, {QStringLiteral("content_version"), record.contentVersion},
            {QStringLiteral("content"), located(record.content, cancelled)}});
    }
    if (stop(cancelled, result)) return result;
    if (versions.isEmpty()) { result.error = QStringLiteral("作品参考资料在准备时发生变化，请更新作品后重试。"); return result; }
    const QJsonObject snapshotIdentity{{QStringLiteral("workId"), result.work.id},
        {QStringLiteral("root"), key(result.work.rootPath)}, {QStringLiteral("importedAtMs"), result.work.importedAtMs},
        {QStringLiteral("title"), result.work.displayName}, {QStringLiteral("references"), versions}};
    result.referenceDigest = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(snapshotIdentity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    return result;
}

SubtitleWorkContext resolveSubtitleWorkContext(const SubtitleTranslationRequest &request,
    const std::atomic_bool &cancelled, bool includeReferences)
{
    return resolveContext(request, cancelled, includeReferences, false);
}

SubtitleWorkContext resolveSubtitleWorkContextSelection(const SubtitleTranslationRequest &request,
    const std::atomic_bool &cancelled, bool includeReferences)
{
    return resolveContext(request, cancelled, includeReferences, true);
}

bool revalidateSubtitleWorkContextIdentity(const SubtitleTranslationRequest &request, const LibraryWork &expected,
    const std::atomic_bool &cancelled, QString *error)
{
    const auto current = identity(request, cancelled);
    if (!current.succeeded() || current.work.id != expected.id || key(current.work.rootPath) != key(expected.rootPath)
        || current.work.importedAtMs != expected.importedAtMs) {
        if (error) *error = current.error.isEmpty() ? QStringLiteral("作品已删除或身份已变化，任务已停止。") : current.error;
        return false;
    }
    return true;
}
}
