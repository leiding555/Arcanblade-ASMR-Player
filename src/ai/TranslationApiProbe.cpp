#include "ai/TranslationApiProbe.h"

#include "ai/ApiKeyStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

namespace asmr {
namespace {
using State = TranslationApiProbeState;
using Result = TranslationApiProbeResult;

QString httpFailure(int status)
{
    if (status >= 300 && status < 400)
        return QStringLiteral("服务返回重定向；为保护 API Key，未继续请求。请直接填写最终服务地址。");
    switch (status) {
    case 400: return QStringLiteral("服务拒绝请求格式，请检查兼容接口和模型设置。");
    case 401: return QStringLiteral("API Key 认证失败，请检查或更换密钥。");
    case 402: return QStringLiteral("服务账户余额不足，请检查账户余额。");
    case 403: return QStringLiteral("服务拒绝访问，请检查密钥及模型权限。");
    case 404: return QStringLiteral("未找到接口或模型，请检查服务地址和模型名称。");
    case 408: return QStringLiteral("服务请求超时，请稍后重试。");
    case 422: return QStringLiteral("服务不接受当前参数，请检查模型和兼容服务设置。");
    case 429: return QStringLiteral("服务请求达到限额，请稍后手动重试。");
    default:
        return status >= 500 && status < 600
            ? QStringLiteral("服务暂时不可用，请稍后手动重试。")
            : QStringLiteral("连接测试失败，服务返回 HTTP %1。").arg(status);
    }
}
}

TranslationApiProbe::TranslationApiProbe(QObject *parent)
    : QObject(parent), m_manager(new QNetworkAccessManager(this)), m_timeout(new QTimer(this))
{
    qRegisterMetaType<TranslationApiProbeResult>();
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this] {
        if (m_busy) complete({State::TimedOut, QStringLiteral("连接测试超时，请检查网络或增加超时时间。")});
    });
}

TranslationApiProbe::~TranslationApiProbe()
{
    m_timeout->stop();
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
    }
}

QString TranslationApiProbe::sampleText()
{
    return QStringLiteral("こんにちは。");
}

QByteArray TranslationApiProbe::fixedRequestBody(const TranslationApiSettings &settings)
{
    QJsonObject body{
        {QStringLiteral("model"), settings.model},
        {QStringLiteral("messages"), QJsonArray{
            QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                {QStringLiteral("content"), QStringLiteral("Translate the user's short Japanese greeting into Simplified Chinese. Return only the translation.")}},
            QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), sampleText()}}}},
        {QStringLiteral("stream"), false},
        {QStringLiteral("max_tokens"), translationApiOutputTokenBudget(settings, 128, 2048)}};
    if (settings.provider == TranslationApiProvider::DeepSeek) {
        body.insert(QStringLiteral("thinking"), QJsonObject{{QStringLiteral("type"),
            translationApiThinkingEnabled(settings) ? QStringLiteral("enabled") : QStringLiteral("disabled")}});
        if (translationApiThinkingEnabled(settings)) body.insert(QStringLiteral("reasoning_effort"), QStringLiteral("low"));
    }
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

bool TranslationApiProbe::start(const TranslationApiSettings &settings, const QString &apiKey)
{
    if (m_busy) return false;
    const quint64 generation = ++m_generation;
    m_busy = true;
    m_completing = false;
    m_result = {};
    m_response.clear();
    m_maximumResponseBytes = translationApiThinkingEnabled(settings) ? 256 * 1024 : 64 * 1024;
    m_elapsed.start();
    const QString settingsError = validateTranslationApiSettings(settings);
    const QString keyError = validateTranslationApiKey(apiKey);
    QPointer<TranslationApiProbe> guard(this);
    emit busyChanged(true);
    if (!guard || !m_busy || m_completing || m_generation != generation) return true;
    if (!settingsError.isEmpty() || !keyError.isEmpty()) {
        // Queue even an invalid request so the UI has one completion protocol.
        const QString message = !settingsError.isEmpty() ? settingsError : keyError;
        QTimer::singleShot(0, this, [this, generation, message] {
            if (m_busy && generation == m_generation) complete({State::Failed, message});
        });
        return true;
    }

    QNetworkRequest request(translationChatCompletionsUrl(settings));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
    request.setRawHeader("Accept", "application/json");
    // Never forward credentials, including to a same-origin redirect. TLS
    // errors use Qt's normal failure handling and are never ignored.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    auto *reply = m_manager->post(request, fixedRequestBody(settings));
    m_reply = reply;
    reply->setReadBufferSize(m_maximumResponseBytes + 1);
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply, generation] {
        if (!m_busy || m_reply != reply || generation != m_generation) return;
        bool ok = false;
        const qint64 bytes = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&ok);
        if (ok && bytes > m_maximumResponseBytes)
            complete({State::Failed, QStringLiteral("服务响应过大，已停止连接测试。")});
    });
    connect(reply, &QIODevice::readyRead, this, [this, reply, generation] { readAvailable(reply, generation); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] { replyFinished(reply, generation); });
    // Absolute deadline: whitespace/keep-alives cannot reset this timer.
    m_timeout->start(translationApiRequestTimeoutSeconds(settings) * 1000);
    return true;
}

