#pragma once

#include <windows.h>

namespace snowdesktop::popup_window_pair_z_order
{

inline bool IsTopmost(HWND window)
{
    return window && IsWindow(window) &&
        (GetWindowLongPtrW(window, GWL_EXSTYLE) &
            WS_EX_TOPMOST) != 0;
}

inline bool IsPaired(HWND contentWindow, HWND backdropWindow)
{
    return contentWindow && backdropWindow &&
        GetWindow(backdropWindow, GW_HWNDPREV) == contentWindow;
}

/**
 * @brief 将 popup 内容窗及其 backdrop 安全地放入同一 Z 序带并保持相邻。
 *
 * DeferWindowPos 不能在一次事务中可靠地把一个窗口跨 TOPMOST 边界，
 * 同时再用另一个正在跨边界的窗口作为 hWndInsertAfter。跨带时按视觉
 * 安全顺序分别归一化：降级先移动 backdrop，提升先移动内容窗；两窗
 * 已在同一带时仍使用延迟事务完成普通重排。
 */
inline bool Apply(
    HWND contentWindow,
    HWND backdropWindow,
    HWND contentInsertAfter,
    bool topmost,
    POINT backdropOrigin,
    SIZE backdropSize)
{
    if (!contentWindow || !IsWindow(contentWindow))
        return false;

    constexpr UINT contentFlags =
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
        SWP_NOOWNERZORDER;
    constexpr UINT backdropFlags =
        SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    constexpr UINT bandOnlyFlags =
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
        SWP_NOOWNERZORDER;

    if (!backdropWindow || !IsWindow(backdropWindow))
    {
        return SetWindowPos(
            contentWindow, contentInsertAfter,
            0, 0, 0, 0, contentFlags) != FALSE;
    }

    const bool changesZOrderBand =
        IsTopmost(contentWindow) != topmost ||
        IsTopmost(backdropWindow) != topmost;
    if (changesZOrderBand)
    {
        bool succeeded = true;
        if (!topmost)
        {
            // Remove the glass helper from TOPMOST before moving its content.
            // The still-topmost content keeps the helper from surfacing alone.
            succeeded = SetWindowPos(
                backdropWindow, HWND_NOTOPMOST,
                0, 0, 0, 0, bandOnlyFlags) != FALSE &&
                succeeded;
        }

        // On promotion the content enters TOPMOST first. On demotion the
        // backdrop is already in the destination band, so it cannot cover the
        // content while this call crosses the boundary.
        succeeded = SetWindowPos(
            contentWindow, contentInsertAfter,
            0, 0, 0, 0, contentFlags) != FALSE &&
            succeeded;
        succeeded = SetWindowPos(
            backdropWindow, contentWindow,
            backdropOrigin.x, backdropOrigin.y,
            backdropSize.cx, backdropSize.cy,
            backdropFlags) != FALSE && succeeded;
        return succeeded &&
            IsTopmost(contentWindow) == topmost &&
            IsTopmost(backdropWindow) == topmost &&
            IsPaired(contentWindow, backdropWindow);
    }

    HDWP deferred = BeginDeferWindowPos(2);
    if (deferred)
    {
        deferred = DeferWindowPos(
            deferred, contentWindow, contentInsertAfter,
            0, 0, 0, 0, contentFlags);
    }
    if (deferred)
    {
        deferred = DeferWindowPos(
            deferred, backdropWindow, contentWindow,
            backdropOrigin.x, backdropOrigin.y,
            backdropSize.cx, backdropSize.cy,
            backdropFlags);
    }
    if (deferred && EndDeferWindowPos(deferred) != FALSE)
        return IsPaired(contentWindow, backdropWindow);

    const bool contentPositioned = SetWindowPos(
        contentWindow, contentInsertAfter,
        0, 0, 0, 0, contentFlags) != FALSE;
    const bool backdropPositioned = SetWindowPos(
        backdropWindow, contentWindow,
        backdropOrigin.x, backdropOrigin.y,
        backdropSize.cx, backdropSize.cy,
        backdropFlags) != FALSE;
    return contentPositioned && backdropPositioned &&
        IsPaired(contentWindow, backdropWindow);
}

} // namespace snowdesktop::popup_window_pair_z_order
