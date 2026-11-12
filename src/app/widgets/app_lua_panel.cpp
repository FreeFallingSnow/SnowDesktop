#include "ui/input/private_pointer.h"
#include "app/app.h"
#include "diagnostics/diagnostic_log.h"
#include "widget/runtime/widget_engine.h"
#include "settings/animation_settings.h"
#include "layout/floating_popup_rules.h"

#include <utility>

// Lua widget panel lifecycle.

void DesktopApp::ReleaseLuaWidgetPanelCaptureIfOwned()
{
    const HWND recordedCapture =
        std::exchange(luaWidgetPanelCaptureHwnd_, nullptr);
    if (snowdesktop::floating_popup_rules::
            ShouldReleaseRecordedPanelCapture(
                recordedCapture, snowdesktop::private_pointer::Capture()))
    {
        ReleaseCapturePreservingPointerState();
    }
}

void DesktopApp::ForgetLuaWidgetPanelCapture(
    HWND hostWindow)
{
    if (!hostWindow ||
        luaWidgetPanelCaptureHwnd_ != hostWindow)
        return;
    luaWidgetPanelCaptureHwnd_ = nullptr;
    luaWidgetPanelMouseDown_ = false;
}

void DesktopApp::OpenLuaWidgetPanel(
    const LuaWidgetPanelRequest& request)
{
    if (request.widgetId.empty())
        return;
    const auto resolveAnchor = [&]() {
        POINT anchor = lastMousePoint_;
        const auto source = std::find_if(widgets_.begin(), widgets_.end(),
            [&](const DesktopWidget& widget) { return widget.id == request.widgetId; });
        if (source != widgets_.end())
        {
            const RECT frame = GetStandaloneWidgetFrameRect(*source);
            if (!PtInRect(&frame, anchor))
                anchor = {(frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2};
        }
        if (request.hasAnchor)
            anchor = {(request.anchorRect.left + request.anchorRect.right) / 2,
                (request.anchorRect.top + request.anchorRect.bottom) / 2};
        return anchor;
    };
    // Distinct desktop elements may open panels with the same title. A repeat
    // of the current opener leaves dismissal to the outside-click policy.
    const bool sameWidget = luaWidgetPanelRequest_.widgetId == request.widgetId;
    const bool samePanel = sameWidget &&
        luaWidgetPanelRequest_.surface == request.surface &&
        luaWidgetPanelRequest_.title == request.title &&
        luaWidgetPanelRequest_.anchorKey == request.anchorKey &&
        luaWidgetPanelRequest_.desktopOpenerKey == request.desktopOpenerKey;
    if (request.desktopGesture && samePanel)
        return;
    if (widgetEngine_)
        widgetEngine_->SetPanelTheme(collectionPopupAppearance_);
    {
        wchar_t message[320]{};
        swprintf_s(
            message,
            L"Popup input trace: lua-open widget=%ls surface=%hs generation=%u collection=%d currentLua=%d",
            request.widgetId.c_str(),
            request.surface.c_str(),
            floatingPopupMouseHookGeneration_,
            GetOpenPopupWidget() ? 1 : 0,
            luaWidgetPanelRequest_.widgetId.empty() ? 0 : 1);
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Widgets, message);
    }
    if (luaWidgetPanelFinalizing_)
    {
        pendingLuaWidgetPanelOpen_ = request;
        return;
    }
    if (!luaWidgetPanelRequest_.widgetId.empty())
    {
        pendingLuaWidgetPanelOpen_ = request;
        if ((sameWidget && !samePanel) || luaWidgetPanelAnimation_.IsClosing())
        {
            // A later pointer move must not relocate the queued popup.
            if (!request.hasAnchor)
            {
                const POINT anchor = resolveAnchor();
                pendingLuaWidgetPanelOpen_->hasAnchor = true;
                pendingLuaWidgetPanelOpen_->anchorRect = {anchor.x, anchor.y, anchor.x, anchor.y};
            }
            // The close completion opens the latest queued request. An
            // existing close keeps running when another opener is pressed.
            CloseLuaWidgetPanel(luaWidgetPanelRequest_.widgetId, "switch");
        }
        else
            FinalizeCloseLuaWidgetPanel();
        return;
    }
    if (widgetEngine_)
        widgetEngine_->PrepareWidgetPanelFocus(request.widgetId);
    AdvanceFloatingPopupContentGeneration();
    luaWidgetPanelRequest_ = request;
    luaWidgetPanelAnchorPoint_ = resolveAnchor();
    luaWidgetPanelRect_ = GetLuaWidgetPanelRect();
    luaWidgetPanelMouseDown_ = false;
    luaWidgetPanelAnimation_.ResetHidden();
    const bool animate = snowdesktop::animation::RuntimePopupEffect() != 0;
    if (animate)
    {
        luaWidgetPanelAnimation_.Open(
            static_cast<std::uint64_t>(
                snowdesktop::UiAnimationScheduler::
                    MonotonicMilliseconds()));
    }
    else
    {
        luaWidgetPanelAnimation_.ShowImmediately();
    }
    if (animate)
    {
        UpdateFloatingPopupWindowBounds(false);
        PrepareLuaWidgetPanelAnimationCache();
        if (!StartLuaWidgetPanelCompositionAnimation())
        {
            UpdateLuaWidgetPanelCompositionAnimation();
            EnsureUiAnimationFrame();
        }
    }
    else
        ResetLuaWidgetPanelAnimationCache();
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateFloatingPopupWindowBounds(true);
    if (widgetEngine_)
    {
        const char* openedEvent = request.surface == "dialog"
            ? "onDialogOpened"
            : (request.surface == "popover"
                ? "onPopoverOpened" : "onPanelOpened");
        // This is deliberately the last operation. Lua may synchronously
        // close or replace the panel from its opened callback.
        widgetEngine_->InvokeMouseEvent(
            request.widgetId, openedEvent,
            0, 0, 0, 0);
    }
}

