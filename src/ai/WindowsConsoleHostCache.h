#pragma once

#include <QDir>
#include <QString>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

namespace asmr::detail {
// Private to the process runner. The supplied path is obtained from Windows,
// never from the external engine. A failed query alone cannot prove identity.
class WindowsConsoleHostCache final {
public:
    using ImageQuery = std::function<bool(HANDLE, DWORD, QString *, DWORD *)>;

    explicit WindowsConsoleHostCache(QString expectedSystemConsoleHostPath, ImageQuery query = {})
        : m_expectedPath(normalizedPath(expectedSystemConsoleHostPath)),
          m_query(query ? std::move(query) : ImageQuery(queryImage))
    {
        if (!QDir::isAbsolutePath(m_expectedPath)) m_expectedPath.clear();
        if (!m_expectedPath.isEmpty()) m_expectedNativePath = nativePathForFile(m_expectedPath);
    }

    WindowsConsoleHostCache(const WindowsConsoleHostCache &) = delete;
    WindowsConsoleHostCache &operator=(const WindowsConsoleHostCache &) = delete;

    // imageError reports this call's image query, even when a retained identity
    // safely supplies the previously verified path. The caller checks latest
    // liveness and job membership separately before using this classification.
    bool isVerifiedHost(HANDLE process, QString *imagePath = nullptr, DWORD *imageError = nullptr)
    {
        QString observedPath;
        DWORD queryError = ERROR_SUCCESS;
        bool queried = m_query(process, 0, &observedPath, &queryError);
        observedPath = queried ? normalizedPath(observedPath) : QString{};
        bool matchesExpected = queried && !observedPath.isEmpty()
            && observedPath.compare(m_expectedPath, Qt::CaseInsensitive) == 0;
        if (!queried && !m_expectedNativePath.isEmpty()) {
            // The Win32 path query can fail during console-host teardown even
            // while this same process handle still exposes its native image.
            // Compare the complete native name against a separately resolved
            // system file; a failed query or a matching basename is no proof.
            queried = m_query(process, PROCESS_NAME_NATIVE, &observedPath, &queryError);
            observedPath = queried ? QDir::fromNativeSeparators(observedPath) : QString{};
            matchesExpected = queried && !observedPath.isEmpty()
                && observedPath.compare(m_expectedNativePath, Qt::CaseInsensitive) == 0;
            if (matchesExpected) observedPath = m_expectedPath;
        }
        if (imagePath) *imagePath = observedPath;
        if (imageError) *imageError = queried ? ERROR_SUCCESS : queryError;

        // A retained handle binds a cached identity to its kernel process object.
        // Drop terminated or invalid entries rather than carrying their PID to
        // a later process. Unknown identities always fail closed.
        std::erase_if(m_hosts, [](const Entry &entry) {
            return WaitForSingleObject(entry.handle.get(), 0) != WAIT_TIMEOUT;
        });
        const auto current = identity(process);
        if (!current || WaitForSingleObject(process, 0) != WAIT_TIMEOUT || m_expectedPath.isEmpty())
            return false;
        auto known = std::find_if(m_hosts.begin(), m_hosts.end(), [&](const Entry &entry) {
            const auto retained = identity(entry.handle.get());
            return retained && sameIdentity(*current, entry.identity) && sameIdentity(*retained, entry.identity);
        });
        if (queried) {
            // Do not inherit an old exemption if a newer successful observation
            // contradicts it, even if an injected/internal backend is faulty.
            if (!matchesExpected) {
                if (known != m_hosts.end()) m_hosts.erase(known);
                return false;
            }
            if (known == m_hosts.end() && m_hosts.size() < MaximumRetainedHosts) {
                HANDLE duplicate = nullptr;
                if (DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), &duplicate,
                        0, FALSE, DUPLICATE_SAME_ACCESS))
                    m_hosts.push_back(Entry{*current, OwnedHandle(duplicate)});
            }
            // This observation itself proves the path even if handle retention
            // failed or the bounded cache is full; later failures remain closed.
            return true;
        }
        if (known == m_hosts.end()) return false;
        if (imagePath) *imagePath = m_expectedPath;
        return true;
    }

private:
    class OwnedHandle final {
    public:
        explicit OwnedHandle(HANDLE handle) : m_handle(handle) {}
        ~OwnedHandle() { if (m_handle) CloseHandle(m_handle); }
        OwnedHandle(const OwnedHandle &) = delete;
        OwnedHandle &operator=(const OwnedHandle &) = delete;
        OwnedHandle(OwnedHandle &&other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
        OwnedHandle &operator=(OwnedHandle &&other) noexcept
        {
            if (this != &other) {
                if (m_handle) CloseHandle(m_handle);
                m_handle = std::exchange(other.m_handle, nullptr);
            }
            return *this;
        }
        HANDLE get() const { return m_handle; }
    private:
        HANDLE m_handle = nullptr;
    };
    struct Identity { DWORD pid; FILETIME created; };
    struct Entry { Identity identity; OwnedHandle handle; };
    static constexpr size_t MaximumRetainedHosts = 64;

    static QString normalizedPath(const QString &path)
    {
        return path.isEmpty() ? QString{} : QDir::cleanPath(QDir::fromNativeSeparators(path));
    }
    static QString nativePathForFile(const QString &path)
    {
        HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
            FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return {};
        const OwnedHandle owned(file);
        wchar_t nativePath[32768]{};
        const DWORD length = GetFinalPathNameByHandleW(file, nativePath, DWORD(std::size(nativePath)),
            FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
        if (!length || length >= std::size(nativePath)) return {};
        return QDir::fromNativeSeparators(QString::fromWCharArray(nativePath, int(length)));
    }
    static std::optional<Identity> identity(HANDLE process)
    {
        if (!process) return std::nullopt;
        const DWORD pid = GetProcessId(process);
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!pid || !GetProcessTimes(process, &created, &exited, &kernel, &user)) return std::nullopt;
        return Identity{pid, created};
    }
    static bool sameIdentity(const Identity &left, const Identity &right)
    {
        return left.pid == right.pid && left.created.dwLowDateTime == right.created.dwLowDateTime
            && left.created.dwHighDateTime == right.created.dwHighDateTime;
    }
    static bool queryImage(HANDLE process, DWORD flags, QString *path, DWORD *error)
    {
        wchar_t image[32768]{};
        DWORD length = DWORD(std::size(image));
        if (!QueryFullProcessImageNameW(process, flags, image, &length)) {
            const DWORD code = GetLastError();
            path->clear();
            *error = code;
            return false;
        }
        *path = QString::fromWCharArray(image, int(length));
        *error = ERROR_SUCCESS;
        return true;
    }

    QString m_expectedPath;
    QString m_expectedNativePath;
    ImageQuery m_query;
    std::vector<Entry> m_hosts;
};
} // namespace asmr::detail
#endif
