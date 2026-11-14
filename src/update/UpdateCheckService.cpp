#include "update/UpdateCheckService.h"

#include "update/ApplicationVersion.h"
#include "update/SemanticVersion.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>

namespace asmr {
namespace {
using State = UpdateCheckState;
using Result = UpdateCheckResult;
const QString Repository = QStringLiteral("/leiding555/AsmrPlayer-Arcanblade");
const QString ApiPath = QStringLiteral("/repos") + Repository + QStringLiteral("/releases");

QUrl defaultEndpoint()
{
    return QUrl(QStringLiteral("https://api.github.com") + ApiPath + QStringLiteral("?per_page=100"));
}

bool noUrlExtras(const QUrl &url)
{
    return url.isValid() && !url.isRelative() && url.userInfo().isEmpty() && !url.hasFragment();
}

bool loopback(const QUrl &url)
{
    if (!noUrlExtras(url) || url.scheme() != QStringLiteral("http")) return false;
    QHostAddress address;
    return url.host().compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0
        || (address.setAddress(url.host()) && address.isLoopback());
}

bool pageQuery(const QUrl &url)
{
    QSet<QString> seen;
    const auto items = QUrlQuery(url).queryItems(QUrl::FullyDecoded);
    for (const auto &item : items) {
        if (seen.contains(item.first)
            || (item.first != QStringLiteral("page") && item.first != QStringLiteral("per_page")))
            return false;
        seen.insert(item.first);
        bool ok = false;
        const int value = item.second.toInt(&ok);
        if (!ok || value <= 0 || value > (item.first == QStringLiteral("per_page") ? 100 : 1000000))
            return false;
    }
    return true;
}

bool allowedEndpoint(const QUrl &url)
{
    if (!noUrlExtras(url) || !pageQuery(url)) return false;
    if (loopback(url)) return !url.path().isEmpty();
    return url.scheme() == QStringLiteral("https")
        && url.host() == QStringLiteral("api.github.com")
        && (url.port() == -1 || url.port() == 443) && url.path(QUrl::FullyDecoded) == ApiPath;
}

bool samePageEndpoint(const QUrl &url, const QUrl &initial)
{
    return allowedEndpoint(url) && url.scheme() == initial.scheme() && url.host() == initial.host()
        && url.port() == initial.port()
        && url.path(QUrl::FullyDecoded) == initial.path(QUrl::FullyDecoded);
}

Result failure(const QString &message)
{
    Result result;
    result.message = message;
    return result;
}

QDateTime retryTime(QNetworkReply *reply)
{
    const auto now = QDateTime::currentDateTimeUtc();
    const auto raw = reply->rawHeader("Retry-After").trimmed();
    QDateTime retry;
    bool ok = false;
    const auto seconds = raw.toLongLong(&ok);
    if (ok && seconds >= 0 && seconds <= 7 * 24 * 60 * 60) retry = now.addSecs(seconds);
    if (!retry.isValid() && !raw.isEmpty()) retry = QDateTime::fromString(QString::fromLatin1(raw), Qt::RFC2822Date).toUTC();
    if (!retry.isValid()) {
        const auto reset = reply->rawHeader("X-RateLimit-Reset").trimmed().toLongLong(&ok);
        if (ok && reset > 0) retry = QDateTime::fromSecsSinceEpoch(reset).toUTC();
    }
    if (!retry.isValid() || retry < now || retry > now.addDays(7)) return {};
    return retry;
}
} // namespace

bool isAllowedReleasePageUrl(const QUrl &url)
{
    if (!noUrlExtras(url) || url.scheme() != QStringLiteral("https")
        || url.host() != QStringLiteral("github.com") || url.hasQuery()
        || (url.port() != -1 && url.port() != 443)) return false;
    const QString prefix = Repository + QStringLiteral("/releases/tag/");
    const auto path = url.path(QUrl::FullyDecoded);
    return path.startsWith(prefix)
        && SemanticVersion::parse(path.mid(prefix.size()), true).has_value();
}

bool isDownloadableUpdateRelease(const UpdateRelease &release)
{
    const auto version = SemanticVersion::parse(release.version);
    if (!version || !isAllowedReleasePageUrl(release.pageUrl)
        || release.packageSize <= 0 || release.packageSize > 2LL * 1024 * 1024 * 1024)
        return false;
    const auto tag = release.pageUrl.path(QUrl::FullyDecoded).section(QLatin1Char('/'), -1);
    const auto tagged = SemanticVersion::parse(tag, true);
    if (!tagged || tagged->toString() != release.version) return false;
    const QString name = QStringLiteral("Arcanblade-") + release.version
        + QStringLiteral("-windows-x64.zip");
    const auto allowed = [&](const QUrl &url, const QString &file) {
        return noUrlExtras(url) && url.scheme() == QStringLiteral("https")
            && url.host() == QStringLiteral("github.com") && !url.hasQuery()
            && (url.port() == -1 || url.port() == 443)
            && url.path(QUrl::FullyDecoded) == Repository + QStringLiteral("/releases/download/")
                + tag + QLatin1Char('/') + file;
    };
    static const QRegularExpression digest(QStringLiteral("^[0-9a-fA-F]{64}$"));
    return allowed(release.packageUrl, name)
        && allowed(release.checksumUrl, name + QStringLiteral(".sha256"))
        && (release.packageSha256.isEmpty() || digest.match(release.packageSha256).hasMatch());
}

UpdateCheckResult selectUpdateRelease(const QByteArray &json, const QString &currentVersion,
                                      bool includePrereleases)
{
    const auto current = SemanticVersion::parse(currentVersion);
    if (!current) return failure(QStringLiteral("当前程序版本无效，无法检查更新。"));
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray())
        return failure(QStringLiteral("GitHub 返回的版本列表格式无效，请稍后重试。"));
    Result result;
    result.state = State::UpToDate;
    result.message = QStringLiteral("当前已是所选更新通道的最新版本。");
    auto best = *current;
    for (const auto &value : document.array()) {
        if (!value.isObject()) continue;
        const auto object = value.toObject();
        const auto draft = object.value(QStringLiteral("draft"));
        const auto preview = object.value(QStringLiteral("prerelease"));
        if (!draft.isBool() || !preview.isBool() || draft.toBool()) continue;
        const auto tagValue = object.value(QStringLiteral("tag_name"));
        if (!tagValue.isString()) continue;
        const auto tag = tagValue.toString();
        const auto version = SemanticVersion::parse(tag, true);
        if (!version || SemanticVersion::compare(*version, best) <= 0) continue;
        const bool prerelease = preview.toBool() || version->isPrerelease();
        if (!includePrereleases && prerelease) continue;
        const QUrl pageUrl(object.value(QStringLiteral("html_url")).toString(), QUrl::StrictMode);
        if (!isAllowedReleasePageUrl(pageUrl)
            || pageUrl.path(QUrl::FullyDecoded) != Repository + QStringLiteral("/releases/tag/") + tag)
            continue;
        const QString expectedName = QStringLiteral("Arcanblade-") + version->toString()
            + QStringLiteral("-windows-x64.zip");
        bool hasPackage = false;
        QJsonObject package;
        QJsonObject checksum;
        int packageCount = 0;
        int checksumCount = 0;
        for (const auto &assetValue : object.value(QStringLiteral("assets")).toArray()) {
            const auto asset = assetValue.toObject();
            if (asset.value(QStringLiteral("name")).toString() == expectedName
                && asset.value(QStringLiteral("state")).toString() == QStringLiteral("uploaded")
                && asset.value(QStringLiteral("size")).toDouble() > 0) {
                hasPackage = true;
                package = asset;
                ++packageCount;
            }
            if (asset.value(QStringLiteral("name")).toString() == expectedName + QStringLiteral(".sha256")
                && asset.value(QStringLiteral("state")).toString() == QStringLiteral("uploaded")
                && asset.value(QStringLiteral("size")).toDouble() > 0
                && asset.value(QStringLiteral("size")).toDouble() <= 65536) {
                checksum = asset;
                ++checksumCount;
            }
        }
        if (!hasPackage) continue;
        best = *version;
        result.state = State::UpdateAvailable;
        result.message = QStringLiteral("发现新版本 %1。").arg(version->toString());
        result.release.version = version->toString();
        result.release.title = object.value(QStringLiteral("name")).toString().trimmed();
        if (result.release.title.isEmpty()) result.release.title = tag;
        result.release.notes = object.value(QStringLiteral("body")).toString();
        result.release.pageUrl = pageUrl;
        result.release.prerelease = prerelease;
        result.release.packageUrl = {};
        result.release.checksumUrl = {};
        result.release.packageSize = 0;
        result.release.packageSha256.clear();
        const double packageSize = package.value(QStringLiteral("size")).toDouble();
        const QString digest = package.value(QStringLiteral("digest")).toString();
        static const QRegularExpression assetDigest(QStringLiteral("^sha256:[0-9a-fA-F]{64}$"));
        if (packageCount == 1 && checksumCount == 1 && std::isfinite(packageSize)
            && packageSize == std::floor(packageSize) && packageSize <= 2.0 * 1024 * 1024 * 1024
            && (digest.isEmpty() || assetDigest.match(digest).hasMatch())) {
            result.release.packageUrl = QUrl(package.value(QStringLiteral("browser_download_url")).toString(), QUrl::StrictMode);
            result.release.checksumUrl = QUrl(checksum.value(QStringLiteral("browser_download_url")).toString(), QUrl::StrictMode);
            result.release.packageSize = qint64(packageSize);
            result.release.packageSha256 = digest.mid(7).toLower();
            if (!isDownloadableUpdateRelease(result.release)) {
                result.release.packageUrl = {};
                result.release.checksumUrl = {};
                result.release.packageSize = 0;
                result.release.packageSha256.clear();
            }
        }
    }
    return result;
}

