#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace snowdesktop::dock_window_order
{
// Keep lifetime order independently of focus, titles, app-identity refinement,
// preview visibility, and temporary task-window eligibility. Windows which
// predate monitoring receive their initial order when first discovered.
class Tracker
{
public:
    void Observe(HWND window)
    {
        if (!window || GetAncestor(window, GA_ROOT) != window) return;
        DWORD process = 0;
        const DWORD thread = GetWindowThreadProcessId(window, &process);
        if (!process || !thread) return;
        const auto found = windows_.find(window);
        if (found != windows_.end() && found->second.process == process && found->second.thread == thread)
            return;
        windows_[window] = {process, thread, nextOrder_++};
    }

    void Forget(HWND window) { windows_.erase(window); }

    void Prune()
    {
        std::erase_if(windows_, [](const auto& entry) {
            DWORD process = 0;
            const DWORD thread = GetWindowThreadProcessId(entry.first, &process);
            return !thread || process != entry.second.process || thread != entry.second.thread;
        });
    }

    template <typename Item>
    void Sort(std::vector<Item>& items)
    {
        Prune();
        for (const auto& item : items) Observe(item.window);
        std::stable_sort(items.begin(), items.end(), [this](const Item& left, const Item& right) {
            return Order(left.window) < Order(right.window);
        });
    }

private:
    struct Entry
    {
        DWORD process = 0;
        DWORD thread = 0;
        std::uint64_t order = 0;
    };

    std::uint64_t Order(HWND window) const
    {
        const auto found = windows_.find(window);
        return found == windows_.end() ? UINT64_MAX : found->second.order;
    }

    std::unordered_map<HWND, Entry> windows_;
    std::uint64_t nextOrder_ = 0;
};

// Discovery and forwarded WinEvents run on the host UI thread. Share its
// history across Dock hosts and preserve it through desktop HWND recovery.
inline Tracker& Current()
{
    static thread_local Tracker tracker;
    return tracker;
}
}
