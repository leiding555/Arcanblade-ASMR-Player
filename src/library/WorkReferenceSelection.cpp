#include "library/WorkReferenceSelection.h"
#include "subtitle/GeneratedSubtitleInfo.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <algorithm>

namespace asmr {
namespace {
QString pathKey(QString value)
{
    value = QDir::cleanPath(QDir::fromNativeSeparators(value));
#ifdef Q_OS_WIN
    value = value.toCaseFolded();
#endif
    return value;
}
bool subtitleFormat(const QString &format)
{
    return format == QStringLiteral("srt") || format == QStringLiteral("vtt")
        || format == QStringLiteral("lrc");
}
}

WorkReferenceSelection selectWorkReferences(
    const DirectoryScanResult &scan, const std::function<bool()> &cancelled)
{
    WorkReferenceSelection result;
    const auto stop = [&] {
        if (!cancelled || !cancelled()) return false;
        result = {};
        result.cancelled = true;
        result.error = QStringLiteral("参考资料筛选已取消。");
        return true;
    };
    if (stop()) return result;
    if (!scan.isComplete()) {
        result.error = QStringLiteral("目录扫描未完整完成，参考资料清单保持不变。");
        return result;
    }
    QHash<QString, QString> audioPaths;
    for (const auto &file : scan.audioFiles)
        audioPaths.insert(pathKey(file.relativePath), file.relativePath);
    QHash<QString, QVector<WorkReferenceCandidate>> asr;
    for (auto candidate : scan.referenceFiles) {
        if (stop()) return result;
        candidate.source = subtitleFormat(candidate.format)
            ? QStringLiteral("external-subtitle") : QStringLiteral("file");
        candidate.audioRelativePath.clear();
        candidate.completedAt = {};
        candidate.warning.clear();
        if (!subtitleFormat(candidate.format)) {
            result.files.push_back(std::move(candidate));
            continue;
        }
        const auto info = readGeneratedSubtitleInfo(candidate.absolutePath);
        if (!info) {
            result.files.push_back(std::move(candidate));
            continue;
        }
        if (info->mode == QStringLiteral("offline-direct") || info->mode == QStringLiteral("cloud-translation")
            || info->mode == QStringLiteral("cloud-correction-ja")) {
            result.excludedPaths.push_back(candidate.relativePath);
            continue;
        }
        const auto audio = QDir::cleanPath(QDir(QFileInfo(candidate.relativePath).path())
            .filePath(QStringLiteral("../") + info->audioFileName));
        if (!audioPaths.contains(pathKey(audio))) {
            // A valid ASR record without its recorded audio cannot be assigned
            // to an unrelated track through weak filename matching.
            result.excludedPaths.push_back(candidate.relativePath);
            result.warnings.push_back(QStringLiteral("ASR 参考字幕对应的音轨不存在：%1").arg(candidate.relativePath));
            continue;
        }
        candidate.source = QStringLiteral("generated-asr");
        candidate.audioRelativePath = audioPaths.value(pathKey(audio));
        candidate.completedAt = info->completedAt.toUTC();
        asr[pathKey(audio)].push_back(std::move(candidate));
    }
    for (auto it = asr.begin(); it != asr.end(); ++it) {
        if (stop()) return result;
        auto &versions = it.value();
        std::sort(versions.begin(), versions.end(), [](const auto &left, const auto &right) {
            if (left.completedAt != right.completedAt) return left.completedAt > right.completedAt;
            return QString::compare(left.relativePath, right.relativePath, Qt::CaseSensitive) < 0;
        });
        if (versions.size() > 1 && versions.at(0).completedAt == versions.at(1).completedAt) {
            versions[0].warning = QStringLiteral("ASR 完成时间并列，按相对路径固定选择：%1").arg(versions[0].relativePath);
            result.warnings.push_back(versions[0].warning);
        }
        result.files.push_back(versions.front());
        for (qsizetype index = 1; index < versions.size(); ++index)
            result.excludedPaths.push_back(versions.at(index).relativePath);
    }
    std::sort(result.files.begin(), result.files.end(), [](const auto &left, const auto &right) {
        return QString::compare(left.relativePath, right.relativePath, Qt::CaseSensitive) < 0;
    });
    std::sort(result.excludedPaths.begin(), result.excludedPaths.end());
    std::sort(result.warnings.begin(), result.warnings.end());
    if (stop()) return result;
    result.success = true;
    return result;
}
} // namespace asmr
