#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>

#include <random>

namespace asmr {

struct LibraryWork;
struct LibraryFile;

enum class PlaybackMode { Sequential, RepeatAll, RepeatOne, Shuffle };

// The destination alone cannot distinguish back/forward through a history
// such as A -> B -> A. Keep the requested direction until media is accepted.
enum class PlaybackQueueNavigation { Select, Previous, Next, Natural };

// Session values only. Queue IDs are distinct from persisted file identities.
struct PlaybackQueueEntry {
    quint64 id = 0;
    qint64 workId = 0;
    qint64 fileId = 0;
    QString rootPath;
    QString relativePath;
    QString workName;
    QString displayName;
    bool available = true;
    QString issue;

    bool operator==(const PlaybackQueueEntry &) const = default;
};

// Pure value state: never opens media or reads/writes library history.
class PlaybackQueue final {
public:
    PlaybackQueue();
    PlaybackQueue(const PlaybackQueue &) = default;
    // Accepting a validated candidate retains this session's ID high-water
    // mark, including IDs allocated while that candidate was being checked.
    PlaybackQueue &operator=(const PlaybackQueue &other);
    [[nodiscard]] const QVector<PlaybackQueueEntry> &entries() const { return m_entries; }
    [[nodiscard]] quint64 currentId() const { return m_currentId; }
    [[nodiscard]] int currentIndex() const;
    [[nodiscard]] const PlaybackQueueEntry *entry(quint64 id) const;
    [[nodiscard]] const PlaybackQueueEntry *current() const { return entry(m_currentId); }
    [[nodiscard]] PlaybackMode mode() const { return m_mode; }
    // Session-only preference. Changing it never changes the current item.
    void setMode(PlaybackMode mode);

    // Incoming IDs are ignored; replacement never reuses an earlier queue ID.
    // Invalid/unavailable startIndex leaves the queue without a current item.
    void replace(QVector<PlaybackQueueEntry> entries, int startIndex = 0);
    // Reserve replacement IDs immediately while leaving the accepted entries
    // and current item untouched during asynchronous media validation.
    [[nodiscard]] PlaybackQueue replacementFor(QVector<PlaybackQueueEntry> entries,
                                               int startIndex = 0);
    // Stable library identity, or a normalized temporary path, occurs only once.
    // Appending never changes the current item, including an empty queue.
    qsizetype append(QVector<PlaybackQueueEntry> entries);
    bool setCurrent(quint64 id,
                    PlaybackQueueNavigation navigation = PlaybackQueueNavigation::Select);
    // Queries never consume a random candidate or move through history.
    [[nodiscard]] quint64 previousId() const;
    [[nodiscard]] quint64 nextId() const;
    [[nodiscard]] quint64 naturalNextId() const;
    bool remove(quint64 id);
    void clearOthers();
    void invalidateWork(qint64 workId);
    // Display metadata only: no queue identity, order, cursor or availability
    // changes. Returns the number of entries whose workName was changed.
    qsizetype updateWorkName(qint64 workId, const QString &name);
    bool setIssue(quint64 id, QString issue, bool unavailable = false);

private:
    friend class PlaybackQueueTests;
    [[nodiscard]] bool playable(quint64 id) const;
    [[nodiscard]] quint64 orderedId(bool forward, bool wrap) const;
    [[nodiscard]] qsizetype historyPosition(bool forward) const;
    void resetShuffle();
    void refillShuffleRound(bool excludeCurrent);
    void pruneShuffle();
    void commitShuffle(quint64 id, PlaybackQueueNavigation navigation);

    QVector<PlaybackQueueEntry> m_entries;
    quint64 m_currentId = 0;
    quint64 m_nextId = 1;
    PlaybackMode m_mode = PlaybackMode::Sequential;
    std::mt19937_64 m_shuffleGenerator;
    QVector<quint64> m_shuffleRemaining;
    QVector<quint64> m_shuffleHistory;
    qsizetype m_shuffleHistoryIndex = -1;
};

// Uses the complete in-memory snapshot. Directory-first natural DFS agrees
// with the browser; only present audio with valid relative identities is used.
[[nodiscard]] QVector<PlaybackQueueEntry> buildDirectoryPlaybackEntries(
    const LibraryWork &work, const QVector<LibraryFile> &files,
    QString relativeDirectory = {}, bool recursive = false);

} // namespace asmr

Q_DECLARE_METATYPE(asmr::PlaybackMode)
