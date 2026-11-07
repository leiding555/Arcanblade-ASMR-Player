#include "library/SubtitleMatcher.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <optional>
#include <utility>

namespace asmr {
namespace {

QString separators(QString path)
{
    return path.replace(QLatin1Char('\\'), QLatin1Char('/'));
}

QString nameKey(const QString &name)
{
    // Canonical equivalence only: preserve width, punctuation and whitespace.
    return name.normalized(QString::NormalizationForm_C).toCaseFolded()
        .normalized(QString::NormalizationForm_C);
}

bool safeComponent(const QString &part)
{
    if (part == QStringLiteral("..") || part.endsWith(QLatin1Char('.'))
        || part.endsWith(QLatin1Char(' ')))
        return false;
    for (const auto character : part) {
        if (character.unicode() < 32 || QStringLiteral("<>:\"|?*").contains(character))
            return false;
    }
    const QString device = part.section(QLatin1Char('.'), 0, 0).trimmed().toUpper();
    if (device == QStringLiteral("CON") || device == QStringLiteral("PRN")
        || device == QStringLiteral("AUX") || device == QStringLiteral("NUL")
        || device == QStringLiteral("CONIN$") || device == QStringLiteral("CONOUT$"))
        return false;
    if (device.size() == 4 && (device.startsWith(QStringLiteral("COM"))
                               || device.startsWith(QStringLiteral("LPT")))) {
        const auto last = device[3];
        if ((last >= QLatin1Char('1') && last <= QLatin1Char('9'))
            || last == QChar(0x00b9) || last == QChar(0x00b2) || last == QChar(0x00b3))
            return false;
    }
    return true;
}

bool relativePath(const QString &input, QString &cleaned)
{
    const QString path = separators(input);
    if (path.isEmpty() || path.contains(QChar::Null) || path.startsWith(QLatin1Char('/'))
        || QDir::isAbsolutePath(path))
        return false;
    for (const auto &part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part != QStringLiteral(".") && !safeComponent(part))
            return false;
    }
    cleaned = QDir::cleanPath(path);
    return cleaned != QStringLiteral(".");
}

bool usableRoot(const LibraryWork &work, QString &root)
{
    root = separators(work.rootPath);
    if (work.id <= 0 || root.isEmpty() || root.contains(QChar::Null)
        || !QDir::isAbsolutePath(root) || root.startsWith(QStringLiteral("//?/"))
        || root.startsWith(QStringLiteral("//./")))
        return false;
#ifdef Q_OS_WIN
    const bool drive = root.size() >= 3 && root[0].isLetter()
        && root[1] == QLatin1Char(':') && root[2] == QLatin1Char('/');
    const bool unc = root.startsWith(QStringLiteral("//"))
        && !root.startsWith(QStringLiteral("///"))
        && root.split(QLatin1Char('/'), Qt::SkipEmptyParts).size() >= 2;
    if (!drive && !unc)
        return false;
#endif
    const auto parts = root.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (qsizetype index = 0; index < parts.size(); ++index) {
        const auto &part = parts[index];
        const bool driveComponent = index == 0 && !root.startsWith(QLatin1Char('/'))
            && part.size() == 2 && part[0].isLetter() && part[1] == QLatin1Char(':');
        if (part != QStringLiteral(".") && !driveComponent && !safeComponent(part))
            return false;
    }
    root = QDir::cleanPath(root);
    const QFileInfo info(root);
    return info.isDir() && !info.isSymbolicLink() && !info.isJunction();
}

bool existingFileWithinRoot(const QString &root, const QString &relative)
{
    // Reject every link component, matching DirectoryScanner's policy. A safe
    // relative path without links cannot escape the work root. No file is opened.
    QString path = root;
    const auto parts = relative.split(QLatin1Char('/'));
    for (qsizetype index = 0; index < parts.size(); ++index) {
        path = QDir(path).filePath(parts[index]);
        const QFileInfo info(path);
        if (!info.exists() || info.isSymbolicLink() || info.isJunction()
            || (index + 1 == parts.size() ? !info.isFile() : !info.isDir()))
            return false;
    }
    return true;
}

struct Stem {
    QString full;
    QString key;
    QString withoutLanguageKey;
    QString language;
    QString mediaExtension;
    QString withoutMediaExtensionKey;
    QString number;
    bool numberOnly = false;
};

QString leadingNumber(const QString &stem)
{
    qsizetype begin = 0;
    QChar closing;
    if (stem.startsWith(QLatin1Char('['))) {
        begin = 1;
        closing = QLatin1Char(']');
    } else if (stem.startsWith(QLatin1Char('('))) {
        begin = 1;
        closing = QLatin1Char(')');
    }
    qsizetype end = begin;
    while (end < stem.size() && stem[end] >= QLatin1Char('0')
           && stem[end] <= QLatin1Char('9'))
        ++end;
    if (end == begin)
        return {};
    if (!closing.isNull()) {
        if (end == stem.size() || stem[end] != closing)
            return {};
    } else if (end < stem.size() && !stem[end].isSpace()
               && stem[end] != QLatin1Char('.') && stem[end] != QLatin1Char('_')
               && stem[end] != QLatin1Char('-')) {
        return {};
    }
    QString number = stem.mid(begin, end - begin);
    while (number.size() > 1 && number.startsWith(QLatin1Char('0')))
        number.remove(0, 1);
    return number;
}

bool onlyNumberSyntax(const QString &title)
{
    // Classification only: preserve Unicode width and punctuation for matching.
    bool hasNumber = false;
    for (const auto character : title.toUcs4()) {
        hasNumber = hasNumber || QChar::isNumber(character);
        if (!QChar::isNumber(character) && !QChar::isSpace(character)
            && !QChar::isPunct(character))
            return false;
    }
    return hasNumber;
}

QString languageSlot(const QString &language)
{
    // Filename aliases group versions only; actual language is never inferred.
    if (language == QStringLiteral("cn") || language == QStringLiteral("chs")
        || language == QStringLiteral("zh-cn"))
        return QStringLiteral("zh-hans");
    if (language == QStringLiteral("cht") || language == QStringLiteral("zh-tw"))
        return QStringLiteral("zh-hant");
    if (language == QStringLiteral("jp") || language == QStringLiteral("jpn"))
        return QStringLiteral("ja");
    if (language == QStringLiteral("eng"))
        return QStringLiteral("en");
    return language; // Unspecified, zh and regional English remain independent.
}

Stem describeStem(const QString &fileName, bool subtitle)
{
    Stem stem;
    stem.full = QFileInfo(fileName).completeBaseName();
    stem.key = nameKey(stem.full);
    stem.withoutLanguageKey = stem.key;
    static const QSet<QString> languages{
        QStringLiteral("ja"), QStringLiteral("zh"), QStringLiteral("zh-cn"),
        QStringLiteral("zh-tw"), QStringLiteral("zh-hans"), QStringLiteral("zh-hant"),
        QStringLiteral("en"), QStringLiteral("en-us"), QStringLiteral("en-gb"),
        QStringLiteral("ko"), QStringLiteral("de"), QStringLiteral("fr"),
        QStringLiteral("es"), QStringLiteral("it"), QStringLiteral("ru"),
        QStringLiteral("pt"), QStringLiteral("pt-br"),
        QStringLiteral("cn"), QStringLiteral("chs"), QStringLiteral("cht"),
        QStringLiteral("jp"), QStringLiteral("jpn"), QStringLiteral("eng")};
    const qsizetype separator = std::max(stem.key.lastIndexOf(QLatin1Char('.')),
                                         stem.key.lastIndexOf(QLatin1Char('_')));
    if (separator > 0 && languages.contains(stem.key.mid(separator + 1))) {
        stem.language = stem.key.mid(separator + 1);
        stem.withoutLanguageKey = stem.key.left(separator); // Strip at most once.
    }
    stem.number = leadingNumber(stem.withoutLanguageKey);
    stem.numberOnly = onlyNumberSyntax(stem.withoutLanguageKey);
    if (subtitle) {
        // Strip one retained audio extension AFTER at most one terminal language.
        // Never reparse the resulting title, trim it, or change its real path.
        const auto dot = stem.withoutLanguageKey.lastIndexOf(QLatin1Char('.'));
        const auto extension = stem.withoutLanguageKey.mid(dot + 1);
        if (dot > 0 && (extension == QStringLiteral("mp3")
                        || extension == QStringLiteral("wav"))) {
            stem.mediaExtension = extension;
            stem.withoutMediaExtensionKey = stem.withoutLanguageKey.left(dot);
        }
    }
    return stem;
}

struct PreparedFile {
    qint64 id;
    LibraryFileType type;
    QString extension;
    QString path;
    QString pathKey;
    QStringList directories;
    Stem stem;
};

std::optional<PreparedFile> prepare(const LibraryFile &file, qint64 workId,
                                   const QString &root)
{
    QString path;
    if (file.id <= 0 || file.workId != workId || file.missing
        || !relativePath(file.relativePath, path))
        return std::nullopt;
    const QFileInfo info(path);
    const QString extension = info.suffix().toLower();
    const bool audio = file.type == LibraryFileType::Audio
        && (extension == QStringLiteral("mp3") || extension == QStringLiteral("wav"));
    const bool subtitle = file.type == LibraryFileType::Subtitle
        && (extension == QStringLiteral("srt") || extension == QStringLiteral("vtt")
            || extension == QStringLiteral("lrc"));
    if ((!audio && !subtitle) || file.fileName != info.fileName()
        || file.extension.toLower() != extension || info.completeBaseName().isEmpty()
        || !existingFileWithinRoot(root, path))
        return std::nullopt;
    QStringList directories = path.split(QLatin1Char('/'));
    directories.removeLast();
    for (auto &directory : directories)
        directory = nameKey(directory);
    return PreparedFile{file.id, file.type, extension, path, nameKey(path), directories,
                        describeStem(file.fileName, subtitle)};
}

QVector<PreparedFile> prepareUniqueFiles(const LibraryWork &work,
                                       const QVector<LibraryFile> &files,
                                       QSet<qint64> &rejected)
{
    QString root;
    const bool rootValid = usableRoot(work, root);
    QMap<qint64, LibraryFile> unique;
    for (const auto &file : files) {
        const auto found = unique.constFind(file.id);
        if (found != unique.cend() && *found != file)
            rejected.insert(file.id);
        else
            unique.insert(file.id, file);
    }
    QVector<PreparedFile> prepared;
    QHash<QString, qsizetype> pathCounts;
    for (auto it = unique.cbegin(); it != unique.cend(); ++it) {
        const auto file = rootValid && !rejected.contains(it.key())
            ? prepare(it.value(), work.id, root) : std::nullopt;
        if (!file) {
            rejected.insert(it.key());
            continue;
        }
        ++pathCounts[file->path.toCaseFolded()];
        prepared.push_back(*file);
    }
    prepared.erase(std::remove_if(prepared.begin(), prepared.end(), [&](const auto &file) {
        if (pathCounts.value(file.path.toCaseFolded()) == 1) return false;
        rejected.insert(file.id);
        return true;
    }), prepared.end());
    return prepared;
}

QHash<qint64, qint64> audioPartners(const QMap<qint64, PreparedFile> &audioFiles)
{
    QHash<QString, QVector<qint64>> groups;
    for (const auto &file : audioFiles) groups[file.stem.key].append(file.id);
    QHash<qint64, qint64> result;
    for (const auto &ids : groups) {
        if (ids.size() != 2) continue;
        const auto &first = audioFiles.constFind(ids[0]).value();
        const auto &second = audioFiles.constFind(ids[1]).value();
        if (first.extension == second.extension) continue;
        result.insert(first.id, second.id);
        result.insert(second.id, first.id);
    }
    return result;
}

qsizetype distance(const PreparedFile &audio, const PreparedFile &subtitle)
{
    qsizetype shared = 0;
    while (shared < audio.directories.size() && shared < subtitle.directories.size()
           && audio.directories[shared] == subtitle.directories[shared])
        ++shared;
    return audio.directories.size() + subtitle.directories.size() - 2 * shared;
}

std::optional<SubtitleMatchRule> matchingRule(const Stem &audio, const Stem &subtitle,
                                             const QString &audioExtension)
{
    if (audio.full == subtitle.full)
        return SubtitleMatchRule::ExactStem;
    if (audio.key == subtitle.key)
        return SubtitleMatchRule::NormalizedStem;
    if ((!audio.language.isEmpty() || !subtitle.language.isEmpty())
        && audio.withoutLanguageKey == subtitle.withoutLanguageKey)
        return SubtitleMatchRule::LanguageSuffixStem;
    if (!subtitle.mediaExtension.isEmpty() && subtitle.mediaExtension == audioExtension
        && (audio.key == subtitle.withoutMediaExtensionKey
            || audio.withoutLanguageKey == subtitle.withoutMediaExtensionKey))
        return SubtitleMatchRule::MediaExtensionStem;
    if (!audio.number.isEmpty() && audio.number == subtitle.number)
        return SubtitleMatchRule::TrackNumberOnly;
    return std::nullopt;
}

} // namespace

