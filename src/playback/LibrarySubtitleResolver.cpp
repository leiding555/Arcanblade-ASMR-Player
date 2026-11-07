#include "playback/LibrarySubtitleResolver.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <filesystem>

namespace asmr {
namespace {
std::filesystem::path nativePath(const QString &path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}
QString qtPath(const std::filesystem::path &path)
{
#ifdef Q_OS_WIN
    return QDir::fromNativeSeparators(QString::fromStdWString(path.wstring()));
#else
    return QString::fromStdString(path.string());
#endif
}
bool contained(const std::filesystem::path &file, const std::filesystem::path &root)
{
    auto f = file.begin();
    for (auto r = root.begin(); r != root.end(); ++r, ++f) {
        if (f == file.end())
            return false;
#ifdef Q_OS_WIN
        if (QString::fromStdWString(f->wstring()).compare(
                QString::fromStdWString(r->wstring()), Qt::CaseInsensitive) != 0)
            return false;
#else
        if (*f != *r)
            return false;
#endif
    }
    return f != file.end();
}
bool safeRelative(QString path)
{
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (path.isEmpty() || path.startsWith(QLatin1Char('/')) || QDir::isAbsolutePath(path))
        return false;
    for (const auto &part : path.split(QLatin1Char('/'))) {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")
            || part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' ')))
            return false;
        for (const auto ch : part) {
            if (ch.unicode() < 32 || QStringLiteral("<>:\"|?*").contains(ch))
                return false;
        }
        const auto base = part.section(QLatin1Char('.'), 0, 0).toUpper();
        if (base == QStringLiteral("CON") || base == QStringLiteral("PRN")
            || base == QStringLiteral("AUX") || base == QStringLiteral("NUL")
            || base == QStringLiteral("CONIN$") || base == QStringLiteral("CONOUT$")
            || (base.size() == 4 && (base.startsWith(QStringLiteral("COM"))
                || base.startsWith(QStringLiteral("LPT")))
                && (QStringLiteral("123456789").contains(base[3])
                    || base[3] == QChar(0x00b9) || base[3] == QChar(0x00b2)
                    || base[3] == QChar(0x00b3))))
            return false;
    }
    return true;
}
}
LibrarySubtitleOrderKey librarySubtitleOrderKey(const QString &validatedPath, const LibraryFile &file,
                                               const std::optional<GeneratedSubtitleInfo> &generation)
{
    LibrarySubtitleOrderKey key;
    key.relativePath = QDir::fromNativeSeparators(file.relativePath);
    key.id = file.id;
    if (generation && generation->completedAt.isValid()) {
        key.time = generation->completedAt;
        key.chinese = generation->language == QStringLiteral("zh")
            || (generation->language.isEmpty()
                && (generation->mode == QStringLiteral("offline-direct")
                    || generation->mode == QStringLiteral("cloud-translation")));
    } else if (!validatedPath.isEmpty()) {
        key.time = QFileInfo(validatedPath).lastModified();
    }
    return key;
}

LibrarySubtitlePathResult validateLibrarySubtitlePath(const QString &root, const LibraryFile &file)
{
    LibrarySubtitlePathResult result;
    result.issue = {LibrarySubtitleIssueCode::UnsafePath, file.id, file.relativePath,
                    QStringLiteral("字幕路径无效或越出作品目录。")};
    if (file.id <= 0 || file.workId <= 0 || file.type != LibraryFileType::Subtitle) {
        result.issue.code = LibrarySubtitleIssueCode::Identity;
        return result;
    }
    if (file.missing) {
        result.issue.code = LibrarySubtitleIssueCode::Missing;
        result.issue.message = QStringLiteral("指定字幕已在最近扫描中标记为缺失。");
        return result;
    }
    QString safeRoot = root;
    safeRoot.replace(QLatin1Char('\\'), QLatin1Char('/'));
    QString relative = file.relativePath;
    relative.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (!safeRelative(relative) || !QDir::isAbsolutePath(safeRoot)
        || safeRoot.contains(QChar::Null) || root.startsWith(QStringLiteral("\\\\?\\"))
        || safeRoot.startsWith(QStringLiteral("//?/")) || safeRoot.startsWith(QStringLiteral("//./")))
        return result;
    const QString path = QDir(root).filePath(relative);
    result.issue.path = path;
    std::error_code error;
    const auto canonicalRoot = std::filesystem::canonical(nativePath(root), error);
    if (error || !QFileInfo(root).isDir())
        return result;
    const auto canonicalFile = std::filesystem::canonical(nativePath(path), error);
    if (error || !QFileInfo(path).exists()) {
        result.issue.code = LibrarySubtitleIssueCode::Missing;
        result.issue.message = QStringLiteral("字幕文件不存在或无法访问。");
        return result;
    }
    if (!contained(canonicalFile, canonicalRoot))
        return result; // Resolves Windows junctions as well as file symlinks.
    if (!QFileInfo(path).isFile())
        return result;
    const auto suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("srt"))
        result.format = SubtitleFormat::Srt;
    else if (suffix == QStringLiteral("vtt"))
        result.format = SubtitleFormat::Vtt;
    else if (suffix == QStringLiteral("lrc"))
        result.format = SubtitleFormat::Lrc;
    else {
        result.issue.code = LibrarySubtitleIssueCode::Unsupported;
        result.issue.message = QStringLiteral("字幕扩展名不受支持。");
        return result;
    }
    // Open the resolved path, rather than retaining an intermediate junction alias.
    result.path = qtPath(canonicalFile);
    QFile probe(result.path);
    if (!probe.open(QIODevice::ReadOnly)) {
        result.issue.code = LibrarySubtitleIssueCode::Unreadable;
        result.issue.message = QStringLiteral("无法读取字幕：%1").arg(probe.errorString());
        return result;
    }
    result.success = true;
    return result;
}

