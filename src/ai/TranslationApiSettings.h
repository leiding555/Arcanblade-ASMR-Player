#pragma once
#include <QString>
#include <QUrl>

class QSettings;

namespace asmr {
enum class TranslationApiProvider { DeepSeek, OpenAiCompatible };
struct TranslationApiSettings {
    TranslationApiProvider provider = TranslationApiProvider::DeepSeek;
    QString baseUrl = QStringLiteral("https://api.deepseek.com");
    QString model = QStringLiteral("deepseek-flash");
    int timeoutSeconds = 60;
    bool thinkingEnabled = false;
};
[[nodiscard]] QString translationApiProviderKey(TranslationApiProvider provider);
[[nodiscard]] QString translationApiModelDisplayName(const QString &model);
// Only the official provider accepts DeepSeek-specific thinking parameters.
[[nodiscard]] bool translationApiThinkingEnabled(const TranslationApiSettings &settings);
[[nodiscard]] int translationApiRequestTimeoutSeconds(const TranslationApiSettings &settings);
// Both text and reasoning share the API completion budget. All budgets stay bounded.
[[nodiscard]] int translationApiOutputTokenBudget(const TranslationApiSettings &settings,
    int textTokenBudget, int thinkingTokenBudget = 8192);
// Known model context capacity (input + output), or -1 when unavailable.
[[nodiscard]] qint64 translationApiContextTokenLimit(const TranslationApiSettings &settings);
[[nodiscard]] TranslationApiSettings defaultTranslationApiSettings(TranslationApiProvider provider);
[[nodiscard]] QString validateTranslationApiSettings(const TranslationApiSettings &settings);
// A root or API prefix (e.g. /v1) gains /chat/completions. An existing
// /chat/completions suffix is accepted without duplication.
[[nodiscard]] QUrl translationChatCompletionsUrl(const TranslationApiSettings &settings);
// Credentials are scoped to provider + normalized endpoint, not model/timeout.
[[nodiscard]] QString translationCredentialScope(const TranslationApiSettings &settings);
[[nodiscard]] TranslationApiSettings readTranslationApiSettings(const QSettings &settings,
    TranslationApiProvider provider);
[[nodiscard]] bool writeTranslationApiSettings(QSettings &settings,
    const TranslationApiSettings &value, QString *error = nullptr);
[[nodiscard]] TranslationApiProvider readSelectedTranslationApiProvider(const QSettings &settings);
[[nodiscard]] bool writeSelectedTranslationApiProvider(QSettings &settings,
    TranslationApiProvider provider, QString *error = nullptr);
} // namespace asmr
