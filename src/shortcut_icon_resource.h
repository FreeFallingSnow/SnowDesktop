#pragma once

#include <optional>
#include <string>
#include <string_view>

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
} // namespace snowdesktop::shortcut_icon_resource
