// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace snowdesktop::steam_bridge
{
class ManagerPreparedPreview
{
public:
    ManagerPreparedPreview(const ManagerPreparedPreview&) = delete;
    ManagerPreparedPreview& operator=(const ManagerPreparedPreview&) = delete;

    ~ManagerPreparedPreview()
    {
        if (!ownsFile_) return;
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    const std::filesystem::path& Path() const { return path_; }

private:
    explicit ManagerPreparedPreview(std::filesystem::path path)
        : path_(std::move(path)) {}

    friend std::shared_ptr<ManagerPreparedPreview> FreezeManagerPreview(
        const std::filesystem::path&, const std::filesystem::path&, std::string&);

    std::filesystem::path path_;
    bool ownsFile_ = false;
};

// The caller supplies its existing, exclusively owned package directory.
// Keep this shared owner as the last member of PreparedManagerPublish so the
// preview is removed before PackagedWidget tries to remove the empty directory.
inline std::shared_ptr<ManagerPreparedPreview> FreezeManagerPreview(
    const std::filesystem::path& source,
    const std::filesystem::path& directory, std::string& error)
{
    error.clear();
    auto extension = source.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    if (extension != L".png" && extension != L".jpg" &&
        extension != L".jpeg" && extension != L".gif")
    {
        error = "Steam Workshop previews must be PNG, JPG, or GIF";
        return {};
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec) || ec ||
        !std::filesystem::is_directory(directory, ec) || ec)
    {
        error = "The prepared Workshop preview source or artifact directory is unavailable";
        return {};
    }
    constexpr std::uintmax_t maximumBytes = 1024ull * 1024ull;
    const auto size = std::filesystem::file_size(source, ec);
    if (ec || size == 0 || size >= maximumBytes)
    {
        error = "Steam Workshop preview must be nonempty and smaller than 1 MiB";
        return {};
    }
    const auto destination = directory / (L"prepared-preview" + extension);
    const bool exists = std::filesystem::exists(destination, ec);
    if (ec || exists)
    {
        error = "The prepared Workshop preview destination already exists or is unavailable";
        return {};
    }

    // Allocate the owner before copying so allocation failure cannot leave an
    // unowned file. A failed no-overwrite copy never acquires an existing file.
    auto frozen = std::shared_ptr<ManagerPreparedPreview>(
        new ManagerPreparedPreview(destination));
    if (!std::filesystem::copy_file(source, destination,
            std::filesystem::copy_options::none, ec))
    {
        if (ec != std::errc::file_exists)
        {
            std::error_code ignored;
            std::filesystem::remove(destination, ignored);
        }
        error = "Cannot copy the prepared Workshop preview";
        if (ec) error += ": " + ec.message();
        return {};
    }
    frozen->ownsFile_ = true;
    const auto frozenSize = std::filesystem::file_size(destination, ec);
    if (ec || frozenSize == 0 || frozenSize >= maximumBytes)
    {
        error = "The copied Workshop preview is unreadable or exceeds the preview size limit";
        return {};
    }
    return frozen;
}
}
