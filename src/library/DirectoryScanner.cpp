#include "DirectoryScanner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

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

QString errorText(const std::error_code &error)
{
    return QString::fromLocal8Bit(error.message().c_str());
}

bool accessError(const std::error_code &error)
{
    return error == std::errc::permission_denied
        || error == std::errc::operation_not_permitted;
}

bool isLink(const fs::file_status &status, const QString &path)
{
    const QFileInfo information(path);
    return fs::is_symlink(status) || information.isSymbolicLink()
        || information.isJunction();
}

struct PendingDirectory {
    QString absolutePath;
    QString relativePath;
    int depth;
};

struct DirectoryEntry {
    fs::directory_entry entry;
    QString fileName;
};

QString childRelativePath(const QString &parent, const QString &fileName)
{
    return parent.isEmpty() ? fileName : parent + QLatin1Char('/') + fileName;
}

void sortResult(DirectoryScanResult &result)
{
    const auto byRelativePath = [](const DiscoveredFile &left,
                                   const DiscoveredFile &right) {
        return QString::compare(left.relativePath, right.relativePath,
                                Qt::CaseSensitive) < 0;
    };
    std::sort(result.audioFiles.begin(), result.audioFiles.end(), byRelativePath);
    std::sort(result.subtitleFiles.begin(), result.subtitleFiles.end(), byRelativePath);
    std::sort(result.referenceFiles.begin(), result.referenceFiles.end(), [](const auto &left, const auto &right) {
        return QString::compare(left.relativePath, right.relativePath, Qt::CaseSensitive) < 0;
    });
    std::sort(result.diagnostics.begin(), result.diagnostics.end(),
              [](const ScanDiagnostic &left, const ScanDiagnostic &right) {
        const int pathOrder = QString::compare(left.relativePath, right.relativePath,
                                               Qt::CaseSensitive);
        if (pathOrder != 0)
            return pathOrder < 0;
        if (left.code != right.code)
            return left.code < right.code;
        if (left.severity != right.severity)
            return left.severity < right.severity;
        return QString::compare(left.message, right.message, Qt::CaseSensitive) < 0;
    });
}

} // namespace

bool DirectoryScanResult::isComplete() const
{
    return status == DirectoryScanStatus::Completed
        || status == DirectoryScanStatus::EmptyDirectory
        || status == DirectoryScanStatus::NoSupportedFiles;
}

