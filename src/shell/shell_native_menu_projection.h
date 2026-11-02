#pragma once
#include "shell_extension_menu.h"
#include <algorithm>
#include <map>
#include <cwctype>

namespace snowdesktop::shell_extensions
{
// Only the added commands use this range. Shell owns 1..0x7fff and keeps its
// original IDs, owner-draw callbacks and IContextMenu2/3 message forwarding.
class NativeMenuProjection
{
public:
    ~NativeMenuProjection() { for (auto bitmap : bitmaps_) DeleteObject(bitmap); }
    NativeMenuProjection() = default;
    NativeMenuProjection(const NativeMenuProjection&) = delete;
    NativeMenuProjection& operator=(const NativeMenuProjection&) = delete;

    std::optional<UINT> Token(UINT id) const
    {
        const auto found = tokens_.find(id);
        return found == tokens_.end() ? std::nullopt : std::optional(found->second);
    }
    bool Insert(HMENU menu, UINT position, const Entry& entry, UINT dpi, unsigned depth = 0)
    {
        if (!menu || depth > 8 || next_ >= 0x8800) return false;
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_FTYPE;
        item.fType = entry.separator ? MFT_SEPARATOR : MFT_STRING;
        std::wstring label;
        HMENU children = nullptr;
        if (!entry.separator)
        {
            // Entry text has already had native access-key markers decoded.
            bool marked = false;
            for (const auto character : entry.label)
            {
                if (!marked && entry.accessKey && towlower(character) == towlower(entry.accessKey))
                { label += L'&'; marked = true; }
                label += character;
                if (character == L'&') label += L'&';
            }
            item.fMask |= MIIM_ID | MIIM_STRING | MIIM_STATE;
            item.wID = next_++;
            item.dwTypeData = label.data();
            item.fState = (entry.enabled ? MFS_ENABLED : MFS_DISABLED) | (entry.checked ? MFS_CHECKED : 0);
            if (!entry.children.empty())
            {
                children = CreatePopupMenu();
                if (!children) return false;
                for (const auto& child : entry.children)
                    if (!Insert(children, static_cast<UINT>(GetMenuItemCount(children)), child, dpi, depth + 1))
                    { DestroyMenu(children); return false; }
                item.fMask |= MIIM_SUBMENU;
                item.hSubMenu = children;
            }
            else if (entry.token) tokens_.emplace(item.wID, entry.token);
            else item.fState |= MFS_DISABLED;
            if (const auto bitmap = Bitmap(entry, dpi))
            { item.fMask |= MIIM_BITMAP; item.hbmpItem = bitmap; }
        }
        if (InsertMenuItemW(menu, position, TRUE, &item)) return true;
        if (children) DestroyMenu(children);
        tokens_.erase(item.wID);
        return false;
    }

private:
    UINT next_ = 0x8000;
    std::map<UINT, UINT> tokens_;
    std::vector<HBITMAP> bitmaps_;
    HBITMAP Bitmap(const Entry& entry, UINT dpi)
    {
        if (entry.width <= 0 || entry.height <= 0 || entry.width > 128 || entry.height > 128 ||
            entry.pixels.size() != static_cast<size_t>(entry.width) * entry.height * 4) return nullptr;
        const int size = std::clamp(MulDiv(entry.imageLogicalSize ? entry.imageLogicalSize : 16,
            static_cast<int>(dpi), 96), 1, 128);
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
        void* pixels = nullptr;
        const auto bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap || !pixels) { if (bitmap) DeleteObject(bitmap); return nullptr; }
        auto* output = static_cast<unsigned char*>(pixels);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                const auto source = static_cast<size_t>((y * entry.height / size) * entry.width + x * entry.width / size) * 4;
                std::copy_n(entry.pixels.data() + source, 4, output + static_cast<size_t>(y * size + x) * 4);
            }
        bitmaps_.push_back(bitmap);
        return bitmap;
    }
};
}
