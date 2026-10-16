#pragma once

#include "shortcut_application_rules.h"

#include <string>
#include <string_view>

namespace snowdesktop::shell_item_display_name
{
// Only presentation changes: callers retain the filesystem name and path.
// Compare with the real stem so a Shell-hidden Tool.lnk.lnk stays Tool.lnk.
inline std::wstring ForPath(std::wstring_view path,
    std::wstring_view displayName, bool directory)
{
    const auto separator = path.find_last_of(L"\\/");
    const auto fileName = separator == std::wstring_view::npos
        ? path : path.substr(separator + 1);
    if (displayName.empty()) displayName = fileName;
    if (!directory && fileName.size() > 4 && displayName.size() > 4 &&
        shortcut_application_rules::EqualsIgnoreCase(
            fileName.substr(fileName.size() - 4), L".lnk") &&
        shortcut_application_rules::EqualsIgnoreCase(
            displayName.substr(displayName.size() - 4), L".lnk") &&
        !shortcut_application_rules::EqualsIgnoreCase(
            displayName, fileName.substr(0, fileName.size() - 4)))
        displayName.remove_suffix(4);
    return std::wstring(displayName);
}
}
