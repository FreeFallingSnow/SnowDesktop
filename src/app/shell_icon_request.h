#pragma once
#include "../types.h"
#include <string>
#include <utility>

namespace snowdesktop::shell_icon_request
{
enum class Phase { Phase1, Phase2, Shortcut };

// Refresh first images are provisional. Keep the last visible image (including
// thumbnails) until refinement succeeds, and release every rejected result.
// The caller supplies cache invalidation before an owned bitmap is destroyed.
template<class Item, class EraseCachedBitmap>
void ApplyBitmap(Item& item, Phase phase, HBITMAP& bitmap, SIZE size,
    bool mediaThumbnail, EraseCachedBitmap eraseCachedBitmap)
{
    if (!bitmap) return;
    if (phase == Phase::Shortcut || (phase == Phase::Phase1 && item.iconBitmap))
    {
        DeleteObject(std::exchange(bitmap, nullptr));
        return;
    }
    if (item.iconBitmap)
    {
        eraseCachedBitmap(item.iconBitmap);
        DeleteObject(item.iconBitmap);
    }
    item.iconBitmap = std::exchange(bitmap, nullptr);
    item.iconBitmapSize = size;
    item.iconIsMediaThumbnail = mediaThumbnail;
}

// A classification may arrive after full-quality pixels. It must not reset
// quality or replace pixels; conversely bitmap refreshes retain known metadata.
template<class Item>
void ApplyPresentation(Item& item, Phase phase, bool isShortcut, bool isApplicationShortcut)
{
    if (phase == Phase::Shortcut)
    {
        item.isShortcut = isShortcut;
        item.isApplicationShortcut = isApplicationShortcut;
        item.shortcutArrow = isShortcut && !isApplicationShortcut;
    }
    else if (phase == Phase::Phase2 || item.iconState != IconState::FullQuality)
    {
        item.iconState = phase == Phase::Phase1 ? IconState::IconReady : IconState::FullQuality;
    }
}

inline std::wstring Stamp(const std::optional<FILETIME>& modified,
    const std::optional<std::uint64_t>& bytes)
{
    return L"\nV:" + (modified ? std::to_wstring(
        (static_cast<std::uint64_t>(modified->dwHighDateTime) << 32) |
            modified->dwLowDateTime) : L"unknown") + L":" +
        (bytes ? std::to_wstring(*bytes) : L"unknown");
}
inline std::wstring Stamp(const DesktopItem& item) { return Stamp(item.modifiedTime, item.fileSize); }
inline std::wstring Stamp(const FolderEntry& item) { return Stamp(item.lastWriteTime, item.fileSize); }
template<class Item>
bool Matches(const std::wstring& request, const Item& item) { return request.ends_with(Stamp(item)); }
}
