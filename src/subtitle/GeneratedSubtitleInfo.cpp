#include "subtitle/GeneratedSubtitleInfo.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
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
const QString RecordName = QStringLiteral(".arcanblade-subtitle.json");
const QString Owner = QStringLiteral("Arcanblade.GeneratedSubtitle.v1");
constexpr qint64 RecordLimit = 16 * 1024;
bool ordinaryPath(QString path)
{
    if (!QDir::isAbsolutePath(path))
        return false;
    path = QDir::cleanPath(path);
    for (;;) {
#ifdef Q_OS_WIN
        const DWORD attributes = GetFileAttributesW(
            reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
#else
        if (!QFileInfo::exists(path) || QFileInfo(path).isSymLink())
            return false;
#endif
        const QString parent = QFileInfo(path).absolutePath();
        if (parent == path)
            return true;
        path = parent;
    }
}
bool fileNameOnly(const QString &name)
{
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")
        || name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))
        || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
        return false;
    for (const auto character : name) {
        if (character.unicode() < 32 || QStringLiteral("<>:\"|?*").contains(character))
            return false;
    }
    return true;
}
QString generationLanguage(const QString &mode)
{
    if (mode == QStringLiteral("offline-direct") || mode == QStringLiteral("cloud-translation")) return QStringLiteral("zh");
    if (mode == QStringLiteral("offline-asr-ja") || mode == QStringLiteral("cloud-correction-ja")) return QStringLiteral("ja");
    return {};
}
bool validTranslationInfo(const GeneratedSubtitleInfo &info)
{
    if (info.mode != QStringLiteral("cloud-translation") && info.mode != QStringLiteral("cloud-correction-ja"))
        return info.translationService.isEmpty() && info.translationModel.isEmpty();
    if ((info.translationService != QStringLiteral("deepseek")
        && info.translationService != QStringLiteral("openai-compatible"))
        || info.translationModel.isEmpty() || info.translationModel.size() > 256) return false;
    for (const auto ch : info.translationModel)
        if (ch.isSpace() || ch.category() == QChar::Other_Control) return false;
    return true;
}
bool validInfo(const GeneratedSubtitleInfo &info)
{
    const QString language = generationLanguage(info.mode);
    const QString audioSuffix = QFileInfo(info.audioFileName).suffix().toLower();
    const QString audioStem = QFileInfo(info.audioFileName).completeBaseName();
    const QString subtitleStem = QFileInfo(info.subtitleFileName).completeBaseName();
    // Old direct-translation records keep their original untagged filename.
    const QString taggedSuffix = QLatin1Char('.') + language;
    const QString suffixedStem = subtitleStem.endsWith(taggedSuffix)
        ? subtitleStem.left(subtitleStem.size() - taggedSuffix.size()) : QString();
    const bool suffixed = suffixedStem.startsWith(audioStem + QLatin1Char('_'))
        && QRegularExpression(QStringLiteral("\\A[A-Za-z0-9_-]{1,128}\\z"))
            .match(suffixedStem.mid(audioStem.size() + 1)).hasMatch();
    // The generated correction marker is not part of the caller's collision token.
    const QString correctionStem = info.mode == QStringLiteral("cloud-correction-ja")
            && suffixedStem.endsWith(QStringLiteral("_corrected"))
        ? suffixedStem.chopped(10) : QString();
    const bool correctedPair = !correctionStem.isEmpty() && (correctionStem == audioStem
        || (correctionStem.startsWith(audioStem + QLatin1Char('_'))
            && QRegularExpression(QStringLiteral("\\A[A-Za-z0-9_-]{1,128}\\z"))
                .match(correctionStem.mid(audioStem.size() + 1)).hasMatch()));
    const bool paired = subtitleStem == audioStem + QLatin1Char('.') + language || suffixed || correctedPair
        || (info.mode == QStringLiteral("offline-direct") && subtitleStem == audioStem);
    return !language.isEmpty() && (info.language.isEmpty() || info.language == language)
        && info.completedAt.isValid() && fileNameOnly(info.audioFileName)
        && fileNameOnly(info.subtitleFileName)
        && (audioSuffix == QStringLiteral("mp3") || audioSuffix == QStringLiteral("wav"))
        && QFileInfo(info.subtitleFileName).suffix().compare(QStringLiteral("srt"), Qt::CaseInsensitive) == 0
        && paired && validTranslationInfo(info);
}
bool failure(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}
bool definitelyMissing(const QString &path)
{
    std::error_code error;
#ifdef Q_OS_WIN
    const auto status = std::filesystem::symlink_status(std::filesystem::path(path.toStdWString()), error);
#else
    const auto status = std::filesystem::symlink_status(std::filesystem::path(QFile::encodeName(path).constData()), error);
#endif
    return (!error && status.type() == std::filesystem::file_type::not_found)
        || error == std::errc::no_such_file_or_directory;
}
std::optional<GeneratedSubtitleInfo> readInfoRecord(const QString &record, const QString &subtitleFileName,
    bool requireLanguage = false)
{
    if (!ordinaryPath(record) || !QFileInfo(record).isFile()) return std::nullopt;
    QFile file(record);
    if (!file.open(QIODevice::ReadOnly) || file.isSequential() || file.size() <= 0 || file.size() > RecordLimit)
        return std::nullopt;
    const QByteArray bytes = file.read(RecordLimit + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > RecordLimit || !file.atEnd())
        return std::nullopt;
    const auto document = QJsonDocument::fromJson(bytes);
    if (!document.isObject()) return std::nullopt;
    const auto object = document.object();
    if (object.value(QStringLiteral("owner")).toString() != Owner
        || object.value(QStringLiteral("version")).toInt(-1) != 1)
        return std::nullopt;
    if (object.contains(QStringLiteral("language")) && !object.value(QStringLiteral("language")).isString())
        return std::nullopt;
    if (requireLanguage && object.value(QStringLiteral("language")).toString()
        != generationLanguage(object.value(QStringLiteral("mode")).toString())) return std::nullopt;
    for (const auto &field : {QStringLiteral("translationService"), QStringLiteral("translationModel")})
        if (object.contains(field) && !object.value(field).isString()) return std::nullopt;
    const QString completed = object.value(QStringLiteral("completedUtc")).toString();
    GeneratedSubtitleInfo info{object.value(QStringLiteral("mode")).toString(),
        QDateTime::fromString(completed, Qt::ISODateWithMs),
        object.value(QStringLiteral("audioFileName")).toString(),
        object.value(QStringLiteral("subtitleFileName")).toString(),
        object.value(QStringLiteral("language")).toString(),
        object.value(QStringLiteral("translationService")).toString(),
        object.value(QStringLiteral("translationModel")).toString()};
    if (!completed.endsWith(QLatin1Char('Z')) || !validInfo(info) || info.subtitleFileName != subtitleFileName)
        return std::nullopt;
    if (info.language.isEmpty()) info.language = generationLanguage(info.mode);
    return info;
}
}

