#include "app/app.h"

bool DesktopApp::IsDockStartMenuActive(HMONITOR monitor) const
{
    return dockStartMenu_ && dockStartMenu_->Active() &&
        (!monitor || dockStartMenu_->Monitor() == monitor);
}

void DesktopApp::ToggleWindowsStartMenu(DockContainer* dock)
{
    if (IsDockStartMenuActive())
    {
        // The physical down may already have dismissed the native window.
        // Cancel never sends a second Win press unless Start still owns focus.
        dockStartMenu_->Cancel(true);
        return;
    }
    const auto native = [this] {
        CloseFloatingDockThen([] { snowdesktop::NativeStartMenu::Toggle(); },
            FloatingDockCloseFocusPolicy::PreserveCurrent);
    };
    if (!dock || !generalSettings_.dockEnabled || !dockSettings_.startMenuKeepTaskbarHidden ||
        (!dockSettings_.suppressSystemTaskbar && !IsSystemTaskbarAutoHideEnabled()))
    { native(); return; }
    auto* host = FindPersistentDockHost(dock);
    if (!host || !host->active || !host->monitor) { native(); return; }
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(host->monitor, &info)) { native(); return; }
    snowdesktop::start_menu::Placement placement;
    placement.screen = info.rcMonitor;
    placement.work = info.rcWork;
    placement.edge = static_cast<snowdesktop::start_menu::Edge>(dockSettings_.position);
    placement.avoid = dock->GetInteractiveBounds();
    OffsetRect(&placement.avoid, virtualLeft_, virtualTop_);
    if (dock->IsMergedWithStatusBar() && statusBar_)
        if (const auto strip = statusBar_->MergedStripBounds(host->monitor))
        {
            RECT combined{};
            UnionRect(&combined, &placement.avoid, &*strip);
            placement.avoid = combined;
        }
    UINT dpiX = 96, dpiY = 96;
    if (FAILED(GetDpiForMonitor(host->monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) dpiX = 96;
    placement.gap = MulDiv(8, static_cast<int>(dpiX), 96);
    if (!snowdesktop::start_menu::Position(placement, SIZE{1, 1})) { native(); return; }

    RefreshSystemTaskbarAppearance(true);
    if (!dockStartMenu_) dockStartMenu_ = std::make_unique<snowdesktop::NativeStartMenu>();
    if (!dockStartMenu_->Open(controlHwnd_ ? controlHwnd_ : hwnd_, host->monitor, placement))
    { native(); return; }
    dockStartMenuHeldMonitor_ = host->monitor;
    dockStartMenuPosition_ = dockSettings_.position;
    dockStartMenuPlacement_ = placement;
    dockStartMenuWasPromoted_ = host->promoted;
    host->promoted = true;
    host->mergedCloseAfterInteraction = false;
    HideDockWindowPreview();
    EndFloatingDockKeyboardSession(FloatingDockCloseFocusPolicy::PreserveCurrent);
    RefreshFloatingDockVisibilityState();
    if (statusBar_) statusBar_->RefreshDockState(host->monitor);
    UpdatePersistentDockHostVisibility(*host);
}

void DesktopApp::UpdateDockStartMenu()
{
    if (!dockStartMenu_ || !dockStartMenuHeldMonitor_) return;
    const HMONITOR monitor = dockStartMenuHeldMonitor_;
    auto found = std::find_if(persistentDockHosts_.begin(), persistentDockHosts_.end(),
        [monitor](const auto& host) { return host && host->active && host->monitor == monitor; });
    MONITORINFO info{sizeof(info)};
    if (!generalSettings_.dockEnabled || !dockSettings_.startMenuKeepTaskbarHidden ||
        !dockSettings_.showWindowsButton || dockSettings_.position != dockStartMenuPosition_ ||
        desktopPassthroughActive_ || !GetMonitorInfoW(monitor, &info) ||
        !EqualRect(&info.rcMonitor, &dockStartMenuPlacement_.screen) ||
        !EqualRect(&info.rcWork, &dockStartMenuPlacement_.work) ||
        found == persistentDockHosts_.end() ||
        (!dockSettings_.suppressSystemTaskbar && !IsSystemTaskbarAutoHideEnabled()))
        dockStartMenu_->Cancel();
    if (found != persistentDockHosts_.end() && (*found)->container)
    {
        RECT avoid = (*found)->container->GetInteractiveBounds();
        OffsetRect(&avoid, virtualLeft_, virtualTop_);
        if ((*found)->container->IsMergedWithStatusBar() && statusBar_)
            if (const auto strip = statusBar_->MergedStripBounds(monitor))
            {
                RECT combined{};
                UnionRect(&combined, &avoid, &*strip);
                avoid = combined;
            }
        if (!EqualRect(&avoid, &dockStartMenuPlacement_.avoid)) dockStartMenu_->Cancel();
    }
    if (dockStartMenu_->Active()) return;
    dockStartMenuHeldMonitor_ = nullptr;
    if (found != persistentDockHosts_.end())
    {
        auto& host = **found;
        // An outside click may already have explicitly closed the Dock.
        if (!dockStartMenuWasPromoted_ && host.promoted)
            CloseFloatingDock(host, FloatingDockCloseFocusPolicy::PreserveCurrent);
        if (statusBar_) statusBar_->RefreshDockState(monitor);
        UpdatePersistentDockHostVisibility(host);
    }
    RefreshSystemTaskbarAppearance(true);
}
