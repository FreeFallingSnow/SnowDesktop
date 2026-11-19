#pragma once

#include "common/types.h"

#include <filesystem>
#include <functional>
#include <unordered_map>

namespace snowdesktop::dock_usage
{
using Records = std::unordered_map<std::wstring, DockUsageRecord>;

void Load(const std::filesystem::path& path, Records& records);
void Save(const std::filesystem::path& path, const Records& records,
    const std::function<bool(const std::wstring&)>& eligible);
}
