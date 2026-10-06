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

inline bool IsAbove(HWND upperWindow, HWND lowerWindow)
{
    if (!upperWindow || !lowerWindow)
        return false;
    for (HWND current = upperWindow; current;
         current = GetWindow(current, GW_HWNDNEXT))
    {
        if (current == lowerWindow)
            return true;
    }
    return false;
}

// Called from the host's WM_WINDOWPOSCHANGING handler, so geometry updates,
// compositor fallbacks and direct SetWindowPos calls share the same guard.
// The caller supplies the current modern menu, not an arbitrary owned window.
inline bool PreserveOwnedMenuZOrder(
    HWND contentWindow, HWND menuWindow, WINDOWPOS& position)
{
    if ((position.flags & (SWP_NOZORDER | SWP_HIDEWINDOW)) != 0 ||
        !menuWindow || !IsWindowVisible(menuWindow) ||
        GetWindow(menuWindow, GW_OWNER) != contentWindow ||
        !IsAbove(menuWindow, contentWindow))
    {
        return false;
    }
    position.flags |= SWP_NOZORDER;
    return true;
}

/**
 * @brief 将 popup 内容窗及其 backdrop 安全地放入同一 Z 序带并保持相邻。
 *
 * DeferWindowPos 不能在一次事务中可靠地把一个窗口跨 TOPMOST 边界，
 * 同时再用另一个正在跨边界的窗口作为 hWndInsertAfter。跨带时按视觉
 * 安全顺序归一化：降级先移动 backdrop；提升在同一事务内分别指定
 * TOPMOST（backdrop 在先、内容窗在后），不从边界窗口推断所属带。
 * 两窗已在同一带时仍使用延迟事务完成普通重排。
 */
