#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
namespace snowdesktop::icon_pack
{
struct Entry { std::string name, bytes; };
struct Archive { std::vector<Entry> images, documents; std::vector<std::string> skipped; std::string original; };
// Reads bounded ZIP/APK data only. Never executes package code or extracts paths.
bool Read(const std::filesystem::path& path, Archive& result, std::string& error);
}