SubtitleMatchResult SubtitleMatcher::match(const LibraryWork &work,
                                           const QVector<LibraryFile> &files)
{
    SubtitleMatchResult result;
    result.workId = work.id;
    QSet<qint64> rejected;
    const auto prepared = prepareUniqueFiles(work, files, rejected);
    QMap<qint64, PreparedFile> audioFiles;
    QMap<qint64, PreparedFile> subtitles;
    for (const auto &file : prepared) {
        if (file.type == LibraryFileType::Audio) {
            audioFiles.insert(file.id, file);
        } else {
            subtitles.insert(file.id, file);
        }
    }
    const auto partners = audioPartners(audioFiles);
    QHash<qint64, QVector<qint64>> owners;
    for (const auto &audio : audioFiles) {
        TrackSubtitleMatches track;
        track.audioFileId = audio.id;
        for (const auto &subtitle : subtitles) {
            const auto rule = matchingRule(audio.stem, subtitle.stem, audio.extension);
            if (!rule)
                continue;
            SubtitleMatchCandidate candidate;
            candidate.subtitleFileId = subtitle.id;
            candidate.rule = *rule;
            candidate.ruleScore = static_cast<int>(*rule);
            candidate.language = subtitle.stem.language;
            candidate.directoryDistance = distance(audio, subtitle);
            candidate.trackNumberAgrees = !audio.stem.number.isEmpty()
                && audio.stem.number == subtitle.stem.number;
            // The media rule proves equality to the raw or language-stripped
            // audio title. Keep its classification even for 01.ja.mp3.srt.
            candidate.numberOnly = audio.stem.numberOnly
                && (*rule == SubtitleMatchRule::MediaExtensionStem || subtitle.stem.numberOnly);
            candidate.recommended = *rule != SubtitleMatchRule::TrackNumberOnly
                && !candidate.numberOnly;
            track.candidates.push_back(candidate);
            owners[subtitle.id].push_back(audio.id);
        }
        result.tracks.push_back(std::move(track));
    }
    QHash<qint64, QHash<QString, QSet<qint64>>> strongVersions;
    for (const auto &track : result.tracks) {
        const auto groupId = std::min(track.audioFileId,
            partners.value(track.audioFileId, track.audioFileId));
        for (const auto &candidate : track.candidates)
            if (candidate.recommended)
                strongVersions[groupId][languageSlot(candidate.language)].insert(candidate.subtitleFileId);
    }
    for (auto &track : result.tracks) {
        const auto partner = partners.value(track.audioFileId);
        const auto groupId = partner ? std::min(track.audioFileId, partner) : track.audioFileId;
        for (auto &candidate : track.candidates) {
            candidate.ambiguousVersion = candidate.recommended
                && strongVersions.value(groupId).value(languageSlot(candidate.language)).size() > 1;
            candidate.competingAudioFileIds = owners.value(candidate.subtitleFileId);
            candidate.competingAudioFileIds.removeAll(track.audioFileId);
            if (partner) candidate.competingAudioFileIds.removeAll(partner);
            candidate.autoAssociationAllowed = candidate.recommended
                && !candidate.ambiguousVersion && candidate.competingAudioFileIds.isEmpty();
        }
        std::sort(track.candidates.begin(), track.candidates.end(),
                  [&subtitles](const auto &left, const auto &right) {
            if (left.ruleScore != right.ruleScore)
                return left.ruleScore > right.ruleScore;
            if (left.directoryDistance != right.directoryDistance)
                return left.directoryDistance < right.directoryDistance;
            if (left.trackNumberAgrees != right.trackNumberAgrees)
                return left.trackNumberAgrees;
            const auto &a = subtitles.constFind(left.subtitleFileId).value();
            const auto &b = subtitles.constFind(right.subtitleFileId).value();
            const int keyOrder = QString::compare(a.pathKey, b.pathKey, Qt::CaseSensitive);
            if (keyOrder != 0)
                return keyOrder < 0;
            const int rawOrder = QString::compare(a.path, b.path, Qt::CaseSensitive);
            if (rawOrder != 0)
                return rawOrder < 0;
            return left.subtitleFileId < right.subtitleFileId;
        });
    }
    result.rejectedFileIds = rejected.values();
    std::sort(result.rejectedFileIds.begin(), result.rejectedFileIds.end());
    return result;
}

QHash<qint64, qint64> SubtitleMatcher::sharedAudioPartners(
    const LibraryWork &work, const QVector<LibraryFile> &files)
{
    QSet<qint64> rejected;
    QMap<qint64, PreparedFile> audioFiles;
    for (const auto &file : prepareUniqueFiles(work, files, rejected))
        if (file.type == LibraryFileType::Audio) audioFiles.insert(file.id, file);
    return audioPartners(audioFiles);
}

} // namespace asmr
