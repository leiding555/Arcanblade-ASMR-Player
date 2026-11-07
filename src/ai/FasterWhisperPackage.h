#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>

namespace asmr {
struct FasterWhisperDownloadFile {
    QString name;
    QUrl url;
    qint64 size = 0;
    QByteArray sha256; // 64 lowercase hexadecimal characters.
};
struct FasterWhisperPackage {
    QString version;
    FasterWhisperDownloadFile engineArchive;
    qint64 unpackedSize = 0;
    QString modelRevision;
    QList<FasterWhisperDownloadFile> modelFiles;
};
[[nodiscard]] FasterWhisperPackage fasterWhisperPackage();
[[nodiscard]] bool validateFasterWhisperPackage(const FasterWhisperPackage &package,
    QString *error = nullptr, bool allowLoopbackHttp = false);
} // namespace asmr
