#pragma once

#include <windows.h>
#include <unordered_map>

namespace snowdesktop::dock_window_attention
{

struct WindowIdentity
{
    DWORD process = 0;
    DWORD thread = 0;
    bool operator==(const WindowIdentity&) const = default;
};

// Shell flash notifications are requests for attention, not unread counts.
// Keep each window's request until it is acknowledged or that window expires.
class Tracker
{
public:
    bool Flash(HWND window, WindowIdentity identity, bool foreground)
    {
        if (!window || !identity.process || !identity.thread || foreground)
            return false;
        const auto found = windows_.find(window);
        if (found != windows_.end() && found->second == identity)
            return false;
        windows_[window] = identity;
        return true;
    }

    bool Remove(HWND window) { return windows_.erase(window) != 0; }
    void Clear() { windows_.clear(); }

    template <typename Keep>
    bool Retain(Keep&& keep)
    {
        bool changed = false;
        for (auto it = windows_.begin(); it != windows_.end();)
        {
            if (keep(it->first, it->second))
                ++it;
            else
            {
                it = windows_.erase(it);
                changed = true;
            }
        }
        return changed;
    }

    template <typename Windows>
    bool HasAny(HWND representative, const Windows& trackedWindows) const
    {
        if (windows_.contains(representative)) return true;
        for (HWND window : trackedWindows)
            if (windows_.contains(window)) return true;
        return false;
    }

private:
    std::unordered_map<HWND, WindowIdentity> windows_;
};

} // namespace snowdesktop::dock_window_attention
