#pragma once

#include "layout/page_navigation_rules.h"
#include "settings/general_settings_types.h"
#include "ui/input/private_pointer.h"

namespace snowdesktop::grid_preview_shortcut
{
// The preview belongs to this desktop render window and can outlive a key
// message when focus moves or a native modal loop takes over the queue.
inline constexpr UINT_PTR TimerId = 30;
inline constexpr wchar_t ActiveProperty[] = L"SnowDesktop.GridPreview.Active";

inline bool IsActive(HWND window) noexcept
{
    return window && GetPropW(window, ActiveProperty) != nullptr;
}

inline void SetActive(HWND window, bool active)
{
    if (!window || IsActive(window) == active) return;
    if (active)
    {
        if (!SetTimer(window, TimerId, 33, nullptr)) return;
        if (!SetPropW(window, ActiveProperty, window))
        {
            KillTimer(window, TimerId);
            return;
        }
    }
    else
    {
        KillTimer(window, TimerId);
        RemovePropW(window, ActiveProperty);
    }
    InvalidateRect(window, nullptr, FALSE);
}

inline bool MatchesHeldKeys(const GeneralSettings& settings) noexcept
{
    const UINT key = settings.gridPreviewHotkeyVirtualKey;
    const auto held = [](int vk) { return (private_pointer::AsyncKeyState(vk) & 0x8000) != 0; };
    if (!settings.gridPreviewHotkeyEnabled || key == 0 || key > 0xff ||
        !held(static_cast<int>(key)))
        return false;

    UINT modifiers = 0;
    if (key != VK_MENU && held(VK_MENU)) modifiers |= MOD_ALT;
    if (held(VK_CONTROL)) modifiers |= MOD_CONTROL;
    if (held(VK_SHIFT)) modifiers |= MOD_SHIFT;
    if (held(VK_LWIN) || held(VK_RWIN)) modifiers |= MOD_WIN;
    return modifiers == page_navigation_rules::NormalizeModifiers(
        settings.gridPreviewHotkeyModifiers);
}
}
