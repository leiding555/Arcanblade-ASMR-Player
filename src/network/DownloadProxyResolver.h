#pragma once

#include <QFuture>
#include <QNetworkProxy>
#include <QNetworkProxyQuery>

#include <atomic>
#include <functional>
#include <memory>

QT_BEGIN_NAMESPACE
class QNetworkAccessManager;
QT_END_NAMESPACE

namespace asmr {

// Call resolve() on a worker: querying Windows/PAC settings can block. Only a
// local HTTP proxy's existing endpoint is probed; no ports or client files are scanned.
class DownloadProxyResolver final {
public:
    using SystemQuery = std::function<QList<QNetworkProxy>(const QNetworkProxyQuery &)>;
    explicit DownloadProxyResolver(SystemQuery query = {}, int probeTimeoutMs = 1000,
        int cacheLifetimeMs = 60000);
    ~DownloadProxyResolver();

    QList<QNetworkProxy> resolve(const QNetworkProxyQuery &query, const std::atomic_bool &cancelled);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

// For the installers' worker-owned managers; resolves each target, including redirects.
void useAutomaticDownloadProxy(QNetworkAccessManager &manager, const std::atomic_bool &cancelled,
    std::shared_ptr<DownloadProxyResolver> resolver = {});

// For event-driven downloads: resolve on a worker, then apply on the manager's thread.
QFuture<QList<QNetworkProxy>> resolveDownloadProxiesAsync(
    std::shared_ptr<DownloadProxyResolver> resolver, const QUrl &url,
    std::shared_ptr<const std::atomic_bool> cancelled);
void applyDownloadProxies(QNetworkAccessManager &manager, const QList<QNetworkProxy> &proxies);

} // namespace asmr
