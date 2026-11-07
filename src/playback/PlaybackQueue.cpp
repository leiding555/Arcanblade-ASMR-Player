#include "playback/PlaybackQueue.h"

#include "library/LibraryRepository.h"
#include "library/NaturalSort.h"

#include <QDir>
#include <QRandomGenerator>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <utility>

namespace asmr {
namespace {
QString normalizedPath(QString path)
{
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return QDir::cleanPath(path);
}

QString identity(const PlaybackQueueEntry &item)
{
    if (item.workId > 0 && item.fileId > 0)
        return QStringLiteral("library:%1:%2").arg(item.workId).arg(item.fileId);
    // All supported runtime platforms are Windows; temporary identity follows
    // its case-insensitive path spelling without probing the filesystem.
    return QStringLiteral("temporary:")
        + normalizedPath(item.rootPath + QLatin1Char('/') + item.relativePath).toCaseFolded();
}

bool validRelativePath(QString &path, bool emptyAllowed)
{
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (path.isEmpty()) return emptyAllowed;
    if (path.startsWith(QLatin1Char('/')) || path.contains(QLatin1Char(':'))
        || path.contains(QChar::Null)) return false;
    const auto parts = path.split(QLatin1Char('/'));
    return std::none_of(parts.cbegin(), parts.cend(), [](const QString &part) {
        return part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..");
    });
}
} // namespace

PlaybackQueue::PlaybackQueue()
    : m_shuffleGenerator(QRandomGenerator::global()->generate64())
{
}

PlaybackQueue &PlaybackQueue::operator=(const PlaybackQueue &other)
{
    if (this == &other) return *this;
    const auto next = m_nextId == 0 || other.m_nextId == 0
        ? quint64(0) : qMax(m_nextId, other.m_nextId);
    m_entries = other.m_entries;
    m_currentId = other.m_currentId;
    m_nextId = next;
    m_mode = other.m_mode;
    m_shuffleGenerator = other.m_shuffleGenerator;
    m_shuffleRemaining = other.m_shuffleRemaining;
    m_shuffleHistory = other.m_shuffleHistory;
    m_shuffleHistoryIndex = other.m_shuffleHistoryIndex;
    return *this;
}

void PlaybackQueue::setMode(PlaybackMode mode)
{
    switch (mode) {
    case PlaybackMode::Sequential:
    case PlaybackMode::RepeatAll:
    case PlaybackMode::RepeatOne:
    case PlaybackMode::Shuffle:
        break;
    default:
        return;
    }
    if (m_mode == mode) return;
    m_mode = mode;
    resetShuffle();
}

int PlaybackQueue::currentIndex() const
{
    for (qsizetype i = 0; i < m_entries.size(); ++i)
        if (m_entries[i].id == m_currentId) return static_cast<int>(i);
    return -1;
}

const PlaybackQueueEntry *PlaybackQueue::entry(quint64 id) const
{
    if (!id) return nullptr;
    const auto found = std::find_if(m_entries.cbegin(), m_entries.cend(),
                                    [id](const auto &item) { return item.id == id; });
    return found == m_entries.cend() ? nullptr : &*found;
}

void PlaybackQueue::replace(QVector<PlaybackQueueEntry> entries, int startIndex)
{
    // Resolve the requested identity before filtering duplicate values.
    const QString startIdentity = startIndex >= 0 && startIndex < entries.size()
        ? identity(entries[startIndex]) : QString();
    m_entries.clear();
    m_currentId = 0;
    m_shuffleRemaining.clear();
    m_shuffleHistory.clear();
    m_shuffleHistoryIndex = -1;
    append(std::move(entries));
    if (!startIdentity.isEmpty()) {
        for (const auto &item : std::as_const(m_entries)) {
            if (item.available && identity(item) == startIdentity) {
                m_currentId = item.id;
                break;
            }
        }
    }
    resetShuffle();
}

PlaybackQueue PlaybackQueue::replacementFor(QVector<PlaybackQueueEntry> entries, int startIndex)
{
    auto replacement = *this;
    replacement.replace(std::move(entries), startIndex);
    m_nextId = replacement.m_nextId;
    return replacement;
}

qsizetype PlaybackQueue::append(QVector<PlaybackQueueEntry> entries)
{
    QSet<QString> identities;
    for (const auto &item : std::as_const(m_entries)) identities.insert(identity(item));
    const auto before = m_entries.size();
    for (auto &item : entries) {
        const QString key = identity(item);
        if (identities.contains(key) || m_nextId == 0) continue;
        identities.insert(key);
        item.id = m_nextId++;
        if (m_mode == PlaybackMode::Shuffle && item.available)
            m_shuffleRemaining.push_back(item.id);
        m_entries.push_back(std::move(item));
    }
    if (m_mode == PlaybackMode::Shuffle && m_entries.size() != before)
        std::shuffle(m_shuffleRemaining.begin(), m_shuffleRemaining.end(), m_shuffleGenerator);
    return m_entries.size() - before;
}

bool PlaybackQueue::setCurrent(quint64 id, PlaybackQueueNavigation navigation)
{
    const auto *item = entry(id);
    if (!item || !item->available) return false;
    if (id == m_currentId) return true;
    if (m_mode == PlaybackMode::Shuffle) commitShuffle(id, navigation);
    m_currentId = id;
    if (m_mode == PlaybackMode::Shuffle && m_shuffleRemaining.isEmpty())
        refillShuffleRound(false);
    return true;
}

bool PlaybackQueue::playable(quint64 id) const
{
    const auto *item = entry(id);
    return item && item->available;
}

quint64 PlaybackQueue::orderedId(bool forward, bool wrap) const
{
    const auto index = currentIndex();
    if (index < 0) return 0;
    const auto count = m_entries.size();
    for (qsizetype distance = 1; distance < count; ++distance) {
        const auto offset = forward ? distance : -distance;
        auto candidate = qsizetype(index) + offset;
        if (candidate < 0 || candidate >= count) {
            if (!wrap) break;
            candidate = (candidate + count) % count;
        }
        if (m_entries[candidate].available) return m_entries[candidate].id;
    }
    return 0;
}

qsizetype PlaybackQueue::historyPosition(bool forward) const
{
    if (m_shuffleHistoryIndex < 0) return -1;
    const qsizetype direction = forward ? 1 : -1;
    for (auto i = m_shuffleHistoryIndex + direction;
         i >= 0 && i < m_shuffleHistory.size(); i += direction) {
        const auto id = m_shuffleHistory[i];
        if (id != m_currentId && playable(id)) return i;
    }
    return -1;
}

quint64 PlaybackQueue::previousId() const
{
    if (m_mode != PlaybackMode::Shuffle)
        return orderedId(false, m_mode != PlaybackMode::Sequential);
    const auto position = historyPosition(false);
    return position >= 0 ? m_shuffleHistory[position] : quint64(0);
}

quint64 PlaybackQueue::nextId() const
{
    if (currentIndex() < 0) return 0;
    if (m_mode != PlaybackMode::Shuffle)
        return orderedId(true, m_mode != PlaybackMode::Sequential);
    const auto position = historyPosition(true);
    if (position >= 0) return m_shuffleHistory[position];
    for (const auto id : m_shuffleRemaining)
        if (id != m_currentId && playable(id)) return id;
    return 0;
}

quint64 PlaybackQueue::naturalNextId() const
{
    if (m_mode == PlaybackMode::RepeatOne && playable(m_currentId)) return m_currentId;
    if (const auto next = nextId()) return next;
    // Manual navigation is disabled for a single playable current item;
    // natural completion can still restart it in any looping mode.
    return m_mode != PlaybackMode::Sequential && playable(m_currentId) ? m_currentId : 0;
}

void PlaybackQueue::refillShuffleRound(bool excludeCurrent)
{
    m_shuffleRemaining.clear();
    for (const auto &item : std::as_const(m_entries)) {
        if (item.available && (!excludeCurrent || item.id != m_currentId))
            m_shuffleRemaining.push_back(item.id);
    }
    std::shuffle(m_shuffleRemaining.begin(), m_shuffleRemaining.end(), m_shuffleGenerator);
    // Include every playable item in subsequent rounds, while ensuring the
    // boundary cannot repeat the last item when another choice exists.
    if (m_shuffleRemaining.size() > 1 && m_shuffleRemaining.first() == m_currentId)
        std::swap(m_shuffleRemaining[0], m_shuffleRemaining[1]);
}

void PlaybackQueue::resetShuffle()
{
    m_shuffleRemaining.clear();
    m_shuffleHistory.clear();
    m_shuffleHistoryIndex = -1;
    if (m_mode != PlaybackMode::Shuffle) return;
    if (current()) {
        m_shuffleHistory.push_back(m_currentId);
        m_shuffleHistoryIndex = 0;
    }
    refillShuffleRound(true);
}

void PlaybackQueue::pruneShuffle()
{
    if (m_mode != PlaybackMode::Shuffle) return;
    m_shuffleRemaining.erase(std::remove_if(m_shuffleRemaining.begin(), m_shuffleRemaining.end(),
        [this](quint64 id) { return !playable(id); }), m_shuffleRemaining.end());
    QVector<quint64> history;
    qsizetype position = -1;
    for (qsizetype i = 0; i < m_shuffleHistory.size(); ++i) {
        const auto id = m_shuffleHistory[i];
        if (!playable(id) && id != m_currentId) continue;
        if (i == m_shuffleHistoryIndex) position = history.size();
        history.push_back(id);
    }
    m_shuffleHistory = std::move(history);
    m_shuffleHistoryIndex = position;
    if (m_shuffleRemaining.isEmpty()) refillShuffleRound(false);
}

void PlaybackQueue::commitShuffle(quint64 id, PlaybackQueueNavigation navigation)
{
    const bool backwards = navigation == PlaybackQueueNavigation::Previous;
    const bool forwards = navigation == PlaybackQueueNavigation::Next
        || navigation == PlaybackQueueNavigation::Natural;
    if (backwards || forwards) {
        const auto position = historyPosition(forwards);
        if (position >= 0 && m_shuffleHistory[position] == id) {
            m_shuffleHistoryIndex = position;
            return;
        }
    }
    // A direct selection branches from the current history position. Random
    // candidates are committed here only after the caller accepts the media.
    m_shuffleHistory.resize(m_shuffleHistoryIndex + 1);
    m_shuffleHistory.push_back(id);
    m_shuffleHistoryIndex = m_shuffleHistory.size() - 1;
    m_shuffleRemaining.removeAll(id);
}

bool PlaybackQueue::remove(quint64 id)
{
    if (id == 0 || id == m_currentId) return false;
    const auto found = std::find_if(m_entries.begin(), m_entries.end(),
                                    [id](const auto &item) { return item.id == id; });
    if (found == m_entries.end()) return false;
    m_entries.erase(found);
    pruneShuffle();
    return true;
}

void PlaybackQueue::clearOthers()
{
    const auto *item = current();
    if (item) {
        const auto retained = *item;
        m_entries = {retained};
    } else {
        m_entries.clear();
    }
    resetShuffle();
}

void PlaybackQueue::invalidateWork(qint64 workId)
{
    if (workId <= 0) return;
    for (auto &item : m_entries) {
        if (item.workId != workId) continue;
        item.available = false;
        item.issue = QStringLiteral("作品已从媒体库移除");
    }
    pruneShuffle();
}

bool PlaybackQueue::setIssue(quint64 id, QString issue, bool unavailable)
{
    for (auto &item : m_entries) {
        if (item.id != id) continue;
        item.issue = std::move(issue);
        if (unavailable) item.available = false;
        if (unavailable) pruneShuffle();
        return true;
    }
    return false;
}

qsizetype PlaybackQueue::updateWorkName(qint64 workId, const QString &name)
{
    if (workId <= 0) return 0;
    qsizetype updated = 0;
    for (auto &item : m_entries) {
        if (item.workId != workId || item.workName == name) continue;
        item.workName = name;
        ++updated;
    }
    return updated;
}

QVector<PlaybackQueueEntry> buildDirectoryPlaybackEntries(
    const LibraryWork &work, const QVector<LibraryFile> &files,
    QString relativeDirectory, bool recursive)
{
    QVector<PlaybackQueueEntry> result;
    if (work.id <= 0 || !validRelativePath(relativeDirectory, true)) return result;
    struct Candidate {
        LibraryFile file;
        QStringList parts;
        QVector<NaturalSortKey> keys;
    };
    QVector<Candidate> candidates;
    const QString prefix = relativeDirectory.isEmpty()
        ? QString() : relativeDirectory + QLatin1Char('/');
    for (auto file : files) {
        if (file.id <= 0 || file.workId != work.id || file.missing
            || file.type != LibraryFileType::Audio || !validRelativePath(file.relativePath, false)) continue;
        if (!file.relativePath.startsWith(prefix, Qt::CaseSensitive)) continue;
        const QString suffix = file.relativePath.mid(prefix.size());
        if (suffix.isEmpty() || (!recursive && suffix.contains(QLatin1Char('/')))) continue;
        Candidate item;
        item.file = std::move(file);
        item.parts = suffix.split(QLatin1Char('/'));
        if (item.file.fileName.isEmpty()) item.file.fileName = item.parts.last();
        for (qsizetype i = 0; i < item.parts.size(); ++i)
            item.keys.push_back(NaturalSortKey(i + 1 == item.parts.size()
                                                  ? item.file.fileName : item.parts[i]));
        candidates.push_back(std::move(item));
    }
    // Invalid duplicate snapshots resolve deterministically before display sort.
    std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
        if (a.file.id != b.file.id) return a.file.id < b.file.id;
        return a.file.relativePath < b.file.relativePath;
    });
    QSet<qint64> seen;
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const auto &item) {
        if (seen.contains(item.file.id)) return true;
        seen.insert(item.file.id);
        return false;
    }), candidates.end());
    std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
        const auto length = qMin(a.parts.size(), b.parts.size());
        for (qsizetype i = 0; i < length; ++i) {
            const bool aDirectory = i + 1 < a.parts.size();
            const bool bDirectory = i + 1 < b.parts.size();
            if (aDirectory != bDirectory) return aDirectory;
            if (const int name = a.keys[i].compare(b.keys[i])) return name < 0;
            // Distinct directory spellings with equal normalized keys must
            // remain separate exactly as in LibraryTrackTreeModel.
            if (a.parts[i] != b.parts[i]) return a.parts[i] < b.parts[i];
        }
        if (a.file.relativePath != b.file.relativePath)
            return a.file.relativePath < b.file.relativePath;
        return a.file.id < b.file.id;
    });
    for (const auto &item : std::as_const(candidates)) {
        PlaybackQueueEntry entry;
        entry.workId = work.id;
        entry.fileId = item.file.id;
        entry.rootPath = work.rootPath;
        entry.relativePath = item.file.relativePath;
        entry.workName = work.displayName;
        entry.displayName = item.file.fileName;
        result.push_back(std::move(entry));
    }
    return result;
}

} // namespace asmr
