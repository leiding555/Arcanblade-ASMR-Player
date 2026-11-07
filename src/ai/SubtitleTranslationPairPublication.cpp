#include "ai/SubtitleTranslationPairPublication.h"
#include "ai/AiSubtitleOutput.h"
#include "ai/SubtitleTranslationRepository.h"
#include "subtitle/GeneratedSubtitleInfo.h"
#include "subtitle/SrtParser.h"
#include "subtitle/SubtitleFileLoader.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QUuid>
#include <array>
#include <filesystem>
#include <system_error>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace asmr {
namespace {
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
QString key(const QString &path)
{
    const auto normalized = QDir::cleanPath(QDir::fromNativeSeparators(path));
#ifdef Q_OS_WIN
    return normalized.toCaseFolded();
#else
    return normalized;
#endif
}
bool safePath(QString path, bool required = true)
{
    if (path.isEmpty() || !QDir::isAbsolutePath(path) || path.contains(QChar::Null)) return false;
    path = QDir::cleanPath(path);
    bool leaf = true;
    for (;;) {
        const QFileInfo info(path);
        if (!leaf && info.exists() && !info.isDir()) return false;
#ifdef Q_OS_WIN
        const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
        } else {
            const auto code = GetLastError();
            if ((leaf && required) || (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)) return false;
        }
#else
        if (info.isSymLink() || info.isJunction() || (leaf && required && !info.exists())) return false;
#endif
        const auto parent = info.absolutePath();
        if (parent == path) return info.isDir();
        path = parent; leaf = false;
    }
}
bool missing(const QString &path)
{
    if (!safePath(path, false)) return false;
    std::error_code error;
#ifdef Q_OS_WIN
    const auto status = std::filesystem::symlink_status(std::filesystem::path(path.toStdWString()), error);
#else
    const auto status = std::filesystem::symlink_status(std::filesystem::path(QFile::encodeName(path).constData()), error);
#endif
    return status.type() == std::filesystem::file_type::not_found
        && (!error || error == std::errc::no_such_file_or_directory);
}
QString stamp(qint64 ms)
{
    return QStringLiteral("%1:%2:%3,%4").arg(ms / 3600000, 2, 10, QLatin1Char('0'))
        .arg(ms / 60000 % 60, 2, 10, QLatin1Char('0')).arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}
