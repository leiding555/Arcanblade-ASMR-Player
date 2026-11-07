#include "WorkDirectoryDiscovery.h"

#include "library/NaturalSort.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>

#include <algorithm>
#include <filesystem>
#include <system_error>
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
    return QFile::decodeName(path.native().c_str());
#endif
}

bool isLink(const fs::file_status &status, const QString &path)
{
    const QFileInfo information(path);
    return fs::is_symlink(status) || information.isSymbolicLink()
        || information.isJunction();
}

// Accept an unambiguous absolute filesystem path, never a resource, URL,
// drive-relative/root-relative path or dot component. Cleaning is lexical:
// canonicalPath would silently follow links and alter persistent identities.
QString absolutePath(const QString &input)
{
    if (input.isEmpty() || input.contains(QChar::Null))
        return {};
    const QString path = QDir::fromNativeSeparators(input);
#ifdef Q_OS_WIN
    if (path.startsWith(QStringLiteral("//?/"))
        || path.startsWith(QStringLiteral("//./")))
        return {};
    const bool drive = path.size() >= 3 && path.at(0).isLetter()
        && path.at(1) == u':' && path.at(2) == u'/';
    const bool unc = path.startsWith(QStringLiteral("//"));
    if (!drive && !unc)
        return {};
    if (path.mid(drive ? 2 : 0).contains(u':'))
        return {};
    if (unc && path.mid(2).split(u'/', Qt::SkipEmptyParts).size() < 2)
        return {};
#else
    if (!QDir::isAbsolutePath(path) || path.startsWith(u':')
        || path.contains(QStringLiteral("://")))
        return {};
#endif
    const QStringList components = path.split(u'/', Qt::SkipEmptyParts);
    for (const QString &component : components) {
        if (component == QStringLiteral(".") || component == QStringLiteral(".."))
            return {};
    }
    return QDir::cleanPath(path);
}

struct Entry {
    QString directory;
    QString name;
    NaturalSortKey sortKey;
};

} // namespace

