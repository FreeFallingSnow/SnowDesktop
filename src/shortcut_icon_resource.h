#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::shortcut_icon_resource
{
struct IconResourceLocation
{
    std::wstring path;
    int index = 0;
};

/**
 * Read the raw icon resource declared by an Internet Shortcut (.url).
 * Environment variables are expanded and relative IconFile values are
 * resolved beside the shortcut file.
 */
std::optional<IconResourceLocation> ReadInternetShortcutIconResource(
    std::wstring_view shortcutPath);

// Local sources in preference order: explicit icon, link target or the
// HTTP(S) association's browser. Does not resolve links or access the network.
std::vector<IconResourceLocation> ReadShortcutIconResources(
    std::wstring_view shortcutPath);
} // namespace snowdesktop::shortcut_icon_resource
