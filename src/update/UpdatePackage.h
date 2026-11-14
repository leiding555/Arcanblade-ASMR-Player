#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>

namespace asmr {
struct UpdatePackageRequest {
    QString archivePath;
    QString expectedVersion;
    QByteArray expectedSha256; // 64 lowercase/uppercase hexadecimal bytes.
    QString workingDirectory; // A new directory, outside the installation.
};
struct UpdatePreparedPackage {
    QString rootDirectory;
    QString version;
    QString error;
    [[nodiscard]] bool success() const { return error.isEmpty() && !rootDirectory.isEmpty(); }
};
using UpdatePackageProgress = std::function<void(qint64, qint64)>;
// Blocking worker functions; do not call on the GUI thread.
[[nodiscard]] UpdatePreparedPackage prepareUpdatePackage(
    const UpdatePackageRequest &request, const std::atomic_bool &cancelled,
    const UpdatePackageProgress &progress = {});
[[nodiscard]] UpdatePreparedPackage validatePreparedUpdatePackage(
    const QString &rootDirectory, const QString &expectedVersion,
    const std::atomic_bool &cancelled, const UpdatePackageProgress &progress = {});

struct UpdateInstallPreparation {
    QString helperPath;
    QString planPath;
    QString sessionDirectory;
    QString error;
    [[nodiscard]] bool success() const { return error.isEmpty() && !planPath.isEmpty(); }
};
// Revalidates the downloaded tree and installed file manifest, copies the
// CURRENT bundled helper outside the program directory, and creates its plan.
[[nodiscard]] UpdateInstallPreparation prepareUpdateInstall(
    const QString &preparedRoot, const QString &targetRoot,
    const QString &currentVersion, const QString &newVersion,
    const QString &sessionDirectory, const std::atomic_bool &cancelled,
    const QByteArray &expectedManifestSha256);
// Starts the helper hidden and waits for its bounded ready handshake. A true
// result means the caller may commit on the GUI thread, then begin its normal
// asynchronous shutdown protocol. Without that explicit commit it never installs.
[[nodiscard]] bool launchPreparedUpdate(const UpdateInstallPreparation &preparation,
                                       const std::atomic_bool &cancelled,
                                       QString *error = nullptr);
[[nodiscard]] bool commitPreparedUpdate(const UpdateInstallPreparation &preparation,
                                       QString *error = nullptr);
void cancelPreparedUpdate(const UpdateInstallPreparation &preparation);
// Call only after the new version's main window has initialized. The marker is
// accepted only for the matching plan/version/executable in the app update cache.
void confirmUpdateStartup(const QStringList &arguments, const QString &currentVersion);
} // namespace asmr
