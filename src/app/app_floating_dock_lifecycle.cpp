#include "app.h"

// Floating-Dock hotkey and edge-swipe lifecycle.

void DesktopApp::UnregisterFloatingDockHotkey()
{
    if (floatingDockHotkeyRegistered_ && floatingDockHotkeyHwnd_)
        UnregisterHotKey(
            floatingDockHotkeyHwnd_, kFloatingDockHotkeyId);
    if (floatingDockEdgeSwipeHwnd_ &&
        IsWindow(floatingDockEdgeSwipeHwnd_))
    {
        KillTimer(
            floatingDockEdgeSwipeHwnd_,
            kFloatingDockEdgeSwipeTimerId);
    }
    floatingDockHotkeyRegistered_ = false;
    floatingDockHotkeyHwnd_ = nullptr;
    floatingDockEdgeSwipeHwnd_ = nullptr;
    floatingDockEdgeSwipeDetector_.Reset();
    floatingDockPointerButtonsDown_ = 0;
}

void DesktopApp::ApplyFloatingDockHotkey()
{
    UnregisterFloatingDockHotkey();
    if (!generalSettings_.dockEnabled ||
        !snowdesktop::floating_dock_rules::
            HasAnySummonTrigger(
                dockSettings_.floatingShortcutMode,
                dockSettings_.floatingEdgeSwipeEnabled))
    {
        CloseFloatingDock(true, true);
        return;
    }

    // Register on the independent top-level control window. The desktop input
    // HWND is a 1x1 child of Explorer's current desktop host and can be
    // reparented or recreated while Explorer settles, which makes it a poor
    // long-lived WM_HOTKEY endpoint.
    HWND target =
        controlHwnd_ && IsWindow(controlHwnd_)
            ? controlHwnd_
            : (inputHwnd_ && IsWindow(inputHwnd_)
                ? inputHwnd_ : hwnd_);
    if (!target)
        return;

    if (dockSettings_.floatingShortcutMode)
    {
        const UINT hotkeyModifiers =
            dockSettings_.floatingHotkeyModifiers |
            MOD_NOREPEAT;
        floatingDockHotkeyRegistered_ =
            RegisterHotKey(target, kFloatingDockHotkeyId,
                hotkeyModifiers,
                dockSettings_.floatingHotkeyVirtualKey) != FALSE;
        NavigationSettings hotkeyTextSettings;
        hotkeyTextSettings.modifiers =
            dockSettings_.floatingHotkeyModifiers;
        hotkeyTextSettings.virtualKey =
            dockSettings_.floatingHotkeyVirtualKey;
        const std::wstring hotkeyText =
            FormatNavigationHotkey(hotkeyTextSettings);
        if (floatingDockHotkeyRegistered_)
        {
            floatingDockHotkeyHwnd_ = target;
            const std::wstring message =
                L"Floating Dock hotkey " + hotkeyText +
                L" registered";
            WriteDiagnosticLogEntry(message.c_str());
        }
        else
        {
            const std::wstring message =
                L"Floating Dock hotkey " + hotkeyText +
                L" registration failed";
            WriteDiagnosticLogEntry(message.c_str());
        }
    }

    // This lightweight control timer serves both optional edge-swipe
    // recognition and mandatory outside-click dismissal. Keeping both in one
    // sampler prevents competing GetAsyncKeyState calls from consuming the
    // same click transition.
    if (SetTimer(
            target, kFloatingDockEdgeSwipeTimerId,
            kFloatingDockEdgeSwipeIntervalMs,
            nullptr) != 0)
    {
        floatingDockEdgeSwipeHwnd_ = target;
    }
}

