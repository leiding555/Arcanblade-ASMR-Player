#pragma once
#include "ai/AiSubtitleTaskTypes.h"
#include <QProcessEnvironment>
#include <QByteArray>
#include <atomic>
#include <functional>

namespace asmr {
struct AiSubtitleProcessRequest {
    QString program;
    QStringList arguments;
    QString workingDirectory;
    QProcessEnvironment environment;
    QString logPath;
    QString runningMessage;
    bool parseWhisperProgress = false;
    QByteArray logPreamble{}; // Informational task notes, excluded from the engine output tail.
};
struct AiSubtitleProcessResult {
    bool succeeded = false;
    int exitCode = -1;
    QString error;
    QStringList warnings;
    QByteArray tail;
    // Natural abnormal parent exit, no live workers before cleanup, job drained,
    // diagnostics saved; false for cancellation, timeout or log/tree failures.
    bool abnormalExitWithoutActiveWorkers = false;
};
// Only a complete tqdm transcription bar is recognised; arbitrary log percentages are ignored.
[[nodiscard]] int fasterWhisperProgressPercent(const QByteArray &tail);
[[nodiscard]] AiSubtitleProcessResult runAiSubtitleProcess(const AiSubtitleProcessRequest &request,
    const std::atomic_bool &cancelled, const std::function<void(const AiSubtitleTaskProgress &)> &report);
} // namespace asmr