LibrarySubtitleResolution resolveLibrarySubtitles(const QString &databasePath,
                                                 qint64 workId, qint64 audioFileId)
{
    LibrarySubtitleResolution result;
    LibraryRepository repository(databasePath); // Construct/use/destroy on this caller's worker.
    const auto opened = repository.open();
    if (opened.success) {
        result.snapshot = repository.trackSubtitles(workId, audioFileId);
        if (result.snapshot.success) {
            const auto previousFiles = result.snapshot.files;
            LibraryWorkSnapshotResult expected;
            expected.success = true;
            expected.work = result.snapshot.work;
            expected.files = result.snapshot.files;
            const auto cleaned = repository.pruneMissingSubtitles(expected);
            if (cleaned.success) {
                result.snapshot = repository.trackSubtitles(workId, audioFileId);
                if (result.snapshot.success) {
                    QSet<qint64> currentIds;
                    for (const auto &file : result.snapshot.files) currentIds.insert(file.id);
                    result.pruned = std::any_of(previousFiles.cbegin(), previousFiles.cend(), [&](const auto &file) {
                        return file.type == LibraryFileType::Subtitle && !currentIds.contains(file.id);
                    });
                }
            } else {
                result.snapshot.success = false;
                result.snapshot.diagnostics = cleaned.diagnostics;
            }
        }
    } else
        result.snapshot.diagnostics = opened.diagnostics;
    const auto closed = repository.close();
    if (!closed.success) {
        result.snapshot.success = false;
        result.snapshot.diagnostics += closed.diagnostics;
    }
    if (!result.snapshot.success) {
        for (const auto &diagnostic : result.snapshot.diagnostics)
            result.issues.push_back({LibrarySubtitleIssueCode::Database, 0,
                                     diagnostic.path, diagnostic.message});
        return result;
    }
    const auto &snapshot = result.snapshot;
    if (snapshot.audio.missing) {
        result.issues.push_back({LibrarySubtitleIssueCode::Missing, 0, snapshot.audio.relativePath,
                                 QStringLiteral("当前音轨已标记缺失，未自动选择字幕。")});
        return result;
    }
    if (snapshot.preference.state == SubtitlePreferenceState::None) {
        result.decision = LibrarySubtitleDecision::Disabled;
        return result;
    }
    QVector<QPair<LibrarySubtitleOrderKey, LibraryFile>> available;
    for (const auto &binding : snapshot.bindings) {
        // An explicit preference never falls back to a different subtitle.
        if (snapshot.preference.state == SubtitlePreferenceState::Subtitle
            && binding.subtitleFileId != snapshot.preference.subtitleFileId)
            continue;
        const auto file = std::find_if(snapshot.files.cbegin(), snapshot.files.cend(),
            [&binding](const auto &entry) { return entry.id == binding.subtitleFileId; });
        if (file == snapshot.files.cend() || file->workId != workId
            || binding.workId != workId || binding.audioFileId != audioFileId) {
            result.issues.push_back({LibrarySubtitleIssueCode::Identity, binding.subtitleFileId,
                                    {}, QStringLiteral("字幕绑定不属于当前音轨或作品。")});
            continue;
        }
        const auto checked = validateLibrarySubtitlePath(snapshot.work.rootPath, *file);
        if (!checked.success) {
            result.issues.push_back(checked.issue);
            continue;
        }
        available.append({librarySubtitleOrderKey(checked.path, *file, readGeneratedSubtitleInfo(checked.path)), *file});
    }
    std::sort(available.begin(), available.end(), [](const auto &left, const auto &right) {
        return librarySubtitleNewer(left.first, right.first);
    });
    for (const auto &candidate : available)
        result.availableSubtitleIds.push_back(candidate.second.id);
    if (!available.isEmpty()) {
        result.selectedFile = available.first().second;
        result.decision = LibrarySubtitleDecision::Selected;
    } else if (snapshot.preference.state == SubtitlePreferenceState::Subtitle) {
        result.decision = LibrarySubtitleDecision::Error;
        if (result.issues.isEmpty())
            result.issues.push_back({LibrarySubtitleIssueCode::Identity,
                snapshot.preference.subtitleFileId, {}, QStringLiteral("首选字幕没有有效绑定。")});
    } else
        result.decision = LibrarySubtitleDecision::NoAvailable;
    return result;
}

SubtitleFileLoadResult loadLibrarySubtitle(const QString &root, const LibraryFile &file)
{
    const auto before = validateLibrarySubtitlePath(root, file);
    if (!before.success)
        return {SubtitleFileError::OpenFailed, before.issue.message, {}, {}};
    auto result = SubtitleFileLoader::load(before.path, before.format);
    const auto after = validateLibrarySubtitlePath(root, file);
    if (!after.success || before.path != after.path)
        return {SubtitleFileError::OpenFailed, QStringLiteral("加载期间字幕路径变化或不可访问，已丢弃结果。"), {}, {}};
    return result;
}

} // namespace asmr
