#pragma once

#include "ai/GpuCompatibility.h"

#include <QByteArray>
#include <atomic>

namespace asmr {

class GpuDetector final {
public:
    // Synchronous and potentially blocking: call only from a worker thread.
    // Cancellation is observed during bounded process waits; no shell is used.
    [[nodiscard]] static GpuDetectionResult detect(const std::atomic_bool &cancelled);
    [[nodiscard]] static QList<GpuDevice> parseNvidiaSmiOutput(
        const QByteArray &output, QStringList *diagnostics = nullptr);
};

} // namespace asmr