#pragma once

// The updater executable deliberately has no Qt dependency. This contract is
// shared with the application, but compiled with each target's own CRT.
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace asmr::updater {
struct File {
    std::string path; // Strict UTF-8, forward-slash relative path.
    std::uint64_t length = 0;
    std::array<unsigned char, 32> sha256{};
};
struct Plan {
    std::filesystem::path targetDirectory;
    std::filesystem::path packageDirectory;
    std::filesystem::path sessionDirectory;
    std::string oldVersion;
    std::string newVersion;
    std::uint32_t parentPid = 0;
    std::uint64_t parentCreationTime = 0;
    std::vector<File> oldFiles;
    std::vector<File> newFiles;
};
struct InstallResult {
    bool success = false;
    bool changedFiles = false;
    bool rollbackSucceeded = false;
    std::wstring error;
};
struct InstallOptions {
    // An in-process test seam. No command line or plan can enable fault injection.
    std::function<bool(std::size_t)> permitMutation;
};
[[nodiscard]] bool safeRelativePath(const std::string &path);
[[nodiscard]] bool managedFilePath(const std::string &path);
[[nodiscard]] std::filesystem::path pathFromUtf8(const std::string &value);
[[nodiscard]] bool safeAncestors(const std::filesystem::path &path);
[[nodiscard]] bool hashFile(const std::filesystem::path &path,
                            std::array<unsigned char, 32> &digest,
                            std::wstring *error = nullptr);
[[nodiscard]] bool writePlan(const std::filesystem::path &path, const Plan &plan,
                             std::wstring *error = nullptr);
[[nodiscard]] bool readPlan(const std::filesystem::path &path, Plan &plan,
                            std::wstring *error = nullptr);
[[nodiscard]] bool validatePlan(const Plan &plan, std::wstring *error = nullptr);
// Validate both managed trees, reject unknown destination collisions and test
// write access. This performs no replacement and never removes user files.
[[nodiscard]] bool preflight(const Plan &plan, std::wstring *error = nullptr);
// The caller must first wait for the exact parent process to exit. The helper
// does that with a process handle; the transaction function stays testable.
[[nodiscard]] InstallResult install(const Plan &plan, const InstallOptions &options = {});
// Used only if launching the newly installed executable itself fails, before
// that version can open/migrate user data. Backups remain available afterwards.
[[nodiscard]] bool restoreBackup(const Plan &plan, std::wstring *error = nullptr);
[[nodiscard]] std::uint64_t currentProcessCreationTime();
// Startup guard: avoid entering application initialization during replacement.
// The native transaction hides the executable entry before changing DLLs.
[[nodiscard]] bool installationInProgress(const std::filesystem::path &targetDirectory);
} // namespace asmr::updater
