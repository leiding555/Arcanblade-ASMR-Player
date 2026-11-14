#pragma once

#include "update/UpdateCheckService.h"

#include <QObject>

#include <memory>

namespace asmr {

class DownloadProxyResolver;

enum class UpdateDownloadState { Idle, Downloading, Preparing, Ready, Failed, Cancelled };

struct UpdateDownloadResult {
    UpdateDownloadState state = UpdateDownloadState::Idle;
    UpdateRelease release;
    QString preparedDirectory;
    QString packagePath;
    QString message;
    // Binds the prepared tree to the FILES.json extracted from the verified ZIP.
    QByteArray preparedManifestSha256; // 64 lowercase hexadecimal bytes.
};

struct UpdateDownloadOptions {
    // Defaults to the current user's AppLocalDataLocation/updates.
    QString cacheRoot;
    int timeoutMs = 30 * 60 * 1000;
    qint64 maximumPackageBytes = 2LL * 1024 * 1024 * 1024;
    // Test seam: remap trusted release requests to this HTTP loopback origin.
    // No production setting or non-loopback alternate endpoint is accepted.
    QUrl loopbackEndpoint;
    // Test seam for proxy preparation/cancellation; no UI or persistent proxy override.
    std::shared_ptr<DownloadProxyResolver> proxyResolverForTests;
};

// Owns download/preparation across About window lifetimes. Construction never
// reads cache files or sends requests. Hashing/extraction run on a worker.
class UpdateDownloadService : public QObject {
    Q_OBJECT
public:
    explicit UpdateDownloadService(QObject *parent = nullptr);
    explicit UpdateDownloadService(const UpdateDownloadOptions &options, QObject *parent = nullptr);
    ~UpdateDownloadService() override;

    virtual bool start(const UpdateRelease &release);
    // False means no applicable cache exists (or another operation is active).
    // Accepted cache verification completes asynchronously via finished().
    virtual bool restore(const UpdateRelease &release);
    virtual void cancel();
    virtual void shutdown();
    [[nodiscard]] bool isBusy() const;
    [[nodiscard]] UpdateDownloadState state() const { return m_result.state; }
    [[nodiscard]] UpdateDownloadResult result() const { return m_result; }

signals:
    void stateChanged(asmr::UpdateDownloadState state);
    void progressChanged(qint64 completedBytes, qint64 totalBytes);
    void busyChanged(bool busy);
    void finished(asmr::UpdateDownloadResult result);
    void shutdownFinished();

protected:
    // Also permits isolated UI implementations to supply transport results.
    void publishResult(UpdateDownloadResult result);
    void reportProgress(qint64 completedBytes, qint64 totalBytes);

private:
    struct Private;
    std::unique_ptr<Private> d;
    UpdateDownloadResult m_result;
};

} // namespace asmr

Q_DECLARE_METATYPE(asmr::UpdateDownloadState)
Q_DECLARE_METATYPE(asmr::UpdateDownloadResult)
