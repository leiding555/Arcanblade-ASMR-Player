#include "ai/FasterWhisperPackage.h"

#include <QSet>
#include <limits>

namespace asmr {
namespace {
bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}
bool validHash(const QByteArray &value)
{
    if (value.size() != 64) return false;
    for (const char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
bool simpleName(const QString &value)
{
    if (value.isEmpty() || value.size() > 180 || value.endsWith(QLatin1Char('.'))
        || value == QStringLiteral(".") || value == QStringLiteral("..")) return false;
    for (const QChar c : value)
        if (!((c >= QLatin1Char('a') && c <= QLatin1Char('z'))
            || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
            || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
            || c == QLatin1Char('_') || c == QLatin1Char('-') || c == QLatin1Char('.'))) return false;
    return true;
}
bool loopbackUrl(const QUrl &url)
{
    return url.scheme() == QStringLiteral("http")
        && (url.host() == QStringLiteral("127.0.0.1") || url.host() == QStringLiteral("::1")
            || url.host() == QStringLiteral("localhost"));
}
bool sameFile(const FasterWhisperDownloadFile &left, const FasterWhisperDownloadFile &right)
{
    return left.name == right.name && left.url == right.url
        && left.size == right.size && left.sha256 == right.sha256;
}
} // namespace

// Pinned on 2026-10-22. Engine archive SHA-256 was computed from the official
// HTTPS asset; GitHub did not publish a digest for this older asset. Model
// bytes are pinned to the Systran revision, with model.bin matching its LFS SHA.
FasterWhisperPackage fasterWhisperPackage()
{
    FasterWhisperPackage package;
    package.version = QStringLiteral("r245.4");
    package.modelRevision = QStringLiteral("f0fe81560cb8b68660e564f55dd99207059c092e");
    package.engineArchive = {
        QStringLiteral("Faster-Whisper-XXL_r245.4_windows.7z"),
        QUrl(QStringLiteral("https://github.com/Purfview/whisper-standalone-win/releases/download/Faster-Whisper-XXL/Faster-Whisper-XXL_r245.4_windows.7z")),
        1424256246LL,
        QByteArray("237dee23939cdabfc96ef859fc5e584b842c3a5557e0d2ca744e1f87c14c5844")};
    package.unpackedSize = 4714874310LL; // Summed from the verified official 7z listing.
    const QString base = QStringLiteral("https://huggingface.co/Systran/faster-whisper-large-v2/resolve/")
        + package.modelRevision + QLatin1Char('/');
    const auto add = [&package, &base](const char *name, qint64 size, const char *hash) {
        package.modelFiles.append({QString::fromLatin1(name), QUrl(base + QString::fromLatin1(name)),
                                   size, QByteArray(hash)});
    };
    add("config.json", 2796, "d86b7a7664a12559d644aa210a32ce9a7e03913e794b7ea4fb7182de69e273a7");
    add("model.bin", 3086912962LL, "bf2a9746382e1aa7ffff6b3a0d137ed9edbd9670c3b87e5d35f5e85e70d0333a");
    add("tokenizer.json", 2203239, "fb7b63191e9bb045082c79fd742a3106a12c99513ab30df4a0d47fa6cb6fd0ab");
    add("vocabulary.txt", 459861, "34ce3fe1c5041027b3f8d42912270993f986dbc4bb34cf27f951e34a1e453913");
    return package;
}

bool validateFasterWhisperPackage(const FasterWhisperPackage &package, QString *error,
                                 bool allowLoopbackHttp)
{
    if (error) error->clear();
    const auto pinned = fasterWhisperPackage();
    if (package.version != pinned.version || package.modelRevision != pinned.modelRevision
        || package.engineArchive.name != pinned.engineArchive.name
        || !simpleName(package.version) || package.modelFiles.size() != pinned.modelFiles.size())
        return fail(error, QStringLiteral("Unsupported Faster-Whisper engine or model revision."));
    if (package.unpackedSize < 0)
        return fail(error, QStringLiteral("Invalid expanded engine size."));
    qint64 budget = package.unpackedSize;
    QSet<QString> names;
    auto validateFile = [&](const FasterWhisperDownloadFile &file,
                            const FasterWhisperDownloadFile &expected) {
        if (!simpleName(file.name) || file.name != expected.name || names.contains(file.name.toCaseFolded())
            || file.size <= 0 || !validHash(file.sha256)
            || file.size > std::numeric_limits<qint64>::max() - budget)
            return false;
        const auto &url = file.url;
        const bool test = allowLoopbackHttp && loopbackUrl(url);
        if (!url.isValid() || url.isRelative() || url.host().isEmpty() || !url.userInfo().isEmpty()
            || url.hasFragment() || url.hasQuery() || url.path().section(QLatin1Char('/'), -1) != file.name
            || (!test && (!sameFile(file, expected) || url.port() != -1))) return false;
        names.insert(file.name.toCaseFolded());
        budget += file.size;
        return true;
    };
    if (!validateFile(package.engineArchive, pinned.engineArchive))
        return fail(error, QStringLiteral("Invalid or unpinned engine archive download."));
    for (qsizetype index = 0; index < package.modelFiles.size(); ++index)
        if (!validateFile(package.modelFiles.at(index), pinned.modelFiles.at(index)))
            return fail(error, QStringLiteral("Invalid or unpinned large-v2 model download."));
    if (budget > std::numeric_limits<qint64>::max() - 64LL * 1024 * 1024)
        return fail(error, QStringLiteral("Package disk-space budget overflows."));
    return true;
}
} // namespace asmr