WorkDirectoryDiscoveryResult WorkDirectoryDiscovery::discover(
    const QString &root, const QVector<LibraryWork> &knownWorks,
    const WorkDirectoryDiscoveryOptions &options,
    const std::function<bool()> &cancellationRequested)
{
    WorkDirectoryDiscoveryResult result;
    const auto diagnose = [&](const QString &message) {
        if (result.diagnostics.size() < options.maximumDiagnostics)
            result.diagnostics.push_back(message);
        else
            ++result.suppressedDiagnostics;
    };
    const auto fail = [&](const QString &message) {
        result.success = false;
        result.candidates.clear();
        result.errorMessage = message;
        diagnose(message);
    };
    const auto cancelled = [&]() {
        if (!cancellationRequested || !cancellationRequested())
            return false;
        result.cancelled = true;
        fail(QStringLiteral("总目录枚举已取消，未返回部分作品列表。"));
        return true;
    };
    if (options.maximumEntries <= 0 || options.maximumDiagnostics <= 0) {
        result.errorMessage = QStringLiteral("目录项和诊断数量上限必须大于零。");
        result.diagnostics.push_back(result.errorMessage);
        return result;
    }
    result.rootDirectory = absolutePath(root);
    if (result.rootDirectory.isEmpty()) {
        fail(QStringLiteral("请选择明确的绝对本地目录路径，不支持 URL、资源路径或点路径分量。"));
        return result;
    }
    if (cancelled())
        return result;

    const auto checkRoot = [&]() {
        std::error_code error;
        const auto status = fs::symlink_status(nativePath(result.rootDirectory), error);
        if (error || !fs::exists(status)) {
            fail(error ? QStringLiteral("无法检查总目录 %1：%2")
                             .arg(result.rootDirectory,
                                  QString::fromLocal8Bit(error.message().c_str()))
                       : QStringLiteral("总目录不存在：%1").arg(result.rootDirectory));
            return false;
        }
        if (isLink(status, result.rootDirectory)) {
            fail(QStringLiteral("总目录不能是符号链接或 junction：%1").arg(result.rootDirectory));
            return false;
        }
        if (!fs::is_directory(status)) {
            fail(QStringLiteral("指定路径不是目录：%1").arg(result.rootDirectory));
            return false;
        }
        return true;
    };
    if (!checkRoot())
        return result;

    std::error_code error;
    fs::directory_iterator iterator(nativePath(result.rootDirectory),
                                    fs::directory_options::none, error);
    const fs::directory_iterator end;
    if (error) {
        fail(QStringLiteral("无法读取总目录 %1：%2")
                 .arg(result.rootDirectory, QString::fromLocal8Bit(error.message().c_str())));
        return result;
    }

    QVector<Entry> entries;
    while (iterator != end) {
        if (cancelled())
            return result;
        if (entries.size() >= options.maximumEntries) {
            fail(QStringLiteral("总目录条目超过上限 %1，未返回部分作品列表。")
                     .arg(options.maximumEntries));
            return result;
        }
        const QString name = qtPath(iterator->path().filename());
        const QString path = QDir::cleanPath(qtPath(iterator->path()));
        // A first-level entry must remain a direct component of this root.
        if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")
            || name.contains(u'/')
#ifdef Q_OS_WIN
            || name.contains(u'\\')
#endif
            || QDir::cleanPath(QFileInfo(path).absolutePath()) != result.rootDirectory) {
            fail(QStringLiteral("总目录返回了不明确的目录项路径，未返回部分作品列表。"));
            return result;
        }
        entries.push_back({path, name, NaturalSortKey(name)});
        iterator.increment(error);
        if (error) {
            fail(QStringLiteral("总目录枚举未完成 %1：%2")
                     .arg(result.rootDirectory, QString::fromLocal8Bit(error.message().c_str())));
            return result;
        }
    }
    if (cancelled() || !checkRoot())
        return result;
    std::sort(entries.begin(), entries.end(), [](const Entry &left, const Entry &right) {
        const int order = left.sortKey.compare(right.sortKey);
        return order != 0 ? order < 0 : left.directory < right.directory;
    });
    if (cancelled())
        return result;

    QHash<QString, qint64> existingIds;
    QMap<QString, QVector<LibraryWork>> worksByKey;
    for (const LibraryWork &work : knownWorks) {
        if (cancelled())
            return result;
        const QString cleaned = absolutePath(work.rootPath);
        if (work.id <= 0 || cleaned.isEmpty())
            continue;
        const QString key = cleaned.toCaseFolded();
        worksByKey[key].push_back(work);
        const auto found = existingIds.constFind(key);
        if (found == existingIds.cend() || work.id < *found)
            existingIds.insert(key, work.id);
    }
    if (cancelled())
        return result;

    for (auto &group : worksByKey) {
        std::sort(group.begin(), group.end(), [](const LibraryWork &left, const LibraryWork &right) {
            return left.id < right.id;
        });
    }

    QVector<WorkDirectoryCandidate> candidates;
    candidates.reserve(entries.size());
    for (const Entry &entry : entries) {
        if (cancelled())
            return result;
        error.clear();
        const auto status = fs::symlink_status(nativePath(entry.directory), error);
        if (error || !fs::exists(status)) {
            fail(error ? QStringLiteral("无法检查总目录项 %1：%2")
                             .arg(entry.directory, QString::fromLocal8Bit(error.message().c_str()))
                       : QStringLiteral("总目录项在枚举过程中已消失：%1").arg(entry.directory));
            return result;
        }
        if (isLink(status, entry.directory)) {
            ++result.linksSkipped;
            diagnose(QStringLiteral("已跳过符号链接或 junction：%1").arg(entry.directory));
            continue;
        }
        if (!fs::is_directory(status)) {
            if (fs::is_regular_file(status)) {
                ++result.rootFilesSkipped;
                diagnose(QStringLiteral("已跳过总目录中的散落文件：%1").arg(entry.directory));
            } else {
                diagnose(QStringLiteral("已跳过非普通文件目录项：%1").arg(entry.directory));
            }
            continue;
        }

        WorkDirectoryCandidate candidate;
        candidate.directory = entry.directory;
        candidate.name = entry.name;
        const QString key = entry.directory.toCaseFolded();
        candidate.existingWorkId = existingIds.value(key, 0);

        // Compare only complete path components: Book1 and Book10 are siblings.
        // Exact matches remain ordinary updates, never overlap warnings.
        QMap<QString, QVector<LibraryWork>> overlaps;
        QString ancestor = key;
        while (true) {
            const qsizetype slash = ancestor.lastIndexOf(u'/');
            if (slash < 0)
                break;
            const QString parent = slash == 0 ? QStringLiteral("/")
                : ancestor.left(slash == 2 && ancestor.at(1) == u':' ? slash + 1 : slash);
            if (parent == ancestor)
                break;
            const auto found = worksByKey.constFind(parent);
            if (found != worksByKey.cend())
                overlaps.insert(parent, *found);
            ancestor = parent;
        }
        const QString prefix = key.endsWith(u'/') ? key : key + u'/';
        for (auto descendant = worksByKey.lowerBound(prefix);
             descendant != worksByKey.cend() && descendant.key().startsWith(prefix);
             ++descendant) {
            if (cancelled())
                return result;
            overlaps.insert(descendant.key(), descendant.value());
        }
        for (const auto &group : overlaps)
            candidate.overlappingWorks += group;
        candidates.push_back(std::move(candidate));
    }
    if (cancelled() || !checkRoot())
        return result;
    result.candidates = std::move(candidates);
    result.success = true;
    return result;
}

} // namespace asmr
