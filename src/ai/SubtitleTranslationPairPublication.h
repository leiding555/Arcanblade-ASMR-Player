#pragma once
#include "ai/SubtitleTranslationDocument.h"

namespace asmr {
// Worker-only. Both completed stages use one output directory and atomic DB intents.
// Partial filesystem publication is verified and completed on retry without replacing files.
[[nodiscard]] AiSubtitleTaskResult publishSubtitleTranslationPair(
    const SubtitleTranslationPlan &correction, const SubtitleTranslations &corrected,
    const SubtitleTranslationPlan &translation, const SubtitleTranslations &translated,
    const std::atomic_bool &cancelled);
} // namespace asmr
