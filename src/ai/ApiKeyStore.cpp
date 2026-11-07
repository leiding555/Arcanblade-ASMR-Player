#include "ai/ApiKeyStore.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <utility>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <wincred.h>
#endif

namespace asmr {
namespace {
bool plain(const QString &text)
{
    if (text.isEmpty() || text != text.trimmed()) return false;
    for (const auto ch : text)
        if (ch.category() == QChar::Other_Control || ch == QChar::LineSeparator
            || ch == QChar::ParagraphSeparator) return false;
    return true;
}

bool failed(QString *error, const QString &text)
{
    if (error) *error = text;
    return false;
}

#ifdef Q_OS_WIN
QString systemIssue(DWORD code)
{
    // Native error numbers are sufficient for diagnostics; never include the
    // credential blob, target URL, or system messages that could echo input.
    return QStringLiteral("Windows 凭据存储不可用（错误 %1），未使用明文保存 API Key。").arg(code);
}
#endif
} // namespace

QString validateTranslationApiKey(const QString &key)
{
    if (!plain(key) || key.toUtf8().size() > 2560)
        return QStringLiteral("API Key 不能为空，不能含首尾空白或控制字符，且不能超过 2560 字节。");
    return {};
}

WindowsApiKeyStore::WindowsApiKeyStore(QString namespacePrefix)
    : m_namespace(std::move(namespacePrefix))
{
    if (!plain(m_namespace) || m_namespace.size() > 128) m_namespace.clear();
}

QString WindowsApiKeyStore::target(const QString &scope) const
{
    if (m_namespace.isEmpty() || !plain(scope) || scope.size() > 4096) return {};
    return m_namespace + QLatin1Char('/')
        + QString::fromLatin1(QCryptographicHash::hash(scope.toUtf8(), QCryptographicHash::Sha256).toHex());
}

bool WindowsApiKeyStore::read(const QString &scope, QString *key, QString *error)
{
    if (error) error->clear();
    if (key) key->clear();
    const auto name = target(scope);
    if (name.isEmpty() || !key) return failed(error, QStringLiteral("凭据范围无效，未读取 API Key。"));
#ifdef Q_OS_WIN
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(reinterpret_cast<LPCWSTR>(name.utf16()), CRED_TYPE_GENERIC, 0, &credential)) {
        const auto code = GetLastError();
        return code == ERROR_NOT_FOUND ? true : failed(error, systemIssue(code));
    }
    struct CredentialGuard {
        PCREDENTIALW value;
        ~CredentialGuard() {
            if (value->CredentialBlob && value->CredentialBlobSize)
                SecureZeroMemory(value->CredentialBlob, value->CredentialBlobSize);
            CredFree(value);
        }
    } guard{credential};
    if (!credential->CredentialBlob || credential->CredentialBlobSize == 0
        || credential->CredentialBlobSize > 2560)
        return failed(error, QStringLiteral("保存的 API Key 无效，请重新保存。"));
    auto bytes = QByteArray::fromRawData(reinterpret_cast<const char *>(credential->CredentialBlob),
        credential->CredentialBlobSize);
    QString value = QString::fromUtf8(bytes);
    if (value.toUtf8() != bytes || !validateTranslationApiKey(value).isEmpty())
        return failed(error, QStringLiteral("保存的 API Key 无效，请重新保存。"));
    *key = std::move(value);
    return true;
#else
    return failed(error, QStringLiteral("当前平台不支持 Windows 凭据存储，未使用明文保存 API Key。"));
#endif
}

bool WindowsApiKeyStore::write(const QString &scope, const QString &key, QString *error)
{
    if (error) error->clear();
    const auto name = target(scope);
    const auto issue = validateTranslationApiKey(key);
    if (name.isEmpty()) return failed(error, QStringLiteral("凭据范围无效，未保存 API Key。"));
    if (!issue.isEmpty()) return failed(error, issue);
#ifdef Q_OS_WIN
    QByteArray bytes = key.toUtf8();
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(name.utf16()));
    credential.CredentialBlobSize = static_cast<DWORD>(bytes.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(bytes.data());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    const bool saved = CredWriteW(&credential, 0) != FALSE;
    const auto code = saved ? ERROR_SUCCESS : GetLastError();
    SecureZeroMemory(bytes.data(), static_cast<SIZE_T>(bytes.size()));
    if (!saved) return failed(error, systemIssue(code));
    return true;
#else
    return failed(error, QStringLiteral("当前平台不支持 Windows 凭据存储，未使用明文保存 API Key。"));
#endif
}

bool WindowsApiKeyStore::remove(const QString &scope, QString *error)
{
    if (error) error->clear();
    const auto name = target(scope);
    if (name.isEmpty()) return failed(error, QStringLiteral("凭据范围无效，未删除 API Key。"));
#ifdef Q_OS_WIN
    if (CredDeleteW(reinterpret_cast<LPCWSTR>(name.utf16()), CRED_TYPE_GENERIC, 0)) return true;
    const auto code = GetLastError();
    return code == ERROR_NOT_FOUND ? true : failed(error, systemIssue(code));
#else
    return failed(error, QStringLiteral("当前平台不支持 Windows 凭据存储。"));
#endif
}
} // namespace asmr