bool render(const SubtitleTranslationPlan &plan, const SubtitleTranslations &items,
    QByteArray *bytes, QString *error)
{
    if (!validateSubtitleTranslationData(plan, items, true, error)) return false;
    QString srt;
    for (int id = 1; id <= plan.entries.size(); ++id) {
        const auto &entry = plan.entries.at(id - 1);
        srt += QStringLiteral("%1\n%2 --> %3\n%4\n\n").arg(id)
            .arg(stamp(entry.startMs), stamp(entry.endMs), items.value(id));
        if (srt.size() > SubtitleFileLoader::MaxExpandedTextCodeUnits)
            return fail(error, QStringLiteral("生成字幕超过展开大小限制，未发布。"));
    }
    *bytes = srt.toUtf8();
    if (bytes->size() > SubtitleFileLoader::MaxFileSizeBytes)
        return fail(error, QStringLiteral("生成字幕超过 4 MiB，检查点保留。"));
    const auto parsed = SrtParser::parse(srt);
    if (parsed.hasErrors() || parsed.entries.size() != plan.entries.size())
        return fail(error, QStringLiteral("成对字幕结构校验失败。"));
    for (int i = 0; i < parsed.entries.size(); ++i)
        if (parsed.entries.at(i).startMs != plan.entries.at(i).startMs
            || parsed.entries.at(i).endMs != plan.entries.at(i).endMs
            || parsed.entries.at(i).text != items.value(i + 1))
            return fail(error, QStringLiteral("成对字幕改变了本地时间轴或正文，未发布。"));
    return true;
}
QString sha(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
bool matchesFile(const QString &path, const QByteArray &expected)
{
    if (!safePath(path) || !QFileInfo(path).isFile() || QFileInfo(path).size() != expected.size()) return false;
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && !file.isSequential()
        && file.read(expected.size() + 1) == expected && file.error() == QFileDevice::NoError && file.atEnd();
}
bool writeNew(const QString &path, const QByteArray &bytes)
{
    if (!safePath(QFileInfo(path).absolutePath()) || !missing(path)) return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return false;
    if (file.write(bytes) == bytes.size() && file.flush()) return true;
    file.close(); QFile::remove(path); return false;
}
QJsonObject details(const SubtitleTranslationPlan &plan, const QDateTime &completed)
{
    QJsonObject result{{QStringLiteral("owner"), QStringLiteral("Arcanblade.SubtitleTranslationResult.v1")},
        {QStringLiteral("version"), 1}, {QStringLiteral("fingerprint"), plan.fingerprint},
        {QStringLiteral("contextGroup"), plan.contextGroup},
        {QStringLiteral("phase"), plan.phase == SubtitleTranslationPhase::Correction ? QStringLiteral("correction") : QStringLiteral("translation")},
        {QStringLiteral("sourceSha256"), QString::fromLatin1(plan.sourceSha256.toHex())},
        {QStringLiteral("referenceDigest"), plan.referenceDigest},
        {QStringLiteral("subtitleFileName"), plan.outputFileName},
        {QStringLiteral("provider"), translationApiProviderKey(plan.request.settings.provider)},
        {QStringLiteral("model"), plan.request.settings.model},
        {QStringLiteral("completedUtc"), completed.toString(Qt::ISODateWithMs)}};
    if (plan.contextKind == SubtitleWorkContextKind::Asr)
        result.insert(QStringLiteral("contextKind"), QStringLiteral("asr"));
    return result;
}
bool matchesDetails(const QString &path, const QJsonObject &expected)
{
    if (!safePath(path) || !QFileInfo(path).isFile() || QFileInfo(path).size() > 64 * 1024) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.isSequential()) return false;
    const auto bytes = file.read(64 * 1024 + 1);
    if (bytes.size() > 64 * 1024 || !file.atEnd() || file.error() != QFileDevice::NoError) return false;
    const auto object = QJsonDocument::fromJson(bytes).object();
    for (auto it = expected.begin(); it != expected.end(); ++it)
        if (it.key() != QStringLiteral("completedUtc") && object.value(it.key()) != it.value()) return false;
    return QDateTime::fromString(object.value(QStringLiteral("completedUtc")).toString(), Qt::ISODateWithMs).isValid();
}
bool matchingGenerated(const QString &path, const GeneratedSubtitleInfo &expected)
{
    const auto found = readGeneratedSubtitleInfo(path);
    return found && found->mode == expected.mode && found->language == expected.language
        && found->subtitleFileName == expected.subtitleFileName && found->audioFileName == expected.audioFileName
        && found->translationService == expected.translationService && found->translationModel == expected.translationModel;
}
bool moveNew(const QString &source, const QString &destination)
{
    if (!safePath(source) || !missing(destination)) return false;
#ifdef Q_OS_WIN
    return MoveFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(source).utf16()),
        reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(destination).utf16())) != 0;
#else
    return QFile::rename(source, destination);
#endif
}
struct Artifact {
    const SubtitleTranslationPlan *plan = nullptr;
    QByteArray bytes;
    QString path, hash, state, staged;
};
}

