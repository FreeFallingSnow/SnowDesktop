#pragma once

#include <windows.h>
#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace snowdesktop
{
namespace desktop_passthrough_rules
{
// Four narrow windows leave the wallpaper's entire interior available for
// input, including monitors with negative coordinates and different DPIs.
inline std::array<RECT, 4> EdgeBounds(const RECT& monitor, UINT dpi)
{
    const int width = monitor.right - monitor.left;
    const int height = monitor.bottom - monitor.top;
    if (width <= 0 || height <= 0)
        return {};
    const int band = std::min({std::max(1, MulDiv(4, dpi ? dpi : 96, 96)),
        width / 2, height / 2});
    return {{
        {monitor.left, monitor.top, monitor.right, monitor.top + band},
        {monitor.left, monitor.bottom - band, monitor.right, monitor.bottom},
        {monitor.left, monitor.top + band, monitor.left + band, monitor.bottom - band},
        {monitor.right - band, monitor.top + band, monitor.right, monitor.bottom - band}
    }};
}
}

// Host-private escape surface. It owns no full-screen input window, never
// activates, and posts dismissal only after a complete click on an edge.
class DesktopPassthroughIndicator final
{
public:
    DesktopPassthroughIndicator() = default;
    ~DesktopPassthroughIndicator() { Hide(); }
    DesktopPassthroughIndicator(const DesktopPassthroughIndicator&) = delete;
    DesktopPassthroughIndicator& operator=(const DesktopPassthroughIndicator&) = delete;

    bool Show(HINSTANCE instance, HWND owner, UINT dismissMessage,
        std::wstring hint);
    void Hide();
    bool OwnsWindow(HWND window) const;

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message,
        WPARAM wParam, LPARAM lParam);
    HWND owner_ = nullptr;
    HWND tooltip_ = nullptr;
    UINT dismissMessage_ = 0;
    std::wstring hint_;
    std::vector<HWND> edges_;
};
}
