#include "artwork/ManagedCoverFile.h"

#include "library/ScanRootStore.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace asmr {
namespace {
constexpr qint64 maximumBytes = 32 * 1024 * 1024;
constexpr qint64 maximumPixels = 32000000;
bool cancelled(const std::shared_ptr<std::atomic_bool> &flag)
{
    return flag && flag->load(std::memory_order_relaxed);
}
QString hash(const QByteArray &value)
{
    return QString::fromLatin1(QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex());
}
QString normalizedIdentity(const QString &path)
{
    auto normalized = ScanRootStore::normalizePath(path);
#ifdef Q_OS_WIN
    normalized = normalized.toCaseFolded();
#endif
    return normalized;
}
// Check every existing ancestor as well as the leaf. In particular, an image
// reached through a junction is not accepted as a regular local file.
bool safeExistingPath(const QString &path, bool directory)
{
    const QString normalized = ScanRootStore::normalizePath(path);
    if (normalized.isEmpty()) return false;
    QFileInfo info(normalized);
    if (directory ? !info.isDir() : !info.isFile()) return false;
    for (;;) {
        if (!info.exists() || info.isSymbolicLink() || info.isJunction()) return false;
        const QString parent = info.absolutePath();
        if (parent == info.absoluteFilePath()) break;
        const QFileInfo ancestor(parent);
        if (ancestor.absoluteFilePath() == info.absoluteFilePath()) break;
        info = ancestor;
    }
    return true;
}
QString workDirectory(const ManualCoverSettings &settings, const ManualCoverIdentity &identity,
    bool create)
{
    const QString database = normalizedIdentity(settings.databasePath);
    const QString root = normalizedIdentity(identity.rootPath);
    if (database.isEmpty() || root.isEmpty() || identity.workId <= 0 || identity.importedAtMs < 0)
        return {};
    QString base = settings.managedDirectory;
    if (base.isEmpty()) base = QDir(QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("Covers"));
    if (base.isEmpty() || base.contains(QChar::Null) || !QDir::isAbsolutePath(base)) return {};
    base = QDir::cleanPath(base);
    // Find a verified ancestor before creating app-owned directories. Never
    // traverse a linked parent merely because the final target is not linked.
    QString ancestor = base;
    while (!QFileInfo::exists(ancestor)) {
        const QString parent = QFileInfo(ancestor).absolutePath();
        if (parent == ancestor) return {};
        ancestor = parent;
    }
    if (!safeExistingPath(ancestor, true)) return {};
    if (!QFileInfo::exists(base) && (!create || !QDir().mkpath(base))) return {};
    if (!safeExistingPath(base, true)) return {};
    const QString databaseDir = QDir(base).filePath(hash(database.toUtf8()));
    const QByteArray key = QByteArray::number(identity.workId) + '\0'
        + QByteArray::number(identity.importedAtMs) + '\0' + root.toUtf8();
    const QString target = QDir(databaseDir).filePath(hash(key));
    for (const QString &part : {databaseDir, target}) {
        if (!QFileInfo::exists(part) && (!create || !QDir().mkdir(part))) return {};
        if (!safeExistingPath(part, true)) return {};
    }
    return target;
}
ManagedCoverDraft invalid(const QString &message)
{
    ManagedCoverDraft result;
    result.errorMessage = message;
    return result;
}
ManagedCoverDraft decode(const QByteArray &bytes,
    const std::shared_ptr<std::atomic_bool> &flag = {})
{
    const auto fail = [] { return invalid(QStringLiteral("图片不可读取、格式不支持或超出安全限制。")); };
    if (bytes.isEmpty() || bytes.size() > maximumBytes) return fail();
    if (cancelled(flag)) return invalid(QStringLiteral("已取消图片读取。"));
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly)) return fail();
    QImageReader reader(&buffer);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    auto format = reader.format().toLower();
    if (format == "jpeg") format = "jpg";
    if (format != "jpg" && format != "png" && format != "webp" && format != "bmp" && format != "gif")
        return fail();
    const QSize original = reader.size();
    if (!reader.canRead() || original.isEmpty()
        || qint64(original.width()) * original.height() > maximumPixels) return fail();
    if (original.width() > 256 && original.height() > 256)
        reader.setScaledSize(original.scaled({256, 256}, Qt::KeepAspectRatioByExpanding));
    QImage image = reader.read(); // Frame zero, with the same EXIF transform as automatic covers.
    if (cancelled(flag)) return invalid(QStringLiteral("已取消图片读取。"));
    if (image.isNull()) return fail();
    const int side = qMin(image.width(), image.height());
    image = image.copy((image.width() - side) / 2, (image.height() - side) / 2, side, side)
        .scaled({256, 256}, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (image.isNull()) return fail();
    image.setDevicePixelRatio(1.0);
    return {true, bytes, QString::fromLatin1(format), image, {}};
}
}

