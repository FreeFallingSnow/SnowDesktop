#include "app/app.h"

bool DesktopApp::IsDockStartMenuActive(HMONITOR monitor) const
{
    return dockStartMenuHeldMonitor_ && dockStartMenu_ && dockStartMenu_->Active() &&
        (!monitor || dockStartMenu_->Monitor() == monitor);
}

void DesktopApp::ToggleWindowsStartMenu(DockContainer* dock)
{
    wchar_t mode[192]{};
    swprintf_s(mode, L"[DockStartMenu] Dock click; keepHidden=%d alwaysHidden=%d autoHideMirror=%d active=%d",
        dockSettings_.startMenuKeepTaskbarHidden ? 1 : 0,
        dockSettings_.suppressSystemTaskbar ? 1 : 0,
        dockSettings_.systemTaskbarAutoHide ? 1 : 0,
        dockStartMenu_ && dockStartMenu_->Active() ? 1 : 0);
    WriteDiagnosticLogEntry(mode);
    if (dockStartMenu_ && dockStartMenu_->Active())
    {
        // The physical down may already have dismissed the native window.
        // Cancel never sends a second Win press unless Start still owns focus.
        dockStartMenu_->DismissThen({});
        UpdateDockStartMenu();
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
    // The periodic taskbar controller owns discovery/attachment. Never force
    // synchronous Shell visibility COM calls from the Start button handler.
    if (!dockStartMenu_) dockStartMenu_ = std::make_unique<snowdesktop::NativeStartMenu>();
    if (!dockStartMenu_->Open(host->monitor))
    { native(); return; }
    dockStartMenuHeldMonitor_ = host->monitor;
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
        !dockSettings_.showWindowsButton || desktopPassthroughActive_ ||
        !GetMonitorInfoW(monitor, &info) ||
        found == persistentDockHosts_.end() ||
        (!dockSettings_.suppressSystemTaskbar && !IsSystemTaskbarAutoHideEnabled()))
        dockStartMenu_->Cancel(false, snowdesktop::NativeStartMenu::CancelReason::ConfigurationChanged);
    dockStartMenu_->Finish([&] {
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
    }, [this] {
        CloseFloatingDockThen([] { snowdesktop::NativeStartMenu::Toggle(); },
            FloatingDockCloseFocusPolicy::PreserveCurrent);
    });
}
