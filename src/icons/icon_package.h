#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::icon_package
{
inline constexpr std::uint64_t MaxArchive = 128ull * 1024 * 1024;
inline constexpr std::uint64_t MaxExpanded = 256ull * 1024 * 1024;
struct Image { std::string id, name, bytes; };
struct Package
{
    std::string id, title, description, license;
    std::vector<Image> images;
};
std::string Hash(std::string_view bytes);
bool SafePath(const std::filesystem::path&, bool directory = false);
bool Read(const std::filesystem::path&, Package&, std::string& error);
bool Write(const std::filesystem::path&, const Package&, std::string& error);
bool Validate(const Package&, std::string& error);
std::string Fingerprint(const Package&);
}
