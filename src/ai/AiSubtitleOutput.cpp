#include "ai/AiSubtitleOutput.h"
#include "playback/AudioFileValidation.h"
#include "subtitle/SubtitleFileLoader.h"
#include "subtitle/GeneratedSubtitleInfo.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QUuid>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace asmr {
namespace {
using Result = AiSubtitleTaskResult;
using State = AiSubtitleTaskState;
bool reparsePoint(const QString &path)
{
#ifdef Q_OS_WIN
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return QFileInfo(path).isSymLink();
#endif
}
bool safeAncestors(QString path)
{
    path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    for (;;) {
        if (reparsePoint(path))
            return false;
        const QString parent = QFileInfo(path).absolutePath();
        if (parent == path)
            return true;
        path = parent;
    }
}
Result fail(Result result, const QString &error, const std::atomic_bool &cancelled)
{
    result.state = cancelled.load() ? State::Cancelled : State::Failed;
    result.srtPath.clear(); // A failed/cancelled partial file is never offered for loading.
    result.error = cancelled.load() ? QStringLiteral("Subtitle task cancelled. Task logs were retained.") : error;
    return result;
}

} // namespace
bool validAiSubtitleOutputToken(const QString &token)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z0-9_-]{1,128}\\z"));
    return pattern.match(token).hasMatch();
}

QString ensureAiSubtitleBatchDirectory(const QString &baseDirectory,
    const QString &directoryPrefix, const QString &batchId, QString *error)
{
    if (error) error->clear();
    const auto failure = [&](const QString &message) {
        if (error) *error = message;
        return QString();
    };
    if (!QDir::isAbsolutePath(baseDirectory) || !QFileInfo(baseDirectory).isDir()
        || !safeAncestors(baseDirectory) || !validAiSubtitleOutputToken(batchId)
        || (directoryPrefix != QStringLiteral("offline_translate_")
            && directoryPrefix != QStringLiteral("offline_asr_")
            && directoryPrefix != QStringLiteral("deepseek_translate_")))
        return failure(QStringLiteral("Invalid or unavailable subtitle batch output directory."));
    const auto directory = QDir(baseDirectory).filePath(directoryPrefix + batchId);
    const bool created = QDir(baseDirectory).mkdir(directoryPrefix + batchId);
    if (!QFileInfo(directory).isDir() || !safeAncestors(directory))
        return failure(QStringLiteral("Could not create an ordinary subtitle batch directory."));
    const auto recordPath = QDir(directory).filePath(QStringLiteral(".arcanblade-batch.json"));
    const QJsonObject expected{{QStringLiteral("owner"), QStringLiteral("Arcanblade.SubtitleBatch.v1")},
        {QStringLiteral("version"), 1}, {QStringLiteral("batchId"), batchId},
        {QStringLiteral("directoryPrefix"), directoryPrefix}};
    if (created) {
        QFile record(recordPath);
        const auto bytes = QJsonDocument(expected).toJson(QJsonDocument::Compact);
        if (!safeAncestors(recordPath) || !record.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            QDir().rmdir(directory);
            return failure(QStringLiteral("Could not create subtitle batch ownership record."));
        }
        if (record.write(bytes) != bytes.size() || !record.flush()) {
            record.close();
            if (safeAncestors(directory)) { QFile::remove(recordPath); QDir().rmdir(directory); }
            return failure(QStringLiteral("Could not save subtitle batch ownership record."));
        }
        return directory;
    }
    QFile record(recordPath);
    if (!safeAncestors(recordPath) || !QFileInfo(recordPath).isFile()
        || !record.open(QIODevice::ReadOnly) || record.isSequential()
        || record.size() <= 0 || record.size() > 4096)
        return failure(QStringLiteral("Existing subtitle batch directory has no valid ownership record."));
    const auto bytes = record.read(4097);
    if (record.error() != QFileDevice::NoError || !record.atEnd()
        || QJsonDocument::fromJson(bytes).object() != expected)
        return failure(QStringLiteral("Existing subtitle batch directory belongs to another batch."));
    return directory;
}

