// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "steam_workshop_core.h"
#include "../../src/theme_package_limits.h"

#include <algorithm>
#include <cwctype>
#include <system_error>
#include <utility>

namespace snowdesktop::steam_bridge
{
// Shared by the production publisher and SDK-free file-boundary regression.
inline bool ValidateWorkshopUploadFile(const std::filesystem::path& path,
    bool preview, WorkshopContentKind kind, CoreError& error)
{
    const auto fail = [&](std::string message) {
        error = {kInvalidArguments, preview ? "invalid_preview" : "invalid_package",
            std::move(message), {}};
        return false;
    };
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec)
        return fail(preview ? "preview file does not exist" : "package does not exist");
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0)
        return fail(preview ? "preview file is empty or unreadable" : "package is empty or unreadable");
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    if (preview)
    {
        if (extension != L".png" && extension != L".jpg" &&
            extension != L".jpeg" && extension != L".gif")
            return fail("Steam Workshop previews must be PNG, JPG, or GIF");
        if (size >= 1024ull * 1024ull)
            return fail("Steam Workshop preview must be smaller than 1 MiB");
        return true;
    }
    const bool theme = kind == WorkshopContentKind::Theme;
    if (extension != (theme ? L".snowtheme" : L".snowwidget"))
        return fail(theme ? "package must be a .snowtheme artifact" :
            "package must be a .snowwidget artifact");
    if (size > (theme ? themes::kMaximumPackageBytes : 20ull * 1024ull * 1024ull))
        return fail(theme ? "theme package exceeds the 4 MiB format limit" :
            "component package exceeds the 20 MiB format limit");
    return true;
}
}
