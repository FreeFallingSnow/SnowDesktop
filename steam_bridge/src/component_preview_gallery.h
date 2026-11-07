// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::steam_bridge
{
struct ComponentPreviewImage
{
    std::filesystem::path path;
    std::string sha256;
    std::filesystem::path source;
};

inline std::string ComponentGalleryPrefix(std::string_view packageId)
{
    return "SnowDesktop-widget-" + std::string(packageId) + "-";
}

// Reserve only our exact filename grammar. Manual images and videos survive.
inline bool ComponentManagedGalleryFilename(std::string_view name, std::string_view prefix)
{
    if (prefix.empty() || !name.starts_with(prefix)) return false;
    name.remove_prefix(prefix.size());
    if (name.size() < 67 || name[0] < '0' || name[0] > '6' || name[1] != '-') return false;
    if (!std::all_of(name.begin() + 2, name.begin() + 66,
            [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); })) return false;
    const auto extension = name.substr(66);
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".gif";
}

class PreparedComponentPreviews
{
public:
    ~PreparedComponentPreviews();
    PreparedComponentPreviews(const PreparedComponentPreviews&) = delete;
    PreparedComponentPreviews& operator=(const PreparedComponentPreviews&) = delete;
    std::optional<ComponentPreviewImage> primary;
    std::optional<std::vector<ComponentPreviewImage>> gallery;
    bool Matches(const std::filesystem::path& primaryPath,
        const std::vector<std::filesystem::path>& galleryPaths) const;
private:
    PreparedComponentPreviews() = default;
    std::filesystem::path directory_;
    std::vector<std::filesystem::path> ownedFiles_;
    friend std::shared_ptr<PreparedComponentPreviews> FreezeComponentPreviews(
        const std::optional<std::filesystem::path>&,
        const std::optional<std::vector<std::filesystem::path>>&,
        const std::filesystem::path&, std::string_view, std::string&);
};

std::shared_ptr<PreparedComponentPreviews> FreezeComponentPreviews(
    const std::optional<std::filesystem::path>& primary,
    const std::optional<std::vector<std::filesystem::path>>& gallery,
    const std::filesystem::path& artifactDirectory, std::string_view packageId,
    std::string& error);
}