AiSubtitleTaskResult publishSubtitleTranslationPair(const SubtitleTranslationPlan &correction,
    const SubtitleTranslations &corrected, const SubtitleTranslationPlan &translation,
    const SubtitleTranslations &translated, const std::atomic_bool &cancelled)
{
    AiSubtitleTaskResult result;
    result.audioPath = correction.request.audioPath; result.taskDirectory = correction.checkpointDirectory;
    result.mode = QStringLiteral("cloud-translation"); result.language = QStringLiteral("zh");
    result.warnings = correction.warnings;
    const auto failure = [&](const QString &message) {
        result.state = cancelled.load() ? AiSubtitleTaskState::Cancelled : AiSubtitleTaskState::Failed;
        result.error = cancelled.load() ? QStringLiteral("已取消字幕发布，两阶段检查点保留。") : message;
        result.srtPath.clear(); result.correctedJapanesePath.clear(); return result;
    };
    QString error;
    if (!correction.request.useWorkContext || !translation.request.useWorkContext
        || correction.contextGroup != correction.fingerprint || translation.contextGroup != correction.contextGroup
        || correction.phase != SubtitleTranslationPhase::Correction || translation.phase != SubtitleTranslationPhase::Translation
        || correction.contextKind != translation.contextKind
        || key(correction.outputBaseDirectory) != key(translation.outputBaseDirectory)
        || correction.request.outputBatchId != translation.request.outputBatchId
        || correction.entries.size() != translation.entries.size())
        return failure(QStringLiteral("成对发布计划不属于同一上下文任务。"));
    for (int i = 0; i < correction.entries.size(); ++i)
        if (translation.entries.at(i).text != corrected.value(i + 1)
            || correction.entries.at(i).startMs != translation.entries.at(i).startMs
            || correction.entries.at(i).endMs != translation.entries.at(i).endMs)
            return failure(QStringLiteral("中文阶段未使用完整日语校正稿，未发布。"));
    std::array<Artifact, 2> artifacts{{Artifact{&correction}, Artifact{&translation}}};
    if (!render(correction, corrected, &artifacts[0].bytes, &error)
        || !render(translation, translated, &artifacts[1].bytes, &error)
        || !revalidateSubtitleTranslationSource(correction, cancelled, &error)
        || !revalidateSubtitleTranslationSource(translation, cancelled, &error)) return failure(error);
    if (!safePath(correction.outputBaseDirectory) || !QFileInfo(correction.outputBaseDirectory).isDir())
        return failure(QStringLiteral("成对字幕输出目录不可用或包含链接。"));
    if (!saveSubtitleTranslationCheckpoint(correction, corrected, cancelled, &error)
        || !saveSubtitleTranslationCheckpoint(translation, translated, cancelled, &error)) return failure(error);
    SubtitleTranslationRepository repository(correction.request.databasePath);
    for (auto &artifact : artifacts) {
        if (!repository.publication(*artifact.plan, &artifact.path, &artifact.hash, &artifact.state, &error)) return failure(error);
        const auto contentHash = sha(artifact.bytes);
        if (!artifact.path.isEmpty() && artifact.hash != contentHash)
            return failure(QStringLiteral("已有发布意图与当前字幕正文冲突，未覆盖文件。"));
        artifact.hash = contentHash;
    }
    if (artifacts[0].path.isEmpty() != artifacts[1].path.isEmpty()
        || (!artifacts[0].path.isEmpty() && artifacts[0].state != artifacts[1].state))
        return failure(QStringLiteral("成对字幕的发布记录不一致，检查点保留。"));
    QString directory;
    if (!artifacts[0].path.isEmpty()) {
        directory = QFileInfo(artifacts[0].path).absolutePath();
        for (const auto &artifact : artifacts) {
            const QFileInfo finalFile(artifact.path), outputDirectory(finalFile.absolutePath());
            if (finalFile.fileName() != artifact.plan->outputFileName
                || key(finalFile.absolutePath()) != key(directory)
                || key(outputDirectory.absolutePath()) != key(correction.outputBaseDirectory)
                || !outputDirectory.fileName().startsWith(QStringLiteral("deepseek_translate_"))
                || !safePath(artifact.path, false))
                return failure(QStringLiteral("已有成对发布目录与当前任务不匹配。"));
        }
    } else {
        QString batch = correction.request.outputBatchId;
        if (batch.isEmpty()) batch = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz_"))
            + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
        directory = ensureAiSubtitleBatchDirectory(correction.outputBaseDirectory, QStringLiteral("deepseek_translate_"), batch, &error);
        if (directory.isEmpty()) return failure(error);
        for (auto &artifact : artifacts) artifact.path = QDir(directory).filePath(artifact.plan->outputFileName);
        for (const auto &artifact : artifacts)
            if (!missing(artifact.path)) return failure(QStringLiteral("结果文件已存在或无法确认空缺，未覆盖任何字幕。"));
    }
    result.taskDirectory = directory;
    if (!safePath(directory) && missing(directory)
        && artifacts[0].state == QStringLiteral("publishing")) {
        const QString prefix = QStringLiteral("deepseek_translate_");
        const auto recreated = ensureAiSubtitleBatchDirectory(correction.outputBaseDirectory, prefix,
            QFileInfo(directory).fileName().mid(prefix.size()), &error);
        if (recreated.isEmpty() || key(recreated) != key(directory)) return failure(error);
    }
    const auto completed = QDateTime::currentDateTimeUtc();
    const auto cleanup = qScopeGuard([&] {
        for (const auto &artifact : artifacts)
            if (!artifact.staged.isEmpty() && safePath(artifact.staged)) QFile::remove(artifact.staged);
    });
    for (auto &artifact : artifacts) {
        if (cancelled.load()) return failure({});
        const bool exists = !missing(artifact.path);
        const auto expected = details(*artifact.plan, completed);
        const auto detailsPath = QDir(directory).filePath(QStringLiteral(".arcanblade-translation-") + artifact.plan->fingerprint + QStringLiteral(".json"));
        GeneratedSubtitleInfo generated{artifact.plan->phase == SubtitleTranslationPhase::Correction
                ? QStringLiteral("cloud-correction-ja") : QStringLiteral("cloud-translation"),
            completed, QFileInfo(correction.request.audioPath).fileName(), artifact.plan->outputFileName,
            artifact.plan->phase == SubtitleTranslationPhase::Correction ? QStringLiteral("ja") : QStringLiteral("zh")};
        generated.translationService = translationApiProviderKey(artifact.plan->request.settings.provider);
        generated.translationModel = artifact.plan->request.settings.model;
        if (exists) {
            if (artifact.state != QStringLiteral("publishing") && artifact.state != QStringLiteral("published"))
                return failure(QStringLiteral("未预留的结果文件已存在，未覆盖。"));
            if (!matchesFile(artifact.path, artifact.bytes) || !matchesDetails(detailsPath, expected)
                || !matchingGenerated(artifact.path, generated))
                return failure(QStringLiteral("已落盘字幕或来源信息发生变化，文件及检查点已保留。"));
            continue;
        }
        if (artifact.state == QStringLiteral("published"))
            return failure(QStringLiteral("此前成对发布的字幕已被删除，请先更新任务状态。"));
        if (QFileInfo::exists(detailsPath) ? !matchesDetails(detailsPath, expected)
            : !writeNew(detailsPath, QJsonDocument(expected).toJson(QJsonDocument::Compact)))
            return failure(QStringLiteral("无法保存成对字幕的来源信息，未覆盖文件。"));
        if (!writeGeneratedSubtitleInfo(directory, generated, &error, true)) return failure(error);
        artifact.staged = QDir(directory).filePath(QStringLiteral(".arcanblade-context-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".part"));
        if (!writeNew(artifact.staged, artifact.bytes)) return failure(QStringLiteral("无法暂存成对字幕，检查点保留。"));
    }
    if (!repository.reservePublicationPair(correction, artifacts[0].path, artifacts[0].hash,
        translation, artifacts[1].path, artifacts[1].hash, cancelled, &error)) return failure(error);
    for (auto &artifact : artifacts) {
        if (artifact.staged.isEmpty()) continue;
        if (!revalidateSubtitleTranslationSource(correction, cancelled, &error)
            || !safePath(directory) || !moveNew(artifact.staged, artifact.path))
            return failure(error.isEmpty() ? QStringLiteral("无法在不覆盖文件的情况下发布成对字幕，重试可补齐。") : error);
        artifact.staged.clear();
    }
    // Once both files exist, finish acknowledgement even if cancellation arrives late.
    const std::atomic_bool committed(false);
    if (!repository.confirmPublicationPair(correction, translation, committed, &error))
        return failure(QStringLiteral("两份字幕已保存；数据库确认失败，重试将核验并补齐确认。\n") + error);
    result.state = AiSubtitleTaskState::Succeeded; result.srtPath = artifacts[1].path;
    result.correctedJapanesePath = artifacts[0].path; result.subtitleCount = translation.entries.size();
    return result;
}
} // namespace asmr
