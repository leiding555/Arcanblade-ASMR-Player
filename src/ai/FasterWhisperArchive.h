#pragma once

#include "ai/FasterWhisperPackage.h"
#include <QList>
#include <QString>
#include <atomic>
#include <functional>

namespace asmr {
struct FasterWhisperArchiveEntry {
    QString path;
    qint64 size = 0;
    bool directory = false;
};
struct FasterWhisperArchiveInspection {
    QList<FasterWhisperArchiveEntry> entries;
    qint64 unpackedBytes = 0;
    QString error;
    [[nodiscard]] bool success() const { return error.isEmpty() && !entries.isEmpty(); }
};
// The packaged, pinned standalone 7-Zip. Never search PATH or the registry.
[[nodiscard]] QString fasterWhisperArchiveToolPath();
[[nodiscard]] bool validateFasterWhisperArchiveTool(const QString &path, QString *error = nullptr);
// Inspect a verified 7z without extracting or executing its contents.
[[nodiscard]] FasterWhisperArchiveInspection inspectFasterWhisperArchive(
    const QString &path, const std::atomic_bool &cancelled,
    qint64 maximumUnpackedBytes = 16LL * 1024 * 1024 * 1024);
// The caller checks test overrides; production always uses the pinned tool above.
[[nodiscard]] QString extractFasterWhisperArchive(const QString &program, const QString &archive,
    const QString &staging, const std::atomic_bool &cancelled);
// Reject reparse points, undeclared files and size mismatches after extraction.
[[nodiscard]] bool validateFasterWhisperExtracted(const QString &staging,
    const FasterWhisperArchiveInspection &inspection, const std::atomic_bool &cancelled,
    QString *error = nullptr);
[[nodiscard]] QString findFasterWhisperEngineRoot(const QString &staging,
    QString *error = nullptr);
[[nodiscard]] bool validateFasterWhisperEngine(const QString &root,
    QString *error = nullptr);
// root is the model leaf, not _models. Run hashing away from the GUI thread.
[[nodiscard]] bool validateFasterWhisperModel(const QString &root,
    const QList<FasterWhisperDownloadFile> &files, const std::atomic_bool &cancelled,
    QString *error = nullptr,
    const std::function<void(qint64, qint64)> &progress = {});
} // namespace asmr
