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

// First-image path: raw .lnk/.url, folder configuration, library iconReference,
// executables/icons and static file-type resources. XML reads are bounded and
// prohibit DTDs; registry hints never activate association/icon providers.
// Never creates a Shell COM object, resolves a PIDL, or follows library locations.
// Unsupported links and nonlocal/reparse/offline sources return no candidates;
// the caller must schedule its Shell fallback outside the local worker pool.
std::vector<IconResourceLocation> ReadLocalIconResources(
    std::wstring_view path);
} // namespace snowdesktop::shortcut_icon_resource