inline bool Apply(
    HWND contentWindow,
    HWND backdropWindow,
    HWND contentInsertAfter,
    bool topmost,
    POINT backdropOrigin,
    SIZE backdropSize,
    HWND preserveAboveWindow = nullptr)
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

    const bool usesBandSentinel =
        contentInsertAfter == HWND_TOPMOST ||
        contentInsertAfter == HWND_NOTOPMOST;
    const bool protectedWindowAlreadyAbove =
        usesBandSentinel &&
        preserveAboveWindow &&
        IsWindow(preserveAboveWindow) &&
        IsAbove(preserveAboveWindow, contentWindow);
    const bool backdropValid =
        backdropWindow && IsWindow(backdropWindow);
    const auto synchronizeBackdropGeometry = [&]() {
        RECT current{};
        const bool hasRect = GetWindowRect(backdropWindow, &current) != FALSE;
        const bool sameOrigin = hasRect && current.left == backdropOrigin.x &&
            current.top == backdropOrigin.y;
        const bool sameSize = hasRect && current.right - current.left == backdropSize.cx &&
            current.bottom - current.top == backdropSize.cy;
        // Even an identical SetWindowPos sends positioning messages and can
        // disturb the native backdrop during a focus/layer-policy refresh.
        if (sameOrigin && sameSize)
            return true;
        const UINT flags = backdropFlags | SWP_NOZORDER |
            (sameOrigin ? SWP_NOMOVE : 0) | (sameSize ? SWP_NOSIZE : 0);
        return SetWindowPos(backdropWindow, nullptr,
            backdropOrigin.x, backdropOrigin.y,
            backdropSize.cx, backdropSize.cy, flags) != FALSE;
    };
    if (protectedWindowAlreadyAbove)
    {
        // Preserve the actual order for either requested band while this
        // menu is visible. A topmost owned menu does not imply that its owner
        // is topmost, so do not infer the requested band from menu styles.
        if (!backdropValid)
            return true;
        return synchronizeBackdropGeometry();
    }

    // Explorer can reorder or destroy the concrete desktop anchor between
    // policy refreshes. Its band must never override the requested Dock band.
    const HWND bandInsertAfter = topmost ? HWND_TOPMOST : HWND_NOTOPMOST;
    if (contentInsertAfter == HWND_TOP ||
        contentInsertAfter == HWND_TOPMOST || contentInsertAfter == HWND_NOTOPMOST ||
        (contentInsertAfter != HWND_BOTTOM &&
            (!IsWindow(contentInsertAfter) || contentInsertAfter == contentWindow ||
                contentInsertAfter == backdropWindow || IsTopmost(contentInsertAfter) != topmost)))
    {
        contentInsertAfter = bandInsertAfter;
    }

    if (!backdropValid)
    {
        if (contentInsertAfter != bandInsertAfter && IsTopmost(contentWindow) != topmost &&
            !SetWindowPos(contentWindow, bandInsertAfter, 0, 0, 0, 0, contentFlags))
            return false;
        return SetWindowPos(
            contentWindow, contentInsertAfter,
            0, 0, 0, 0, contentFlags) != FALSE && IsTopmost(contentWindow) == topmost;
    }

    const bool pairAlreadySynchronized =
        IsTopmost(contentWindow) == topmost &&
        IsTopmost(backdropWindow) == topmost &&
        IsPaired(contentWindow, backdropWindow);
    const bool concreteAnchorAlreadySynchronized =
        contentInsertAfter && IsWindow(contentInsertAfter) &&
        GetWindow(contentWindow, GW_HWNDPREV) == contentInsertAfter;
    if (pairAlreadySynchronized &&
        (contentInsertAfter == bandInsertAfter || concreteAnchorAlreadySynchronized))
    {
        // A layer-policy refresh must not raise an already-correct popup
        // pair above a menu that Windows has since placed over its content.
        // The same applies to an already-correct concrete desktop anchor.
        // Only touch the helper if its geometry actually changed.
        return synchronizeBackdropGeometry();
    }

    const bool changesZOrderBand =
        IsTopmost(contentWindow) != topmost ||
        IsTopmost(backdropWindow) != topmost;
    if (changesZOrderBand && topmost)
    {
        // Inserting after the LAST topmost window preserves a non-topmost
        // helper's band. Both windows therefore need explicit promotion,
        // even when content/backdrop are already adjacent. Promote together
        // with content last so the helper never presents above its content.
        HDWP promotion = BeginDeferWindowPos(2);
        if (promotion)
            promotion = DeferWindowPos(promotion, backdropWindow, HWND_TOPMOST,
                backdropOrigin.x, backdropOrigin.y, backdropSize.cx, backdropSize.cy,
                backdropFlags);
        if (promotion)
            promotion = DeferWindowPos(promotion, contentWindow, HWND_TOPMOST,
                0, 0, 0, 0, contentFlags);
        if (!promotion || !EndDeferWindowPos(promotion))
        {
            // Retain the existing direct-call fallback if User32 cannot
            // allocate or commit a positioning transaction.
            if (!SetWindowPos(backdropWindow, HWND_TOPMOST,
                    backdropOrigin.x, backdropOrigin.y, backdropSize.cx, backdropSize.cy,
                    backdropFlags) ||
                !SetWindowPos(contentWindow, HWND_TOPMOST, 0, 0, 0, 0, contentFlags))
                return false;
        }
        if (contentInsertAfter == bandInsertAfter)
            return IsTopmost(contentWindow) && IsTopmost(backdropWindow) &&
                IsPaired(contentWindow, backdropWindow);
        // Both bands are now explicit; concrete-anchor placement can use the
        // ordinary same-band transaction below.
    }
    else if (changesZOrderBand)
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

        // Normalize the content band independently of concrete placement: a
        // stale/rejected anchor must not undo dismissal of a floating Dock.
        // On demotion the backdrop has already crossed the boundary first.
        if (contentInsertAfter != bandInsertAfter && IsTopmost(contentWindow) != topmost)
        {
            succeeded = SetWindowPos(contentWindow, bandInsertAfter,
                0, 0, 0, 0, contentFlags) != FALSE && succeeded;
        }
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
        return IsTopmost(contentWindow) == topmost &&
            IsTopmost(backdropWindow) == topmost && IsPaired(contentWindow, backdropWindow);

    const bool contentPositioned = SetWindowPos(
        contentWindow, contentInsertAfter,
        0, 0, 0, 0, contentFlags) != FALSE;
    const bool backdropPositioned = SetWindowPos(
        backdropWindow, contentWindow,
        backdropOrigin.x, backdropOrigin.y,
        backdropSize.cx, backdropSize.cy,
        backdropFlags) != FALSE;
    return contentPositioned && backdropPositioned &&
        IsTopmost(contentWindow) == topmost &&
        IsTopmost(backdropWindow) == topmost && IsPaired(contentWindow, backdropWindow);
}

// A transparent popup has no backdrop to establish pair adjacency. Refreshing
// its band must be idempotent too, so it does not overtake drag HUD windows.
inline bool MaintainContentBand(HWND contentWindow, bool topmost,
    HWND preserveAboveWindow = nullptr)
{
    if (!contentWindow || !IsWindow(contentWindow)) return false;
    if (IsTopmost(contentWindow) == topmost) return true;
    return Apply(contentWindow, nullptr,
        topmost ? HWND_TOPMOST : HWND_NOTOPMOST, topmost,
        POINT{}, SIZE{}, preserveAboveWindow);
}

} // namespace snowdesktop::popup_window_pair_z_order
