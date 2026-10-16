#pragma once

#include <windows.h>

#include <map>
#include <vector>

namespace snowdesktop::dock_minimize_completion
{
struct Target
{
    HWND dock = nullptr;
    HMONITOR monitor = nullptr;
    RECT anchor{};
};

struct WindowState
{
    DWORD process = 0; // Zero also covers a destroyed source.
    bool minimized = false;
    bool minimizing = false;
    bool restoring = false;
};

// Observe the native action independently of whether a custom animation could
// start. A presentation's cancellation/timeout is not proof of minimization.
class Pending final
{
public:
    bool Contains(HWND source, DWORD process) const
    {
        const auto found = entries_.find(source);
        return found != entries_.end() && found->second.process == process;
    }
    bool Completed(HWND source) const
    {
        const auto found = entries_.find(source);
        return found != entries_.end() && found->second.completed;
    }
    void Track(HWND source, DWORD process, Target target, ULONGLONG now)
    {
        if (source && process && target.dock && target.monitor)
            entries_[source] = {process, target, now + 3000};
    }
    void Forget(HWND source) { entries_.erase(source); }
    void Clear() { entries_.clear(); }
    bool Waiting() const
    {
        for (const auto& [source, entry] : entries_)
        {
            (void)source;
            if (!entry.completed) return true;
        }
        return false;
    }

    template<class Observe, class Complete>
    void Poll(ULONGLONG now, Observe&& observe, Complete&& complete)
    {
        std::vector<Target> finished;
        for (auto it = entries_.begin(); it != entries_.end();)
        {
            auto& entry = it->second;
            const WindowState state = observe(it->first);
            if (state.process != entry.process || state.restoring ||
                (!state.minimized && (entry.observedMinimized || entry.completed)))
            {
                it = entries_.erase(it);
                continue;
            }
            entry.observedMinimized = entry.observedMinimized || state.minimized;
            if (!entry.completed && !state.minimizing)
            {
                if (state.minimized)
                {
                    entry.completed = true;
                    finished.push_back(entry.target);
                }
                else if (now >= entry.deadline)
                {
                    it = entries_.erase(it);
                    continue;
                }
            }
            ++it;
        }
        // Completion can update Dock windows and dispatch more native events.
        // No iterator into the pending set survives that callback.
        for (const Target& target : finished) complete(target);
    }

private:
    struct Entry
    {
        DWORD process;
        Target target;
        ULONGLONG deadline;
        bool observedMinimized = false;
        bool completed = false;
    };
    std::map<HWND, Entry> entries_;
};

template<class CurrentDock, class Fullscreen, class Dismiss>
bool DismissForFullscreen(const Target& target, CurrentDock&& currentDock,
    Fullscreen&& fullscreen, Dismiss&& dismiss)
{
    if (!target.dock || !target.monitor || !currentDock(target) || !fullscreen(target.monitor))
        return false;
    dismiss(target);
    return true;
}
} // namespace snowdesktop::dock_minimize_completion
