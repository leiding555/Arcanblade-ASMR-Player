#pragma once
#include "ai/AiSubtitleTaskTypes.h"
#include "ai/TranslationApiSettings.h"
#include "ai/SubtitleWorkContext.h"
#include "subtitle/SubtitleEntry.h"
#include "library/LibraryRepository.h"
#include <QByteArray>
#include <QDateTime>
#include <QJsonArray>
#include <QList>
#include <QMap>
#include <QMetaType>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <memory>
class QLockFile;
namespace asmr {
struct SubtitleTranslationRequest {
    QString sourcePath;
    // Optional explicit audio identity; never guessed from a subtitle filename.
    QString audioPath;
    QString libraryRoot;
    QString libraryRelativePath;
    TranslationApiSettings settings;
    // Optional isolated storage root for tests. Legacy JSON files are never read or migrated.
    QString cacheRoot;
    // Explicit application business database; defaults to library.sqlite.
    QString databasePath;
    // Stable queue-task identity. Empty retains pre-task-scoped checkpoints.
    QString taskId;
    // Stable output group, shared by tasks from the same explicit queue batch.
    QString outputBatchId;
    // Assigned only for colliding output names within an original directory.
    QString outputFileSuffix;
    bool useWorkContext = false;
    qint64 workId = 0, audioFileId = 0, subtitleFileId = 0;
    QString expectedReferenceDigest;
};
struct SubtitleTranslationBatch { int firstId = 1; int count = 0; };
enum class SubtitleTranslationMode { IndependentBatches, ToolConversation };
enum class SubtitleTranslationPhase { Correction, Translation };
struct SubtitleTranslationPlan {
    SubtitleTranslationRequest request;
    SubtitleTranslationMode mode = SubtitleTranslationMode::IndependentBatches;
    QByteArray sourceSha256;
    QString fingerprint;
    QList<SubtitleEntry> entries;
    QVector<SubtitleTranslationBatch> batches;
    QString outputBaseDirectory;
    QString outputFileName;
    QString checkpointDirectory;
    // Prepared on the worker; UI retrieval is an implicitly shared string copy.
    QString previewText;
    qint64 audioSize = -1;
    QDateTime audioModified;
    QStringList warnings;
    QString error;
    SubtitleTranslationPhase phase = SubtitleTranslationPhase::Translation;
    LibraryWork contextWork;
    QString contextGroup, referenceDigest, correctedSourceHash, translatedOutputFileName;
    QJsonArray workReferences;
    SubtitleWorkContextKind contextKind = SubtitleWorkContextKind::ReferenceFiles;
    QJsonArray workAsrSubtitles;
    [[nodiscard]] bool succeeded() const { return error.isEmpty() && !entries.isEmpty(); }
};
// Stable 1-based local IDs. Timestamps never come from the model.
using SubtitleTranslations = QMap<int, QString>;
[[nodiscard]] SubtitleTranslationPlan makeSubtitleTranslationPhasePlan(
    const SubtitleTranslationPlan &correction, const SubtitleTranslations &corrected,
    QString *error = nullptr);
// Filesystem and SQLite functions are synchronous: invoke outside the GUI thread.
[[nodiscard]] SubtitleTranslationPlan prepareSubtitleTranslation(
    const SubtitleTranslationRequest &request, const std::atomic_bool &cancelled);
[[nodiscard]] bool revalidateSubtitleTranslationSource(const SubtitleTranslationPlan &plan,
    const std::atomic_bool &cancelled, QString *error = nullptr);
// Full Chat Completions JSON request, no credentials or local paths.
[[nodiscard]] QByteArray subtitleTranslationBatchRequest(const SubtitleTranslationPlan &plan, int batchIndex);
// Every full outgoing batch, fixed rules, model and output location; no credentials.
[[nodiscard]] QString subtitleTranslationPreview(const SubtitleTranslationPlan &plan);
// Inner message.content JSON, exact expected IDs and one nonempty safe text per ID.
[[nodiscard]] bool parseSubtitleTranslationBatch(const SubtitleTranslationPlan &plan,
    int batchIndex, const QByteArray &contentJson, SubtitleTranslations *translations,
    QString *error = nullptr);
// Canonical plan identity and validation shared with the SQLite repository.
[[nodiscard]] QByteArray subtitleTranslationCheckpointManifest(const SubtitleTranslationPlan &plan);
[[nodiscard]] bool validateSubtitleTranslationData(const SubtitleTranslationPlan &plan,
    const SubtitleTranslations &translations, bool requireAll, QString *error = nullptr);
// Normalizes line endings and validates one subtitle text without changing its meaning.
[[nodiscard]] bool validateSubtitleTranslationText(QString *text, QString *error = nullptr);
[[nodiscard]] bool updateSubtitleTranslationCheckpointState(const SubtitleTranslationPlan &plan,
    const QString &state, int batchIndex = -1, QString *error = nullptr);
[[nodiscard]] std::shared_ptr<QLockFile> acquireSubtitleTranslationCheckpointLease(
    const SubtitleTranslationPlan &plan, QString *error = nullptr);
// Missing database checkpoint succeeds with an empty map; malformed data is never reused.
[[nodiscard]] bool loadSubtitleTranslationCheckpoint(const SubtitleTranslationPlan &plan,
    SubtitleTranslations *translations, QString *error = nullptr);
// Only complete validated batches are saved; no raw remote responses or secrets.
[[nodiscard]] bool saveSubtitleTranslationCheckpoint(const SubtitleTranslationPlan &plan,
    const SubtitleTranslations &translations, const std::atomic_bool &cancelled,
    QString *error = nullptr);
[[nodiscard]] AiSubtitleTaskResult publishSubtitleTranslation(const SubtitleTranslationPlan &plan,
    const SubtitleTranslations &translations, const std::atomic_bool &cancelled);
} // namespace asmr
Q_DECLARE_METATYPE(asmr::SubtitleTranslationRequest)
Q_DECLARE_METATYPE(asmr::SubtitleTranslationPlan)
