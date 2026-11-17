#include "ui/input/private_pointer.h"
#include "app/app.h"
#include "app/navigation/quick_navigation_state.h"

void DesktopApp::UpdateDesktopIdleHide()
{
    if (!generalSettings_.desktopIdleHideEnabled && !desktopIdleHide_.AutoHidden())
    {
        desktopIdleHide_.Reset(GetTickCount64());
        return;
    }

    using namespace snowdesktop::desktop_idle_hide;
    Sample sample;
    sample.enabled = generalSettings_.desktopIdleHideEnabled;
    sample.delaySeconds = generalSettings_.desktopIdleHideDelaySeconds;
    sample.desktopHidden = desktopIconsHidden_;
    sample.suspended = !customDesktopVisible_ || desktopPassthroughActive_ ||
        reloading_ || !desktopItemsReady_ || !hwnd_ || !IsWindow(hwnd_);
    sample.interactionBusy = mouseDown_ || dragSession_.HasContext() ||
        dragDropController_.IsTransportActive() ||
        widgetAction_ != WidgetAction::None || largeIconGesture_ ||
        middleButtonWidgetMove_ || detailColumnResizeActive_ ||
        widgetScrollbarDragging_ || popupScrollbarDragging_ || renameController_.IsActive() ||
        HasActiveContextMenuSession() || shellPopupMenuLayerDepth_ > 0 ||
        quickNavigation_->open || IsCollectionPopupInteractive() ||
        !luaWidgetPanelRequest_.widgetId.empty();

    if (sample.enabled && !sample.suspended &&
        generalSettings_.desktopIdleHidePauseWithWindows)
    {
        sample.suspended = observedMonitorWindows_.empty() ||
            std::any_of(observedMonitorWindows_.begin(),
                observedMonitorWindows_.end(),
                [](const auto& entry) { return entry.second.visible; });
    }

    POINT pointer{};
    sample.pointerValid = snowdesktop::private_pointer::CursorPosition(&pointer) != FALSE;
    sample.pointerX = pointer.x;
    sample.pointerY = pointer.y;
    sample.pointerOnDesktop = sample.desktopHidden && desktopIdleHide_.AutoHidden() &&
        !sample.suspended && !sample.interactionBusy && sample.pointerValid &&
        IsBaseDesktopHoverSurfaceWindow(snowdesktop::private_pointer::WindowAtPoint(pointer));
    LASTINPUTINFO input{sizeof(input)};
    sample.inputKnown = !sample.desktopHidden && !sample.suspended &&
        !sample.interactionBusy && sample.pointerValid && GetLastInputInfo(&input) != FALSE;
    sample.now = GetTickCount64();
    if (sample.inputKnown)
    {
        // Unsigned subtraction preserves tick wraparound. A timestamp
        // ahead of this sample (e.g. from SendInput) counts as activity.
        const DWORD elapsed = static_cast<DWORD>(sample.now) - input.dwTime;
        sample.inputIdleMs = elapsed <= MAXLONG ? elapsed : 0;
    }
    const Action action = desktopIdleHide_.Update(sample);
    if (action != Action::None)
        ToggleDesktopIconsVisibility(true);
}
