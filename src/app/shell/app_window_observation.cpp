#include "app/app.h"
#include "dock/dock_appbar.h"
#include "platform/window_observation.h"

void DesktopApp::RefreshWindowObservations()
{
    if (windowObservationInProgress_) return;
    windowObservationInProgress_ = true;
    struct Reset { bool& active; ~Reset() { active = false; } } reset{windowObservationInProgress_};
    const DWORD revision = windowObservationRevision_.load(std::memory_order_relaxed);
    const LONG left = virtualLeft_, top = virtualTop_;
    const HWND desktop = hwnd_;
    std::vector<RECT> pages;
    pages.reserve(gridPages_.size());
    for (const auto& page : gridPages_)
    {
        RECT screen = page.bounds;
        OffsetRect(&screen, left, top);
        pages.push_back(screen);
    }
    auto snapshot = snowdesktop::window_observation::Observe(pages,
        customDesktopVisible_ && desktop ? GetAncestor(desktop, GA_ROOT) : nullptr);
    if (desktop != hwnd_ || left != virtualLeft_ || top != virtualTop_) return;

    const bool sceneChanged = snapshot.monitors.size() != observedMonitorWindows_.size() ||
        std::any_of(snapshot.monitors.begin(), snapshot.monitors.end(), [this](const auto& entry) {
            const auto previous = observedMonitorWindows_.find(entry.first);
            return previous == observedMonitorWindows_.end() ||
                previous->second.visible != entry.second.visible ||
                previous->second.maximized != entry.second.maximized;
        });
    const bool fullscreenChanged = sceneChanged ||
        std::any_of(snapshot.monitors.begin(), snapshot.monitors.end(), [this](const auto& entry) {
            return observedMonitorWindows_.at(entry.first).fullscreenSource != entry.second.fullscreenSource;
        });
    if (sceneChanged) ++monitorSceneRevision_;
    if (sceneChanged && desktop && generalSettings_.dockEnabled &&
        dockSettings_.UsesReservedSpaceBackdrop() && dockAppBars_)
    {
        const auto maximized = [](const auto& monitors, HMONITOR monitor) {
            const auto found = monitors.find(monitor);
            return found != monitors.end() && found->second.maximized;
        };
        for (const auto& page : pages)
        {
            const auto monitor = MonitorFromRect(&page, MONITOR_DEFAULTTONULL);
            if (!monitor || maximized(observedMonitorWindows_, monitor) ==
                maximized(snapshot.monitors, monitor))
                continue;
            if (auto area = dockAppBars_->ApprovedArea(monitor))
            {
                OffsetRect(&*area, -left, -top);
                InvalidateRect(desktop, &*area, FALSE);
            }
        }
    }
    observedMonitorWindows_ = std::move(snapshot.monitors);
    {
        std::scoped_lock lock(windowPlacementMutex_);
        observedWindowPlacements_ = std::move(snapshot.placements);
    }
    observedWindowRevision_ = revision;
    windowObservationTick_ = GetTickCount();
    for (auto& bounds : snapshot.desktopCoverage)
        OffsetRect(&bounds, -left, -top);
    UpdateDesktopWidgetCoverage(std::move(snapshot.desktopCoverage));
    if (statusBar_ && fullscreenChanged) statusBar_->NotifyWindowObservations();
}

std::optional<HWND> DesktopApp::ObservedFullscreenSource(HMONITOR monitor) const
{
    const auto found = observedMonitorWindows_.find(monitor);
    return found == observedMonitorWindows_.end() ? std::nullopt : found->second.fullscreenSource;
}

std::optional<snowdesktop::fullscreen::WindowSnapshot> DesktopApp::RecheckFullscreenSource(
    HMONITOR monitor) const
{
    return snowdesktop::window_observation::RecheckFullscreenSource(
        monitor, ObservedFullscreenSource(monitor).value_or(nullptr));
}
