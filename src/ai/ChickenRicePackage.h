#pragma once

#include "ai/GpuCompatibility.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>

#include <optional>

namespace asmr {

struct ChickenRicePackagePart {
    QString name;
    QUrl url;
    qint64 size = 0;
    QByteArray sha256; // Exactly 64 lowercase hexadecimal characters.
};

struct ChickenRicePackage {
    ChickenRiceVariant variant = ChickenRiceVariant::Cu118;
    QString version;
    QString archiveName;
    qint64 size = 0;
    QByteArray sha256; // SHA-256 of the reconstructed ZIP, lowercase hex.
    QList<ChickenRicePackagePart> parts; // Original byte order.
    qint64 unpackedSize = 0; // Pinned expanded bytes; zero permits small test fixtures.
};

[[nodiscard]] std::optional<ChickenRicePackage> chickenRicePackage(ChickenRiceVariant variant);
[[nodiscard]] bool validateChickenRicePackage(const ChickenRicePackage &package,
    QString *error = nullptr, bool allowLoopbackHttp = false);

} // namespace asmr
