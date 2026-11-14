#include "update/UpdateInstaller.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace asmr::updater {
namespace {
namespace fs = std::filesystem;
constexpr std::size_t MaximumFiles = 10000;
constexpr std::uint64_t MaximumFileBytes = 1024ULL * 1024 * 1024;
constexpr std::uint64_t MaximumTreeBytes = 4ULL * 1024 * 1024 * 1024;
constexpr unsigned char Magic[16] = {'A','R','C','B','U','P','D','A','T','E','P','L','A','N',1,0};
bool fail(std::wstring *error, const std::wstring &message)
{ if (error) *error = message; return false; }

#ifdef _WIN32
class Handle {
public:
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    HANDLE value;
    bool valid() const { return value != INVALID_HANDLE_VALUE && value != nullptr; }
    void close() { if (valid()) CloseHandle(value); value = INVALID_HANDLE_VALUE; }
};
std::wstring utf16(const std::string &value)
{
    if (value.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring result(std::size_t(n), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), result.data(), n) != n) return {};
    return result;
}
std::wstring pathKey(const fs::path &path)
{
    auto result = path.lexically_normal().native();
    std::replace(result.begin(), result.end(), L'/', L'\\');
    if (!result.empty()) CharUpperBuffW(result.data(), DWORD(result.size()));
    while (result.size() > 3 && result.back() == L'\\') result.pop_back();
    return result;
}
bool contains(const fs::path &root, const fs::path &child)
{
    const auto a = pathKey(root), b = pathKey(child);
    return b == a || (b.size() > a.size() && b.compare(0, a.size(), a) == 0 && b[a.size()] == L'\\');
}
bool plainFile(const fs::path &path, std::uint64_t size)
{
    std::error_code ec;
    const auto actual = fs::file_size(path, ec);
    return !ec && actual == size && safeAncestors(path) && fs::is_regular_file(path, ec) && !ec;
}
bool matches(const fs::path &root, const File &file, std::wstring *error)
{
    const auto path = root / pathFromUtf8(file.path);
    std::array<unsigned char, 32> digest{};
    return (plainFile(path, file.length) && hashFile(path, digest, error) && digest == file.sha256)
        || fail(error, L"文件已修改、缺失或校验失败：" + path.wstring());
}
bool writableDirectory(const fs::path &root, std::wstring *error)
{
    const auto probe = root / (L".arcanblade-update-probe-" + std::to_wstring(GetCurrentProcessId()));
    Handle handle(CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    return handle.valid() || fail(error, L"程序目录不可写，请手动下载并解压新版到可写目录。");
}
bool copyNew(const fs::path &from, const fs::path &to, std::wstring *error)
{
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (ec || !safeAncestors(to) || !safeAncestors(from))
        return fail(error, L"无法复制文件：" + to.wstring());
    // CREATE_NEW establishes ownership of the output. A failed copy can then
    // remove only its own partial file and never an existing user collision.
    Handle output(CreateFileW(to.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!output.valid()) return fail(error, L"无法创建新的更新文件：" + to.wstring());
    Handle input(CreateFileW(from.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    bool ok = input.valid();
    std::vector<unsigned char> buffer(1024 * 1024);
    while (ok) {
        DWORD count = 0, written = 0;
        if (!ReadFile(input.value, buffer.data(), DWORD(buffer.size()), &count, nullptr)) { ok = false; break; }
        if (!count) break;
        if (!WriteFile(output.value, buffer.data(), count, &written, nullptr) || written != count) { ok = false; break; }
    }
    if (ok) ok = FlushFileBuffers(output.value);
    output.close();
    if (!ok) { DeleteFileW(to.c_str()); return fail(error, L"更新文件复制未完成：" + to.wstring()); }
    return ok;
}
bool replaceFrom(const fs::path &from, const fs::path &to, std::size_t serial, std::wstring *error)
{
    const auto temporary = to.parent_path() / (L".arcanblade-update-" + std::to_wstring(GetCurrentProcessId())
        + L"-" + std::to_wstring(serial) + L".tmp");
    if (!safeAncestors(from) || !safeAncestors(to) || !copyNew(from, temporary, error)) return false;
    if (!MoveFileExW(temporary.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return fail(error, L"无法替换文件（可能被其他程序占用）：" + to.wstring());
    }
    return true;
}
bool appendLog(const fs::path &path, const std::string &line)
{
    Handle handle(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (!handle.valid()) return false;
    const auto bytes = line + "\r\n";
    DWORD written = 0;
    return WriteFile(handle.value, bytes.data(), DWORD(bytes.size()), &written, nullptr)
        && written == bytes.size() && FlushFileBuffers(handle.value);
}
std::wstring mutexName(const fs::path &directory)
{
    const auto value = pathKey(directory);
    std::array<unsigned char, 32> digest{};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    const auto status = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(value.data())),
        ULONG(value.size() * sizeof(wchar_t)), digest.data(), ULONG(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) return {};
    std::wstring result = L"Local\\Arcanblade.Update.";
    constexpr wchar_t digits[] = L"0123456789abcdef";
    for (const auto c : digest) { result.push_back(digits[c >> 4]); result.push_back(digits[c & 15]); }
    return result;
}
class InstallLock {
public:
    explicit InstallLock(const fs::path &directory) {
        const auto name = mutexName(directory);
        if (name.empty()) return;
        handle = CreateMutexW(nullptr, FALSE, name.c_str());
        if (handle) { const auto result = WaitForSingleObject(handle, 0); owned = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED; }
    }
    ~InstallLock() { if (owned) ReleaseMutex(handle); if (handle) CloseHandle(handle); }
    bool owned = false;
    HANDLE handle = nullptr;
};
#else
std::wstring utf16(const std::string &value) { return std::wstring(value.begin(), value.end()); }
#endif

std::string genericUtf8(const fs::path &path)
{
    const auto value = path.generic_u8string();
    return std::string(reinterpret_cast<const char *>(value.data()), value.size());
}
void put32(std::vector<unsigned char> &bytes, std::uint32_t value)
{ for (unsigned int i = 0; i < 4; ++i) bytes.push_back(static_cast<unsigned char>(value >> (8 * i))); }
void put64(std::vector<unsigned char> &bytes, std::uint64_t value)
{ for (unsigned int i = 0; i < 8; ++i) bytes.push_back(static_cast<unsigned char>(value >> (8 * i))); }
void putString(std::vector<unsigned char> &bytes, const std::string &value)
{ put32(bytes, std::uint32_t(value.size())); bytes.insert(bytes.end(), value.begin(), value.end()); }
void putFiles(std::vector<unsigned char> &bytes, const std::vector<File> &files)
{
    put32(bytes, std::uint32_t(files.size()));
    for (const auto &file : files) {
        putString(bytes, file.path); put64(bytes, file.length);
        bytes.insert(bytes.end(), file.sha256.begin(), file.sha256.end());
    }
}
class Reader {
public:
    explicit Reader(const std::vector<unsigned char> &value) : bytes(value) {}
    bool read(void *out, std::size_t size) {
        if (size > bytes.size() - at) return false;
        std::copy_n(bytes.data() + at, size, static_cast<unsigned char *>(out)); at += size; return true;
    }
    bool number32(std::uint32_t &value) {
        unsigned char raw[4]; if (!read(raw, 4)) return false;
        value = 0; for (unsigned int i = 0; i < 4; ++i) value |= std::uint32_t(raw[i]) << (8 * i); return true;
    }
    bool number64(std::uint64_t &value) {
        unsigned char raw[8]; if (!read(raw, 8)) return false;
        value = 0; for (unsigned int i = 0; i < 8; ++i) value |= std::uint64_t(raw[i]) << (8 * i); return true;
    }
    bool string(std::string &value) {
        std::uint32_t length; if (!number32(length) || length > 32768 || length > bytes.size() - at) return false;
        value.assign(reinterpret_cast<const char *>(bytes.data() + at), length); at += length;
        return value.find('\0') == std::string::npos && (value.empty() || !utf16(value).empty());
    }
    bool files(std::vector<File> &files) {
        std::uint32_t count; if (!number32(count) || count == 0 || count > MaximumFiles) return false;
        files.resize(count);
        for (auto &file : files) if (!string(file.path) || !number64(file.length)
            || !read(file.sha256.data(), file.sha256.size())) return false;
        return true;
    }
    bool atEnd() const { return at == bytes.size(); }
private:
    const std::vector<unsigned char> &bytes;
    std::size_t at = 0;
};
bool validVersion(const std::string &value)
{
    if (value.empty() || value.size() > 128) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || c == '.' || c == '-' || c == '+';
    });
}
} // namespace

fs::path pathFromUtf8(const std::string &value)
{
    return fs::path(std::u8string(reinterpret_cast<const char8_t *>(value.data()), value.size()));
}

bool safeRelativePath(const std::string &path)
{
    if (path.empty() || path.size() > 2048 || path.front() == '/' || path.back() == '/' || utf16(path).empty()) return false;
    std::string component;
    const auto check = [](std::string part) {
        if (part.empty() || part.size() > 255 || part == "." || part == ".."
            || part.back() == '.' || part.back() == ' ') return false;
        for (const auto c : part) if (static_cast<unsigned char>(c) < 32 || c == 127
            || std::string(":*?\"<>|\\").find(c) != std::string::npos) return false;
        auto stem = part.substr(0, part.find('.'));
        std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return char(std::toupper(c)); });
        if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || stem == "CONIN$" || stem == "CONOUT$") return false;
        if (stem.size() == 4 && (stem.substr(0, 3) == "COM" || stem.substr(0, 3) == "LPT") && stem[3] >= '1' && stem[3] <= '9') return false;
        return true;
    };
    for (char c : path) {
        if (c == '/') { if (!check(component)) return false; component.clear(); }
        else component.push_back(c);
    }
    if (!check(component)) return false;
    const auto slash = path.find_last_of('/');
    auto filename = path.substr(slash == std::string::npos ? 0 : slash + 1);
    const auto dot = filename.find_last_of('.');
    auto extension = dot == std::string::npos ? std::string() : filename.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const std::set<std::string> userExtensions{ ".mp3", ".wav", ".flac", ".ogg", ".m4a", ".aac", ".srt", ".vtt", ".lrc", ".sqlite", ".sqlite3", ".db", ".ini", ".key", ".pem", ".pfx", ".env" };
    std::transform(filename.begin(), filename.end(), filename.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return userExtensions.count(extension) == 0 && filename != ".env"
        && (extension != ".exe" || path == "Arcanblade.exe" || path == "ArcanbladeUpdater.exe"
            || path == "tools/7zip/7za.exe");
}

