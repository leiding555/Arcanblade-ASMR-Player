#include "ai/TranslationApiSettings.h"

#include <QList>
#include <QPair>
#include <QRegularExpression>
#include <QSettings>
#include <QVariant>
#include <algorithm>

namespace asmr {
namespace {
const QString Prefix = QStringLiteral("Ai/TranslationApi/");

bool hasControls(const QString &text)
{
    for (const auto ch : text)
        if (ch.category() == QChar::Other_Control || ch == QChar::LineSeparator
            || ch == QChar::ParagraphSeparator) return true;
    return false;
}

bool validModel(const QString &model)
{
    if (model.isEmpty() || model.size() > 256) return false;
    for (const auto ch : model)
        if (ch.isSpace() || ch.category() == QChar::Other_Control) return false;
    return true;
}

QUrl endpoint(const TranslationApiSettings &settings)
{
    const auto &text = settings.baseUrl;
    if (translationApiProviderKey(settings.provider).isEmpty() || text.isEmpty()
        || text.size() > 2048 || text != text.trimmed() || hasControls(text)
        || text.contains(QLatin1Char('\\'))) return {};
    QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || url.isRelative() || url.host().isEmpty()
        || !url.userInfo().isEmpty() || url.authority().contains(QLatin1Char('@'))
        || url.hasQuery() || url.hasFragment()) return {};
    const auto scheme = url.scheme().toLower();
    const auto host = url.host().toLower();
    if (scheme != QStringLiteral("https")) {
        if (scheme != QStringLiteral("http") || (host != QStringLiteral("localhost")
            && host != QStringLiteral("127.0.0.1") && host != QStringLiteral("::1"))) return {};
    }
    QString path = url.path(QUrl::FullyDecoded);
    if (path.size() > 1024 || hasControls(path) || path.contains(QLatin1Char('\\'))) return {};
    const auto segments = path.split(QLatin1Char('/'));
    if (segments.contains(QStringLiteral(".")) || segments.contains(QStringLiteral(".."))) return {};
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    if (!path.endsWith(QStringLiteral("/chat/completions"))) path += QStringLiteral("/chat/completions");
    if (settings.provider == TranslationApiProvider::DeepSeek
        && (scheme != QStringLiteral("https") || host != QStringLiteral("api.deepseek.com")
            || (url.port(-1) != -1 && url.port() != 443)
            || (path != QStringLiteral("/chat/completions") && path != QStringLiteral("/v1/chat/completions")))) return {};
    url.setScheme(scheme);
    url.setHost(host);
    if ((scheme == QStringLiteral("https") && url.port(-1) == 443)
        || (scheme == QStringLiteral("http") && url.port(-1) == 80)) url.setPort(-1);
    url.setPath(path);
    return url;
}

QString profilePrefix(TranslationApiProvider provider)
{
    return Prefix + QStringLiteral("profiles/") + translationApiProviderKey(provider) + QLatin1Char('/');
}

bool store(QSettings &settings, const QList<QPair<QString, QVariant>> &values, QString *error)
{
    if (error) error->clear();
    if (!settings.isWritable() || settings.status() != QSettings::NoError) {
        if (error) *error = QStringLiteral("无法保存翻译服务设置，请检查设置位置的写入权限。");
        return false;
    }
    QList<QPair<QString, QVariant>> previous;
    for (const auto &[name, value] : values) {
        previous.append({name, settings.value(name)});
        settings.setValue(name, value);
    }
    settings.sync();
    if (settings.status() == QSettings::NoError) return true;
    // Also restore this QSettings object's view when persistence fails.
    for (const auto &[name, value] : previous) {
        if (value.isValid()) settings.setValue(name, value);
        else settings.remove(name);
    }
    settings.sync();
    if (error) *error = QStringLiteral("翻译服务设置未能保存，请检查设置位置的写入权限。");
    return false;
}
} // namespace

QString translationApiProviderKey(TranslationApiProvider provider)
{
    switch (provider) {
    case TranslationApiProvider::DeepSeek: return QStringLiteral("deepseek");
    case TranslationApiProvider::OpenAiCompatible: return QStringLiteral("openai-compatible");
    }
    return {};
}

QString translationApiModelDisplayName(const QString &model)
{
    if (model == QStringLiteral("deepseek-flash")) return QStringLiteral("DeepSeek Flash");
    if (model == QStringLiteral("deepseek-v4-pro")) return QStringLiteral("DeepSeek V4 Pro");
    return model;
}

bool translationApiThinkingEnabled(const TranslationApiSettings &settings)
{
    return settings.provider == TranslationApiProvider::DeepSeek && settings.thinkingEnabled;
}

int translationApiRequestTimeoutSeconds(const TranslationApiSettings &settings)
{
    const int configured = std::clamp(settings.timeoutSeconds, 1, 600);
    return translationApiThinkingEnabled(settings) ? std::max(configured, 180) : configured;
}

int translationApiOutputTokenBudget(const TranslationApiSettings &settings,
    int textTokenBudget, int thinkingTokenBudget)
{
    const int text = std::clamp(textTokenBudget, 1, 8192);
    return translationApiThinkingEnabled(settings)
        ? text + std::clamp(thinkingTokenBudget, 0, 8192) : text;
}

