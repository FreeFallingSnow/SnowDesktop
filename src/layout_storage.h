#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace snowdesktop::layout_storage
{
enum class LoadStatus
{
    Missing,
    LoadedPrimary,
    RecoveredBackup,
    Invalid,
};

struct LoadResult
{
    LoadStatus status = LoadStatus::Missing;
    std::string error;
};

std::filesystem::path BackupPath(const std::filesystem::path& layoutPath);
bool ValidateDocument(std::string_view contents, std::string* error = nullptr);
LoadResult LoadDocument(const std::filesystem::path& layoutPath,
    std::string& contents);
bool SaveDocument(const std::filesystem::path& layoutPath,
    std::string_view contents, std::string* error = nullptr);
}
