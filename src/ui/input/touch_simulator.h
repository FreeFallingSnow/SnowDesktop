#pragma once

#include <windows.h>

namespace snowdesktop::touch_simulation
{
inline constexpr UINT DefaultKey = VK_RSHIFT;
inline constexpr UINT ModifierMask = MOD_SHIFT | MOD_CONTROL | MOD_ALT | MOD_WIN;
inline constexpr wchar_t BindingCaptureProperty[] = L"SnowDesktop.TouchSimulation.BindingCapture";

constexpr bool IsMouseBinding(UINT key) noexcept
{
    return key == VK_RBUTTON || key == VK_MBUTTON || key == VK_XBUTTON1 || key == VK_XBUTTON2;
}

constexpr UINT KeyModifier(UINT key) noexcept
{
    switch (key)
    {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return MOD_SHIFT;
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return MOD_CONTROL;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return MOD_ALT;
    case VK_LWIN: case VK_RWIN: return MOD_WIN;
    default: return 0;
    }
}
constexpr bool ValidBinding(UINT modifiers, UINT key) noexcept
{
    if (key == 0) return modifiers == 0;
    return !(modifiers & (~ModifierMask | KeyModifier(key))) &&
        (IsMouseBinding(key) || (key >= VK_BACK && key <= 0xFE &&
            key != VK_PROCESSKEY && key != VK_PACKET && key != VK_LWIN && key != VK_RWIN &&
            key != VK_TAB && key != VK_ESCAPE && key != VK_RETURN));
}

bool Configure(bool enabled, UINT modifiers, UINT key);
bool Enabled() noexcept;
void Stop() noexcept;
}
