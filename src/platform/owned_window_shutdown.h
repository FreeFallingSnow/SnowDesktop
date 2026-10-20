#pragma once

#include <windows.h>

#include <initializer_list>

namespace snowdesktop
{

// Detach the entire set first: destroying one owned window can synchronously
// dispatch messages to another. The owner may already have retired services.
inline void RetireOwnedWindows(std::initializer_list<HWND*> windows) noexcept
{
    for (HWND* window : windows)
        if (*window && IsWindow(*window))
            SetWindowLongPtrW(*window, GWLP_USERDATA, 0);
    for (HWND* window : windows)
    {
        if (*window && IsWindow(*window))
            DestroyWindow(*window);
        *window = nullptr;
    }
}

} // namespace snowdesktop
