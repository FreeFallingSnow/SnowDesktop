// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT
#pragma once

#include "steam_workshop_core.h"
#include "workshop_project.h"
#include "bridge_json.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>

namespace snowdesktop::steam_bridge
{
struct ManagerLocalPreviews
{
    std::filesystem::path primary;
    std::optional<std::vector<std::filesystem::path>> gallery;
    std::string error;
};

inline ManagerLocalPreviews ReadManagerLocalPreviews(const WorkshopProject& project)
{
    ManagerLocalPreviews result;
    const auto manifest = project.sourceDirectory / L"widget.json";
    std::error_code ec;
    const auto size = std::filesystem::file_size(manifest, ec);
    if (ec || size > 4 * 1024 * 1024)
    { result.error = "Cannot read widget.json previews"; return result; }
    std::ifstream input(manifest, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    JsonValue root;
    if (!input || !ParseJson(text, root, result.error) || !root.IsObject())
    { result.error = "Cannot read widget.json previews: " + result.error; return result; }
    const auto path = [&](std::string_view value) {
        return project.sourceDirectory / std::filesystem::path(std::u8string(value.begin(), value.end()));
    };
    if (const auto preview = JsonString(root, "preview")) result.primary = path(*preview);
    if (const auto* gallery = root.Find("previewGallery"))
    {
        if (!gallery->IsArray() || gallery->array.size() > 7)
        { result.error = "previewGallery must contain at most seven images"; return result; }
        result.gallery.emplace();
        for (const auto& image : gallery->array)
        {
            if (!image.IsString() || image.string.empty())
            { result.error = "previewGallery contains an invalid image path"; return result; }
            result.gallery->push_back(path(image.string));
        }
    }
    return result;
}

inline bool ValidateManagerPreviewUpdate(const WorkshopPreviewUpdate& update,
    const PublishedItem& current, std::uint64_t owner, std::uint32_t appId)
{
    if (!owner || !update.expected.publishedFileId || current.banned ||
        current.publishedFileId != update.expected.publishedFileId ||
        current.ownerSteamId != owner || update.expected.ownerSteamId != owner ||
        current.consumerAppId != appId || update.expected.consumerAppId != appId ||
        current.previewUrl != update.expected.previewUrl ||
        current.additionalPreviews != update.expected.additionalPreviews)
        return false;
    std::set<std::uint32_t> seen;
    std::size_t removals = 0;
    for (const auto& edit : update.edits)
    {
        if (edit.index >= current.additionalPreviews.size() ||
            current.additionalPreviews[edit.index].type != 0 ||
            !seen.insert(edit.index).second) return false;
        if (edit.replacement.empty()) ++removals;
    }
    if (current.additionalPreviews.size() - removals + update.additions.size() > 256) return false;
    return update.primary || !update.edits.empty() || !update.additions.empty();
}

// Replace using original indices before removing in descending order. Unedited
// images, videos and unknown media retain their identity and relative order.
template<typename Primary, typename Replace, typename Remove, typename Add>
bool ApplyManagerPreviewUpdate(const WorkshopPreviewUpdate& update,
    Primary&& primary, Replace&& replace, Remove&& remove, Add&& add)
{
    if (update.primary && !primary(*update.primary)) return false;
    std::vector<std::uint32_t> removals;
    for (const auto& edit : update.edits)
    {
        if (edit.replacement.empty()) removals.push_back(edit.index);
        else if (!replace(edit.index, edit.replacement)) return false;
    }
    std::sort(removals.rbegin(), removals.rend());
    for (const auto index : removals) if (!remove(index)) return false;
    for (const auto& image : update.additions) if (!add(image)) return false;
    return true;
}
}