QString generatedSubtitleInfoPath(const QString &directory, const QString &subtitleFileName)
{
    if (!fileNameOnly(subtitleFileName)) return {};
    QString name = subtitleFileName;
#ifdef Q_OS_WIN
    name = name.toCaseFolded();
#endif
    const auto hash = QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(directory).filePath(QStringLiteral(".arcanblade-subtitle-")
        + QString::fromLatin1(hash) + QStringLiteral(".json"));
}

std::optional<GeneratedSubtitleInfo> readGeneratedSubtitleInfo(const QString &subtitlePath)
{
    if (!ordinaryPath(subtitlePath) || !QFileInfo(subtitlePath).isFile())
        return std::nullopt;
    const auto directory = QFileInfo(subtitlePath).absolutePath();
    const auto itemRecord = generatedSubtitleInfoPath(directory, QFileInfo(subtitlePath).fileName());
    // An invalid per-file record must not borrow a legacy neighbouring record.
    const QString record = QFileInfo::exists(itemRecord) || QFileInfo(itemRecord).isSymLink()
        ? itemRecord : QDir(directory).filePath(RecordName);
    return readInfoRecord(record, QFileInfo(subtitlePath).fileName(), record == itemRecord);
}

bool writeGeneratedSubtitleInfo(const QString &directory, const GeneratedSubtitleInfo &info, QString *error,
    bool perSubtitle)
{
    if (error) error->clear();
    if (!ordinaryPath(directory) || !QFileInfo(directory).isDir() || !validInfo(info))
        return failure(error, QStringLiteral("Invalid generated-subtitle directory or metadata."));
    if (perSubtitle && !definitelyMissing(QDir(directory).filePath(info.subtitleFileName)))
        return failure(error, QStringLiteral("The destination subtitle already exists or cannot be checked; metadata was not changed."));
    QJsonObject object{{QStringLiteral("owner"), Owner}, {QStringLiteral("version"), 1},
        {QStringLiteral("mode"), info.mode},
        {QStringLiteral("completedUtc"), info.completedAt.toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("audioFileName"), info.audioFileName},
        {QStringLiteral("subtitleFileName"), info.subtitleFileName},
        {QStringLiteral("language"), generationLanguage(info.mode)}};
    if (info.mode == QStringLiteral("cloud-translation") || info.mode == QStringLiteral("cloud-correction-ja")) {
        object.insert(QStringLiteral("translationService"), info.translationService);
        object.insert(QStringLiteral("translationModel"), info.translationModel);
    }
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (bytes.size() > RecordLimit)
        return failure(error, QStringLiteral("Generated-subtitle metadata exceeds 16 KiB."));
    QFile file(perSubtitle ? generatedSubtitleInfoPath(directory, info.subtitleFileName)
        : QDir(directory).filePath(RecordName));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        const auto previous = perSubtitle ? readInfoRecord(file.fileName(), info.subtitleFileName, true) : std::nullopt;
        if (previous && previous->mode == info.mode && previous->audioFileName == info.audioFileName
            && previous->subtitleFileName == info.subtitleFileName
            && previous->language == generationLanguage(info.mode)
            && previous->translationService == info.translationService
            && previous->translationModel == info.translationModel
            && definitelyMissing(QDir(directory).filePath(info.subtitleFileName))) return true;
        return failure(error, QStringLiteral("Could not create generated-subtitle metadata: %1").arg(file.errorString()));
    }
    if (file.write(bytes) != bytes.size() || !file.flush()) {
        const QString message = QStringLiteral("Could not save generated-subtitle metadata: %1").arg(file.errorString());
        file.close();
        // NewOnly succeeded, so this partial record belongs to this attempt.
        QFile::remove(file.fileName());
        return failure(error, message);
    }
    return true;
}
} // namespace asmr
