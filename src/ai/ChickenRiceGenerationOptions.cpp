#include "ai/ChickenRiceGenerationOptions.h"

#include <QLocale>

#include <cmath>

namespace asmr {

QString chickenRiceGenerationPresetKey(ChickenRiceGenerationPreset preset)
{
    switch (preset) {
    case ChickenRiceGenerationPreset::ComponentDefault: return QStringLiteral("component-default");
    case ChickenRiceGenerationPreset::QuietSpeech: return QStringLiteral("quiet-speech");
    case ChickenRiceGenerationPreset::Conservative: return QStringLiteral("conservative");
    case ChickenRiceGenerationPreset::Custom: return QStringLiteral("custom");
    }
    return {};
}

std::optional<ChickenRiceGenerationPreset> chickenRiceGenerationPresetFromKey(const QString &key)
{
    if (key == QStringLiteral("component-default")) return ChickenRiceGenerationPreset::ComponentDefault;
    if (key == QStringLiteral("quiet-speech")) return ChickenRiceGenerationPreset::QuietSpeech;
    if (key == QStringLiteral("conservative")) return ChickenRiceGenerationPreset::Conservative;
    if (key == QStringLiteral("custom")) return ChickenRiceGenerationPreset::Custom;
    return std::nullopt;
}

QString validateChickenRiceGenerationOptions(const ChickenRiceGenerationOptions &options)
{
    if (chickenRiceGenerationPresetKey(options.preset).isEmpty())
        return QStringLiteral("Unknown subtitle generation preset.");
    if (options.preset != ChickenRiceGenerationPreset::Custom)
        return {};
    if (!std::isfinite(options.vadThreshold)
        || options.vadThreshold < ChickenRiceVadThresholdMinimum
        || options.vadThreshold > ChickenRiceVadThresholdMaximum)
        return QStringLiteral("The voice detection threshold must be between 0.3 and 0.7.");
    if (options.minSpeechDurationMs < ChickenRiceMinSpeechDurationMinimumMs
        || options.minSpeechDurationMs > ChickenRiceMinSpeechDurationMaximumMs)
        return QStringLiteral("The minimum speech duration must be between 50 and 2000 ms.");
    if (options.minSilenceDurationMs < ChickenRiceMinSilenceDurationMinimumMs
        || options.minSilenceDurationMs > ChickenRiceMinSilenceDurationMaximumMs)
        return QStringLiteral("The minimum silence duration must be between 50 and 2000 ms.");
    if (options.speechPadMs < ChickenRiceSpeechPadMinimumMs
        || options.speechPadMs > ChickenRiceSpeechPadMaximumMs)
        return QStringLiteral("Speech padding must be between 0 and 1000 ms.");
    return {};
}

QStringList chickenRiceGenerationArguments(const ChickenRiceGenerationOptions &options)
{
    switch (options.preset) {
    case ChickenRiceGenerationPreset::ComponentDefault:
        return {};
    case ChickenRiceGenerationPreset::QuietSpeech:
        return {QStringLiteral("--vad_threshold"), QStringLiteral("0.4")};
    case ChickenRiceGenerationPreset::Conservative:
        return {QStringLiteral("--vad_threshold"), QStringLiteral("0.6")};
    case ChickenRiceGenerationPreset::Custom:
        return {QStringLiteral("--vad_threshold"), QLocale::c().toString(options.vadThreshold, 'g', QLocale::FloatingPointShortest),
            QStringLiteral("--vad_min_speech_duration_ms"), QString::number(options.minSpeechDurationMs),
            QStringLiteral("--vad_min_silence_duration_ms"), QString::number(options.minSilenceDurationMs),
            QStringLiteral("--vad_speech_pad_ms"), QString::number(options.speechPadMs),
            options.mergeSegments ? QStringLiteral("--merge_segments") : QStringLiteral("--no_merge_segments")};
    }
    return {};
}

QJsonObject chickenRiceGenerationRecord(const ChickenRiceGenerationOptions &options)
{
    QJsonObject overrides;
    if (options.preset == ChickenRiceGenerationPreset::QuietSpeech
        || options.preset == ChickenRiceGenerationPreset::Conservative) {
        overrides.insert(QStringLiteral("vad_parameters"), QJsonObject{
            {QStringLiteral("threshold"), options.preset == ChickenRiceGenerationPreset::QuietSpeech ? 0.4 : 0.6}});
    } else if (options.preset == ChickenRiceGenerationPreset::Custom) {
        overrides.insert(QStringLiteral("vad_parameters"), QJsonObject{
            {QStringLiteral("threshold"), options.vadThreshold},
            {QStringLiteral("min_speech_duration_ms"), options.minSpeechDurationMs},
            {QStringLiteral("min_silence_duration_ms"), options.minSilenceDurationMs},
            {QStringLiteral("speech_pad_ms"), options.speechPadMs}});
        overrides.insert(QStringLiteral("segment_merge"),
            QJsonObject{{QStringLiteral("enabled"), options.mergeSegments}});
    }
    return {{QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("preset"), chickenRiceGenerationPresetKey(options.preset)},
        {QStringLiteral("baseConfiguration"), QStringLiteral("generation_config.snapshot.json5")},
        {QStringLiteral("overrides"), overrides}};
}

} // namespace asmr
