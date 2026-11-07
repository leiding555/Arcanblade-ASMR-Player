#pragma once

#include <QJsonObject>
#include <QStringList>

#include <optional>

namespace asmr {

enum class ChickenRiceGenerationPreset { ComponentDefault, QuietSpeech, Conservative, Custom };

inline constexpr double ChickenRiceVadThresholdMinimum = 0.3;
inline constexpr double ChickenRiceVadThresholdMaximum = 0.7;
inline constexpr int ChickenRiceMinSpeechDurationMinimumMs = 50;
inline constexpr int ChickenRiceMinSpeechDurationMaximumMs = 2000;
inline constexpr int ChickenRiceMinSilenceDurationMinimumMs = 50;
inline constexpr int ChickenRiceMinSilenceDurationMaximumMs = 2000;
inline constexpr int ChickenRiceSpeechPadMinimumMs = 0;
inline constexpr int ChickenRiceSpeechPadMaximumMs = 1000;

// Values are overrides only in Custom. The two speech presets override just
// the threshold; ComponentDefault leaves the component's JSON5 unchanged.
struct ChickenRiceGenerationOptions {
    ChickenRiceGenerationPreset preset = ChickenRiceGenerationPreset::ComponentDefault;
    double vadThreshold = 0.5;
    int minSpeechDurationMs = 300;
    int minSilenceDurationMs = 100;
    int speechPadMs = 200;
    bool mergeSegments = true;
};

[[nodiscard]] QString chickenRiceGenerationPresetKey(ChickenRiceGenerationPreset preset);
[[nodiscard]] std::optional<ChickenRiceGenerationPreset>
chickenRiceGenerationPresetFromKey(const QString &key);
// Empty means valid. Dormant custom values do not affect the three presets.
[[nodiscard]] QString validateChickenRiceGenerationOptions(const ChickenRiceGenerationOptions &options);
// Callers must validate before passing these arguments to an external engine.
[[nodiscard]] QStringList chickenRiceGenerationArguments(const ChickenRiceGenerationOptions &options);
// Together with the byte-for-byte base snapshot, describes the effective
// configuration without claiming unknown component defaults as actual values.
[[nodiscard]] QJsonObject chickenRiceGenerationRecord(const ChickenRiceGenerationOptions &options);

} // namespace asmr
