#include "network/DownloadProxyResolver.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QPromise>
#include <QTcpSocket>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>

namespace asmr {
namespace {
bool localHost(const QString &host)
{
    return host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0
        || QHostAddress(host).isLoopback();
}

bool supportsSocks5(const QNetworkProxy &proxy, int timeoutMs, const std::atomic_bool &cancelled)
{
    if (cancelled.load()) return false;
    QTcpSocket socket;
    socket.setProxy(QNetworkProxy::NoProxy); // The capability probe must not proxy itself.
    socket.setReadBufferSize(2);
    QEventLoop loop;
    QTimer timeout, cancellation;
    QByteArray response;
    bool done = false;
    auto finish = [&] { done = true; loop.quit(); };
    QObject::connect(&socket, &QTcpSocket::connected, &loop, [&] {
        socket.write(QByteArray::fromHex("050100")); // SOCKS5, no authentication. No destination request.
    });
    QObject::connect(&socket, &QTcpSocket::readyRead, &loop, [&] {
        response += socket.read(2 - response.size());
        if (response.size() == 2) finish();
    });
    QObject::connect(&socket, &QTcpSocket::errorOccurred, &loop, finish);
    QObject::connect(&socket, &QTcpSocket::disconnected, &loop, finish);
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, finish);
    QObject::connect(&cancellation, &QTimer::timeout, &loop, [&] {
        if (cancelled.load()) finish();
    });
    timeout.start(timeoutMs);
    cancellation.start(25);
    socket.connectToHost(proxy.hostName(), proxy.port());
    if (!done) loop.exec();
    socket.abort();
    return !cancelled.load() && response == QByteArray::fromHex("0500");
}

class AutomaticProxyFactory final : public QNetworkProxyFactory {
public:
    AutomaticProxyFactory(std::shared_ptr<DownloadProxyResolver> resolver, const std::atomic_bool &cancelled)
        : m_resolver(std::move(resolver)), m_cancelled(cancelled) {}
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery &query) override
    { return m_resolver->resolve(query, m_cancelled); }
private:
    std::shared_ptr<DownloadProxyResolver> m_resolver;
    const std::atomic_bool &m_cancelled;
};

class ResolvedProxyFactory final : public QNetworkProxyFactory {
public:
    explicit ResolvedProxyFactory(QList<QNetworkProxy> proxies) : m_proxies(std::move(proxies)) {}
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery &) override { return m_proxies; }
private:
    QList<QNetworkProxy> m_proxies;
};
} // namespace

struct DownloadProxyResolver::Private {
    SystemQuery query;
    int probeTimeoutMs;
    int cacheLifetimeMs;
    QMutex mutex;
    QList<QNetworkProxy> cachedSystem;
    QElapsedTimer cacheAge;
    bool supported = false;
};

DownloadProxyResolver::DownloadProxyResolver(SystemQuery query, int probeTimeoutMs, int cacheLifetimeMs)
    : d(std::make_unique<Private>())
{
    d->query = query ? std::move(query) : QNetworkProxyFactory::systemProxyForQuery;
    d->probeTimeoutMs = std::clamp(probeTimeoutMs, 1, 5000);
    d->cacheLifetimeMs = std::clamp(cacheLifetimeMs, 0, 60000);
}

DownloadProxyResolver::~DownloadProxyResolver() = default;

QList<QNetworkProxy> DownloadProxyResolver::resolve(const QNetworkProxyQuery &query,
    const std::atomic_bool &cancelled)
{
    const QList<QNetworkProxy> unchanged{QNetworkProxy::DefaultProxy};
    if (cancelled.load()) return unchanged;
    // Local fixtures/resources remain direct, regardless of the machine's proxy settings.
    if (localHost(query.peerHostName())) return {QNetworkProxy::NoProxy};
    QList<QNetworkProxy> proxies;
    try { proxies = d->query(query); }
    catch (...) { return unchanged; }
    if (cancelled.load() || proxies.isEmpty()) return unchanged;

    QMutexLocker lock(&d->mutex);
    if (proxies != d->cachedSystem) {
        d->cachedSystem = proxies;
        d->cacheAge.invalidate();
    }
    const auto original = proxies.first();
    // Preserve remote proxies, authentication and all system fallback/bypass choices.
    if (original.type() != QNetworkProxy::HttpProxy || !localHost(original.hostName())
        || original.port() == 0 || !original.user().isEmpty() || !original.password().isEmpty())
        return proxies;

    if (!d->cacheAge.isValid() || d->cacheAge.elapsed() >= d->cacheLifetimeMs) {
        d->supported = supportsSocks5(original, d->probeTimeoutMs, cancelled);
        if (cancelled.load()) return unchanged; // Cancellation must not cache an unsuccessful probe.
        d->cacheAge.start();
        qInfo("Download proxy: local SOCKS5 %s; %s.", d->supported ? "supported" : "unavailable",
            d->supported ? "using SOCKS5" : "keeping system proxy");
    }
    if (d->supported) {
        auto socks = original;
        socks.setType(QNetworkProxy::Socks5Proxy);
        proxies[0] = socks;
    }
    return proxies;
}

void useAutomaticDownloadProxy(QNetworkAccessManager &manager, const std::atomic_bool &cancelled,
    std::shared_ptr<DownloadProxyResolver> resolver)
{
    if (!resolver) resolver = std::make_shared<DownloadProxyResolver>();
    manager.setProxyFactory(new AutomaticProxyFactory(std::move(resolver), cancelled));
}

QFuture<QList<QNetworkProxy>> resolveDownloadProxiesAsync(std::shared_ptr<DownloadProxyResolver> resolver,
    const QUrl &url, std::shared_ptr<const std::atomic_bool> cancelled)
{
    auto promise = std::make_shared<QPromise<QList<QNetworkProxy>>>();
    auto future = promise->future();
    QThreadPool::globalInstance()->start([resolver = std::move(resolver), url,
        cancelled = std::move(cancelled), promise] {
        promise->start();
        promise->addResult(resolver->resolve(QNetworkProxyQuery(url), *cancelled));
        promise->finish();
    });
    return future;
}

void applyDownloadProxies(QNetworkAccessManager &manager, const QList<QNetworkProxy> &proxies)
{
    manager.setProxyFactory(new ResolvedProxyFactory(proxies));
}
} // namespace asmr
