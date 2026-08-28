#pragma once

#include <array>
#include <cstddef>
#include <cwchar>
#include <filesystem>
#include <string>

namespace snowdesktop::demo_asset_paths
{

inline constexpr wchar_t kEnvironmentVariable[] =
    L"SNOWDESKTOP_DEMO_ICON_DIR";

inline std::size_t VisualIndexFromFilename(
    const std::filesystem::path& path) noexcept
{
    const std::wstring name = path.filename().wstring();
    if (name.size() < 5 || name[3] != L'_' ||
        name[0] < L'0' || name[0] > L'9' ||
        name[1] < L'0' || name[1] > L'9' ||
        name[2] < L'0' || name[2] > L'9' ||
        _wcsicmp(path.extension().c_str(), L".png") != 0)
        return static_cast<std::size_t>(-1);
    return static_cast<std::size_t>(name[0] - L'0') * 100 +
        static_cast<std::size_t>(name[1] - L'0') * 10 +
        static_cast<std::size_t>(name[2] - L'0');
}

template <std::size_t IconCount>
inline std::array<std::filesystem::path, IconCount> EnumerateIcons(
    const std::filesystem::path& directory)
{
    std::array<std::filesystem::path, IconCount> result{};
    if (directory.empty()) return result;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(
            directory, std::filesystem::directory_options::skip_permission_denied,
            error), end;
        !error && iterator != end; iterator.increment(error))
    {
        if (!iterator->is_regular_file(error) || error)
            continue;
        const std::size_t visualIndex =
            VisualIndexFromFilename(iterator->path());
        if (visualIndex < result.size())
            result[visualIndex] = iterator->path();
    }
    return result;
}

template <std::size_t IconCount>
inline bool HasRequiredIcons(
    const std::array<std::filesystem::path, IconCount>& icons) noexcept
{
    if constexpr (IconCount == 0) return false;
    for (const auto& icon : icons)
        if (icon.empty()) return false;
    return true;
}

inline std::filesystem::path ResolveDirectory(
    const std::filesystem::path& executableDirectory,
    const std::filesystem::path& configuredDirectory = {})
{
    std::error_code error;
    if (!configuredDirectory.empty())
    {
        const auto configured = std::filesystem::absolute(
            configuredDirectory, error);
        if (!error && std::filesystem::is_directory(configured, error) && !error)
            return configured;
    }

    const std::array<std::filesystem::path, 4> relativeCandidates{
        std::filesystem::path(L"developer_assets") / L"demo_icons",
        std::filesystem::path(L"..") / L"developer_assets" / L"demo_icons",
        std::filesystem::path(L"..") / L".." / L"developer_assets" / L"demo_icons",
        std::filesystem::path(L"..") / L".." / L".." / L"developer_assets" / L"demo_icons",
    };
    for (const auto& relative : relativeCandidates)
    {
        error.clear();
        const auto candidate = std::filesystem::weakly_canonical(
            executableDirectory / relative, error);
        if (!error && std::filesystem::is_directory(candidate, error) && !error)
            return candidate;
    }
    return {};
}

} // namespace snowdesktop::demo_asset_paths