bool managedFilePath(const std::string &path)
{
    if (!safeRelativePath(path)) return false;
    auto key = path;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const auto slash = key.find('/');
    if (slash == std::string::npos) {
        const std::set<std::string> names{"arcanblade.exe", "arcanbladeupdater.exe", "files.json", "build-info.json", "qt.conf", "readme.txt", "license", "notice"};
        return names.count(key) != 0 || key.ends_with(".dll");
    }
    const auto directory = key.substr(0, slash);
    if (directory == "licenses") return true;
    if (path == "tools/7zip/7za.exe") return true;
    const std::set<std::string> plugins{"platforms", "sqldrivers", "multimedia", "tls", "imageformats", "iconengines", "styles", "networkinformation", "generic", "platforminputcontexts", "platformthemes", "printsupport", "audio", "plugins"};
    return plugins.count(directory) != 0 && key.ends_with(".dll");
}

bool safeAncestors(const fs::path &path)
{
#ifdef _WIN32
    if (path.empty() || !path.is_absolute() || path != path.lexically_normal()
        || path.native().find(L'\0') != std::wstring::npos || path.native().rfind(L"\\\\", 0) == 0) return false;
    auto current = path;
    while (!current.empty()) {
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        if (attributes == INVALID_FILE_ATTRIBUTES && GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) return false;
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return true;
#else
    if (!path.is_absolute() || path != path.lexically_normal()) return false;
    std::error_code ec;
    auto current = path;
    while (!current.empty()) {
        if (fs::is_symlink(current, ec)) return false;
        const auto parent = current.parent_path(); if (parent == current) break; current = parent;
    }
    return true;
#endif
}

bool hashFile(const fs::path &path, std::array<unsigned char, 32> &digest, std::wstring *error)
{
#ifdef _WIN32
    if (!safeAncestors(path)) return fail(error, L"不安全的文件路径。");
    Handle handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!handle.valid()) return fail(error, L"无法读取文件进行校验：" + path.wstring());
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return fail(error, L"无法初始化 SHA-256。");
    DWORD objectLength = 0, returned = 0;
    bool ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &returned, 0) >= 0;
    std::vector<unsigned char> object(objectLength), buffer(1024 * 1024);
    if (ok) ok = BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) >= 0;
    DWORD bytes = 0;
    while (ok) {
        if (!ReadFile(handle.value, buffer.data(), DWORD(buffer.size()), &bytes, nullptr)) { ok = false; break; }
        if (!bytes) break;
        ok = BCryptHashData(hash, buffer.data(), bytes, 0) >= 0;
    }
    if (ok) ok = BCryptFinishHash(hash, digest.data(), DWORD(digest.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok || fail(error, L"SHA-256 文件校验失败：" + path.wstring());
#else
    (void)path; (void)digest; return fail(error, L"自动更新仅支持 Windows。");
#endif
}

bool validatePlan(const Plan &plan, std::wstring *error)
{
#ifdef _WIN32
    if (!safeAncestors(plan.targetDirectory) || !safeAncestors(plan.packageDirectory) || !safeAncestors(plan.sessionDirectory)
        || plan.targetDirectory == plan.targetDirectory.root_path() || plan.packageDirectory == plan.packageDirectory.root_path()
        || plan.sessionDirectory == plan.sessionDirectory.root_path()
        || contains(plan.targetDirectory, plan.packageDirectory) || contains(plan.packageDirectory, plan.targetDirectory)
        || contains(plan.targetDirectory, plan.sessionDirectory) || contains(plan.sessionDirectory, plan.targetDirectory)
        || contains(plan.packageDirectory, plan.sessionDirectory) || contains(plan.sessionDirectory, plan.packageDirectory)
        || !validVersion(plan.oldVersion) || !validVersion(plan.newVersion) || plan.oldVersion == plan.newVersion
        || plan.parentPid == 0 || plan.parentCreationTime == 0)
        return fail(error, L"更新计划路径、版本或父进程信息无效。");
    const auto checkFiles = [error](const std::vector<File> &files) {
        if (files.empty() || files.size() > MaximumFiles) return fail(error, L"更新文件清单为空或过大。");
        std::set<std::wstring> seen;
        std::uint64_t total = 0;
        bool executable = false, manifest = false, metadata = false, helper = false;
        for (const auto &file : files) {
            if (!managedFilePath(file.path) || file.length > MaximumFileBytes || total > MaximumTreeBytes - file.length
                || !seen.insert(pathKey(pathFromUtf8(file.path))).second
                || std::all_of(file.sha256.begin(), file.sha256.end(), [](auto c) { return c == 0; }))
                return fail(error, L"更新清单包含不安全、重复或过大的文件。");
            total += file.length;
            executable |= file.path == "Arcanblade.exe";
            manifest |= file.path == "FILES.json";
            metadata |= file.path == "BUILD-INFO.json";
            helper |= file.path == "ArcanbladeUpdater.exe";
        }
        for (const auto &key : seen) {
            auto parent = fs::path(key).parent_path();
            while (!parent.empty()) {
                if (seen.count(pathKey(parent))) return fail(error, L"清单中的文件被用作目录。");
                parent = parent.parent_path();
            }
        }
        return (executable && manifest && metadata && helper) || fail(error, L"更新清单缺少主程序、更新器或版本元数据。");
    };
    return checkFiles(plan.oldFiles) && checkFiles(plan.newFiles);
#else
    (void)plan; return fail(error, L"自动更新仅支持 Windows。");
#endif
}

bool writePlan(const fs::path &path, const Plan &plan, std::wstring *error)
{
    if (!validatePlan(plan, error) || !safeAncestors(path)) return false;
    std::vector<unsigned char> bytes(std::begin(Magic), std::end(Magic));
    putString(bytes, genericUtf8(plan.targetDirectory)); putString(bytes, genericUtf8(plan.packageDirectory));
    putString(bytes, genericUtf8(plan.sessionDirectory)); putString(bytes, plan.oldVersion); putString(bytes, plan.newVersion);
    put32(bytes, plan.parentPid); put64(bytes, plan.parentCreationTime); putFiles(bytes, plan.oldFiles); putFiles(bytes, plan.newFiles);
#ifdef _WIN32
    Handle handle(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
    DWORD written = 0;
    return (handle.valid() && WriteFile(handle.value, bytes.data(), DWORD(bytes.size()), &written, nullptr)
        && written == bytes.size() && FlushFileBuffers(handle.value)) || fail(error, L"无法保存新的更新计划。");
#else
    return fail(error, L"自动更新仅支持 Windows。");
#endif
}

bool readPlan(const fs::path &path, Plan &plan, std::wstring *error)
{
    if (!safeAncestors(path)) return fail(error, L"更新计划路径不安全。");
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size < sizeof(Magic) || size > 16 * 1024 * 1024) return fail(error, L"更新计划缺失或过大。");
    std::ifstream stream(path, std::ios::binary);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    if (!stream.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(bytes.size()))) return fail(error, L"无法完整读取更新计划。");
    Reader reader(bytes);
    unsigned char magic[sizeof(Magic)];
    std::string target, package, session;
    Plan result;
    if (!reader.read(magic, sizeof(magic)) || !std::equal(std::begin(magic), std::end(magic), std::begin(Magic))
        || !reader.string(target) || !reader.string(package) || !reader.string(session)
        || !reader.string(result.oldVersion) || !reader.string(result.newVersion)
        || !reader.number32(result.parentPid) || !reader.number64(result.parentCreationTime)
        || !reader.files(result.oldFiles) || !reader.files(result.newFiles) || !reader.atEnd())
        return fail(error, L"更新计划格式无效。");
    result.targetDirectory = pathFromUtf8(target);
    result.packageDirectory = pathFromUtf8(package);
    result.sessionDirectory = pathFromUtf8(session);
    if (!validatePlan(result, error)) return false;
    plan = std::move(result);
    return true;
}