bool ManagedCoverFile::validToken(const QString &token)
{
    static const QRegularExpression expression(QStringLiteral(
        "\\A[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\.(jpg|png|webp|bmp|gif)\\z"));
    return expression.match(token).hasMatch();
}

ManagedCoverDraft ManagedCoverFile::readExternal(const QString &input,
    const std::shared_ptr<std::atomic_bool> &flag)
{
    if (cancelled(flag)) return invalid(QStringLiteral("已取消图片读取。"));
    if (!safeExistingPath(input, false)) return invalid(QStringLiteral("请选择可读取的本地图片文件，不能使用链接。"));
    const QFileInfo before(input);
    const QString canonicalBefore = before.canonicalFilePath();
    if (before.size() <= 0 || before.size() > maximumBytes)
        return invalid(QStringLiteral("图片文件大小必须在 32 MiB 以内。"));
    QFile file(input);
    if (!file.open(QIODevice::ReadOnly)) return invalid(QStringLiteral("无法打开所选图片。"));
    const QByteArray bytes = file.read(maximumBytes + 1);
    if (bytes.size() != before.size() || file.error() != QFileDevice::NoError
        || file.size() != before.size()) return invalid(QStringLiteral("图片在读取期间发生变化，请重新选择。"));
    const QFileInfo after(input);
    if (after.size() != before.size() || after.lastModified() != before.lastModified()
        || !safeExistingPath(input, false) || after.canonicalFilePath() != canonicalBefore)
        return invalid(QStringLiteral("图片在读取期间发生变化，请重新选择。"));
    return decode(bytes, flag);
}

ManagedCoverStageResult ManagedCoverFile::stage(const ManualCoverSettings &settings,
    const ManualCoverIdentity &identity, const ManagedCoverDraft &draft)
{
    if (!draft.success) return {false, {}, QStringLiteral("没有可保存的封面图片。")};
    // Revalidate caller-owned bytes rather than trusting extension/image flags.
    const auto verified = decode(draft.bytes);
    if (!verified.success || verified.extension != draft.extension)
        return {false, {}, QStringLiteral("封面图片快照无效。")};
    const QString directory = workDirectory(settings, identity, true);
    if (directory.isEmpty()) return {false, {}, QStringLiteral("无法创建封面副本目录。")};
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces)
        + u'.' + verified.extension;
    const QString target = QDir(directory).filePath(token);
    if (QFileInfo::exists(target)) return {false, {}, QStringLiteral("封面副本文件已存在，请重试。")};
    QSaveFile file(target);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(draft.bytes) != draft.bytes.size()
        || !safeExistingPath(directory, true) || !file.commit())
        return {false, {}, QStringLiteral("保存封面副本失败，原封面保持不变。")};
    if (path(settings, identity, token).isEmpty()) {
        static_cast<void>(discard(settings, identity, token));
        return {false, {}, QStringLiteral("无法确认封面副本的安全位置。")};
    }
    return {true, token, {}};
}

QString ManagedCoverFile::path(const ManualCoverSettings &settings,
    const ManualCoverIdentity &identity, const QString &token)
{
    if (!validToken(token)) return {};
    const QString directory = workDirectory(settings, identity, false);
    if (directory.isEmpty()) return {};
    const QString target = QDir(directory).filePath(token);
    return safeExistingPath(target, false) ? target : QString{};
}

bool ManagedCoverFile::discard(const ManualCoverSettings &settings,
    const ManualCoverIdentity &identity, const QString &token)
{
    if (!validToken(token)) return false;
    const QString directory = workDirectory(settings, identity, false);
    if (directory.isEmpty()) return false;
    const QString target = QDir(directory).filePath(token);
    if (!QFileInfo::exists(target)) return true;
    return safeExistingPath(target, false) && QFile::remove(target);
}

bool ManagedCoverFile::cleanup(const ManualCoverSettings &settings,
    const ManualCoverIdentity &identity, const QString &keepToken)
{
    if (!keepToken.isEmpty() && !validToken(keepToken)) return false;
    const QString directory = workDirectory(settings, identity, false);
    if (directory.isEmpty()) return true; // No copies, or inaccessible: never traverse elsewhere.
    bool success = true;
    const QDir folder(directory);
    for (const auto &entry : folder.entryInfoList(QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden)) {
        const QString token = entry.fileName();
        if (token != keepToken && validToken(token) && !discard(settings, identity, token)) success = false;
    }
    return success;
}

} // namespace asmr
