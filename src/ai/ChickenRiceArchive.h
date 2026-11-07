#pragma once

#include <QList>
#include <QString>

#include <atomic>

namespace asmr {

struct ChickenRiceArchiveEntry {
    QString path;
    qint64 size = 0;
    bool directory = false;
    quint32 crc32 = 0;
};

struct ChickenRiceArchiveInspection {
    QList<ChickenRiceArchiveEntry> entries;
    qint64 unpackedBytes = 0;
    QString error;
    [[nodiscard]] bool success() const { return error.isEmpty() && !entries.isEmpty(); }
};

// Reads ZIP / ZIP64 metadata and local headers. Does not extract or run code.
[[nodiscard]] ChickenRiceArchiveInspection inspectChickenRiceArchive(
    const QString &path, const std::atomic_bool &cancelled);
// Accepts infer.exe in staging itself or exactly one immediate child directory.
[[nodiscard]] QString findChickenRiceEngineRoot(
    const QString &staging, QString *error = nullptr);
// Requires the fixed v1.10 local engine, main model, VAD and feature files.
[[nodiscard]] bool validateChickenRiceEngine(
    const QString &root, QString *error = nullptr);

} // namespace asmr