bool preflight(const Plan &plan, std::wstring *error)
{
#ifdef _WIN32
    if (!validatePlan(plan, error)) return false;
    std::error_code ec;
    if (!fs::is_directory(plan.targetDirectory, ec) || ec || !fs::is_directory(plan.packageDirectory, ec) || ec
        || !fs::is_directory(plan.sessionDirectory, ec) || ec)
        return fail(error, L"程序、更新包或更新会话目录不存在。");
    std::set<std::wstring> old;
    std::uint64_t oldBytes = 0, newBytes = 0;
    for (const auto &file : plan.oldFiles) {
        if (!matches(plan.targetDirectory, file, error)) return false;
        old.insert(pathKey(pathFromUtf8(file.path))); oldBytes += file.length;
        const auto attrs = GetFileAttributesW((plan.targetDirectory / pathFromUtf8(file.path)).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_READONLY)) return fail(error, L"程序文件只读，无法自动更新。");
    }
    for (const auto &file : plan.newFiles) {
        if (!matches(plan.packageDirectory, file, error)) return false;
        newBytes += file.length;
        const auto destination = plan.targetDirectory / pathFromUtf8(file.path);
        if (!safeAncestors(destination)) return fail(error, L"更新目标包含链接或重解析点。");
        if (fs::exists(destination, ec) && !old.count(pathKey(pathFromUtf8(file.path))))
            return fail(error, L"新版文件与目录中的非程序文件冲突，请手动更新：" + destination.wstring());
        if (ec) return fail(error, L"无法访问更新目标。");
    }
    ULARGE_INTEGER freeCache{}, freeTarget{};
    // Reserve the combined peak at both locations. Drive letters, SUBST and
    // mount points can refer to the same volume; a conservative budget avoids
    // accidentally spending recovery space twice under different path names.
    const std::uint64_t requiredFreeBytes = oldBytes + newBytes + 16 * 1024 * 1024;
    if (!GetDiskFreeSpaceExW(plan.sessionDirectory.c_str(), &freeCache, nullptr, nullptr)
        || !GetDiskFreeSpaceExW(plan.targetDirectory.c_str(), &freeTarget, nullptr, nullptr)
        || freeCache.QuadPart < requiredFreeBytes || freeTarget.QuadPart < requiredFreeBytes)
        return fail(error, L"磁盘空间不足，无法备份和安装更新。");
    return writableDirectory(plan.targetDirectory, error) && writableDirectory(plan.sessionDirectory, error);
