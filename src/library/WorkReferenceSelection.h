#pragma once

#include "library/DirectoryScanner.h"

namespace asmr {
// Uses an already completed directory snapshot. Never guesses generation
// provenance from a filename, and never reads subtitle payloads to rank ASR.
[[nodiscard]] WorkReferenceSelection selectWorkReferences(
    const DirectoryScanResult &scan,
    const std::function<bool()> &cancelled = {});
} // namespace asmr
