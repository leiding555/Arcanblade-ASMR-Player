#pragma once
#include "ai/AiSubtitleTaskTypes.h"
#include <atomic>
#include <QDateTime>
#include <functional>
namespace asmr {
struct AiSubtitleOutputRequest {
    QString audioPath;
    QString libraryRoot;
    QString libraryRelativePath;
    QString sourceSrt;
    QString mode;
    QString language;
    QString directoryPrefix;
    QString logTail;
    qint64 audioSize = -1;
    QDateTime audioModified;
    QString outputBatchId;
    QString outputFileSuffix;
};
[[nodiscard]] bool validAiSubtitleOutputToken(const QString &token);
// Worker-only. Creates a new owned batch directory, or verifies its immutable
// ownership record before reusing it. Never adopts an unmarked directory.
[[nodiscard]] QString ensureAiSubtitleBatchDirectory(const QString &baseDirectory,
    const QString &directoryPrefix, const QString &batchId, QString *error = nullptr);
[[nodiscard]] AiSubtitleTaskResult validateAndPublishAiSubtitle(const AiSubtitleOutputRequest &request,
    AiSubtitleTaskResult result, const std::atomic_bool &cancelled,
    const std::function<void(const AiSubtitleTaskProgress &)> &report);
} // namespace asmr
