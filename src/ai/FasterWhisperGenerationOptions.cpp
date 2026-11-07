#include "ai/FasterWhisperGenerationOptions.h"
#include <QLocale>
#include <cmath>

namespace asmr {
namespace {
QString precision(AiSubtitleComputeMode mode)
{
    switch (mode) {
    case AiSubtitleComputeMode::GpuLowMemory: return QStringLiteral("int8_float16");
    case AiSubtitleComputeMode::GpuHighMemory: return QStringLiteral("float16");
    case AiSubtitleComputeMode::Cpu: return QStringLiteral("int8");
    }
    return {};
}
} // namespace
QString fasterWhisperGenerationPresetKey(FasterWhisperGenerationPreset preset)
{
    switch (preset) {
    case FasterWhisperGenerationPreset::ComponentDefault: return QStringLiteral("component-default");
    case FasterWhisperGenerationPreset::QuietSpeech: return QStringLiteral("quiet-speech");
    case FasterWhisperGenerationPreset::Conservative: return QStringLiteral("conservative");
    case FasterWhisperGenerationPreset::Custom: return QStringLiteral("custom");
    }
    return {};
}
std::optional<FasterWhisperGenerationPreset> fasterWhisperGenerationPresetFromKey(const QString &key)
{
    for (const auto preset : {FasterWhisperGenerationPreset::ComponentDefault, FasterWhisperGenerationPreset::QuietSpeech,
             FasterWhisperGenerationPreset::Conservative, FasterWhisperGenerationPreset::Custom})
        if (fasterWhisperGenerationPresetKey(preset) == key) return preset;
    return std::nullopt;
}
QString validateFasterWhisperGenerationOptions(const FasterWhisperGenerationOptions &options)
{
    if (fasterWhisperGenerationPresetKey(options.preset).isEmpty())
        return QStringLiteral("Unknown Japanese recognition preset.");
    if (options.preset != FasterWhisperGenerationPreset::Custom) return {};
    if (!std::isfinite(options.vadThreshold) || options.vadThreshold < 0.3 || options.vadThreshold > 0.7)
        return QStringLiteral("The voice detection threshold must be between 0.3 and 0.7.");
    if (options.beamSize < 1 || options.beamSize > 10)
        return QStringLiteral("Beam size must be between 1 and 10.");
    return {};
}
QStringList fasterWhisperGenerationArguments(const FasterWhisperGenerationOptions &options, AiSubtitleComputeMode mode)
{
    if (!isValidAiSubtitleComputeMode(mode)) return {};
    QStringList arguments{QStringLiteral("--compute_type"), precision(mode)};
    switch (options.preset) {
    case FasterWhisperGenerationPreset::ComponentDefault: break;
    case FasterWhisperGenerationPreset::QuietSpeech:
        arguments.append({QStringLiteral("--vad_threshold"), QStringLiteral("0.35")});
        break;
    case FasterWhisperGenerationPreset::Conservative:
        arguments.append({QStringLiteral("--vad_threshold"), QStringLiteral("0.55")});
        break;
    case FasterWhisperGenerationPreset::Custom:
        arguments.append({QStringLiteral("--vad_filter"), options.vadFilter ? QStringLiteral("True") : QStringLiteral("False"),
            QStringLiteral("--vad_threshold"), QLocale::c().toString(options.vadThreshold, 'g', QLocale::FloatingPointShortest),
            QStringLiteral("--beam_size"), QString::number(options.beamSize),
            QStringLiteral("--condition_on_previous_text"), options.conditionOnPreviousText ? QStringLiteral("True") : QStringLiteral("False")});
        if (options.subtitleFormatting) arguments.append(QStringLiteral("--standard_asia"));
        break;
    }
    return arguments;
}
QJsonObject fasterWhisperGenerationRecord(const FasterWhisperGenerationOptions &options, AiSubtitleComputeMode mode)
{
    QJsonObject overrides{{QStringLiteral("compute_type"), precision(mode)}};
    if (options.preset == FasterWhisperGenerationPreset::QuietSpeech || options.preset == FasterWhisperGenerationPreset::Conservative)
        overrides.insert(QStringLiteral("vad_threshold"), options.preset == FasterWhisperGenerationPreset::QuietSpeech ? 0.35 : 0.55);
    if (options.preset == FasterWhisperGenerationPreset::Custom) {
        overrides.insert(QStringLiteral("vad_filter"), options.vadFilter);
        overrides.insert(QStringLiteral("vad_threshold"), options.vadThreshold);
        overrides.insert(QStringLiteral("beam_size"), options.beamSize);
        overrides.insert(QStringLiteral("condition_on_previous_text"), options.conditionOnPreviousText);
        overrides.insert(QStringLiteral("standard_asia"), options.subtitleFormatting);
    }
    return {{QStringLiteral("schemaVersion"), 2}, {QStringLiteral("preset"), fasterWhisperGenerationPresetKey(options.preset)},
        {QStringLiteral("computeMode"), aiSubtitleComputeModeKey(mode)},
        {QStringLiteral("subtitleFormatting"), options.preset == FasterWhisperGenerationPreset::Custom && options.subtitleFormatting},
        {QStringLiteral("baseConfiguration"), QStringLiteral("Faster-Whisper-XXL r245.4 CLI defaults except explicit execution mode")},
        {QStringLiteral("overrides"), overrides}};
}
} // namespace asmr