UpdateCheckService::UpdateCheckService(QObject *parent)
    : UpdateCheckService(UpdateCheckOptions{}, parent)
{
}

UpdateCheckService::UpdateCheckService(const UpdateCheckOptions &options, QObject *parent)
    : QObject(parent), m_options(options), m_manager(new QNetworkAccessManager(this)),
      m_timeout(new QTimer(this))
{
    if (m_options.endpoint.isEmpty()) m_options.endpoint = defaultEndpoint();
    if (loopback(m_options.endpoint)) m_manager->setProxy(QNetworkProxy::NoProxy);
    qRegisterMetaType<UpdateCheckResult>();
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this] {
        if (m_busy) complete(failure(QStringLiteral("检查更新超时，请检查网络或代理后重试。")));
    });
}

UpdateCheckService::~UpdateCheckService()
{
    m_timeout->stop();
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
    }
}

bool UpdateCheckService::start(const QString &currentVersion, bool includePrereleases)
{
    if (m_busy) return false;
    const auto generation = ++m_generation;
    m_busy = true;
    m_completing = false;
    m_result = {};
    m_best = {};
    m_best.state = State::UpToDate;
    m_best.message = QStringLiteral("当前已是所选更新通道的最新版本。");
    m_response.clear();
    m_seenPages.clear();
    m_totalResponseBytes = 0;
    m_previousPageBytes = 0;
    m_pageCount = 0;
    m_currentVersion = currentVersion;
    m_includePrereleases = includePrereleases;
    QPointer<UpdateCheckService> guard(this);
    emit busyChanged(true);
    if (!guard || !m_busy || m_completing || generation != m_generation) return true;
    QString error;
    if (!SemanticVersion::parse(currentVersion))
        error = QStringLiteral("当前程序版本无效，无法检查更新。");
    else if (!allowedEndpoint(m_options.endpoint) || m_options.timeoutMs <= 0
        || m_options.timeoutMs > 120000 || m_options.maximumResponseBytes <= 0
        || m_options.maximumResponseBytes > 16 * 1024 * 1024
        || m_options.maximumPages <= 0 || m_options.maximumPages > 20)
        error = QStringLiteral("更新检查配置无效，未发送网络请求。");
    if (!error.isEmpty()) {
        QTimer::singleShot(0, this, [this, generation, error] {
            if (m_busy && !m_completing && generation == m_generation) complete(failure(error));
        });
        return true;
    }
    m_timeout->start(m_options.timeoutMs);
    requestPage(m_options.endpoint, generation);
    return true;
}