void TranslationApiProbe::cancel()
{
    if (m_busy) complete({State::Cancelled, QStringLiteral("已取消连接测试。")});
}

void TranslationApiProbe::readAvailable(QNetworkReply *reply, quint64 generation)
{
    if (!m_busy || m_reply != reply || generation != m_generation) return;
    const qsizetype remaining = m_maximumResponseBytes - m_response.size();
    m_response += reply->read(remaining + 1);
    if (m_response.size() > m_maximumResponseBytes)
        complete({State::Failed, QStringLiteral("服务响应过大，已停止连接测试。")});
}

void TranslationApiProbe::replyFinished(QNetworkReply *reply, quint64 generation)
{
    if (!m_busy || m_reply != reply || generation != m_generation) return;
    QPointer<TranslationApiProbe> guard(this);
    readAvailable(reply, generation);
    if (!guard || !m_busy || m_reply != reply || generation != m_generation) return;
    Result result;
    result.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (result.statusCode >= 300) {
        result.message = httpFailure(result.statusCode);
        complete(result); return;
    }
    if (reply->error() != QNetworkReply::NoError || result.statusCode < 200) {
        result.message = QStringLiteral("无法连接服务，请检查网络、代理或证书后重试。");
        complete(result); return;
    }
    const auto document = QJsonDocument::fromJson(m_response);
    const auto choices = document.object().value(QStringLiteral("choices")).toArray();
    const auto first = choices.isEmpty() ? QJsonObject() : choices.first().toObject();
    const auto content = first.value(QStringLiteral("message")).toObject().value(QStringLiteral("content"));
    const auto reason = first.value(QStringLiteral("finish_reason"));
    if (!document.isObject() || !content.isString() || content.toString().trimmed().isEmpty()
        || (!reason.isUndefined() && !reason.isNull()
            && (!reason.isString() || reason.toString() != QStringLiteral("stop")))) {
        result.message = reason.toString() == QStringLiteral("length")
            ? QStringLiteral("测试响应被截断，请检查模型或兼容接口。")
            : reason.toString() == QStringLiteral("content_filter")
            ? QStringLiteral("服务未返回测试译文，请检查服务限制。")
            : QStringLiteral("服务未返回有效的 Chat Completions 文本响应，请检查接口和模型。");
        complete(result); return;
    }
    const auto usage = document.object().value(QStringLiteral("usage")).toObject();
    result.promptTokens = usage.value(QStringLiteral("prompt_tokens")).toInteger(-1);
    result.completionTokens = usage.value(QStringLiteral("completion_tokens")).toInteger(-1);
    if (result.promptTokens < 0) result.promptTokens = -1;
    if (result.completionTokens < 0) result.completionTokens = -1;
    result.state = State::Succeeded;
    result.message = QStringLiteral("连接测试成功，服务已返回有效文本。");
    complete(result);
}

void TranslationApiProbe::complete(TranslationApiProbeResult result)
{
    if (!m_busy || m_completing) return;
    // Completing can be entered from Qt's transport readyRead/metadata stack.
    // A receiver may destroy this probe from finished(), so never notify it
    // until that transport stack has returned to the event loop.
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
    result.elapsedMs = m_elapsed.elapsed();
    m_result = result;
    const quint64 generation = m_generation;
    QTimer::singleShot(0, this, [this, generation, result] {
        if (!m_busy || !m_completing || generation != m_generation) return;
        m_completing = false;
        m_busy = false;
        QPointer<TranslationApiProbe> guard(this);
        emit finished(result);
        if (guard && !m_busy && generation == m_generation) emit busyChanged(false);
    });
}

} // namespace asmr