AiSubtitleTaskResult validateAndPublishAiSubtitle(const AiSubtitleOutputRequest &request,
    AiSubtitleTaskResult result, const std::atomic_bool &cancelled,
    const std::function<void(const AiSubtitleTaskProgress &)> &report)
{
    QString error;
    if ((!request.outputBatchId.isEmpty() && !validAiSubtitleOutputToken(request.outputBatchId))
        || (!request.outputFileSuffix.isEmpty() && !validAiSubtitleOutputToken(request.outputFileSuffix)))
        return fail(result, QStringLiteral("Invalid subtitle output batch or filename suffix."), cancelled);
    report({State::Validating, QStringLiteral("Validating generated UTF-8 SRT with the current subtitle loader"), request.logTail});
    const QString expectedSrt = request.sourceSrt;
    const QFileInfo output(expectedSrt);
    if (!output.isFile() || output.size() <= 0 || !safeAncestors(expectedSrt))
        return fail(result, QStringLiteral("The engine did not produce the expected ordinary nonempty SRT file."), cancelled);
    const auto loaded = SubtitleFileLoader::load(expectedSrt, SubtitleFormat::Srt);
    if (cancelled.load())
        return fail(result, {}, cancelled);
    if (!loaded.succeeded())
        return fail(result, QStringLiteral("Generated SRT failed validation: %1").arg(loaded.errorMessage), cancelled);
    for (const auto &diagnostic : loaded.diagnostics) {
        if (diagnostic.severity == SubtitleLoadDiagnosticSeverity::Error)
            return fail(result, QStringLiteral("Generated SRT contains parser errors at line %1: %2")
                .arg(diagnostic.line).arg(diagnostic.message), cancelled);
        result.warnings.append(QStringLiteral("SRT line %1: %2").arg(diagnostic.line).arg(diagnostic.message));
    }
    for (const auto &entry : loaded.entries) {
        if (entry.startMs < 0 || entry.endMs <= entry.startMs || entry.text.trimmed().isEmpty())
            return fail(result, QStringLiteral("Generated SRT contains an invalid subtitle interval or empty text."), cancelled);
    }
    // Recheck the audio identity before publishing next to it. A library work
    // may have moved or disappeared while the external process was running.
    const bool libraryTrack = !request.libraryRoot.isEmpty() || !request.libraryRelativePath.isEmpty();
    const auto currentAudio = libraryTrack
        ? validateLibraryAudioFile(request.libraryRoot, request.libraryRelativePath)
        : validateAudioFile(request.audioPath);
    const QFileInfo currentInfo(request.audioPath);
    const QString audioDirectory = currentInfo.absolutePath();
    if (!currentAudio.success || !safeAncestors(audioDirectory) || !QFileInfo(audioDirectory).isDir())
        return fail(result, QStringLiteral("The audio directory changed or is no longer safely writable; validated SRT remains in the task directory."), cancelled);
    if (request.audioSize < 0 || !request.audioModified.isValid()
        || currentInfo.size() != request.audioSize || currentInfo.lastModified() != request.audioModified)
        return fail(result, QStringLiteral("The audio file changed during generation; validated SRT remains in the task directory and was not associated."), cancelled);
    if (cancelled.load())
        return fail(result, {}, cancelled);
    const QDateTime completedAt = QDateTime::currentDateTimeUtc();
    const QString directoryName = request.directoryPrefix
        + completedAt.toLocalTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    QString publishedDirectory;
    if (!request.outputBatchId.isEmpty()) {
        publishedDirectory = ensureAiSubtitleBatchDirectory(audioDirectory,
            request.directoryPrefix, request.outputBatchId, &error);
        if (publishedDirectory.isEmpty()) return fail(result, error, cancelled);
    }
    for (int attempt = 0; request.outputBatchId.isEmpty() && attempt < 4 && publishedDirectory.isEmpty(); ++attempt) {
        const QString name = directoryName + (attempt == 0 ? QString()
            : QLatin1Char('_') + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
        if (QDir(audioDirectory).mkdir(name))
            publishedDirectory = QDir(audioDirectory).filePath(name);
    }
    if (publishedDirectory.isEmpty())
        return fail(result, QStringLiteral("Could not create a result directory beside the audio. Check directory write permissions; validated SRT remains in %1.").arg(result.taskDirectory), cancelled);
    const QString subtitleName = QFileInfo(request.audioPath).completeBaseName()
        + (request.outputFileSuffix.isEmpty() ? QString() : QLatin1Char('_') + request.outputFileSuffix)
        + QLatin1Char('.') + request.language + QStringLiteral(".srt");
    const QString temporarySrt = QDir(publishedDirectory).filePath(QStringLiteral(".arcanblade-srt-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".part"));
    const QString recordPath = request.outputBatchId.isEmpty()
        ? QDir(publishedDirectory).filePath(QStringLiteral(".arcanblade-subtitle.json"))
        : generatedSubtitleInfoPath(publishedDirectory, subtitleName);
    bool ownTemporarySrt = false;
    bool ownRecord = false;
    bool published = false;
    const auto cleanup = qScopeGuard([&] {
        if (published || !safeAncestors(publishedDirectory))
            return;
        // Remove only artifacts created by this attempt, never a directory tree
        // or files that another process may have placed in this directory.
        if (ownTemporarySrt) QFile::remove(temporarySrt);
        if (ownRecord) QFile::remove(recordPath);
        if (request.outputBatchId.isEmpty()) QDir().rmdir(publishedDirectory);
    });
    if (!safeAncestors(publishedDirectory))
        return fail(result, QStringLiteral("Result directory became linked; no subtitle was published."), cancelled);
    QFile source(expectedSrt);
    QFile destination(temporarySrt);
    if (!safeAncestors(expectedSrt) || !source.open(QIODevice::ReadOnly)
        || !destination.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return fail(result, QStringLiteral("Could not stage the result beside the audio; validated SRT remains in %1.").arg(result.taskDirectory), cancelled);
    ownTemporarySrt = true;
    qint64 copied = 0;
    while (!source.atEnd()) {
        if (cancelled.load())
            return fail(result, {}, cancelled);
        const QByteArray block = source.read(64 * 1024);
        copied += block.size();
        if (source.error() != QFileDevice::NoError || block.isEmpty()
            || copied > SubtitleFileLoader::MaxFileSizeBytes
            || destination.write(block) != block.size())
            return fail(result, QStringLiteral("Could not copy the validated result beside the audio; internal result was retained."), cancelled);
    }
    if (!destination.flush() || copied != output.size())
        return fail(result, QStringLiteral("Could not finish saving the subtitle beside the audio; internal result was retained."), cancelled);
    destination.close();
    source.close();
    const GeneratedSubtitleInfo info{request.mode, completedAt,
        QFileInfo(request.audioPath).fileName(), subtitleName, request.language};
    const bool recordExisted = QFileInfo::exists(recordPath) || QFileInfo(recordPath).isSymLink();
    if (!writeGeneratedSubtitleInfo(publishedDirectory, info, &error, !request.outputBatchId.isEmpty()))
        return fail(result, error, cancelled);
    ownRecord = !recordExisted;
    const QString finalSrt = QDir(publishedDirectory).filePath(info.subtitleFileName);
    if (cancelled.load())
        return fail(result, {}, cancelled);
#ifdef Q_OS_WIN
    // MoveFileW does not replace an existing destination or fall back to a
    // visible copy, unlike QFile::rename's fallback behaviour.
    if (!safeAncestors(publishedDirectory) || !safeAncestors(temporarySrt)
        || !MoveFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(temporarySrt).utf16()),
            reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(finalSrt).utf16())))
        return fail(result, QStringLiteral("Could not publish the completed subtitle without overwriting an existing file; internal result was retained."), cancelled);
#else
    if (!safeAncestors(publishedDirectory) || QFileInfo::exists(finalSrt) || !QFile::rename(temporarySrt, finalSrt))
        return fail(result, QStringLiteral("Could not publish the completed subtitle without overwriting an existing file."), cancelled);
#endif
    // This rename is the publication commit point. Cancellation arriving after
    // it keeps a successful, complete result rather than reporting a phantom cancel.
    published = true;
    result.mode = request.mode;
    result.language = request.language;
    result.state = State::Succeeded;
    result.srtPath = finalSrt;
    result.subtitleCount = loaded.entries.size();
    return result;
}
} // namespace asmr