qint64 translationApiContextTokenLimit(const TranslationApiSettings &settings)
{
    // Official /models metadata: https://api-docs.deepseek.com/zh-cn/api/list-models/
    if (settings.provider == TranslationApiProvider::DeepSeek
        && (settings.model == QStringLiteral("deepseek-flash")
            || settings.model == QStringLiteral("deepseek-v4-pro"))) return 1048576;
    return -1;
}

TranslationApiSettings defaultTranslationApiSettings(TranslationApiProvider provider)
{
    TranslationApiSettings result;
    result.provider = provider;
    if (provider == TranslationApiProvider::OpenAiCompatible) {
        result.baseUrl.clear();
        result.model.clear();
    }
    return result;
}

QString validateTranslationApiSettings(const TranslationApiSettings &settings)
{
    if (translationApiProviderKey(settings.provider).isEmpty()) return QStringLiteral("请选择有效的翻译服务。");
    if (endpoint(settings).isEmpty()) return settings.provider == TranslationApiProvider::DeepSeek
        ? QStringLiteral("DeepSeek 需使用官方 HTTPS 服务地址。")
        : QStringLiteral("请输入有效的 HTTPS 服务地址；仅本机 localhost、127.0.0.1 或 ::1 可使用 HTTP，地址不能含账号、查询参数或片段。");
    if (!validModel(settings.model)) return QStringLiteral("请输入有效的模型名称（不能包含空白或控制字符）。");
    if (settings.timeoutSeconds < 1 || settings.timeoutSeconds > 600)
        return QStringLiteral("超时需设置为 1 到 600 秒。");
    return {};
}

QUrl translationChatCompletionsUrl(const TranslationApiSettings &settings)
{
    return endpoint(settings);
}

QString translationCredentialScope(const TranslationApiSettings &settings)
{
    const auto url = endpoint(settings);
    if (url.isEmpty()) return {};
    return translationApiProviderKey(settings.provider) + QLatin1Char('|') + url.toString(QUrl::FullyEncoded);
}

TranslationApiSettings readTranslationApiSettings(const QSettings &settings, TranslationApiProvider provider)
{
    if (translationApiProviderKey(provider).isEmpty()) provider = TranslationApiProvider::DeepSeek;
    auto result = defaultTranslationApiSettings(provider);
    const auto prefix = profilePrefix(provider);
    const auto base = settings.value(prefix + QStringLiteral("baseUrl"));
    if (base.metaType().id() == QMetaType::QString) {
        auto candidate = result; candidate.baseUrl = base.toString();
        if (!endpoint(candidate).isEmpty()) result.baseUrl = candidate.baseUrl;
    }
    const auto model = settings.value(prefix + QStringLiteral("model"));
    if (model.metaType().id() == QMetaType::QString && validModel(model.toString())) result.model = model.toString();
    const auto timeout = settings.value(prefix + QStringLiteral("timeoutSeconds"));
    const auto type = timeout.metaType().id();
    const bool integerType = type == QMetaType::Int || type == QMetaType::UInt
        || type == QMetaType::LongLong || type == QMetaType::ULongLong;
    static const QRegularExpression Digits(QStringLiteral("^[0-9]+$"));
    if (integerType || (type == QMetaType::QString && Digits.match(timeout.toString()).hasMatch())) {
        bool ok = false; const int seconds = timeout.toInt(&ok);
        if (ok && seconds >= 1 && seconds <= 600) result.timeoutSeconds = seconds;
    }
    const auto thinking = settings.value(prefix + QStringLiteral("thinkingEnabled"));
    if (provider == TranslationApiProvider::DeepSeek) {
        // QSettings INI reloads booleans as strings. Reject arbitrary truthy values.
        if (thinking.metaType().id() == QMetaType::Bool) result.thinkingEnabled = thinking.toBool();
        else if (thinking.metaType().id() == QMetaType::QString)
            result.thinkingEnabled = thinking.toString() == QStringLiteral("true");
    }
    return result;
}

bool writeTranslationApiSettings(QSettings &settings, const TranslationApiSettings &value, QString *error)
{
    const auto issue = validateTranslationApiSettings(value);
    if (!issue.isEmpty()) { if (error) *error = issue; return false; }
    const auto prefix = profilePrefix(value.provider);
    return store(settings, {{prefix + QStringLiteral("baseUrl"), value.baseUrl},
        {prefix + QStringLiteral("model"), value.model}, {prefix + QStringLiteral("timeoutSeconds"), value.timeoutSeconds},
        {prefix + QStringLiteral("thinkingEnabled"), translationApiThinkingEnabled(value)}}, error);
}

TranslationApiProvider readSelectedTranslationApiProvider(const QSettings &settings)
{
    const auto value = settings.value(Prefix + QStringLiteral("selectedProvider"));
    return value.metaType().id() == QMetaType::QString && value.toString() == QStringLiteral("openai-compatible")
        ? TranslationApiProvider::OpenAiCompatible : TranslationApiProvider::DeepSeek;
}

bool writeSelectedTranslationApiProvider(QSettings &settings, TranslationApiProvider provider, QString *error)
{
    const auto key = translationApiProviderKey(provider);
    if (key.isEmpty()) { if (error) *error = QStringLiteral("请选择有效的翻译服务。"); return false; }
    return store(settings, {{Prefix + QStringLiteral("selectedProvider"), key}}, error);
}
} // namespace asmr