void UpdateCheckService::cancel()
{
    if (m_busy && !m_completing) {
        Result result;
        result.state = State::Cancelled;
        result.message = QStringLiteral("已取消检查更新。");
        complete(result);
    }
}

void UpdateCheckService::requestPage(const QUrl &url, quint64 generation)
{
    if (!m_busy || m_completing || generation != m_generation) return;
    const QString identity = url.toString(QUrl::FullyEncoded);
    if (m_seenPages.contains(identity) || m_pageCount >= m_options.maximumPages) {
        complete(failure(QStringLiteral("GitHub 版本列表分页异常，请直接查看发行页面。")));
        return;
    }
    m_seenPages.insert(identity);
    ++m_pageCount;
    m_previousPageBytes = m_totalResponseBytes;
    m_response.clear();
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", QByteArray("Arcanblade/") + applicationBuildVersion().toLatin1());
    request.setRawHeader("X-GitHub-Api-Version", "2026-03-10");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    auto *reply = m_manager->get(request);
    m_reply = reply;
    reply->setReadBufferSize(m_options.maximumResponseBytes - m_totalResponseBytes + 1);
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply, generation] {
        if (!m_busy || m_completing || m_reply != reply || generation != m_generation) return;
        bool ok = false;
        const qint64 length = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&ok);
        if (ok && length > m_options.maximumResponseBytes - m_previousPageBytes)
            complete(failure(QStringLiteral("GitHub 版本列表过大，已停止检查更新。")));
    });
    connect(reply, &QIODevice::readyRead, this,
        [this, reply, generation] { readAvailable(reply, generation); });
    connect(reply, &QNetworkReply::finished, this,
        [this, reply, generation] { replyFinished(reply, generation); });
}

