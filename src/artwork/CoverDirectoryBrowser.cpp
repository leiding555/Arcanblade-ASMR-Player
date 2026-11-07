#include "artwork/CoverDirectoryBrowser.h"

#include "library/NaturalSort.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <filesystem>
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
bool linked(const fs::path &path, const fs::file_status &status)
{
    const QFileInfo info(qtPath(path));
    return fs::is_symlink(status) || info.isSymbolicLink() || info.isJunction();
}
bool within(const fs::path &path, const fs::path &root)
{
    auto ancestor = path;
    while (!ancestor.empty()) {
        std::error_code error;
        if (fs::equivalent(ancestor, root, error) && !error) return true;
        const auto parent = ancestor.parent_path();
        if (parent == ancestor) break;
        ancestor = parent;
    }
    return false;
}
bool imageSuffix(const QString &path)
{
    const auto suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg")
        || suffix == QLatin1String("png") || suffix == QLatin1String("webp")
        || suffix == QLatin1String("bmp") || suffix == QLatin1String("gif");
}
}

CoverDirectoryPage CoverDirectoryBrowser::list(const QString &rootPath,
    const QString &relativeDirectory, const std::shared_ptr<std::atomic_bool> &cancelled,
    qsizetype maxEntries)
{
    const auto stopped = [&] { return cancelled && cancelled->load(std::memory_order_relaxed); };
    const auto fail = [&](CoverDirectoryStatus status, const QString &message) {
        return CoverDirectoryPage{status, relativeDirectory, {}, message};
    };
    if (stopped()) return fail(CoverDirectoryStatus::Cancelled, {});
    if (rootPath.isEmpty() || rootPath.contains(QChar::Null)
        || !QDir::isAbsolutePath(rootPath) || maxEntries < 0
        || relativeDirectory.contains(QChar::Null) || relativeDirectory.contains(QLatin1Char('\\'))
        || relativeDirectory.contains(QLatin1Char(':'))
        || QDir::isAbsolutePath(relativeDirectory))
        return fail(CoverDirectoryStatus::Error, QStringLiteral("作品目录路径无效。"));
    const auto components = relativeDirectory.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    if (!relativeDirectory.isEmpty())
        for (const auto &part : components)
            if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String(".."))
                return fail(CoverDirectoryStatus::Error, QStringLiteral("目录路径必须位于作品内部。"));

    const fs::path root = nativePath(QDir::cleanPath(rootPath));
    std::error_code error;
    const auto rootStatus = fs::symlink_status(root, error);
    if (error || !fs::is_directory(rootStatus) || linked(root, rootStatus))
        return fail(CoverDirectoryStatus::Error, QStringLiteral("作品目录不存在、不可读或为链接。"));
    const auto physicalRoot = fs::canonical(root, error);
    if (error) return fail(CoverDirectoryStatus::Error, QStringLiteral("无法确认作品目录的实际路径。"));
    auto directory = root;
    if (!relativeDirectory.isEmpty()) {
        for (const auto &part : components) {
            if (stopped()) return fail(CoverDirectoryStatus::Cancelled, {});
            directory /= nativePath(part);
            const auto status = fs::symlink_status(directory, error);
            if (error || !fs::is_directory(status) || linked(directory, status))
                return fail(CoverDirectoryStatus::Error, QStringLiteral("目录不存在、不可读或为链接。"));
            const auto physical = fs::canonical(directory, error);
            if (error || !within(physical, physicalRoot))
                return fail(CoverDirectoryStatus::Error, QStringLiteral("目录超出作品范围。"));
        }
    }
    fs::directory_iterator iterator(directory, error), end;
    if (error) return fail(CoverDirectoryStatus::Error, QStringLiteral("无法读取目录，可能没有权限。"));
    struct SortedEntry { CoverDirectoryEntry entry; NaturalSortKey key; };
    QVector<SortedEntry> entries;
    qsizetype counted = 0;
    while (iterator != end) {
        if (stopped()) return fail(CoverDirectoryStatus::Cancelled, {});
        if (counted++ >= maxEntries)
            return fail(CoverDirectoryStatus::LimitExceeded, QStringLiteral("目录包含超过 %1 个条目，请整理后重试。未显示不完整结果。").arg(maxEntries));
        const auto entry = *iterator;
        const auto status = entry.symlink_status(error);
        if (error) return fail(CoverDirectoryStatus::Error, QStringLiteral("读取目录条目失败，请重试。"));
        if (!linked(entry.path(), status)) {
            const bool isDirectory = fs::is_directory(status);
            const QString name = qtPath(entry.path().filename());
            if (isDirectory || (fs::is_regular_file(status) && imageSuffix(name))) {
                const QString relative = relativeDirectory.isEmpty() ? name
                    : relativeDirectory + QLatin1Char('/') + name;
                entries.append({{relative, name, isDirectory}, NaturalSortKey(name)});
            }
        }
        iterator.increment(error);
        if (error) return fail(CoverDirectoryStatus::Error, QStringLiteral("目录读取中断，请重试。"));
    }
    // A folder can be replaced while its entries are being read. Do not adopt
    // a page whose final directory no longer has the validated identity.
    const auto finalRootStatus = fs::symlink_status(root, error);
    if (error || linked(root, finalRootStatus) || !fs::is_directory(finalRootStatus))
        return fail(CoverDirectoryStatus::Error, QStringLiteral("作品目录已变化，请重试。"));
    const auto finalStatus = fs::symlink_status(directory, error);
    if (error || linked(directory, finalStatus) || !fs::is_directory(finalStatus))
        return fail(CoverDirectoryStatus::Error, QStringLiteral("当前目录已变化，请重试。"));
    const auto physicalDirectory = fs::canonical(directory, error);
    if (error || !within(physicalDirectory, physicalRoot))
        return fail(CoverDirectoryStatus::Error, QStringLiteral("当前目录已超出作品范围。"));
    std::sort(entries.begin(), entries.end(), [](const auto &left, const auto &right) {
        if (left.entry.directory != right.entry.directory) return left.entry.directory;
        return left.key.compare(right.key) < 0;
    });
    CoverDirectoryPage page{CoverDirectoryStatus::Ready, relativeDirectory, {}, {}};
    page.entries.reserve(entries.size());
    for (auto &entry : entries) page.entries.append(std::move(entry.entry));
    if (stopped()) return fail(CoverDirectoryStatus::Cancelled, {});
    return page;
}
} // namespace asmr
