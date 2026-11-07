#pragma once

#include <QDateTime>
#include <QString>
#include <optional>

namespace asmr {
// Written beside a completed generated SRT, never inferred from a folder name.
struct GeneratedSubtitleInfo {
    QString mode;
    QDateTime completedAt;
    QString audioFileName;
    QString subtitleFileName;
    QString language; // Empty legacy records are resolved from their generation mode.
    QString translationService; // deepseek / openai-compatible for cloud-translation.
    QString translationModel;
};

// Bounded synchronous filesystem operations; UI callers must use a worker.
[[nodiscard]] std::optional<GeneratedSubtitleInfo> readGeneratedSubtitleInfo(const QString &subtitlePath);
[[nodiscard]] QString generatedSubtitleInfoPath(const QString &directory, const QString &subtitleFileName);
// Never replaces a record. Per-subtitle records may be reused after an
// interrupted publish only when their identity matches and the SRT is absent.
[[nodiscard]] bool writeGeneratedSubtitleInfo(const QString &directory,
    const GeneratedSubtitleInfo &info, QString *error = nullptr, bool perSubtitle = false);
} // namespace asmr
