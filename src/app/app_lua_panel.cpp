#include "app.h"

// Lua widget panel lifecycle.

void DesktopApp::OpenLuaWidgetPanel(
    const LuaWidgetPanelRequest& request)
{
    if (request.widgetId.empty())
        return;
    if (!luaWidgetPanelRequest_.widgetId.empty())
    {
        if (luaWidgetPanelRequest_.widgetId ==
                request.widgetId &&
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
        FinalizeCloseLuaWidgetPanel();
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
    if (widgetEngine_)
    {
        const char* openedEvent = request.surface == "dialog"
            ? "onDialogOpened"
            : (request.surface == "popover"
                ? "onPopoverOpened" : "onPanelOpened");
        widgetEngine_->InvokeMouseEvent(
            request.widgetId, openedEvent,
            0, 0, 0, 0);
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
}

void DesktopApp::FinalizeCloseLuaWidgetPanel()
{
    const std::wstring closingId =
        luaWidgetPanelRequest_.widgetId;
    const std::string closingSurface =
        luaWidgetPanelRequest_.surface;
    if (!closingId.empty() && widgetEngine_)
    {
        const char* closedEvent = closingSurface == "dialog"
            ? "onDialogClosed"
            : (closingSurface == "popover"
                ? "onPopoverClosed" : "onPanelClosed");
        widgetEngine_->InvokeMouseEvent(
            closingId, closedEvent,
            0, 0, 0, 0);
        widgetEngine_->CloseWidgetPanelSurface(
            closingId, closingSurface);
    }
    luaWidgetPanelAnimation_.ResetHidden();
    ResetLuaWidgetPanelAnimationCache();
    luaWidgetPanelRequest_ = {};
    luaWidgetPanelRect_ = {};
    luaWidgetPanelAnchorPoint_ = {};
    luaWidgetPanelMouseDown_ = false;
    ReleaseCapture();
    UpdateHostInputImePosition();
    UpdateFloatingPopupWindowBounds(true);
    if (hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, FALSE);
}

void DesktopApp::CloseLuaWidgetPanel(
    const std::wstring& widgetId,
    const char* reason)
{
    (void)reason;
    if (luaWidgetPanelRequest_.widgetId.empty() ||
        (!widgetId.empty() &&
         widgetId !=
            luaWidgetPanelRequest_.widgetId))
        return;
    if (luaWidgetPanelAnimation_.IsClosing())
        return;
    if (widgetEngine_)
        widgetEngine_->BlurHostInput(false);
    luaWidgetPanelMouseDown_ = false;
    ReleaseCapture();
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
