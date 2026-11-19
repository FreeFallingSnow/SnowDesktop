#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace snowdesktop::layout_backup
{
struct Result
{
    bool ok = false;
    bool cancelled = false;
    std::string error;
};

struct RestoreFiles : Result
{
    std::string layout;
    std::optional<std::string> storage;
};

bool ExistingPathHasReparsePoint(const std::filesystem::path& input) noexcept;
bool IsLayoutBackupId(std::wstring_view id);
std::wstring StorageCompanionName(std::wstring_view layoutName);
std::wstring MakeLayoutTimestampName();
std::optional<std::filesystem::path> FindLayoutBackup(
    const std::filesystem::path& directory, std::wstring_view id);
Result Create(
    const std::filesystem::path& layoutFile,
    const std::filesystem::path& storageFile,
    const std::filesystem::path& directory,
    std::wstring requestedName, std::stop_token stop,
    const std::function<bool()>& beginCommit);
RestoreFiles ReadForRestore(
    const std::filesystem::path& directory,
    std::wstring_view id, std::stop_token stop);
}
