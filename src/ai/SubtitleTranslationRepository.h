#pragma once
#include "ai/SubtitleTranslationDocument.h"
#include <QString>
#include <atomic>

namespace asmr {
// Synchronous worker-only persistence. Transactions never contain API or file I/O.
// Completed batches are immutable and independently retained from queue history.
// Tool conversations append validated continuous prefixes in v5 physical rows;
// each row holds up to 40 entries without limiting a model's write size.
class SubtitleTranslationRepository final {
public:
    explicit SubtitleTranslationRepository(QString databasePath);
    bool load(const SubtitleTranslationPlan &plan, SubtitleTranslations *translations,
        QString *error = nullptr);
    bool save(const SubtitleTranslationPlan &plan, const SubtitleTranslations &translations,
        const std::atomic_bool &cancelled, QString *error = nullptr);
    bool setState(const SubtitleTranslationPlan &plan, const QString &state,
        int batchIndex = -1, QString *error = nullptr);
    bool publication(const SubtitleTranslationPlan &plan, QString *path, QString *sha256,
        QString *state, QString *error = nullptr);
    bool reservePublication(const SubtitleTranslationPlan &plan, const QString &path,
        const QString &sha256, const std::atomic_bool &cancelled, QString *error = nullptr);
    // Caller first verifies that the reserved final artifact is safely absent.
    bool clearPublicationReservation(const SubtitleTranslationPlan &plan,
        const QString &expectedPath, const QString &expectedSha256, QString *error = nullptr);
    bool confirmPublication(const SubtitleTranslationPlan &plan, const std::atomic_bool &cancelled,
        QString *error = nullptr);
    // The two context stages reserve and acknowledge their files atomically.
    bool reservePublicationPair(const SubtitleTranslationPlan &correction,
        const QString &japanesePath, const QString &japaneseSha256,
        const SubtitleTranslationPlan &translation, const QString &chinesePath,
        const QString &chineseSha256, const std::atomic_bool &cancelled, QString *error = nullptr);
    bool confirmPublicationPair(const SubtitleTranslationPlan &correction,
        const SubtitleTranslationPlan &translation, const std::atomic_bool &cancelled,
        QString *error = nullptr);
private:
    QString m_databasePath;
};
} // namespace asmr
