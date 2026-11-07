#pragma once
#include "ai/AiSubtitleTaskTypes.h"
#include <QJsonObject>
#include <optional>

namespace asmr {
enum class FasterWhisperGenerationPreset { ComponentDefault, QuietSpeech, Conservative, Custom };
struct FasterWhisperGenerationOptions {
    FasterWhisperGenerationPreset preset = FasterWhisperGenerationPreset::ComponentDefault;
    bool vadFilter = true;
    double vadThreshold = 0.45;
    bool subtitleFormatting = false;
    int beamSize = 5;
    bool conditionOnPreviousText = true;
};
[[nodiscard]] QString fasterWhisperGenerationPresetKey(FasterWhisperGenerationPreset preset);
[[nodiscard]] std::optional<FasterWhisperGenerationPreset> fasterWhisperGenerationPresetFromKey(const QString &key);
[[nodiscard]] QString validateFasterWhisperGenerationOptions(const FasterWhisperGenerationOptions &options);
[[nodiscard]] QStringList fasterWhisperGenerationArguments(const FasterWhisperGenerationOptions &options, AiSubtitleComputeMode mode);
[[nodiscard]] QJsonObject fasterWhisperGenerationRecord(const FasterWhisperGenerationOptions &options,
    AiSubtitleComputeMode mode = AiSubtitleComputeMode::GpuLowMemory);
} // namespace asmr
