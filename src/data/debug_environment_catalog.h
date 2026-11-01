#pragma once

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::debug_profile
{
// Names are presentation only. Opaque, validated IDs alone select managed roots.
struct Environment
{
    std::wstring id = L"default";
    std::wstring name;
    bool pendingReset = false;
    bool pendingDesktopChange = false;
    std::filesystem::path desktop;
};

inline constexpr std::size_t MaximumEnvironments = 32;
inline constexpr std::size_t MaximumEnvironmentNameLength = 60;

inline bool ValidEnvironmentId(std::wstring_view id)
{
    if (id == L"default") return true;
    if (id.size() < 5 || id.size() > 64 || id.substr(0, 4) != L"env-") return false;
    return std::all_of(id.begin() + 4, id.end(), [](wchar_t c) {
        return (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-';
    });
}

inline std::wstring TrimEnvironmentName(std::wstring_view name)
{
    while (!name.empty() && std::iswspace(name.front())) name.remove_prefix(1);
    while (!name.empty() && std::iswspace(name.back())) name.remove_suffix(1);
    return std::wstring(name);
}

inline bool ValidEnvironmentName(std::wstring_view name)
{
    return !name.empty() && name.size() <= MaximumEnvironmentNameLength &&
        std::none_of(name.begin(), name.end(), [](wchar_t c) { return c < 32 || c == 127; });
}
}
