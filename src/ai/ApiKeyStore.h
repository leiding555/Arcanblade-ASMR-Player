#pragma once
#include <QString>

namespace asmr {
[[nodiscard]] QString validateTranslationApiKey(const QString &key);

// Synchronous credential I/O: callers must run these operations off the UI thread.
// A missing credential is a successful read with an empty key; remove is idempotent.
class ApiKeyStore {
public:
    virtual ~ApiKeyStore() = default;
    virtual bool read(const QString &scope, QString *key, QString *error) = 0;
    virtual bool write(const QString &scope, const QString &key, QString *error) = 0;
    virtual bool remove(const QString &scope, QString *error) = 0;
};

class WindowsApiKeyStore final : public ApiKeyStore {
public:
    explicit WindowsApiKeyStore(QString namespacePrefix = QStringLiteral("Arcanblade/TranslationApi"));
    bool read(const QString &scope, QString *key, QString *error = nullptr) override;
    bool write(const QString &scope, const QString &key, QString *error = nullptr) override;
    bool remove(const QString &scope, QString *error = nullptr) override;
private:
    [[nodiscard]] QString target(const QString &scope) const;
    QString m_namespace;
};
} // namespace asmr
