#pragma once

#include "library/WorkReferenceTypes.h"
#include <atomic>

namespace asmr {
// All synchronous filesystem and Qt PDF operations belong on a worker.
[[nodiscard]] WorkReferenceRecord extractWorkReference(
    const WorkReferenceCandidate &candidate, const std::atomic_bool &cancelled,
    const WorkReferenceRecord *previous = nullptr, bool force = false);
[[nodiscard]] bool validWorkReferenceCache(const WorkReferenceRecord &record);
[[nodiscard]] QString workReferenceContentVersion(const WorkReferenceRecord &record);
} // namespace asmr
