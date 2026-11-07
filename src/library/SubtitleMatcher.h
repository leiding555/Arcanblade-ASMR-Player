#pragma once

#include "library/LibraryRepository.h"

#include <QHash>

namespace asmr {

// Ordered rules, not probabilities. Directory distance never changes a rule.
enum class SubtitleMatchRule {
    TrackNumberOnly = 100,
    MediaExtensionStem = 150,
    LanguageSuffixStem = 200,
    NormalizedStem = 300,
    ExactStem = 400,
};

struct SubtitleMatchCandidate {
    qint64 subtitleFileId = 0;
    SubtitleMatchRule rule = SubtitleMatchRule::TrackNumberOnly;
    int ruleScore = 0; // Ordered rule value, not a probability.
    QString language; // Recognized filename suffix, lowercase; empty = unspecified.
    qsizetype directoryDistance = 0; // Parent/child edges between relative directories.
    bool trackNumberAgrees = false;
    bool numberOnly = false; // Effective matched titles contain only number syntax.
    bool recommended = false; // Named title rule; may still be ambiguous.
    bool ambiguousVersion = false; // Other strong versions in this track/format pair's language slot.
    QVector<qint64> competingAudioFileIds; // Other candidate owners, ascending stable ID.
    bool autoAssociationAllowed = false; // Advice only; never writes a binding.

    bool operator==(const SubtitleMatchCandidate &) const = default;
};

struct TrackSubtitleMatches {
    qint64 audioFileId = 0;
    QVector<SubtitleMatchCandidate> candidates;

    bool operator==(const TrackSubtitleMatches &) const = default;
};

struct SubtitleMatchResult {
    qint64 workId = 0;
    QVector<TrackSubtitleMatches> tracks; // Every valid audio, ascending stable ID.
    QVector<qint64> rejectedFileIds; // Invalid/missing records, unique ascending ID.

    bool operator==(const SubtitleMatchResult &) const = default;
};

// Stateless, synchronous suggestions from ONE COMPLETE work-file snapshot.
// Checks filesystem metadata only (including live existence and link components).
// Call off the GUI thread; no SQLite, media/subtitle reads, loading or mutation.
// Repeated identical records coalesce; conflicting records sharing an ID reject
// that ID. Invalid roots reject all records. Results are not an atomic disk view.
class SubtitleMatcher final {
public:
    static SubtitleMatchResult match(const LibraryWork &work,
                                     const QVector<LibraryFile> &files);
    // Bidirectional IDs for one present WAV and one present MP3 with the same
    // complete base name anywhere in the work. Duplicate formats do not pair.
    // Uses the same path/link guards as match(); call off the GUI thread.
    static QHash<qint64, qint64> sharedAudioPartners(const LibraryWork &work,
                                                  const QVector<LibraryFile> &files);
};

} // namespace asmr
