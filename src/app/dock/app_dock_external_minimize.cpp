#include "app/app.h"
#include "dock/dock_minimize_protocol.h"
#include "platform/deployment_context.h"
#include "settings/animation_settings.h"

namespace
{
bool TracksWindow(HWND target, HWND primary, const std::vector<HWND>& tracked)
{
    return target == primary || std::find(tracked.begin(), tracked.end(), target) != tracked.end();
}
}

void DesktopApp::UpdateDockExternalMinimizeTargets()
{
    if (!generalSettings_.dockEnabled || !dockWindowTransition_ ||
        !snowdesktop::animation::RuntimeAnimationsEnabled() ||
        snowdesktop::animation::RuntimeWindowEffect() == snowdesktop::animation::NoEffect)
    {
        dockExternalMinimize_.reset();
        return;
    }
    const HWND receiver = controlHwnd_ && IsWindow(controlHwnd_) ? controlHwnd_ : hwnd_;
    if (!receiver) return;
    if (!dockExternalMinimize_)
    {
        auto monitor = std::make_unique<DockExternalMinimize>();
        if (!monitor->Start(receiver,
                snowdesktop::deployment::GetInjectableRuntimeFilePath(L"SnowDesktopDockMinimizeHook.dll"),
                snowdesktop::deployment::GetInjectableRuntimeFilePath(L"SnowDesktopDockMinimizeHost32.exe"),
                snowdesktop::deployment::GetInjectableRuntimeFilePath(L"SnowDesktopDockMinimizeHook32.dll")))
            return;
        dockExternalMinimize_ = std::move(monitor);
    }
    std::vector<HWND> windows;
    for (const auto& [key, state] : dockRunningWindows_)
    {
        (void)key;
        if (!state.running) continue;
        windows.push_back(state.window);
        windows.insert(windows.end(), state.trackedWindows.begin(), state.trackedWindows.end());
    }
    for (const auto& state : dockUnpinnedRunningApps_)
    {
        windows.push_back(state.window);
        windows.insert(windows.end(), state.trackedWindows.begin(), state.trackedWindows.end());
    }
    std::sort(windows.begin(), windows.end(), std::less<HWND>{});
    windows.erase(std::unique(windows.begin(), windows.end()), windows.end());
    dockExternalMinimize_->UpdateTargets(windows);
}

bool DesktopApp::HandleDockExternalMinimize(HWND window, DWORD deadline)
{
    if (!snowdesktop::dock_minimize::RequestIsCurrent(deadline, GetTickCount()) ||
        !generalSettings_.dockEnabled || reloading_ || dragSession_.HasContext() ||
        graphicsDeviceRecovery_.Pending() || !dockExternalMinimize_ ||
        !dockExternalMinimize_->OwnsTarget(window) || !dockWindowTransition_ ||
        dockWindowTransition_->IsActive() || !IsWindowVisible(window) || IsIconic(window) ||
        !snowdesktop::animation::RuntimeAnimationsEnabled() ||
        snowdesktop::animation::RuntimeWindowEffect() == snowdesktop::animation::NoEffect)
        return false;

    const HMONITOR sourceMonitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    RECT anchor{};
    HWND keepBelow = nullptr;
    bool foundOnMonitor = false;
    for (const auto& container : containers_)
    {
        auto* dock = dynamic_cast<DockContainer*>(container.get());
        if (!dock) continue;
        const auto* host = FindPersistentDockHost(dock);
        if ((!host || !host->active) && (!customDesktopVisible_ || desktopIconsHidden_)) continue;
        for (const auto& slot : dock->GetSlots())
        {
            if (!slot || !slot->GetItem()) continue;
            const Item* item = slot->GetItem();
            bool matches = false;
            if (const auto* running = dynamic_cast<const DockRunningItem*>(item))
            {
                const size_t index = running->GetRunningIndex();
                if (index < dockUnpinnedRunningApps_.size())
                {
                    const auto& state = dockUnpinnedRunningApps_[index];
                    matches = state.presence.Interactive() &&
                        TracksWindow(window, state.window, state.trackedWindows);
                }
            }
            else
            {
                size_t index = items_.size();
                if (const auto* entry = dynamic_cast<const DockEntryItem*>(item))
                {
                    if (entry->GetEntryType() == DockEntryType::DesktopItem)
                        index = FindItemIndexByKey(entry->GetReference());
                }
                else if (const auto* frequent = dynamic_cast<const DockFrequentItem*>(item))
                    index = frequent->GetItemIndex();
                if (index < items_.size())
                {
                    const auto state = dockRunningWindows_.find(DockItemWindowKey(items_[index]));
                    matches = state != dockRunningWindows_.end() && state->second.running &&
                        TracksWindow(window, state->second.window, state->second.trackedWindows);
                }
            }
            if (!matches) continue;
            RECT candidate = dock->GetElementVisualRect(item->GetBounds(), lastMousePoint_);
            RECT intersection{};
            const RECT viewport = dock->GetInteractiveBounds();
            if (!IntersectRect(&intersection, &candidate, &viewport)) continue;
            MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&candidate), 2);
            const bool sameMonitor = MonitorFromRect(&candidate, MONITOR_DEFAULTTONEAREST) == sourceMonitor;
            if (IsRectEmpty(&anchor) || (sameMonitor && !foundOnMonitor))
            {
                anchor = candidate;
                foundOnMonitor = sameMonitor;
                keepBelow = host && IsWindowVisible(host->hwnd) ? host->hwnd : nullptr;
            }
        }
    }
    if (IsRectEmpty(&anchor) ||
        !snowdesktop::dock_minimize::RequestIsCurrent(deadline, GetTickCount())) return false;
    DismissDockWindowPreviewUntilLeave();
    CancelDockWindowActivationObservation(window);
    return dockWindowTransition_->StartExternalMinimize(window, anchor, deadline, keepBelow);
}
