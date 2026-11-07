#include "ai/ChickenRicePackage.h"

#include <QSet>

#include <limits>

namespace asmr {
namespace {
bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

bool validHash(const QByteArray &hash)
{
    if (hash.size() != 64)
        return false;
    for (const char value : hash) {
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f')))
            return false;
    }
    return true;
}

bool simpleName(const QString &name)
{
    if (name.isEmpty() || name.size() > 180 || name == QStringLiteral(".")
        || name == QStringLiteral("..") || name.endsWith(QLatin1Char('.'))
        || name.endsWith(QLatin1Char(' ')))
        return false;
    for (const QChar value : name) {
        if (!((value >= QLatin1Char('a') && value <= QLatin1Char('z'))
            || (value >= QLatin1Char('A') && value <= QLatin1Char('Z'))
            || (value >= QLatin1Char('0') && value <= QLatin1Char('9'))
            || value == QLatin1Char('_') || value == QLatin1Char('-')
            || value == QLatin1Char('.')))
            return false;
    }
    return true;
}
} // namespace

// Pinned metadata, read from the public v1.10 release on 2026-10-21:
// https://api.github.com/repos/TransWithAI/Faster-Whisper-TransWithAI-ChickenRice/releases/tags/v1.10
// Whole ZIP hashes: release body. Per-part hashes: official asset digest fields.
// The runtime never scrapes HTML, resolves "latest", or contacts a worker proxy.
std::optional<ChickenRicePackage> chickenRicePackage(ChickenRiceVariant variant)
{
    ChickenRicePackage package;
    package.variant = variant;
    package.version = QStringLiteral("v1.10");
    package.archiveName = chickenRiceArchiveName(variant);
    const QString base = QStringLiteral(
        "https://github.com/TransWithAI/Faster-Whisper-TransWithAI-ChickenRice/releases/download/v1.10/");
    const auto addPart = [&package, &base](qint64 size, const char *hash) {
        const QString name = package.archiveName + QLatin1Char('.')
            + QString::number(package.parts.size()).rightJustified(4, QLatin1Char('0'));
        package.parts.append({name, QUrl(base + name), size, QByteArray(hash)});
        package.size += size;
    };
    switch (variant) {
    case ChickenRiceVariant::Cu118:
        package.unpackedSize = 7755734587LL;
        package.sha256 = QByteArray("cbf3cd8bd016c5db85f0b8be8bb819515e9dab0e3f599f05fbfd6b4562481685");
        addPart(2097152000LL, "9fe17efc0d4ff0a743e236910791bad7726c853e55da400c990764457a6c562f");
        addPart(2097152000LL, "20d7b0f5a902fe8eff41abc237a983a959f2d96f1995728ed7d6d59584f3bcad");
        addPart(677540447LL, "b1f8b96f70da2b809c043d4b01a4fa63bfeb15b110201eb30b6fd69f6c98a8b7");
        break;
    case ChickenRiceVariant::Cu122:
        package.unpackedSize = 8491548912LL;
        package.sha256 = QByteArray("b6afeda70992806d2cc5cabfc3a612fc3ad393e354690a8ae629b7a75a9a7f04");
        addPart(2097152000LL, "c30faa6ba2b3ad2a3ed479f3aa80db4c8b29fdf3e4f38b897d909b46c5d9cec2");
        addPart(2097152000LL, "887dc02ccd0ece067ae6e407e5e8ca91e9c3557c89eccfcca0b50c8a0eba7ef8");
        addPart(1108066092LL, "266fec7ba82d3621121247c33f84e51e3c15cecfa05e418b3242df8955b80ad6");
        break;
    case ChickenRiceVariant::Cu128:
        package.unpackedSize = 9405670028LL;
        package.sha256 = QByteArray("cf8b258e6393e14152fd019df631ab5a65104f27dfe21602c36ca27d7896ae8f");
        addPart(2097152000LL, "fdb9c5b985453224cd20f72847d8feda2348838f299e86bd286cb9239a226f18");
        addPart(2097152000LL, "9f42666e56d791b27584ed6f158a5648cd74db88523808e8047c9b10ec8510da");
        addPart(1768804763LL, "0490029d4f72f7219a9f61c21048c121d1050fc4a150a507eb5247883dd4b7ab");
        break;
    case ChickenRiceVariant::Gfx101xDgpu:
        package.unpackedSize = 5775632759LL;
        package.sha256 = QByteArray("1d0d66f5be78417f814a4dc7f2e486daf149793b63603e9d128c42cab9aa761e");
        addPart(2097152000LL, "f1a2429da35ca4ac1ca3dec361851a780ae87e9d3a68ed281c442a02d0cf50fd");
        addPart(1094506047LL, "db24924583af2b6a305409a5b32a72edb555fa62b5250a54f44e57d4e56aca1c");
        break;
    case ChickenRiceVariant::Gfx103xDgpu:
        package.unpackedSize = 5775633251LL;
        package.sha256 = QByteArray("538d52ff9e443a899b9000be2ec2e883686ad5d209acec5c41e9cc6f905b7604");
        addPart(2097152000LL, "e03c6e638c8448d06237ae5c8e38cfccd0791fc409e8abfafe602011f80bf8f6");
        addPart(1094506226LL, "4699536ff29baf7707bf3ca59a89bea5bfd7f8ac842b986f216a99b5a52a3d8c");
        break;
    case ChickenRiceVariant::Gfx110xAll:
        package.unpackedSize = 5775633374LL;
        package.sha256 = QByteArray("8c2d4453d890658ea5c8985fba4b21d55a89a74e1e183483c7eb05549602d398");
        addPart(2097152000LL, "730f41b852ff1c27d0679bf764d70cb1ab083ebe732fc3b0326fd678bd01c81e");
        addPart(1094506573LL, "b73507fd04aa8a9f4e3c38c8edb9f7fdb4be922fe16f8907f04ed31d007285a1");
        break;
    case ChickenRiceVariant::Gfx120xAll:
        package.unpackedSize = 6226520719LL;
        package.sha256 = QByteArray("b1470ca35dfda10906b8ef94ffa2fbd3bc55ddc940b8c0c71db88d7fd4a5bc55");
        addPart(2097152000LL, "e10765a7f7f87f3347bc9fc572370456909fa775c37387f9f7e8b5a8ea4ca570");
        addPart(1238394972LL, "f82f51c1a3cab33c5764d0b11c619d51972b93c6f91c6fd502f273d8d3282b5e");
        break;
    default:
        return std::nullopt;
    }
    return package;
}

bool validateChickenRicePackage(const ChickenRicePackage &package, QString *error,
                                bool allowLoopbackHttp)
{
    if (error)
        error->clear();
    if (!allChickenRiceVariants().contains(package.variant))
        return fail(error, QStringLiteral("Unknown ChickenRice package variant."));
    if (!simpleName(package.version) || !simpleName(package.archiveName)
        || !package.archiveName.endsWith(QStringLiteral(".zip"), Qt::CaseSensitive))
        return fail(error, QStringLiteral("Invalid package version or ZIP file name."));
    if (package.version != QStringLiteral("v1.10")
        || package.archiveName != chickenRiceArchiveName(package.variant))
        return fail(error, QStringLiteral("Package must match its pinned v1.10 variant."));
    if (package.size <= 0 || !validHash(package.sha256)
        || package.parts.isEmpty() || package.parts.size() > 64)
        return fail(error, QStringLiteral("Invalid package size, hash, or part count."));
    constexpr qint64 ReserveBytes = 64LL * 1024 * 1024;
    const qint64 maximum = std::numeric_limits<qint64>::max();
    if (package.unpackedSize < 0 || package.size > maximum - ReserveBytes
        || package.unpackedSize > maximum - ReserveBytes - package.size)
        return fail(error, QStringLiteral("Invalid or overflowing package disk space budget."));
    qint64 total = 0;
    QSet<QString> names;
    for (qsizetype index = 0; index < package.parts.size(); ++index) {
        const auto &part = package.parts.at(index);
        const QString expectedName = package.archiveName + QLatin1Char('.')
            + QString::number(index).rightJustified(4, QLatin1Char('0'));
        if (part.name != expectedName || !simpleName(part.name)
            || names.contains(part.name.toCaseFolded()) || !validHash(part.sha256)
            || part.size <= 0 || part.size > std::numeric_limits<qint64>::max() - total)
            return fail(error, QStringLiteral("Invalid ordered package part or size overflow."));
        const auto &url = part.url;
        const bool loopback = allowLoopbackHttp && url.scheme() == QStringLiteral("http")
            && (url.host() == QStringLiteral("127.0.0.1") || url.host() == QStringLiteral("::1")
                || url.host() == QStringLiteral("localhost"));
        if (!url.isValid() || url.isRelative() || url.host().isEmpty()
            || (!loopback && (url.scheme() != QStringLiteral("https")
                || url.host() != QStringLiteral("github.com") || url.port() != -1
                || url.path() != QStringLiteral(
                    "/TransWithAI/Faster-Whisper-TransWithAI-ChickenRice/releases/download/v1.10/") + part.name))
            || !url.userInfo().isEmpty() || url.hasFragment() || url.hasQuery()
            || url.path().section(QLatin1Char('/'), -1) != part.name)
            return fail(error, QStringLiteral("Invalid or insecure package part URL."));
        names.insert(part.name.toCaseFolded());
        total += part.size;
    }
    if (total != package.size)
        return fail(error, QStringLiteral("Package part sizes do not match the ZIP size."));
    return true;
}
} // namespace asmr
