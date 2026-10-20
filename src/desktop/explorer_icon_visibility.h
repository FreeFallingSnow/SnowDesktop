#pragma once

#include <windows.h>
#include "common/constants.h"

namespace snowdesktop
{
// Restoration belongs to the presentation transition, not to a cached HWND.
// Explorer may replace the ListView while the custom desktop is active.
class ExplorerIconVisibility
{
public:
    void Hide(HWND listView) noexcept
    {
        restorePending_ = false;
        if (!listView || !IsWindow(listView)) return;
        const bool marked = GetPropW(listView, kHiddenBySnowDesktopProp) != nullptr;
        if (!(GetWindowLongPtrW(listView, GWL_STYLE) & WS_VISIBLE) && !marked)
            return;
        // Do not hide a window without leaving recovery evidence behind.
        if (!SetPropW(listView, kHiddenBySnowDesktopProp, reinterpret_cast<HANDLE>(1)))
            return;
        hiddenByUs_ = true;
        ShowWindow(listView, SW_HIDE);
    }

    void RequestRestore(bool force = false) noexcept
    {
        restorePending_ = restorePending_ || hiddenByUs_ || force;
    }

    bool RestorePending() const noexcept { return restorePending_; }

    // The caller discovers the current Explorer desktop on every attempt.
    // Missing windows and hidden ancestors retain the request for the next poll.
    bool TryRestore(HWND listView) noexcept
    {
        const bool valid = listView && IsWindow(listView);
        const bool marked = valid && GetPropW(listView, kHiddenBySnowDesktopProp) != nullptr;
        if (!restorePending_ && !marked) return true;
        restorePending_ = true;
        if (!valid) return false;
        // Explorer has a different input queue. Post the request so a busy
        // Shell cannot block shutdown; a later observation acknowledges it.
        if (!IsWindowVisible(listView) && !SetWindowPos(listView, nullptr, 0, 0, 0, 0,
                SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE | SWP_NOSIZE |
                    SWP_ASYNCWINDOWPOS))
            return false;
        if (!IsWindowVisible(listView))
            return false;
        RedrawWindow(listView, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        RemovePropW(listView, kHiddenBySnowDesktopProp);
        hiddenByUs_ = false;
        restorePending_ = false;
        return true;
    }

    // Exit and the standalone recovery command have no future host timer.
    // Rediscover on each retry and stop within the caller's finite budget.
    template <typename FindListView>
    bool RestoreWhenReady(FindListView&& findListView, DWORD timeoutMs = 1500) noexcept
    {
        RequestRestore();
        const ULONGLONG start = GetTickCount64();
        for (;;)
        {
            if (TryRestore(findListView())) return true;
            const ULONGLONG elapsed = GetTickCount64() - start;
            if (elapsed >= timeoutMs) return false;
            const auto remaining = static_cast<DWORD>(timeoutMs - elapsed);
            Sleep(remaining < 50 ? remaining : 50);
        }
    }

private:
    bool hiddenByUs_ = false;
    bool restorePending_ = false;
};
}
