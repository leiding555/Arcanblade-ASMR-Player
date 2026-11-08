#include "update/UpdateInstaller.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
using asmr::updater::Plan;
class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value(value) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    HANDLE value;
};
bool markerExists(const fs::path &path)
{
    return asmr::updater::safeAncestors(path) && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
bool writeMarker(const fs::path &path, const std::string &content)
{
    if (!asmr::updater::safeAncestors(path)) return false;
    Handle handle(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (handle.value == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    return WriteFile(handle.value, content.data(), DWORD(content.size()), &written, nullptr)
        && written == content.size() && FlushFileBuffers(handle.value);
}
bool matchingMarker(const fs::path &path, const std::string &content)
{
    if (!markerExists(path)) return false;
    std::ifstream file(path, std::ios::binary);
    std::string actual(content.size() + 1, '\0');
    file.read(actual.data(), std::streamsize(actual.size()));
    actual.resize(std::size_t(file.gcount()));
    return actual == content;
}
void releaseLease(const Plan &plan)
{
    const auto lease = plan.packageDirectory.parent_path() / L".install-lease";
    const auto session = plan.sessionDirectory.generic_u8string();
    const std::string expected(reinterpret_cast<const char *>(session.data()), session.size());
    if (matchingMarker(lease, expected)) DeleteFileW(lease.c_str());
}
void failed(const Plan &plan, const std::wstring &error, bool show)
{
    releaseLease(plan);
    (void)writeMarker(plan.sessionDirectory / L"failed", "failed");
    std::ofstream diagnostic(plan.sessionDirectory / L"diagnostic.txt", std::ios::binary | std::ios::app);
    if (diagnostic) {
        const int count = WideCharToMultiByte(CP_UTF8, 0, error.data(), int(error.size()), nullptr, 0, nullptr, nullptr);
        std::string text(std::size_t(count), '\0');
        if (count > 0) WideCharToMultiByte(CP_UTF8, 0, error.data(), int(error.size()), text.data(), count, nullptr, nullptr);
        diagnostic << text << "\r\n"; diagnostic.flush();
    }
    if (show) {
        const auto message = error + L"\n\n备份和日志保留在：\n" + plan.sessionDirectory.wstring();
        MessageBoxW(nullptr, message.c_str(), L"Arcanblade 更新", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }
}
std::wstring pathKey(const fs::path &value)
{
    auto result = value.lexically_normal().native();
    std::replace(result.begin(), result.end(), L'/', L'\\');
    if (!result.empty()) CharUpperBuffW(result.data(), DWORD(result.size()));
    return result;
}
std::wstring quote(const std::wstring &value)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto c : value) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'\"') { result.append(slashes * 2 + 1, L'\\'); result.push_back(c); }
        else { result.append(slashes, L'\\'); result.push_back(c); }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\'); result.push_back(L'\"'); return result;
}
bool restart(const Plan &plan, Handle &process)
{
    const auto executable = plan.targetDirectory / L"Arcanblade.exe";
    if (!asmr::updater::safeAncestors(executable)) return false;
    auto command = quote(executable.wstring()) + L" --arcanblade-update-session " + quote(plan.sessionDirectory.wstring());
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        plan.targetDirectory.c_str(), &startup, &child)) return false;
    CloseHandle(child.hThread); process.value = child.hProcess; return true;
}
int run(const fs::path &planPath)
{
    Plan plan;
    std::wstring error;
    if (!asmr::updater::readPlan(planPath, plan, &error)) return 2;
    wchar_t ownFile[32768]{};
    const auto count = GetModuleFileNameW(nullptr, ownFile, DWORD(std::size(ownFile)));
    if (!count || count >= std::size(ownFile)
        || pathKey(planPath) != pathKey(plan.sessionDirectory / L"install.plan")
        || pathKey(fs::path(ownFile)) != pathKey(plan.sessionDirectory / L"ArcanbladeUpdater.exe")
        || plan.parentPid == GetCurrentProcessId()) {
        failed(plan, L"更新器的位置或计划不匹配。", false); return 3;
    }
    Handle parent(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, plan.parentPid));
    FILETIME creation{}, exit{}, kernel{}, user{};
    wchar_t image[32768]{}; DWORD imageSize = DWORD(std::size(image));
    if (!parent.value || !GetProcessTimes(parent.value, &creation, &exit, &kernel, &user)
        || (((std::uint64_t(creation.dwHighDateTime) << 32) | creation.dwLowDateTime) != plan.parentCreationTime)
        || !QueryFullProcessImageNameW(parent.value, 0, image, &imageSize)
        || pathKey(fs::path(image)) != pathKey(plan.targetDirectory / L"Arcanblade.exe")
        || WaitForSingleObject(parent.value, 0) != WAIT_TIMEOUT) {
        failed(plan, L"无法确认仍在运行的原版程序，更新已取消。", false); return 4;
    }
    const auto helperRecord = std::find_if(plan.oldFiles.begin(), plan.oldFiles.end(), [](const auto &file) { return file.path == "ArcanbladeUpdater.exe"; });
    std::array<unsigned char, 32> helperDigest{};
    if (helperRecord == plan.oldFiles.end() || !asmr::updater::hashFile(fs::path(ownFile), helperDigest)
        || helperDigest != helperRecord->sha256) {
        failed(plan, L"更新器副本与当前发布包不一致。", false); return 4;
    }
    const auto ready = plan.sessionDirectory / L"ready", proceed = plan.sessionDirectory / L"proceed";
    const auto abort = plan.sessionDirectory / L"abort";
    if (markerExists(abort) || markerExists(proceed) || !writeMarker(ready, plan.newVersion)) return 5;
    const auto readyAt = GetTickCount64();
    while (!matchingMarker(proceed, plan.newVersion)) {
        if (markerExists(abort) || WaitForSingleObject(parent.value, 0) != WAIT_TIMEOUT || GetTickCount64() - readyAt >= 15000) {
            failed(plan, L"未收到本次重启更新的确认，更新已取消。", false); return 6;
        }
        Sleep(25);
    }
    const auto committedAt = GetTickCount64();
    DWORD parentState = WAIT_TIMEOUT;
    while ((parentState = WaitForSingleObject(parent.value, 100)) == WAIT_TIMEOUT) {
        if (markerExists(abort) || GetTickCount64() - committedAt >= 5 * 60 * 1000) {
            failed(plan, L"程序尚未完成退出或更新已取消，未替换任何文件。", false); return 7;
        }
    }
    if (parentState != WAIT_OBJECT_0) { failed(plan, L"无法确认原版程序已经退出，未替换任何文件。", false); return 7; }
    if (markerExists(abort)) { failed(plan, L"更新已取消。", false); return 8; }
    const auto installed = asmr::updater::install(plan);
    if (!installed.success) {
        auto message = installed.error;
        if (installed.rollbackSucceeded) message += L"\n已恢复原版程序，可以重新打开使用。";
        else if (installed.changedFiles) message += L"\n旧版恢复未完成，请保留备份并根据日志恢复程序文件。";
        else message += L"\n原版程序文件未改动。";
        failed(plan, message, true); return 9;
    }
    Handle restarted;
    if (!restart(plan, restarted)) {
        const bool restored = asmr::updater::restoreBackup(plan, &error);
        failed(plan, restored ? L"新版程序无法启动，已恢复原版程序，请重新打开。"
                             : L"新版程序无法启动，旧版恢复也未完成。" + error, true);
        return 10;
    }
    const auto startedAt = GetTickCount64();
    while (GetTickCount64() - startedAt < 30000) {
        if (matchingMarker(plan.sessionDirectory / L"startup-confirmed", plan.newVersion)) {
            releaseLease(plan); (void)writeMarker(plan.sessionDirectory / L"completed", plan.newVersion); return 0;
        }
        if (WaitForSingleObject(restarted.value, 100) != WAIT_TIMEOUT) break;
    }
    // A short-lived process can write the marker between the first check and
    // its process handle becoming signalled. Observe that final write as well.
    if (matchingMarker(plan.sessionDirectory / L"startup-confirmed", plan.newVersion)) {
        releaseLease(plan); (void)writeMarker(plan.sessionDirectory / L"completed", plan.newVersion); return 0;
    }
    // A started new application may already have migrated its database. Keep
    // binary backups and diagnostics; never silently downgrade its user data.
    failed(plan, L"新版已安装，但尚未确认启动成功。旧版程序备份仍保留；不会自动回退可能已升级的用户数据库。", true);
    return 11;
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    const bool valid = argc == 3 && std::wstring(argv[1]) == L"--plan";
    const fs::path plan = valid ? fs::path(argv[2]) : fs::path();
    LocalFree(argv);
    if (!valid) return 1;
    try { return run(plan); }
    catch (...) { return 12; }
}