DirectoryScanResult DirectoryScanner::scan(
    const QString &directory, const DirectoryScanOptions &options,
    const std::function<bool()> &cancellationRequested)
{
    DirectoryScanResult result;
    const auto diagnose = [&](ScanDiagnosticSeverity severity,
                              ScanDiagnosticCode code,
                              const QString &absolutePath,
                              const QString &relativePath,
                              const QString &message) {
        if (result.diagnostics.size() < options.maximumDiagnostics) {
            result.diagnostics.push_back({severity, code, absolutePath,
                                          relativePath, message});
        } else {
            ++result.suppressedDiagnostics;
        }
    };

    if (options.maximumDepth < 0 || options.maximumEntries <= 0
        || options.maximumDiagnostics <= 0) {
        // Invalid options still produce a diagnostic even when the requested
        // diagnostic budget itself is invalid.
        result.diagnostics.push_back({
            ScanDiagnosticSeverity::Error, ScanDiagnosticCode::InvalidOptions,
            {}, {}, QStringLiteral("扫描深度必须非负，条目和诊断数量上限必须大于零。")});
        return result;
    }

    const bool drivePath = directory.size() >= 3 && directory.at(0).isLetter()
        && directory.at(1) == QLatin1Char(':')
        && (directory.at(2) == QLatin1Char('/') || directory.at(2) == QLatin1Char('\\'));
    if (directory.isEmpty() || directory.contains(QChar::Null)
        || directory.startsWith(QLatin1Char(':'))
        || directory.startsWith(QStringLiteral("qrc:"), Qt::CaseInsensitive)
        || directory.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)
        || (!drivePath && directory.contains(QStringLiteral("://")))) {
        diagnose(ScanDiagnosticSeverity::Error, ScanDiagnosticCode::InvalidPath,
                 {}, {}, QStringLiteral("请提供非空的本地目录路径，不支持 URL、Qt 资源路径或空字符。"));
        return result;
    }

    result.rootDirectory = QDir::cleanPath(
        QDir::fromNativeSeparators(QFileInfo(directory).absoluteFilePath()));

    const auto cancelled = [&]() {
        if (!cancellationRequested || !cancellationRequested())
            return false;
        result.status = DirectoryScanStatus::Cancelled;
        diagnose(ScanDiagnosticSeverity::Info, ScanDiagnosticCode::Cancelled,
                 result.rootDirectory, {}, QStringLiteral("目录扫描已取消，发现的文件记录已丢弃。"));
        return true;
    };
    if (cancelled())
        return result;

    std::error_code error;
    const fs::file_status rootStatus = fs::symlink_status(nativePath(result.rootDirectory), error);
    if (error || !fs::exists(rootStatus)) {
        ScanDiagnosticCode code = ScanDiagnosticCode::DirectoryReadFailed;
        if (accessError(error))
            code = ScanDiagnosticCode::AccessDenied;
        else if (!error || error == std::errc::no_such_file_or_directory)
            code = ScanDiagnosticCode::DirectoryNotFound;
        else if (error == std::errc::not_a_directory)
            code = ScanDiagnosticCode::NotDirectory;
        else if (error == std::errc::invalid_argument
                 || error == std::errc::filename_too_long)
            code = ScanDiagnosticCode::InvalidPath;
        diagnose(ScanDiagnosticSeverity::Error, code, result.rootDirectory, {},
                 error ? QStringLiteral("无法检查作品目录：%1").arg(errorText(error))
                       : QStringLiteral("作品目录不存在。"));
        return result;
    }
    if (isLink(rootStatus, result.rootDirectory)) {
        diagnose(ScanDiagnosticSeverity::Error, ScanDiagnosticCode::LinkSkipped,
                 result.rootDirectory, {},
                 QStringLiteral("作品根目录不能是符号链接或 junction；请选择实际目录。"));
        return result;
    }
    if (!fs::is_directory(rootStatus)) {
        diagnose(ScanDiagnosticSeverity::Error, ScanDiagnosticCode::NotDirectory,
                 result.rootDirectory, {}, QStringLiteral("指定路径不是目录。"));
        return result;
    }

    QVector<PendingDirectory> pending{{result.rootDirectory, {}, 0}};
    bool partialFailure = false;
    bool aborted = false;
    while (!pending.isEmpty() && !aborted) {
        if (cancelled())
            break;
        const PendingDirectory current = pending.takeLast();
        error.clear();
        const fs::file_status currentStatus = fs::symlink_status(
            nativePath(current.absolutePath), error);
        const bool currentIsLink = !error && fs::exists(currentStatus)
            && isLink(currentStatus, current.absolutePath);
        if (error || currentIsLink || !fs::is_directory(currentStatus)) {
            const bool root = current.depth == 0;
            const ScanDiagnosticCode code = currentIsLink
                ? ScanDiagnosticCode::LinkSkipped
                : accessError(error) ? ScanDiagnosticCode::AccessDenied
                                     : ScanDiagnosticCode::MetadataFailed;
            diagnose(currentIsLink && !root ? ScanDiagnosticSeverity::Warning
                                            : ScanDiagnosticSeverity::Error,
                     code, current.absolutePath, current.relativePath,
                     currentIsLink
                         ? QStringLiteral("目录在扫描过程中变为链接，未跟随其目标。")
                         : error ? QStringLiteral("无法再次检查目录：%1").arg(errorText(error))
                                 : QStringLiteral("目录在扫描过程中已消失或不再是目录。"));
            if (root) {
                result.status = DirectoryScanStatus::Failed;
                aborted = true;
            } else {
                ++result.skippedEntries;
                if (!currentIsLink)
                    partialFailure = true;
            }
            continue;
        }
        // Recheck queued directories before opening them. This narrows, but
        // cannot eliminate, races with concurrent filesystem modifications.
        error.clear();
        fs::directory_iterator iterator(nativePath(current.absolutePath),
                                        fs::directory_options::none, error);
        const fs::directory_iterator end;
        if (error) {
            const ScanDiagnosticCode code = accessError(error)
                ? ScanDiagnosticCode::AccessDenied : ScanDiagnosticCode::DirectoryReadFailed;
            diagnose(ScanDiagnosticSeverity::Error, code, current.absolutePath,
                     current.relativePath,
                     QStringLiteral("无法读取目录：%1").arg(errorText(error)));
            if (current.depth == 0) {
                result.status = DirectoryScanStatus::Failed;
                aborted = true;
            } else {
                ++result.skippedEntries;
                partialFailure = true;
            }
            continue;
        }

        QVector<DirectoryEntry> entries;
        while (iterator != end) {
            if (cancelled()) {
                aborted = true;
                break;
            }
            if (result.visitedEntries >= options.maximumEntries) {
                result.status = DirectoryScanStatus::LimitExceeded;
                diagnose(ScanDiagnosticSeverity::Error, ScanDiagnosticCode::EntryLimitReached,
                         current.absolutePath, current.relativePath,
                         QStringLiteral("发现条目数量超过上限 %1，文件记录已丢弃。")
                             .arg(options.maximumEntries));
                aborted = true;
                break;
            }
            ++result.visitedEntries;
            entries.push_back({*iterator, qtPath(iterator->path().filename())});
            iterator.increment(error);
            if (error) {
                diagnose(ScanDiagnosticSeverity::Error,
                         accessError(error) ? ScanDiagnosticCode::AccessDenied
                                            : ScanDiagnosticCode::DirectoryReadFailed,
                         current.absolutePath, current.relativePath,
                         QStringLiteral("目录枚举未能完成：%1").arg(errorText(error)));
                partialFailure = true;
                break;
            }
        }
        if (aborted)
            break;
        if (cancelled())
            break;
        std::sort(entries.begin(), entries.end(),
                  [](const DirectoryEntry &left, const DirectoryEntry &right) {
            return QString::compare(left.fileName, right.fileName, Qt::CaseSensitive) < 0;
        });
        if (cancelled())
            break;

        QVector<PendingDirectory> children;
        for (const DirectoryEntry &entry : entries) {
            if (cancelled()) {
                aborted = true;
                break;
            }
            const QString absolutePath = qtPath(entry.entry.path());
            const QString relativePath = childRelativePath(current.relativePath, entry.fileName);
            error.clear();
            // Query current metadata rather than a directory_entry cache: a file
            // removed between enumeration and inspection must be diagnosed.
            const fs::file_status status = fs::symlink_status(entry.entry.path(), error);
            if (error || !fs::exists(status)) {
                ++result.skippedEntries;
                partialFailure = true;
                diagnose(ScanDiagnosticSeverity::Error,
                         accessError(error) ? ScanDiagnosticCode::AccessDenied
                                            : ScanDiagnosticCode::MetadataFailed,
                         absolutePath, relativePath,
                         error ? QStringLiteral("无法检查目录项：%1").arg(errorText(error))
                               : QStringLiteral("目录项在扫描过程中已消失。"));
                continue;
            }
            if (isLink(status, absolutePath)) {
                ++result.skippedEntries;
                diagnose(ScanDiagnosticSeverity::Warning, ScanDiagnosticCode::LinkSkipped,
                         absolutePath, relativePath,
                         QStringLiteral("已跳过符号链接或 junction，不跟随其目标。"));
                continue;
            }
            if (fs::is_directory(status)) {
                if (current.depth >= options.maximumDepth) {
                    result.status = DirectoryScanStatus::LimitExceeded;
                    diagnose(ScanDiagnosticSeverity::Error, ScanDiagnosticCode::DepthLimitReached,
                             absolutePath, relativePath,
                             QStringLiteral("目录深度超过上限 %1，文件记录已丢弃。")
                                 .arg(options.maximumDepth));
                    aborted = true;
                    break;
                }
                children.push_back({absolutePath, relativePath, current.depth + 1});
                continue;
            }
            if (!fs::is_regular_file(status)) {
                ++result.skippedEntries;
                diagnose(ScanDiagnosticSeverity::Warning, ScanDiagnosticCode::UnsupportedEntry,
                         absolutePath, relativePath,
                         QStringLiteral("已跳过非普通文件目录项。"));
                continue;
            }

            const QString extension = QFileInfo(entry.fileName).suffix().toLower();
            const bool audio = extension == QStringLiteral("mp3")
                || extension == QStringLiteral("wav");
            const bool subtitle = extension == QStringLiteral("srt")
                || extension == QStringLiteral("vtt") || extension == QStringLiteral("lrc");
            const bool reference = subtitle || extension == QStringLiteral("txt")
                || extension == QStringLiteral("md") || extension == QStringLiteral("markdown")
                || extension == QStringLiteral("nfo") || extension == QStringLiteral("log")
                || extension == QStringLiteral("cue") || extension == QStringLiteral("ks")
                || extension == QStringLiteral("readme") || extension == QStringLiteral("pdf")
                || (extension.isEmpty() && entry.fileName.compare(QStringLiteral("README"), Qt::CaseInsensitive) == 0);
            if (reference) {
                WorkReferenceCandidate candidate;
                candidate.absolutePath = absolutePath;
                candidate.relativePath = relativePath;
                candidate.format = extension.isEmpty() ? QStringLiteral("readme") : extension;
                result.referenceFiles.push_back(std::move(candidate));
            }
            if (!audio && !subtitle && !reference) {
                ++result.skippedEntries;
                diagnose(ScanDiagnosticSeverity::Info, ScanDiagnosticCode::UnsupportedFile,
                         absolutePath, relativePath,
                         QStringLiteral("已跳过不支持的文件扩展名。"));
                continue;
            }
            if (!audio && !subtitle)
                continue;
            DiscoveredFile file{absolutePath, relativePath, entry.fileName, extension,
                                audio ? LibraryFileType::Audio : LibraryFileType::Subtitle};
            if (audio)
                result.audioFiles.push_back(std::move(file));
            else
                result.subtitleFiles.push_back(std::move(file));
        }
        if (aborted)
            break;
        // Stack order yields a case-sensitive, locale-independent depth-first
        // traversal. Files are sorted globally below rather than by OS order.
        for (auto child = children.crbegin(); child != children.crend(); ++child)
            pending.push_back(*child);
    }

    if (result.status == DirectoryScanStatus::Cancelled
        || result.status == DirectoryScanStatus::LimitExceeded || aborted) {
        result.audioFiles.clear();
        result.subtitleFiles.clear();
        result.referenceFiles.clear();
    } else if (cancelled()) {
        result.audioFiles.clear();
        result.subtitleFiles.clear();
        result.referenceFiles.clear();
    } else if (partialFailure) {
        result.status = DirectoryScanStatus::PartialFailure;
    } else if (result.visitedEntries == 0) {
        result.status = DirectoryScanStatus::EmptyDirectory;
    } else if (result.audioFiles.isEmpty() && result.subtitleFiles.isEmpty()) {
        result.status = DirectoryScanStatus::NoSupportedFiles;
    } else {
        result.status = DirectoryScanStatus::Completed;
    }

    sortResult(result);
    // Sorting is bounded by the configured entry/diagnostic budgets. Check once
    // more so a cancellation observed during sorting cannot become a success.
    if (result.isComplete() || result.status == DirectoryScanStatus::PartialFailure) {
        if (cancelled()) {
            result.audioFiles.clear();
            result.subtitleFiles.clear();
            result.referenceFiles.clear();
            sortResult(result);
        }
    }
    return result;
}

} // namespace asmr
