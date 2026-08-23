#include "app.h"

#include <utility>

// Lua widget panel lifecycle.

void DesktopApp::ReleaseLuaWidgetPanelCaptureIfOwned()
{
    const HWND recordedCapture =
        std::exchange(luaWidgetPanelCaptureHwnd_, nullptr);
    if (snowdesktop::floating_popup_rules::
            ShouldReleaseRecordedPanelCapture(
                recordedCapture, GetCapture()))
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
    if (luaWidgetPanelFinalizing_)
    {
        pendingLuaWidgetPanelOpen_ = request;
        return;
    }
    if (!luaWidgetPanelRequest_.widgetId.empty())
    {
        if (luaWidgetPanelRequest_.widgetId ==
                request.widgetId &&
            luaWidgetPanelRequest_.surface ==
                request.surface &&
            luaWidgetPanelAnimation_.IsClosing())
        {
            luaWidgetPanelRequest_ = request;
            if (snowdesktop::dock_launch_animation::
                    SystemAnimationsEnabled())
            {
                UpdateFloatingPopupWindowBounds(false);
                PrepareLuaWidgetPanelAnimationCache();
                luaWidgetPanelAnimation_.Open(
                    static_cast<std::uint64_t>(
                        snowdesktop::UiAnimationScheduler::
                            MonotonicMilliseconds()));
                if (!StartLuaWidgetPanelCompositionAnimation())
                {
                    UpdateLuaWidgetPanelCompositionAnimation();
                    EnsureUiAnimationFrame();
                }
            }
            else
            {
                luaWidgetPanelAnimation_.ShowImmediately();
                ResetLuaWidgetPanelAnimationCache();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            UpdateFloatingPopupWindowBounds(true);
            return;
        }
        pendingLuaWidgetPanelOpen_ = request;
        FinalizeCloseLuaWidgetPanel();
        return;
    }
    luaWidgetPanelRequest_ = request;
    luaWidgetPanelAnchorPoint_ =
        lastMousePoint_;
    const auto source = std::find_if(
        widgets_.begin(), widgets_.end(),
        [&](const DesktopWidget& widget) {
            return widget.id == request.widgetId;
        });
    if (source != widgets_.end())
    {
        const RECT sourceRect =
            GetStandaloneWidgetFrameRect(*source);
        if (!PtInRect(
                &sourceRect,
                luaWidgetPanelAnchorPoint_))
        {
            luaWidgetPanelAnchorPoint_ = {
                (sourceRect.left + sourceRect.right) / 2,
                (sourceRect.top + sourceRect.bottom) / 2
            };
        }
    }
    if (request.hasAnchor)
    {
        luaWidgetPanelAnchorPoint_ = {
            (request.anchorRect.left + request.anchorRect.right) / 2,
            (request.anchorRect.top + request.anchorRect.bottom) / 2
        };
    }
    luaWidgetPanelRect_ = GetLuaWidgetPanelRect();
    luaWidgetPanelMouseDown_ = false;
    luaWidgetPanelAnimation_.ResetHidden();
    const bool animate =
        snowdesktop::dock_launch_animation::
            SystemAnimationsEnabled();
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
    (void)reason;
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
    if (!snowdesktop::dock_launch_animation::
            SystemAnimationsEnabled())
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
    luaWidgetPanelAnimation_.Close(
        static_cast<std::uint64_t>(
            snowdesktop::UiAnimationScheduler::
                MonotonicMilliseconds()));
    if (luaWidgetPanelAnimation_.IsHidden())
    {
        FinalizeCloseLuaWidgetPanel();
        return;
    }
    if (!StartLuaWidgetPanelCompositionAnimation())
    {
        UpdateLuaWidgetPanelCompositionAnimation();
        EnsureUiAnimationFrame();
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateFloatingPopupWindowBounds(true);
}
