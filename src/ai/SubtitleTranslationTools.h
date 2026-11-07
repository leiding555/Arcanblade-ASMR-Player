#pragma once

#include "ai/SubtitleTranslationDocument.h"
#include <QJsonArray>
#include <QJsonObject>

namespace asmr {
inline constexpr qsizetype SubtitleTranslationMaximumRequestBytes = 16 * 1024 * 1024;
inline constexpr auto SubtitleTranslationReadToolName = "read_subtitle_translation_state";
inline constexpr auto SubtitleTranslationWriteToolName = "write_subtitle_translations";
inline constexpr auto SubtitleTranslationReferencesToolName = "read_work_references";
inline constexpr auto SubtitleTranslationAsrToolName = "read_work_asr_subtitles";

[[nodiscard]] QJsonArray subtitleTranslationToolDefinitions();
[[nodiscard]] QJsonArray subtitleTranslationToolDefinitions(const SubtitleTranslationPlan &plan);
// Only one work-context reader is exposed by a prepared task.
[[nodiscard]] const char *subtitleTranslationContextToolName(const SubtitleTranslationPlan &plan);
[[nodiscard]] QJsonObject subtitleTranslationReadWorkReferencesResult(
    const SubtitleTranslationPlan &plan, bool includeReferences);
[[nodiscard]] QJsonObject subtitleTranslationReadWorkAsrResult(
    const SubtitleTranslationPlan &plan, bool includeSubtitles);
[[nodiscard]] QJsonArray subtitleTranslationInitialMessages(const SubtitleTranslationPlan &plan);
// No credentials or local paths. Returns empty for an invalid provider or oversized request.
[[nodiscard]] QByteArray subtitleTranslationToolRequest(
    const SubtitleTranslationPlan &plan, const QJsonArray &messages);
[[nodiscard]] QJsonObject subtitleTranslationToolFeedback(const SubtitleTranslationPlan &plan,
    const SubtitleTranslations &confirmed, const QString &status = QStringLiteral("ok"),
    const QString &code = {}, const QString &message = {});
[[nodiscard]] QJsonObject subtitleTranslationReadToolResult(const SubtitleTranslationPlan &plan,
    const SubtitleTranslations &confirmed, bool includeSource);

struct SubtitleTranslationToolWrite {
    // The submitted items only; callers merge them after a successful database commit.
    SubtitleTranslations translations;
    QString code;
    QString error;
    bool duplicate = false;
    [[nodiscard]] bool succeeded() const { return error.isEmpty(); }
};
[[nodiscard]] SubtitleTranslationToolWrite parseSubtitleTranslationToolWrite(
    const SubtitleTranslationPlan &plan, const SubtitleTranslations &confirmed,
    const QByteArray &arguments, bool sourceRead);
} // namespace asmr