void UpdateCheckService::readAvailable(QNetworkReply *reply, quint64 generation)
{
    if (!m_busy || m_completing || m_reply != reply || generation != m_generation) return;
    const auto remaining = m_options.maximumResponseBytes - m_totalResponseBytes;
    const auto bytes = reply->read(remaining + 1);
    m_response += bytes;
    m_totalResponseBytes += bytes.size();
    if (m_totalResponseBytes > m_options.maximumResponseBytes)
        complete(failure(QStringLiteral("GitHub 版本列表过大，已停止检查更新。")));
}

void UpdateCheckService::replyFinished(QNetworkReply *reply, quint64 generation)
{
    if (!m_busy || m_completing || m_reply != reply || generation != m_generation) return;
    QPointer<UpdateCheckService> guard(this);
    readAvailable(reply, generation);
    if (!guard || !m_busy || m_completing || m_reply != reply || generation != m_generation) return;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 403 || status == 429) {
        auto result = failure(status == 429
            ? QStringLiteral("GitHub 请求达到限额，请稍后再试。")
            : QStringLiteral("GitHub 拒绝访问或请求达到限额，请稍后再试。"));
        result.retryAfterUtc = retryTime(reply);
        if (!result.retryAfterUtc.isValid() && status == 429)
            result.retryAfterUtc = QDateTime::currentDateTimeUtc().addSecs(60);
        complete(result);
        return;
    }
    if (status >= 300 && status < 400) {
        complete(failure(QStringLiteral("更新地址发生重定向，请直接查看 GitHub 发行页面。")));
        return;
    }
    if (reply->error() != QNetworkReply::NoError || status != 200) {
        complete(failure(QStringLiteral("无法检查更新，请检查网络、代理或证书后重试。")));
        return;
    }
    auto result = selectUpdateRelease(m_response, m_currentVersion, m_includePrereleases);
    if (result.state == State::Failed) {
        complete(result);
        return;
    }
    if (result.state == State::UpdateAvailable
        && (m_best.state != State::UpdateAvailable || SemanticVersion::compare(
                *SemanticVersion::parse(result.release.version),
                *SemanticVersion::parse(m_best.release.version)) > 0))
        m_best = result;
    QUrl next;
    static const QRegularExpression linkExpression(
        QStringLiteral(R"link(<([^<>]+)>\s*;\s*rel\s*=\s*"([^"]+)")link"),
        QRegularExpression::CaseInsensitiveOption);
    auto links = linkExpression.globalMatch(QString::fromLatin1(reply->rawHeader("Link")));
    while (links.hasNext()) {
        const auto link = links.next();
        if (!link.captured(2).split(QLatin1Char(' '), Qt::SkipEmptyParts).contains(QStringLiteral("next")))
            continue;
        if (!next.isEmpty()) {
            complete(failure(QStringLiteral("GitHub 版本列表分页异常，请直接查看发行页面。")));
            return;
        }
        next = reply->url().resolved(QUrl(link.captured(1), QUrl::StrictMode));
        if (!samePageEndpoint(next, m_options.endpoint)) {
            complete(failure(QStringLiteral("GitHub 返回了无效的分页地址，已停止检查更新。")));
            return;
        }
    }
    m_reply.clear();
    disconnect(reply, nullptr, this, nullptr);
    reply->deleteLater();
    if (!next.isEmpty()) {
        // Leave the transport's finished stack before starting another request.
        QTimer::singleShot(0, this, [this, next, generation] { requestPage(next, generation); });
    } else {
        complete(m_best);
    }
}

void UpdateCheckService::complete(UpdateCheckResult result)
{
    if (!m_busy || m_completing) return;
    m_completing = true;
    m_timeout->stop();
    if (m_reply) {
        auto *reply = m_reply.data();
        m_reply.clear();
        disconnect(reply, nullptr, this, nullptr);
        if (!reply->isFinished()) reply->abort();
        reply->deleteLater();
    }
    m_response.clear();
    m_result = result;
    const auto generation = m_generation;
    QTimer::singleShot(0, this, [this, generation, result] {
        if (!m_busy || !m_completing || generation != m_generation) return;
        m_busy = false;
        m_completing = false;
        QPointer<UpdateCheckService> guard(this);
        emit finished(result);
        if (guard && !m_busy && generation == m_generation) emit busyChanged(false);
    });
}
} // namespace asmr
