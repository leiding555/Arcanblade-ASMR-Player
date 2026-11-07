#pragma once

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <optional>

namespace asmr {

enum class GpuVendor { Unknown, Nvidia, Amd, Intel };

struct GpuDevice {
    QString id;
    QString name;
    QString driverVersion;
    GpuVendor vendor = GpuVendor::Unknown;
    quint64 dedicatedMemoryBytes = 0; // Zero means unknown, not no memory.
    bool software = false;
};

struct GpuDetectionResult {
    QList<GpuDevice> devices;
    QStringList diagnostics;
    bool cancelled = false;
};

enum class ChickenRiceVariant {
    Cu118, Cu122, Cu128, Gfx101xDgpu, Gfx103xDgpu, Gfx110xAll, Gfx120xAll
};

enum class RecommendationStatus {
    Recommended, NeedsVerification, DriverUpdateRequired, NoAutomaticMatch
};

struct GpuRecommendation {
    std::optional<ChickenRiceVariant> variant;
    RecommendationStatus status = RecommendationStatus::NoAutomaticMatch;
    QString explanation;
    bool lowMemoryMode = true;
};

[[nodiscard]] QList<ChickenRiceVariant> allChickenRiceVariants();
[[nodiscard]] QString chickenRiceVariantKey(ChickenRiceVariant variant);
[[nodiscard]] QString chickenRiceVariantDisplayName(ChickenRiceVariant variant);
// Archive base name only: v1.10 distributes this archive in numbered parts.
[[nodiscard]] QString chickenRiceArchiveName(ChickenRiceVariant variant);
[[nodiscard]] std::optional<ChickenRiceVariant> chickenRiceVariantFromKey(const QString &key);
[[nodiscard]] GpuRecommendation recommendChickenRice(const GpuDevice &device);
// Prefer an actionable candidate, then known dedicated memory; -1 if none matches.
[[nodiscard]] int preferredGpuIndex(const QList<GpuDevice> &devices);

} // namespace asmr

Q_DECLARE_METATYPE(asmr::GpuVendor)
Q_DECLARE_METATYPE(asmr::GpuDevice)
Q_DECLARE_METATYPE(asmr::GpuDetectionResult)
Q_DECLARE_METATYPE(asmr::ChickenRiceVariant)
Q_DECLARE_METATYPE(asmr::RecommendationStatus)
Q_DECLARE_METATYPE(asmr::GpuRecommendation)