#else
    (void)plan; return fail(error, L"自动更新仅支持 Windows。");
#endif
}

InstallResult install(const Plan &plan, const InstallOptions &options)
{
    InstallResult result;
#ifdef _WIN32
    InstallLock lock(plan.targetDirectory);
    if (!lock.owned) { result.error = L"另一个更新器正在处理此程序目录，请稍后重试。"; return result; }
    if (!preflight(plan, &result.error)) return result;
    const auto backup = plan.sessionDirectory / L"backup";
    const auto log = plan.sessionDirectory / L"transaction.log";
    std::error_code ec;
    if (fs::exists(backup, ec) || ec || !fs::create_directory(backup, ec) || ec || !safeAncestors(backup)) {
        result.error = L"更新备份目录已存在或不可创建，请重新准备更新。"; return result;
    }
    if (!appendLog(log, "BEGIN " + plan.oldVersion + " -> " + plan.newVersion)) {
        result.error = L"无法创建更新事务日志。"; return result;
    }
    for (const auto &file : plan.oldFiles) {
        const auto relative = pathFromUtf8(file.path);
        if (!copyNew(plan.targetDirectory / relative, backup / relative, &result.error)
            || !matches(backup, file, &result.error) || !appendLog(log, "BACKUP " + file.path)) return result;
    }
    // Hide the entry point while its DLL set changes. A writable, non-readable
    // image handle first rules out another running instance and prevents a new
    // process racing the rename. Publishing the new EXE is the final mutation.
    const auto entry = plan.targetDirectory / L"Arcanblade.exe";
    const auto hiddenEntry = plan.targetDirectory / (L".arcanblade-entry-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    Handle entryHandle(CreateFileW(entry.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!entryHandle.valid()) { result.error = L"另一个程序实例仍在运行，请关闭后重试更新。"; return result; }
    if (!appendLog(log, "HIDE_ENTRY") || !safeAncestors(hiddenEntry)
        || !MoveFileExW(entry.c_str(), hiddenEntry.c_str(), MOVEFILE_WRITE_THROUGH)) {
        result.error = L"无法暂存旧版程序入口，未替换程序文件。"; return result;
    }
    result.changedFiles = true;
    std::set<std::wstring> newKeys, oldKeys;
    for (const auto &file : plan.newFiles) newKeys.insert(pathKey(pathFromUtf8(file.path)));
    for (const auto &file : plan.oldFiles) oldKeys.insert(pathKey(pathFromUtf8(file.path)));
    std::vector<std::string> touchedNew;
    std::size_t change = 0;
    const auto permit = [&]() { return !options.permitMutation || options.permitMutation(change); };
    bool ok = true;
    const auto replaceFile = [&](const File &file) {
        if (!permit() || !appendLog(log, "REPLACE " + file.path)) { result.error = L"更新写入被中止。"; ok = false; return; }
        const auto relative = pathFromUtf8(file.path);
        touchedNew.push_back(file.path);
        result.changedFiles = true;
        if (!matches(plan.packageDirectory, file, &result.error)
            || !replaceFrom(plan.packageDirectory / relative, plan.targetDirectory / relative, change++, &result.error)
            || !matches(plan.targetDirectory, file, &result.error)) { ok = false; }
    };
    for (const auto &file : plan.newFiles) {
        if (file.path == "Arcanblade.exe") continue;
        replaceFile(file); if (!ok) break;
    }
    if (ok) for (const auto &file : plan.oldFiles) {
        if (newKeys.count(pathKey(pathFromUtf8(file.path)))) continue;
        if (!permit() || !appendLog(log, "REMOVE " + file.path)) { result.error = L"更新写入被中止。"; ok = false; break; }
        const auto target = plan.targetDirectory / pathFromUtf8(file.path);
        result.changedFiles = true;
        if (!safeAncestors(target) || !DeleteFileW(target.c_str())) { result.error = L"无法清理旧版程序文件：" + target.wstring(); ok = false; break; }
        ++change;
    }
    if (ok) {
        const auto executable = std::find_if(plan.newFiles.begin(), plan.newFiles.end(), [](const auto &file) { return file.path == "Arcanblade.exe"; });
        replaceFile(*executable);
    }
    if (ok) {
        ok = appendLog(log, "INSTALLED");
        if (!ok) result.error = L"无法完成更新事务日志。";
    }
    entryHandle.close();
    if (ok && !DeleteFileW(hiddenEntry.c_str())) { result.error = L"无法清理暂存的旧版程序入口。"; ok = false; }
    if (ok) { result.success = true; return result; }
    bool restored = appendLog(log, "ROLLBACK");
    std::size_t serial = change + 10000;
    const auto restoreFile = [&](const File &file) {
        const auto relative = pathFromUtf8(file.path);
        std::wstring rollbackError;
        if (matches(plan.targetDirectory, file, &rollbackError)) return;
        if (!matches(backup, file, &rollbackError)
            || !replaceFrom(backup / relative, plan.targetDirectory / relative, serial++, &rollbackError)
            || !matches(plan.targetDirectory, file, &rollbackError)) restored = false;
    };
    // Remove new-only files first so recovery has as much free space as
    // possible. Every path here was created by this transaction.
    for (const auto &path : touchedNew) {
        if (oldKeys.count(pathKey(pathFromUtf8(path)))) continue;
        const auto target = plan.targetDirectory / pathFromUtf8(path);
        if (!safeAncestors(target)) { restored = false; continue; }
        const auto attributes = GetFileAttributesW(target.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && !DeleteFileW(target.c_str())) restored = false;
    }
    for (const auto &file : plan.oldFiles) {
        if (file.path == "Arcanblade.exe") continue;
        restoreFile(file);
    }
    const auto oldExecutable = std::find_if(plan.oldFiles.begin(), plan.oldFiles.end(), [](const auto &file) { return file.path == "Arcanblade.exe"; });
    // Return the original entry by rename, without requiring free disk space
    // for another copy. Never delete it if restoration itself fails.
    const auto expectedOldDigest = oldExecutable->sha256;
    std::array<unsigned char, 32> hiddenDigest{};
    if (!safeAncestors(hiddenEntry) || !hashFile(hiddenEntry, hiddenDigest) || hiddenDigest != expectedOldDigest
        || !MoveFileExW(hiddenEntry.c_str(), entry.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
        || !matches(plan.targetDirectory, *oldExecutable, nullptr)) restored = false;
    if (!appendLog(log, restored ? "ROLLED_BACK" : "ROLLBACK_INCOMPLETE")) restored = false;
    result.rollbackSucceeded = restored;
#else
    (void)plan; (void)options; result.error = L"自动更新仅支持 Windows。";
#endif
    return result;
}

bool restoreBackup(const Plan &plan, std::wstring *error)
{
#ifdef _WIN32
    InstallLock lock(plan.targetDirectory);
    if (!lock.owned || !validatePlan(plan, error)) return fail(error, L"无法取得恢复锁或更新计划无效。");
    const auto backup = plan.sessionDirectory / L"backup";
    const auto log = plan.sessionDirectory / L"transaction.log";
    if (!safeAncestors(backup)) return fail(error, L"备份目录不安全。");
    for (const auto &file : plan.oldFiles) if (!matches(backup, file, error)) return false;
    for (const auto &file : plan.newFiles) if (!matches(plan.targetDirectory, file, error)) return false;
    if (!appendLog(log, "ROLLBACK_LAUNCH_FAILURE")) return fail(error, L"无法保存恢复日志。");
    const auto entry = plan.targetDirectory / L"Arcanblade.exe";
    const auto hidden = plan.targetDirectory / (L".arcanblade-restore-entry-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    Handle entryHandle(CreateFileW(entry.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!entryHandle.valid() || !safeAncestors(hidden) || !MoveFileExW(entry.c_str(), hidden.c_str(), MOVEFILE_WRITE_THROUGH))
        return fail(error, L"新版程序已被打开或入口不可暂存，未进行旧版恢复。");
    std::set<std::wstring> old;
    for (const auto &file : plan.oldFiles) old.insert(pathKey(pathFromUtf8(file.path)));
    std::size_t index = 30000;
    bool ok = true;
    for (const auto &file : plan.newFiles) {
        if (old.count(pathKey(pathFromUtf8(file.path)))) continue;
        const auto target = plan.targetDirectory / pathFromUtf8(file.path);
        if (!safeAncestors(target)) { ok = false; continue; }
        const auto attributes = GetFileAttributesW(target.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && !DeleteFileW(target.c_str())) ok = false;
    }
    const auto restore = [&](const File &file) {
        if (matches(plan.targetDirectory, file, nullptr)) return;
        const auto relative = pathFromUtf8(file.path);
        if (!safeAncestors(plan.targetDirectory / relative)
            || !replaceFrom(backup / relative, plan.targetDirectory / relative, index++, error)
            || !matches(plan.targetDirectory, file, error)) ok = false;
    };
    for (const auto &file : plan.oldFiles) if (file.path != "Arcanblade.exe") restore(file);
    const auto oldEntry = std::find_if(plan.oldFiles.begin(), plan.oldFiles.end(), [](const auto &file) { return file.path == "Arcanblade.exe"; });
    restore(*oldEntry);
    entryHandle.close();
    if (ok && !DeleteFileW(hidden.c_str())) ok = false;
    if (!appendLog(log, ok ? "ROLLED_BACK" : "ROLLBACK_INCOMPLETE")) ok = false;
    return ok || fail(error, L"旧版恢复未完成，请保留备份并查看日志。");
#else
    (void)plan; return fail(error, L"自动更新仅支持 Windows。");
#endif
}

std::uint64_t currentProcessCreationTime()
{
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        return (std::uint64_t(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
#endif
    return 0;
}
bool installationInProgress(const fs::path &targetDirectory)
{
#ifdef _WIN32
    const auto name = mutexName(targetDirectory);
    if (name.empty()) return true;
    Handle handle(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name.c_str()));
    if (!handle.valid()) return GetLastError() != ERROR_FILE_NOT_FOUND;
    const auto state = WaitForSingleObject(handle.value, 0);
    if (state == WAIT_OBJECT_0 || state == WAIT_ABANDONED) { ReleaseMutex(handle.value); return false; }
    return true;
#else
    (void)targetDirectory; return false;
#endif
}
} // namespace asmr::updater
