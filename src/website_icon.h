#pragma once

#include <windows.h>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::website_icon
{
struct Shortcut
{
    std::filesystem::path path;
    std::wstring url;
    std::string contents;
    BY_HANDLE_FILE_INFORMATION identity{};
};

// Internal host helpers; no component API or automatic network access.
std::wstring ReadUrl(const std::filesystem::path& path);
std::optional<Shortcut> Capture(const std::filesystem::path& path);
std::vector<std::wstring> FindCandidates(std::string_view html, const std::wstring& pageUrl);
std::string ConvertToIco(std::span<const std::byte> image);
std::filesystem::path Fetch(const std::wstring& url,
    const std::filesystem::path& directory, std::stop_token stop);
bool Apply(const Shortcut& shortcut, const std::filesystem::path& icon);
}