void DesktopApp::FinalizeCloseLuaWidgetPanel(
    bool allowPendingOpen)
{
    if (luaWidgetPanelFinalizing_)
        return;
    luaWidgetPanelFinalizing_ = true;
    const std::wstring closingId =
        luaWidgetPanelRequest_.widgetId;
    const std::string closingSurface =
        luaWidgetPanelRequest_.surface;
    if (luaPanelAnimationFrameToken_)
        uiAnimationScheduler_.Cancel(
            luaPanelAnimationFrameToken_);
    luaPanelAnimationFrameToken_ = 0;
    luaWidgetPanelAnimation_.ResetHidden();
    ResetLuaWidgetPanelAnimationCache();
    luaWidgetPanelRequest_ = {};
    luaWidgetPanelRect_ = {};
    luaWidgetPanelAnchorPoint_ = {};
    if (luaWidgetPanelMouseDown_ || luaWidgetPanelCaptureHwnd_)
    {
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
    }
    luaWidgetPanelMouseDown_ = false;
    ReleaseLuaWidgetPanelCaptureIfOwned();
    UpdateHostInputImePosition();
    UpdateFloatingPopupWindowBounds(true);
    if (hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, FALSE);

    if (!closingId.empty() && widgetEngine_)
    {
        const char* closedEvent = closingSurface == "dialog"
            ? "onDialogClosed"
            : (closingSurface == "popover"
                ? "onPopoverClosed" : "onPanelClosed");
        // Remove the old surface before notifying Lua. Any panel requested by
        // the callback is deferred until every old-surface cleanup completes.
        widgetEngine_->CloseWidgetPanelSurface(
            closingId, closingSurface);
        widgetEngine_->InvokeMouseEvent(
            closingId, closedEvent,
            0, 0, 0, 0);
    }

    if (!allowPendingOpen)
        pendingLuaWidgetPanelOpen_.reset();
    auto pending = std::move(
        pendingLuaWidgetPanelOpen_);
    pendingLuaWidgetPanelOpen_.reset();
    luaWidgetPanelFinalizing_ = false;
    if (pending)
        OpenLuaWidgetPanel(*pending);
}

void DesktopApp::CloseLuaWidgetPanel(
    const std::wstring& widgetId,
    const char* reason)
{
    if (luaWidgetPanelFinalizing_)
    {
        if (pendingLuaWidgetPanelOpen_ &&
            (widgetId.empty() ||
             pendingLuaWidgetPanelOpen_->widgetId == widgetId))
        {
            pendingLuaWidgetPanelOpen_.reset();
        }
        return;
    }
    if (luaWidgetPanelRequest_.widgetId.empty() ||
        (!widgetId.empty() &&
         widgetId !=
            luaWidgetPanelRequest_.widgetId))
        return;
    if (luaWidgetPanelAnimation_.IsClosing())
        return;
    // Mark the close before pointer and focus cleanup can call back into Lua.
    luaWidgetPanelAnimation_.Close(static_cast<std::uint64_t>(
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds()));
    if (luaWidgetPanelAnimation_.IsHidden())
    {
        FinalizeCloseLuaWidgetPanel();
        return;
    }
    {
        wchar_t message[320]{};
        swprintf_s(
            message,
            L"Popup input trace: lua-close widget=%ls reason=%hs generation=%u collection=%d capture=%p focus=%p",
            luaWidgetPanelRequest_.widgetId.c_str(),
            reason ? reason : "",
            floatingPopupMouseHookGeneration_,
            GetOpenPopupWidget() ? 1 : 0,
            snowdesktop::private_pointer::Capture(),
            snowdesktop::private_pointer::FocusWindow());
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Widgets, message);
    }
    const bool panelPressActive =
        luaWidgetPanelMouseDown_ ||
        luaWidgetPanelCaptureHwnd_ != nullptr;
    if (panelPressActive)
    {
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        if (widgetEngine_)
        {
            widgetEngine_->CancelInteractionPointerPress(
                luaWidgetPanelRequest_.surface);
        }
    }
    if (widgetEngine_)
        widgetEngine_->BlurHostInput(false);
    luaWidgetPanelMouseDown_ = false;
    ReleaseLuaWidgetPanelCaptureIfOwned();
    UpdateHostInputImePosition();
    if (!(snowdesktop::animation::RuntimePopupEffect() != 0))
    {
        FinalizeCloseLuaWidgetPanel();
        return;
    }
    UpdateFloatingPopupWindowBounds(false);
    PrepareLuaWidgetPanelAnimationCache();
    if (luaWidgetPanelAnimationOverlay_.active &&
        luaWidgetPanelAnimationOverlay_.host ==
            UiCompositionAnimationHost::Desktop)
    {
        ClearDesktopBehindCompositionAnimation(
            luaWidgetPanelAnimationCacheRect_);
    }
    if (!StartLuaWidgetPanelCompositionAnimation())
    {
        UpdateLuaWidgetPanelCompositionAnimation();
        EnsureUiAnimationFrame();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateFloatingPopupWindowBounds(true);
}
