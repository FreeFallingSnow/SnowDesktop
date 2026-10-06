#pragma once

#include <windows.h>

namespace snowdesktop::menu_icon
{

// Internal menu artwork; the Fluent glyph remains available as a fallback.
enum class BuiltinIcon
{
    None,
    Sort,
    Display,
    Widgets,
    Pin,
    AddPage,
    Settings,
    Paste,
    NewItem,
    Refresh,
    Collection,
    CollectionGroup,
    FileGroup,
    DesktopFiles,
    FolderMapping,
    Search,
    Workshop,
};

// Loads embedded, SVG-derived PNG art at the requested physical pixel size.
// The caller owns the premultiplied bitmap. Missing resources return nullptr.
HBITMAP CreateBuiltinIconBitmap(BuiltinIcon icon, bool lightTheme, int pixelSize);

} // namespace snowdesktop::menu_icon
