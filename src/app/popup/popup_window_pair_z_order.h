#pragma once

#include <windows.h>
#include <dwmapi.h>

namespace snowdesktop::popup_window_pair_z_order
{
inline bool SetExcludedFromPeek(HWND contentWindow, HWND backdropWindow, bool excluded)
{
    const BOOL value = excluded ? TRUE : FALSE;
    const bool contentReady = contentWindow && SUCCEEDED(DwmSetWindowAttribute(
        contentWindow, DWMWA_EXCLUDED_FROM_PEEK, &value, sizeof(value)));
    const bool backdropReady = !backdropWindow || SUCCEEDED(DwmSetWindowAttribute(
        backdropWindow, DWMWA_EXCLUDED_FROM_PEEK, &value, sizeof(value)));
    return contentReady && backdropReady;
}

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

    // If User32 cannot commit a pair transaction, keep the helper hidden
    // while falling back to separate calls. A visible helper above its content
    // can sample that content into its BackdropBrush even if the final order
    // is repaired immediately afterwards.
    const auto placeSeparately = [&]() {
        const bool restoreVisible = IsWindowVisible(backdropWindow) != FALSE;
        if (restoreVisible && !SetWindowPos(backdropWindow, nullptr, 0, 0, 0, 0,
                contentFlags | SWP_NOZORDER | SWP_HIDEWINDOW))
            return false;
        const bool positioned =
            SetWindowPos(backdropWindow, bandInsertAfter,
                backdropOrigin.x, backdropOrigin.y, backdropSize.cx, backdropSize.cy,
                backdropFlags) &&
            SetWindowPos(contentWindow, bandInsertAfter, 0, 0, 0, 0, contentFlags) &&
            (contentInsertAfter == bandInsertAfter ||
                SetWindowPos(contentWindow, contentInsertAfter, 0, 0, 0, 0, contentFlags)) &&
            SetWindowPos(backdropWindow, contentWindow,
                backdropOrigin.x, backdropOrigin.y, backdropSize.cx, backdropSize.cy,
                backdropFlags) &&
            IsTopmost(contentWindow) == topmost &&
            IsTopmost(backdropWindow) == topmost && IsPaired(contentWindow, backdropWindow);
        if (!positioned) return false;
        return !restoreVisible || SetWindowPos(backdropWindow, nullptr, 0, 0, 0, 0,
            contentFlags | SWP_NOZORDER | SWP_SHOWWINDOW) != FALSE;
    };

    const bool changesZOrderBand =
        IsTopmost(contentWindow) != topmost ||
        IsTopmost(backdropWindow) != topmost;
    if (changesZOrderBand && topmost)
    {
        // Inserting after the LAST topmost window preserves a non-topmost
        // helper's band. Normalize both bands explicitly before using a
        // concrete anchor. Demotion must also be paired: moving content below
        // the anchor separately leaves the glass above it until the next call.
        HDWP normalization = BeginDeferWindowPos(2);
        if (normalization)
            normalization = DeferWindowPos(normalization, backdropWindow, bandInsertAfter,
                backdropOrigin.x, backdropOrigin.y, backdropSize.cx, backdropSize.cy,
                backdropFlags);
        if (normalization)
            normalization = DeferWindowPos(normalization, contentWindow, bandInsertAfter,
                0, 0, 0, 0, contentFlags);
        if (!normalization || !EndDeferWindowPos(normalization))
            return placeSeparately();
        if (contentInsertAfter == bandInsertAfter)
            return IsTopmost(contentWindow) == topmost && IsTopmost(backdropWindow) == topmost &&
                IsPaired(contentWindow, backdropWindow);
        // Both bands are now explicit; concrete-anchor placement can use the
        // ordinary same-band transaction below.
    }
    else if (changesZOrderBand)
    {
        // Lower the helper first while content remains above it. Normalize
        // content only to the top of the ordinary band; the concrete anchor
        // placement below must move the complete pair together. Moving only
        // content behind that anchor exposes the still-higher helper.
        if (!SetWindowPos(backdropWindow, HWND_NOTOPMOST,
                0, 0, 0, 0, contentFlags) ||
            (IsTopmost(contentWindow) && !SetWindowPos(contentWindow, HWND_NOTOPMOST,
                0, 0, 0, 0, contentFlags)))
            return placeSeparately();
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

    return placeSeparately();
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
