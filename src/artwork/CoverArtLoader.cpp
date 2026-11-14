#include "artwork/CoverArtLoader.h"
#include "artwork/ManagedCoverFile.h"

#include "library/NaturalSort.h"
#include "library/ScanRootStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QSet>
#include <QVector>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <utility>

namespace asmr {
namespace {
namespace fs = std::filesystem;

fs::path nativePath(const QString &path)
{
#ifdef Q_OS_WIN
    return fs::path(path.toStdWString());
#else
    return fs::path(QFile::encodeName(path).constData());
#endif
}
QString qtPath(const fs::path &path)
{
#ifdef Q_OS_WIN
    return QDir::fromNativeSeparators(QString::fromStdWString(path.native()));
#else
    return QFile::decodeName(path.native());
#endif
}
bool isCancelled(const std::shared_ptr<std::atomic_bool> &cancelled)
{
    return cancelled && cancelled->load(std::memory_order_relaxed);
}
QString rootIdentityKey(const QString &path)
{
    const auto normalized = ScanRootStore::normalizePath(path);
#ifdef Q_OS_WIN
    return normalized.toCaseFolded();
#else
    return normalized;
#endif
}
CoverArtResult result(CoverArtStatus status, const QString &message)
{
    return {status, {}, {}, message, false};
}
bool linked(const QFileInfo &info, const fs::file_status &status)
{
    return fs::is_symlink(status) || info.isSymbolicLink() || info.isJunction();
}
bool inside(const fs::path &file, const fs::path &root)
{
    auto ancestor = file.parent_path();
    while (!ancestor.empty()) {
        std::error_code error;
        if (fs::equivalent(ancestor, root, error) && !error) return true;
        const auto parent = ancestor.parent_path();
        if (parent == ancestor) break;
        ancestor = parent;
    }
    return false;
}
bool supported(const QByteArray &format)
{
    const auto lower = format.toLower();
    return lower == "jpg" || lower == "jpeg" || lower == "png"
        || lower == "webp" || lower == "bmp" || lower == "gif";
}
bool supportedSuffix(const QString &path)
{
    return supported(QFileInfo(path).suffix().toLatin1());
}
int namePriority(const QFileInfo &info, const QString &audioStem)
{
    const auto stem = info.completeBaseName().toCaseFolded();
    if (!audioStem.isEmpty() && stem == audioStem) return -1;
    const QStringList preferred{QStringLiteral("cover"), QStringLiteral("folder"),
                                QStringLiteral("front"), QStringLiteral("jacket")};
    const auto index = preferred.indexOf(stem);
    return index < 0 ? 4 : static_cast<int>(index);
}
struct Candidate {
    QString path;
    int priority;
    NaturalSortKey key;
};
QString cacheKey(const fs::path &canonical, const QFileInfo &info, const QSize &size)
{
    // Length-prefix the path so unusual filenames cannot alias another key.
    const auto path = qtPath(canonical);
    // Do not run QString::arg after inserting a user path: a valid filename
    // containing %3 or %4 must remain literal text in this identity.
    return QString::number(path.size()) + QLatin1Char(':') + path + QLatin1Char('|')
        + QString::number(info.size()) + QLatin1Char('|')
        + QString::number(info.lastModified().toMSecsSinceEpoch()) + QLatin1Char('|')
        + QString::number(size.width()) + QLatin1Char('x') + QString::number(size.height());
}
}

CoverArtCache::CoverArtCache(int maximumBytes) : m_images(qMax(0, maximumBytes)) {}
int CoverArtCache::totalCost() const { return static_cast<int>(m_images.totalCost()); }
int CoverArtCache::maximumCost() const { return static_cast<int>(m_images.maxCost()); }
void CoverArtCache::clear() { m_images.clear(); }

CoverArtResult CoverArtLoader::loadImage(const QString &root, const QString &relativePath,
    bool forceReload, const CoverArtOptions &options,
    const std::shared_ptr<std::atomic_bool> &cancelled, CoverArtCache *cache)
{
    const auto failure = [] { return result(CoverArtStatus::Error,
        QStringLiteral("封面图片不存在、不可读取或超出安全限制。")); };
    if (isCancelled(cancelled)) return result(CoverArtStatus::Cancelled, {});
    const auto relative = ManualCoverStore::normalizeRelativePath(relativePath);
    if (relative.isEmpty() || !supportedSuffix(relative)) return failure();
    std::error_code error;
    const auto rootStatus = fs::symlink_status(nativePath(root), error);
    if (error || !fs::is_directory(rootStatus) || linked(QFileInfo(root), rootStatus)) return failure();
    const auto canonicalRoot = fs::canonical(nativePath(root), error);
    if (error || canonicalRoot.empty()) return failure();
    fs::path current = nativePath(root);
    const auto components = relative.split(u'/');
    for (qsizetype index = 0; index < components.size(); ++index) {
        if (isCancelled(cancelled)) return result(CoverArtStatus::Cancelled, {});
        current /= nativePath(components[index]);
        const QFileInfo info(qtPath(current));
        const auto status = fs::symlink_status(current, error);
        if (error || linked(info, status)
            || (index + 1 == components.size() ? !fs::is_regular_file(status) : !fs::is_directory(status)))
            return failure();
        const auto physical = fs::canonical(current, error);
        if (error || !inside(physical, canonicalRoot)) return failure();
    }
    const QString path = qtPath(current);
    const QFileInfo info(path);
    const auto physical = fs::canonical(current, error);
    if (error || info.size() <= 0 || info.size() > options.maxFileBytes) return failure();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > options.maxFileBytes)
        return failure();
    QImageReader reader(&file);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    const QSize original = reader.size();
    if (!supported(reader.format()) || !reader.canRead()
        || original.width() <= 0 || original.height() <= 0
        || qint64(original.width()) * original.height() > options.maxPixels) return failure();
    const QString key = cacheKey(physical, info, options.thumbnailSize);
    if (cache && !forceReload) {
        if (auto *cached = cache->m_images.object(key)) {
            if (isCancelled(cancelled)) return result(CoverArtStatus::Cancelled, {});
            return {CoverArtStatus::Ready, *cached, path, {}, true};
        }
    }
    const int side = qMax(options.thumbnailSize.width(), options.thumbnailSize.height());
    if (original.width() > side && original.height() > side)
        reader.setScaledSize(original.scaled(QSize(side, side), Qt::KeepAspectRatioByExpanding));
    QImage image = reader.read(); // Animation intentionally uses frame zero.
    if (isCancelled(cancelled)) return result(CoverArtStatus::Cancelled, {});
    if (image.isNull()) return failure();
    // Crop before scaling so very thin images cannot create huge intermediates.
    int cropWidth = image.width(), cropHeight = image.height();
    if (qint64(cropWidth) * options.thumbnailSize.height()
        > qint64(cropHeight) * options.thumbnailSize.width())
        cropWidth = qMax(1, static_cast<int>(qint64(cropHeight)
            * options.thumbnailSize.width() / options.thumbnailSize.height()));
    else
        cropHeight = qMax(1, static_cast<int>(qint64(cropWidth)
            * options.thumbnailSize.height() / options.thumbnailSize.width()));
    image = image.copy((image.width() - cropWidth) / 2, (image.height() - cropHeight) / 2,
        cropWidth, cropHeight).scaled(options.thumbnailSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (image.isNull()) return failure();
    image.setDevicePixelRatio(1.0);
    if (isCancelled(cancelled)) return result(CoverArtStatus::Cancelled, {});
    const QFileInfo after(path);
    if (after.size() != info.size() || after.lastModified() != info.lastModified()
        || file.size() != info.size()) return failure();
    if (cache && image.sizeInBytes() <= cache->m_images.maxCost()
        && image.sizeInBytes() <= std::numeric_limits<int>::max())
        cache->m_images.insert(key, new QImage(image), static_cast<int>(image.sizeInBytes()));
    return {CoverArtStatus::Ready, image, path, {}, false};
}

CoverArtResult CoverArtLoader::loadSelected(const QString &rootPath, const QString &relativePath,
    const CoverArtOptions &options, const std::shared_ptr<std::atomic_bool> &cancelled,
    CoverArtCache *cache)
{
    CoverArtRequest request{rootPath, {}, true, true};
    request.manualRelativePath = relativePath;
    request.manualOnly = true;
    return load(request, options, cancelled, cache);
}

CoverArtResult CoverArtLoader::load(const CoverArtRequest &request,
    const CoverArtOptions &options, const std::shared_ptr<std::atomic_bool> &cancelled,
    CoverArtCache *cache)
{
    if (isCancelled(cancelled)) return result(CoverArtStatus::Cancelled, {});
    if (options.maxDepth < 0 || options.maxEntries < 0 || options.maxCandidates < 0
        || options.maxFileBytes <= 0 || options.maxPixels <= 0
        || options.thumbnailSize.isEmpty() || options.thumbnailSize.width() > 4096
        || options.thumbnailSize.height() > 4096) {
        return result(CoverArtStatus::Error, QStringLiteral("封面读取参数无效。"));
    }
    const QString root = QDir::cleanPath(request.rootPath);
    if (request.rootPath.isEmpty() || !QDir::isAbsolutePath(root)
        || root.contains(QChar::Null)) {
        return result(CoverArtStatus::Error, QStringLiteral("封面目录路径无效。"));
    }
    std::error_code error;
    const fs::path rootPath = nativePath(root);
    const auto rootStatus = fs::symlink_status(rootPath, error);
    const QFileInfo rootInfo(root);
    if (error || !fs::is_directory(rootStatus) || linked(rootInfo, rootStatus))
        return result(CoverArtStatus::Error, QStringLiteral("封面目录不存在、不可读或为链接。"));
    const auto canonicalRoot = fs::canonical(rootPath, error);
    if (error || canonicalRoot.empty())
        return result(CoverArtStatus::Error, QStringLiteral("无法确认封面目录的实际路径。"));

    QString manualPath = request.manualRelativePath;
    QString managedPath;
    bool managedChoice = false;
    QString manualError;
    if (request.manualLookup) {
        if (rootIdentityKey(request.manualLookup->identity.rootPath) != rootIdentityKey(root)) {
            manualError = QStringLiteral("手动封面作品目录不一致，已使用自动查找。");
            manualPath.clear();
        } else {
            const auto saved = ManualCoverStore::read(request.manualLookup->settings, request.manualLookup->identity,
                request.manualLookup->readOnly);
            if (saved.success) {
                managedChoice = !saved.managedFile.isEmpty();
                manualPath = managedChoice ? saved.managedFile : saved.relativePath;
                if (managedChoice)
                    managedPath = ManagedCoverFile::path(request.manualLookup->settings,
                        request.manualLookup->identity, saved.managedFile);
            }
            else manualError = saved.errorMessage;
        }
    }
    const bool hasManual = !manualPath.isEmpty();
    if (hasManual && manualError.isEmpty()) {
        auto selected = managedChoice
            ? (managedPath.isEmpty() ? result(CoverArtStatus::Error,
                   QStringLiteral("封面副本不存在或不可读取。"))
                : loadImage(QFileInfo(managedPath).absolutePath(), QFileInfo(managedPath).fileName(),
                    request.forceReload, options, cancelled, cache))
            : loadImage(root, manualPath, request.forceReload, options, cancelled, cache);
        selected.manualRelativePath = manualPath;
        if (selected.status == CoverArtStatus::Ready) {
            selected.manual = true;
            return selected;
        }
        if (selected.status == CoverArtStatus::Cancelled || request.manualOnly) return selected;
        manualError = QStringLiteral("手动封面暂不可用，已保留选择并回退自动查找。");
    } else if (request.manualOnly) {
        return result(CoverArtStatus::Error, manualError.isEmpty()
            ? QStringLiteral("请选择作品目录内部的图片。") : manualError);
    }
    const auto finish = [&manualPath, &manualError](CoverArtResult loaded) {
        if (!manualError.isEmpty() && loaded.status != CoverArtStatus::Cancelled) {
            loaded.manualFallback = true;
            loaded.manualRelativePath = manualPath;
            loaded.message = loaded.message.isEmpty() ? manualError : manualError + u'\n' + loaded.message;
        }
        return loaded;
    };

    const auto audioStem = request.libraryWork ? QString{}
        : QFileInfo(request.audioPath).completeBaseName().toCaseFolded();
    QVector<fs::path> directories{rootPath};
    qsizetype entries = 0;
    qsizetype attempted = 0;
    bool hadFailure = false;
    const int maximumDepth = request.libraryWork ? options.maxDepth : 0;
    for (int depth = 0; depth <= maximumDepth && !directories.isEmpty(); ++depth) {
        QVector<fs::path> nextDirectories;
        QVector<Candidate> candidates;
        for (const auto &directory : directories) {
            if (isCancelled(cancelled)) return result(CoverArtStatus::Cancelled, {});
            error.clear();
            const QFileInfo directoryInfo(qtPath(directory));
            const auto status = fs::symlink_status(directory, error);
            if (error || !fs::is_directory(status) || linked(directoryInfo, status)) {
                hadFailure = true; continue;
            }
            const auto physical = fs::canonical(directory, error);
            if (error || (depth && !inside(physical, canonicalRoot))) {
                hadFailure = true; continue;
            }
            fs::directory_iterator iterator(directory, error), end;
            if (error) { hadFailure = true; continue; }
            while (iterator != end) {
                if (isCancelled(cancelled)) return result(CoverArtStatus::Cancelled, {});
                if (entries >= options.maxEntries)
                    return finish(result(CoverArtStatus::LimitExceeded, QStringLiteral("封面目录枚举达到上限。")));
                ++entries;
                const auto entry = *iterator;
                const QString path = qtPath(entry.path());
                const QFileInfo info(path);
                const auto entryStatus = entry.symlink_status(error);
                if (error) { hadFailure = true; error.clear(); }
                else if (!linked(info, entryStatus)) {
                    if (fs::is_directory(entryStatus) && depth < maximumDepth)
                        nextDirectories.append(entry.path());
                    else if (fs::is_regular_file(entryStatus) && supportedSuffix(path))
                        candidates.append({path, namePriority(info, audioStem),
                                           NaturalSortKey(QDir(root).relativeFilePath(path))});
                }
                iterator.increment(error);
                if (error) { hadFailure = true; error.clear(); break; }
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
            return a.priority != b.priority ? a.priority < b.priority : a.key.compare(b.key) < 0;
        });
        for (const auto &candidate : candidates) {
            if (isCancelled(cancelled)) return result(CoverArtStatus::Cancelled, {});
            if (attempted >= options.maxCandidates)
                return finish(result(CoverArtStatus::LimitExceeded, QStringLiteral("封面候选解码达到上限。")));
            ++attempted;
            auto loaded = loadImage(root, QDir(root).relativeFilePath(candidate.path),
                request.forceReload, options, cancelled, cache);
            if (loaded.status == CoverArtStatus::Ready || loaded.status == CoverArtStatus::Cancelled)
                return finish(std::move(loaded));
            hadFailure = true;
        }
        directories = std::move(nextDirectories);
    }
    return finish(hadFailure ? result(CoverArtStatus::Error, QStringLiteral("未找到可读取的封面图片。"))
                      : result(CoverArtStatus::NotFound, QStringLiteral("暂无封面。")));
}
} // namespace asmr
