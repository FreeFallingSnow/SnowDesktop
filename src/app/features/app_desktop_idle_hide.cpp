#include "ui/input/private_pointer.h"
#include "app/app.h"

void DesktopApp::UpdateDesktopIdleHide()
{
    if (!generalSettings_.desktopIdleHideEnabled && !desktopIdleHide_.AutoHidden())
    {
        desktopIdleHide_.Reset(GetTickCount64());
        return;
    }

    using namespace snowdesktop::desktop_idle_hide;
    Sample sample;
    sample.now = GetTickCount64();
    sample.enabled = generalSettings_.desktopIdleHideEnabled;
    sample.delaySeconds = generalSettings_.desktopIdleHideDelaySeconds;
    sample.desktopHidden = desktopIconsHidden_;
    sample.suspended = !customDesktopVisible_ || desktopPassthroughActive_ ||
        reloading_ || !desktopItemsReady_ || !hwnd_ || !IsWindow(hwnd_);
    sample.interactionBusy = mouseDown_ || dragSession_.HasContext() ||
        dragDropController_.IsTransportActive() ||
        widgetAction_ != WidgetAction::None || largeIconGesture_ ||
        middleButtonWidgetMove_ || renameController_.IsActive() ||
        HasActiveContextMenuSession() || shellPopupMenuLayerDepth_ > 0 ||
        quickNavigationOpen_ || IsCollectionPopupInteractive() ||
        !luaWidgetPanelRequest_.widgetId.empty();

    if (sample.enabled && !sample.suspended &&
        generalSettings_.desktopIdleHidePauseWithWindows)
    {
        // Reuse taskbar/status-bar discovery, including its virtual desktop,
        // cloaking and minimized-window filters, even when Dock is disabled.
        if (systemTaskbarWindowScanTick_ == 0 ||
            GetTickCount() - systemTaskbarWindowScanTick_ >= 1000)
        {
            RefreshSystemTaskbarWindowState();
            systemTaskbarWindowScanTick_ = GetTickCount();
        }
        sample.suspended = systemTaskbarMonitorWindowStates_.empty() ||
            std::any_of(systemTaskbarMonitorWindowStates_.begin(),
                systemTaskbarMonitorWindowStates_.end(),
                [](const auto& entry) { return entry.second.visible; });
    }

    POINT pointer{};
    sample.pointerValid = snowdesktop::private_pointer::CursorPosition(&pointer) != FALSE;
    sample.pointerX = pointer.x;
    sample.pointerY = pointer.y;
    sample.pointerOnDesktop = sample.pointerValid &&
        IsBaseDesktopHoverSurfaceWindow(snowdesktop::private_pointer::WindowAtPoint(pointer));
    LASTINPUTINFO input{sizeof(input)};
    sample.inputKnown = GetLastInputInfo(&input) != FALSE;
    if (sample.inputKnown)
    {
        // GetLastInputInfo may contain a future timestamp supplied by SendInput.
        // Preserve normal DWORD wraparound while treating that case as activity.
        const DWORD elapsed = GetTickCount() - input.dwTime;
        sample.inputIdleMs = elapsed <= MAXLONG ? elapsed : 0;
    }
    const Action action = desktopIdleHide_.Update(sample);
    if (action != Action::None)
        ToggleDesktopIconsVisibility(true);
}
