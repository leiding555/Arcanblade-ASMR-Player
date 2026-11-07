#include "ai/GpuCompatibility.h"

#include <QRegularExpression>
#include <QVersionNumber>

namespace asmr {
namespace {

constexpr quint64 MinimumGpuMemory = 4ULL * 1024 * 1024 * 1024;

std::optional<QVersionNumber> nvidiaDriver(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("^\\s*([0-9]{3,4})\\.([0-9]{1,2})\\s*$"));
    const auto match = pattern.match(text);
    if (!match.hasMatch())
        return std::nullopt;
    return QVersionNumber(match.captured(1).toInt(), match.captured(2).toInt());
}

bool driverAtLeast(const QVersionNumber &driver, int major, int minor)
{
    return QVersionNumber::compare(driver, QVersionNumber(major, minor)) >= 0;
}

bool virtualName(const QString &name)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\b(?:virtual|virtualbox|vmware|remote|software)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern.match(name).hasMatch();
}

std::optional<int> nvidiaFamily(const QString &name)
{
    static const QRegularExpression workstation(
        QStringLiteral("\\b(?:quadro|ada|workstation|professional)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    if (workstation.match(name).hasMatch())
        return std::nullopt;
    static const QRegularExpression pattern(QStringLiteral("\\b(GTX|RTX)\\s*([0-9]{4})(?![0-9])"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = pattern.match(name);
    if (!match.hasMatch())
        return std::nullopt;
    const int model = match.captured(2).toInt();
    const int family = model / 100;
    if (match.captured(1).compare(QStringLiteral("GTX"), Qt::CaseInsensitive) == 0)
        return family == 10 || family == 16 ? std::optional<int>(family) : std::nullopt;
    // RTX 2000/3000/4000/5000 workstation cards are not GeForce 20/30/40/50 series.
    if (model % 100 < 50)
        return std::nullopt;
    if (family == 20 || family == 30 || family == 40 || family == 50)
        return family;
    return std::nullopt;
}

std::optional<ChickenRiceVariant> amdVariant(const QString &name)
{
    static const QRegularExpression pattern(QStringLiteral("\\bRX\\s*([0-9]{4})(?![0-9])"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = pattern.match(name);
    if (!match.hasMatch())
        return std::nullopt;
    const int model = match.captured(1).toInt();
    switch (model / 100) {
    case 53: case 55: case 56: case 57:
        return ChickenRiceVariant::Gfx101xDgpu;
    case 64: case 65: case 66: case 67: case 68: case 69:
        return ChickenRiceVariant::Gfx103xDgpu;
    case 76: case 77: case 78: case 79:
        return ChickenRiceVariant::Gfx110xAll;
    default:
        if (model == 9060 || model == 9070)
            return ChickenRiceVariant::Gfx120xAll;
        return std::nullopt;
    }
}

void qualifyMemory(const GpuDevice &device, GpuRecommendation &recommendation)
{
    if (device.dedicatedMemoryBytes == 0) {
        if (recommendation.status == RecommendationStatus::Recommended)
            recommendation.status = RecommendationStatus::NeedsVerification;
        recommendation.explanation += QStringLiteral(" 未取得独立显存容量，仍需验证实际运行。 ");
    } else if (device.dedicatedMemoryBytes < MinimumGpuMemory) {
        if (recommendation.status == RecommendationStatus::Recommended)
            recommendation.status = RecommendationStatus::NeedsVerification;
        recommendation.explanation += QStringLiteral(" 独立显存不足 4 GiB，不保证 GPU 推理可用；可改用 CPU。 ");
    }
    recommendation.explanation = recommendation.explanation.trimmed();
}

int recommendationRank(RecommendationStatus status)
{
    switch (status) {
    case RecommendationStatus::Recommended: return 3;
    case RecommendationStatus::NeedsVerification: return 2;
    case RecommendationStatus::DriverUpdateRequired: return 1;
    case RecommendationStatus::NoAutomaticMatch: return 0;
    }
    return 0;
}

} // namespace

QList<ChickenRiceVariant> allChickenRiceVariants()
{
    return {ChickenRiceVariant::Cu118, ChickenRiceVariant::Cu122, ChickenRiceVariant::Cu128,
        ChickenRiceVariant::Gfx101xDgpu, ChickenRiceVariant::Gfx103xDgpu,
        ChickenRiceVariant::Gfx110xAll, ChickenRiceVariant::Gfx120xAll};
}

QString chickenRiceVariantKey(ChickenRiceVariant variant)
{
    switch (variant) {
    case ChickenRiceVariant::Cu118: return QStringLiteral("cu118");
    case ChickenRiceVariant::Cu122: return QStringLiteral("cu122");
    case ChickenRiceVariant::Cu128: return QStringLiteral("cu128");
    case ChickenRiceVariant::Gfx101xDgpu: return QStringLiteral("gfx101x_dgpu");
    case ChickenRiceVariant::Gfx103xDgpu: return QStringLiteral("gfx103x_dgpu");
    case ChickenRiceVariant::Gfx110xAll: return QStringLiteral("gfx110x_all");
    case ChickenRiceVariant::Gfx120xAll: return QStringLiteral("gfx120x_all");
    }
    return {};
}

QString chickenRiceVariantDisplayName(ChickenRiceVariant variant)
{
    switch (variant) {
    case ChickenRiceVariant::Cu118: return QStringLiteral("NVIDIA CUDA 11.8");
    case ChickenRiceVariant::Cu122: return QStringLiteral("NVIDIA CUDA 12.2");
    case ChickenRiceVariant::Cu128: return QStringLiteral("NVIDIA CUDA 12.8");
    case ChickenRiceVariant::Gfx101xDgpu: return QStringLiteral("AMD gfx101x（RDNA1 独显）");
    case ChickenRiceVariant::Gfx103xDgpu: return QStringLiteral("AMD gfx103x（RDNA2 独显）");
    case ChickenRiceVariant::Gfx110xAll: return QStringLiteral("AMD gfx110x（RDNA3）");
    case ChickenRiceVariant::Gfx120xAll: return QStringLiteral("AMD gfx120x（RDNA4）");
    }
    return {};
}

QString chickenRiceArchiveName(ChickenRiceVariant variant)
{
    const auto key = chickenRiceVariantKey(variant);
    return key.isEmpty() ? QString() : QStringLiteral("faster_whisper_transwithai_windows_%1-translate.zip").arg(key);
}

std::optional<ChickenRiceVariant> chickenRiceVariantFromKey(const QString &key)
{
    for (const auto variant : allChickenRiceVariants()) {
        if (chickenRiceVariantKey(variant) == key)
            return variant;
    }
    return std::nullopt;
}

GpuRecommendation recommendChickenRice(const GpuDevice &device)
{
    GpuRecommendation result;
    if (device.software || virtualName(device.name)) {
        result.explanation = QStringLiteral("软件或虚拟显示设备没有自动匹配的推理组件，请手动选择或使用 CPU。");
        return result;
    }
    if (device.vendor == GpuVendor::Intel) {
        result.explanation = QStringLiteral("ChickenRice v1.10 没有 Intel GPU 专用发行包，可使用 CPU。");
        return result;
    }
    if (device.vendor == GpuVendor::Amd) {
        result.variant = amdVariant(device.name);
        if (!result.variant) {
            result.explanation = QStringLiteral("此 AMD 设备未在 v1.10 的明确型号列表中，不能自动确认 gfx 版本。");
            return result;
        }
        result.status = RecommendationStatus::NeedsVerification;
        result.explanation = QStringLiteral("按 v1.10 的 AMD 型号列表选择候选版本；没有可靠的最低驱动门槛，仍需实机验证。");
        qualifyMemory(device, result);
        return result;
    }
    if (device.vendor != GpuVendor::Nvidia) {
        result.explanation = QStringLiteral("未识别到 v1.10 支持的 GPU 型号，请手动选择或使用 CPU。");
        return result;
    }
    const auto family = nvidiaFamily(device.name);
    if (!family) {
        result.explanation = QStringLiteral("此 NVIDIA 型号没有明确的自动匹配规则；工作站与未知型号请手动选择。");
        return result;
    }
    if (*family == 10 || *family == 16)
        result.variant = ChickenRiceVariant::Cu118;
    else if (*family == 20 || *family == 30)
        result.variant = ChickenRiceVariant::Cu122;
    else
        result.variant = ChickenRiceVariant::Cu128;

    const auto driver = nvidiaDriver(device.driverVersion);
    if (!driver) {
        result.status = RecommendationStatus::NeedsVerification;
        result.explanation = QStringLiteral("已按 GPU 型号选择候选版本，但未取得可判定的 NVIDIA 驱动版本，仍需验证。");
    } else {
        bool supported = false;
        if (*family == 10 || *family == 16) {
            supported = driverAtLeast(*driver, 522, 6);
        } else if (*family == 20 || *family == 30) {
            supported = driverAtLeast(*driver, 536, 25);
            if (!supported && driverAtLeast(*driver, 522, 6)) {
                result.variant = ChickenRiceVariant::Cu118;
                supported = true;
            }
        } else if (*family == 40) {
            supported = driverAtLeast(*driver, 570, 65);
            if (!supported && driverAtLeast(*driver, 536, 25)) {
                result.variant = ChickenRiceVariant::Cu122;
                supported = true;
            }
        } else {
            supported = driverAtLeast(*driver, 570, 65);
        }
        if (supported) {
            result.status = RecommendationStatus::Recommended;
            result.explanation = QStringLiteral("型号与驱动达到此版本的保守推荐门槛；这仍不是推理已通过的证明，安装后需要自检。");
        } else {
            result.status = RecommendationStatus::DriverUpdateRequired;
            result.explanation = QStringLiteral("当前驱动低于该型号候选版本的保守推荐门槛，请更新驱动后重新检测，或使用 CPU。");
        }
    }
    qualifyMemory(device, result);
    return result;
}

int preferredGpuIndex(const QList<GpuDevice> &devices)
{
    int best = -1;
    int bestRank = 0;
    quint64 bestMemory = 0;
    for (qsizetype index = 0; index < devices.size(); ++index) {
        const auto &device = devices.at(index);
        const auto recommendation = recommendChickenRice(device);
        const int rank = recommendation.variant ? recommendationRank(recommendation.status) : 0;
        if (rank > bestRank || (rank > 0 && rank == bestRank && device.dedicatedMemoryBytes > bestMemory)) {
            best = static_cast<int>(index);
            bestRank = rank;
            bestMemory = device.dedicatedMemoryBytes;
        }
    }
    return best;
}

} // namespace asmr