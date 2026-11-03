#pragma once

#include "dock_fullscreen_policy.h"
#include <windows.h>
#include <algorithm>
#include <utility>

namespace snowdesktop::dock_fullscreen
{
// The picker uses both Dock caches. Pinning removes an app from the unpinned
// running zone; its actual windows remain in the fixed/frequent-item cache.
// ReadWindow validates liveness and resolves missing executable/title metadata.
template<class RunningApps, class PinnedWindows, class ReadWindow>
std::vector<DockFullscreenApplication> CollectApplications(
    const RunningApps& runningApps, const PinnedWindows& pinnedWindows, ReadWindow&& readWindow)
{
    std::vector<DockFullscreenApplication> applications;
    std::vector<HWND> visited;
    const auto append = [&](HWND window, const std::wstring& name, const std::wstring& executable) {
        if (!window || applications.size() >= 128 ||
            std::find(visited.begin(), visited.end(), window) != visited.end()) return;
        visited.push_back(window);
        auto application = readWindow(window, name, executable);
        if (!application) return;
        application->executable = NormalizeExecutable(std::move(application->executable));
        if (application->executable.empty()) return;
        if (std::none_of(applications.begin(), applications.end(), [&](const auto& existing) {
                return SameExecutable(existing.executable, application->executable);
            }))
            applications.push_back(std::move(*application));
    };
    for (const auto& running : runningApps)
        if (running.presence.Visible())
            append(running.window, running.title, running.executablePath);
    for (const auto& [key, state] : pinnedWindows)
    {
        (void)key;
        if (!state.running) continue;
        append(state.window, {}, {});
        for (const HWND window : state.trackedWindows)
            append(window, {}, {});
    }
    return applications;
}


} // namespace snowdesktop::dock_fullscreen
