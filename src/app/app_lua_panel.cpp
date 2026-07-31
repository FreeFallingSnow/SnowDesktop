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
            luaWidgetPanelAnimation_.Open(
                GetTickCount64());
            SetTimer(
                hwnd_,
                kLuaWidgetPanelAnimationTimerId,
                snowdesktop::popup_animation_rules::
                    kFrameIntervalMs,
                nullptr);
            InvalidateRect(
                hwnd_, nullptr, FALSE);
            return;
        }
        FinalizeCloseLuaWidgetPanel();
    }
    if (IsCollectionPopupInteractive())
        CloseCollectionPopup(false);
    if (quickNavigationOpen_)
        CloseQuickNavigation();
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
    luaWidgetPanelRect_ = GetLuaWidgetPanelRect();
    luaWidgetPanelMouseDown_ = false;
    luaWidgetPanelAnimation_.ResetHidden();
    luaWidgetPanelAnimation_.Open(
        GetTickCount64());
    if (hwnd_ && IsWindow(hwnd_))
    {
        SetTimer(
            hwnd_,
            kLuaWidgetPanelAnimationTimerId,
            snowdesktop::popup_animation_rules::
                kFrameIntervalMs,
            nullptr);
    }
    if (widgetEngine_)
    {
        widgetEngine_->InvokeMouseEvent(
            request.widgetId, "onPanelOpened",
            0, 0, 0, 0);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void DesktopApp::FinalizeCloseLuaWidgetPanel()
{
    if (hwnd_ && IsWindow(hwnd_))
    {
        KillTimer(
            hwnd_,
            kLuaWidgetPanelAnimationTimerId);
    }
    const std::wstring closingId =
        luaWidgetPanelRequest_.widgetId;
    if (!closingId.empty() && widgetEngine_)
    {
        widgetEngine_->InvokeMouseEvent(
            closingId, "onPanelClosed",
            0, 0, 0, 0);
    }
    luaWidgetPanelAnimation_.ResetHidden();
    luaWidgetPanelRequest_ = {};
    luaWidgetPanelRect_ = {};
    luaWidgetPanelAnchorPoint_ = {};
    luaWidgetPanelMouseDown_ = false;
    ReleaseCapture();
    UpdateHostInputImePosition();
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
    luaWidgetPanelAnimation_.Close(
        GetTickCount64());
    if (luaWidgetPanelAnimation_.IsHidden())
    {
        FinalizeCloseLuaWidgetPanel();
        return;
    }
    if (hwnd_ && IsWindow(hwnd_))
    {
        SetTimer(
            hwnd_,
            kLuaWidgetPanelAnimationTimerId,
            snowdesktop::popup_animation_rules::
                kFrameIntervalMs,
            nullptr);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}
