#pragma once
#include <QMetaType>
#include <QStringList>
#include <optional>

namespace asmr {
enum class AiSubtitleTaskState { Idle, Preparing, CheckingModel, Running, Validating, Succeeded, Cancelled, Failed };
enum class AiSubtitleComputeMode { GpuLowMemory = 0, Gpu = GpuLowMemory, Cpu = 1, GpuHighMemory = 2 };
[[nodiscard]] inline bool isValidAiSubtitleComputeMode(AiSubtitleComputeMode mode)
{
    return mode == AiSubtitleComputeMode::GpuLowMemory || mode == AiSubtitleComputeMode::GpuHighMemory
        || mode == AiSubtitleComputeMode::Cpu;
}
[[nodiscard]] inline QString aiSubtitleComputeModeKey(AiSubtitleComputeMode mode)
{
    switch (mode) {
    case AiSubtitleComputeMode::GpuLowMemory: return QStringLiteral("gpu-low-memory");
    case AiSubtitleComputeMode::GpuHighMemory: return QStringLiteral("gpu-high-memory");
    case AiSubtitleComputeMode::Cpu: return QStringLiteral("cpu");
    }
    return {};
}
[[nodiscard]] inline std::optional<AiSubtitleComputeMode> aiSubtitleComputeModeFromKey(const QString &key)
{
    if (key == QStringLiteral("gpu") || key == QStringLiteral("gpu-low-memory")) return AiSubtitleComputeMode::GpuLowMemory;
    if (key == QStringLiteral("gpu-high-memory")) return AiSubtitleComputeMode::GpuHighMemory;
    if (key == QStringLiteral("cpu")) return AiSubtitleComputeMode::Cpu;
    return std::nullopt;
}
struct AiSubtitleTaskProgress {
    AiSubtitleTaskState state = AiSubtitleTaskState::Idle;
    QString message;
    QString logTail;
    int percent = -1; // -1: no reliable percentage is available for the current phase.
};
struct AiSubtitleTaskResult {
    AiSubtitleTaskState state = AiSubtitleTaskState::Idle;
    QString audioPath;
    QString srtPath;
    QString taskDirectory;
    QString logPath;
    QString error;
    int exitCode = -1;
    qsizetype subtitleCount = 0;
    QStringList warnings;
    QString mode;
    QString language;
    // Additional Japanese artifact for a completed context-assisted translation.
    QString correctedJapanesePath;
};
} // namespace asmr
Q_DECLARE_METATYPE(asmr::AiSubtitleTaskState)
Q_DECLARE_METATYPE(asmr::AiSubtitleTaskProgress)
Q_DECLARE_METATYPE(asmr::AiSubtitleTaskResult)
