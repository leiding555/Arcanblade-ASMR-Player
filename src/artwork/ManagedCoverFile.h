#pragma once

#include "artwork/ManualCoverStore.h"

#include <QByteArray>
#include <QImage>

#include <atomic>
#include <memory>

namespace asmr {

struct ManagedCoverDraft {
    bool success = false;
    QByteArray bytes;
    QString extension;
    QImage image;
    QString errorMessage;
};

struct ManagedCoverStageResult {
    bool success = false;
    QString token;
    QString errorMessage;
};

// All operations are synchronous and belong on a worker. Draft bytes are an
// immutable snapshot: saving never reopens the user-selected original image.
class ManagedCoverFile final {
public:
    [[nodiscard]] static ManagedCoverDraft readExternal(const QString &path,
        const std::shared_ptr<std::atomic_bool> &cancelled = {});
    [[nodiscard]] static ManagedCoverStageResult stage(const ManualCoverSettings &settings,
        const ManualCoverIdentity &identity, const ManagedCoverDraft &draft);
    [[nodiscard]] static QString path(const ManualCoverSettings &settings,
        const ManualCoverIdentity &identity, const QString &token);
    [[nodiscard]] static bool validToken(const QString &token);
    [[nodiscard]] static bool discard(const ManualCoverSettings &settings,
        const ManualCoverIdentity &identity, const QString &token);
    // Removes only recognisable private copies for this exact import identity.
    // Call only after committing the title and manual-selection changes.
    [[nodiscard]] static bool cleanup(const ManualCoverSettings &settings,
        const ManualCoverIdentity &identity, const QString &keepToken = {});
    // Categories own a separate namespace, scoped by database path and category ID.
    [[nodiscard]] static ManagedCoverStageResult stageCategory(const ManualCoverSettings &settings,
        qint64 categoryId, const ManagedCoverDraft &draft);
    [[nodiscard]] static QString categoryPath(const ManualCoverSettings &settings,
        qint64 categoryId, const QString &token);
    [[nodiscard]] static bool discardCategory(const ManualCoverSettings &settings,
        qint64 categoryId, const QString &token);
    [[nodiscard]] static bool cleanupCategory(const ManualCoverSettings &settings,
        qint64 categoryId, const QString &keepToken = {});
};

} // namespace asmr
