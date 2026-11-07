#pragma once

#include "artwork/ManualCoverStore.h"

#include <QCache>
#include <QImage>
#include <QMetaType>
#include <QSize>
#include <QString>

#include <atomic>
#include <memory>
#include <optional>

namespace asmr {

struct CoverArtRequest {
    QString rootPath;
    QString audioPath;
    bool libraryWork = false;
    bool forceReload = false;
    QString manualRelativePath;
    bool manualOnly = false;
    std::optional<ManualCoverLookup> manualLookup;
};

enum class CoverArtStatus { Ready, NotFound, Error, LimitExceeded, Cancelled };

struct CoverArtResult {
    CoverArtStatus status = CoverArtStatus::NotFound;
    QImage image;
    QString sourcePath;
    QString message;
    bool fromCache = false;
    bool manual = false;
    bool manualFallback = false;
    QString manualRelativePath;
};

struct CoverArtOptions {
    int maxDepth = 4;
    qsizetype maxEntries = 10000;
    qsizetype maxCandidates = 32;
    qint64 maxFileBytes = 32 * 1024 * 1024;
    qint64 maxPixels = 32000000;
    QSize thumbnailSize{256, 256};
};

// Used by one worker at a time. QImage values remain implicitly shared with the UI.
class CoverArtCache final {
public:
    explicit CoverArtCache(int maximumBytes = 16 * 1024 * 1024);
    [[nodiscard]] int totalCost() const;
    [[nodiscard]] int maximumCost() const;
    void clear();
private:
    friend class CoverArtLoader;
    QCache<QString, QImage> m_images;
};

class CoverArtLoader final {
public:
    [[nodiscard]] static CoverArtResult load(
        const CoverArtRequest &request,
        const CoverArtOptions &options = {},
        const std::shared_ptr<std::atomic_bool> &cancelled = {},
        CoverArtCache *cache = nullptr);
    [[nodiscard]] static CoverArtResult loadSelected(
        const QString &rootPath, const QString &relativePath,
        const CoverArtOptions &options = {},
        const std::shared_ptr<std::atomic_bool> &cancelled = {},
        CoverArtCache *cache = nullptr);
private:
    [[nodiscard]] static CoverArtResult loadImage(
        const QString &rootPath, const QString &relativePath, bool forceReload,
        const CoverArtOptions &options,
        const std::shared_ptr<std::atomic_bool> &cancelled, CoverArtCache *cache);
};

} // namespace asmr

Q_DECLARE_METATYPE(asmr::CoverArtResult)
Q_DECLARE_METATYPE(asmr::CoverArtStatus)