void DesktopApp::UpdateFloatingDockEdgeSwipe()
{
    constexpr UINT leftButtonBit = 1u << 0;
    constexpr UINT rightButtonBit = 1u << 1;
    constexpr UINT middleButtonBit = 1u << 2;
    const SHORT leftState =
        GetAsyncKeyState(VK_LBUTTON);
    const SHORT rightState =
        GetAsyncKeyState(VK_RBUTTON);
    const SHORT middleState =
        GetAsyncKeyState(VK_MBUTTON);
    const UINT buttonsDown =
        ((leftState & 0x8000) ? leftButtonBit : 0) |
        ((rightState & 0x8000) ? rightButtonBit : 0) |
        ((middleState & 0x8000) ? middleButtonBit : 0);
    const UINT pressedSinceLastSample =
        ((leftState & 1) ? leftButtonBit : 0) |
        ((rightState & 1) ? rightButtonBit : 0) |
        ((middleState & 1) ? middleButtonBit : 0);
    const bool pointerPressed =
        snowdesktop::floating_dock_rules::
            HasNewPointerButtonPress(
                buttonsDown,
                floatingDockPointerButtonsDown_,
                pressedSinceLastSample);
    floatingDockPointerButtonsDown_ = buttonsDown;

    POINT cursor{};
    if (!GetCursorPos(&cursor))
    {
        floatingDockEdgeSwipeDetector_.Reset();
        return;
    }

    if (floatingDockVisible_ && pointerPressed)
    {
        POINT desktopPoint = cursor;
        if (hwnd_ && IsWindow(hwnd_))
            ScreenToClient(hwnd_, &desktopPoint);
        else
        {
            desktopPoint.x -= virtualLeft_;
            desktopPoint.y -= virtualTop_;
        }
        if (snowdesktop::floating_dock_rules::
                ShouldDismissForPointerDown(
                    dragSession_.IsActive() ||
                        dragDropController_.IsExternalDragActive(),
                    desktopPoint,
                    floatingDockRect_,
                    floatingDockPopupRect_))
        {
            CloseFloatingDock();
            floatingDockEdgeSwipeDetector_.Reset();
            return;
        }
    }

    if (!generalSettings_.dockEnabled ||
        !dockSettings_.floatingEdgeSwipeEnabled ||
        dragSession_.IsActive() ||
        buttonsDown != 0)
    {
        floatingDockEdgeSwipeDetector_.Reset();
        return;
    }

    const HMONITOR monitor = MonitorFromPoint(
        cursor, MONITOR_DEFAULTTONULL);
    MONITORINFO monitorInfo{ sizeof(monitorInfo) };
    if (!monitor ||
        !GetMonitorInfoW(monitor, &monitorInfo))
    {
        floatingDockEdgeSwipeDetector_.Reset();
        return;
    }

    // A monitor outside the configured Dock scope must not redirect the
    // gesture to a Dock on another display.
    DockContainer* dock =
        SelectFloatingDockContainerForMonitor(monitor);
    if (!dock)
    {
        floatingDockEdgeSwipeDetector_.Reset();
        return;
    }
    const RECT dockBounds = dock->GetBounds();
    const POINT dockCenter{
        (dockBounds.left + dockBounds.right) / 2 +
            virtualLeft_,
        (dockBounds.top + dockBounds.bottom) / 2 +
            virtualTop_
    };
    if (MonitorFromPoint(
            dockCenter, MONITOR_DEFAULTTONEAREST) != monitor)
    {
        floatingDockEdgeSwipeDetector_.Reset();
        return;
    }

    UINT dpiX = 96;
    UINT dpiY = 96;
    if (FAILED(GetDpiForMonitor(
            monitor, MDT_EFFECTIVE_DPI,
            &dpiX, &dpiY)))
        dpiX = 96;
    const int edgeBand =
        snowdesktop::floating_dock_rules::
            ScaleEdgeSwipeDip(
                snowdesktop::floating_dock_rules::
                    kEdgeSwipeBandDip,
                dpiX);
    const int requiredTravel =
        snowdesktop::floating_dock_rules::
            ScaleEdgeSwipeDip(
                snowdesktop::floating_dock_rules::
                    kEdgeSwipeTravelDip,
                dpiX);
    const bool triggered =
        floatingDockEdgeSwipeDetector_.Update(
            cursor, monitorInfo.rcMonitor,
            dockSettings_.position,
            GetTickCount(), edgeBand,
            requiredTravel);
    if (triggered && !floatingDockVisible_)
    {
        WriteDiagnosticLogEntry(
            L"Floating Dock edge swipe received");
        ShowFloatingDock();
    }
}
