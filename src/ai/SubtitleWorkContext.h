#pragma once

#include "library/LibraryRepository.h"
#include <QJsonArray>
#include <QStringList>
#include <atomic>

namespace asmr {
enum class SubtitleWorkContextKind { ReferenceFiles, Asr };
struct SubtitleTranslationRequest;
struct SubtitleWorkContext {
    LibraryWork work;
    LibraryFile subtitle, audio;
    QJsonArray references;
    QString referenceDigest, error;
    SubtitleWorkContextKind contextKind = SubtitleWorkContextKind::ReferenceFiles;
    QJsonArray asrSubtitles;
    QStringList warnings;
    [[nodiscard]] bool succeeded() const { return error.isEmpty() && work.id > 0; }
};
// Worker-only lookup. Source identity always comes from the library and the
// generation sidecar, never from subtitle bindings or a filename heuristic.
[[nodiscard]] SubtitleWorkContext resolveSubtitleWorkContext(
    const SubtitleTranslationRequest &request, const std::atomic_bool &cancelled,
    bool includeReferences = true);
// Worker-only UI adapter. A unique WAV/MP3 counterpart may be the selection
// origin, but returned audio always identifies the actual ASR generation source.
// Execution and checkpoint revalidation continue to use the strict lookup above.
[[nodiscard]] SubtitleWorkContext resolveSubtitleWorkContextSelection(
    const SubtitleTranslationRequest &request, const std::atomic_bool &cancelled,
    bool includeReferences = true);
[[nodiscard]] bool revalidateSubtitleWorkContextIdentity(
    const SubtitleTranslationRequest &request, const LibraryWork &expected,
    const std::atomic_bool &cancelled, QString *error = nullptr);
}
