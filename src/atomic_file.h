#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace snowdesktop::atomic_file
{
bool ReadAll(const std::filesystem::path& path, std::string& contents,
    std::string* error = nullptr);

bool WriteAll(const std::filesystem::path& path, std::string_view contents,
    const std::filesystem::path& backupPath = {},
    std::string* error = nullptr);
}
