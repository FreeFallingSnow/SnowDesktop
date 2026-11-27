#include "ui/input/private_pointer.h"
#include "ui/input/touch_input.h"
#include "app/app.h"
#include "app/popup/popup_resize_transition.h"
#include "diagnostics/diagnostic_log.h"
#include "diagnostics/performance_trace.h"
#include "widget/runtime/widget_engine.h"
#include "settings/animation_settings.h"
#include "layout/floating_popup_rules.h"

#include <utility>

// Lua widget panel lifecycle.

namespace
{
bool SamePanelSize(const RECT& first, const RECT& second)
{
    return first.right - first.left == second.right - second.left &&
        first.bottom - first.top == second.bottom - second.top;
}

void InvalidateLuaPanelDesktopFootprint(HWND desktop, RECT panel, RECT snapshot, RECT source = {})
{
    if (!desktop || !IsWindow(desktop)) return;
    RECT dirty{};
    UnionRect(&dirty, &panel, &snapshot);
    if (!UnionRect(&dirty, &dirty, &source)) return;
    InflateRect(&dirty, 6, 6);
    InvalidateRect(desktop, &dirty, FALSE);
}
}

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
        if (request.hasAnchor)
            return POINT{(request.anchorRect.left + request.anchorRect.right) / 2,
                (request.anchorRect.top + request.anchorRect.bottom) / 2};
        if (request.desktopGesture && snowdesktop::touch_input::routingTouch)
        {
            POINT touchPoint = snowdesktop::touch_input::routedPoint;
            if (ScreenToClient(hwnd_, &touchPoint)) return touchPoint;
        }
        POINT anchor = lastMousePoint_;
        const auto source = std::find_if(widgets_.begin(), widgets_.end(),
            [&](const DesktopWidget& widget) { return widget.id == request.widgetId; });
        if (source != widgets_.end())
        {
            const RECT frame = GetStandaloneWidgetFrameRect(*source);
            if (!PtInRect(&frame, anchor))
                anchor = {(frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2};
        }
        return anchor;
    };
    // Distinct desktop elements may open panels with the same title. A repeat
    // of the current opener leaves dismissal to the outside-click policy.
    const bool sameWidget = luaWidgetPanelRequest_.widgetId == request.widgetId;
    const bool sameSurface = sameWidget &&
        luaWidgetPanelRequest_.surface == request.surface &&
        luaWidgetPanelRequest_.title == request.title &&
        luaWidgetPanelRequest_.anchorKey == request.anchorKey;
    const bool samePanel = sameSurface &&
        luaWidgetPanelRequest_.desktopOpenerKey == request.desktopOpenerKey;
    if (request.desktopGesture && samePanel)
        return;
    if (widgetEngine_)
        widgetEngine_->SetPanelTheme(collectionPopupAppearance_);
    // An action inside the current panel inherits its original opener.
    if (request.panelGesture && sameSurface && !luaWidgetPanelFinalizing_ &&
        luaWidgetPanelAnimation_.IsInteractive())
    {
        RefreshLuaWidgetPanel(request);
        return;
    }
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
    const POINT anchor = resolveAnchor();
    if (luaWidgetPanelFinalizing_ || !luaWidgetPanelRequest_.widgetId.empty())
    {
        pendingLuaWidgetPanelOpen_ = request;
        if (!request.hasAnchor)
        {
            // Retain the opener while the previous panel finishes closing.
            pendingLuaWidgetPanelOpen_->hasAnchor = true;
            pendingLuaWidgetPanelOpen_->anchorRect = {anchor.x, anchor.y, anchor.x, anchor.y};
        }
        if (luaWidgetPanelFinalizing_) return;
        if ((sameWidget && !samePanel) || luaWidgetPanelAnimation_.IsClosing())
        {
            // The close completion opens the latest queued request. An
            // existing close keeps running when another opener is pressed.
            CloseLuaWidgetPanel(luaWidgetPanelRequest_.widgetId, "switch");
        }
        else
            FinalizeCloseLuaWidgetPanel();
        return;
    }
    snowdesktop::performance::Scope trace("popup", "lua-panel.open");
    if (widgetEngine_)
        widgetEngine_->PrepareWidgetPanelFocus(request.widgetId);
    AdvanceFloatingPopupContentGeneration();
    luaWidgetPanelRequest_ = request;
    luaWidgetPanelAnchorPoint_ = anchor;
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
    const size_t sourceIndex = FindWidgetIndexById(request.widgetId);
    const RECT sourceFrame = sourceIndex < widgets_.size()
        ? GetStandaloneWidgetFrameRect(widgets_[sourceIndex]) : RECT{};
    InvalidateLuaPanelDesktopFootprint(hwnd_, luaWidgetPanelRect_, luaWidgetPanelAnimationCacheRect_, sourceFrame);
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

void DesktopApp::RefreshLuaWidgetPanel(const LuaWidgetPanelRequest& request)
{
    namespace motion = snowdesktop::animation;
    using namespace snowdesktop::popup_resize_transition;

    const RECT previousLayout = GetLuaWidgetPanelLayoutRect();
    const RECT previous = luaWidgetPanelResize_
        ? luaWidgetPanelResize_->Panel(previousLayout) : previousLayout;
    luaWidgetPanelRequest_.width = request.width;
    luaWidgetPanelRequest_.height = request.height;
    if (!SamePanelSize(previousLayout, GetLuaWidgetPanelLayoutRect()))
    {
        if (IsRectEmpty(&luaWidgetPanelResizeOrigin_))
            luaWidgetPanelResizeOrigin_ = previousLayout;
        const RECT target = GetLuaWidgetPanelLayoutRect();
        luaWidgetPanelResize_.reset();
        if (!IsRectEmpty(&previous) && !IsRectEmpty(&target) &&
            !SamePanelSize(previous, target) && motion::RuntimePopupEffect() != motion::NoEffect)
        {
            // The opening snapshot has the old content and dimensions.
            if (luaWidgetPanelAnimation_.IsAnimating())
            {
                luaWidgetPanelAnimation_.ShowImmediately();
                if (const auto token = std::exchange(luaPanelAnimationFrameToken_, 0))
                    uiAnimationScheduler_.Cancel(token);
                ResetLuaWidgetPanelAnimationCache();
            }
            auto transition = std::make_shared<Transition>(Geometry{previous, {}}, Geometry{target, {}},
                snowdesktop::UiAnimationScheduler::MonotonicMilliseconds(),
                motion::RuntimeDurationScale());
            luaWidgetPanelResize_ = transition;
            const auto token = uiAnimationScheduler_.StartAnimation(snowdesktop::UiAnimationSurface::Popup,
                [this, weak = std::weak_ptr<Transition>(transition)](double now) {
                    const auto current = weak.lock();
                    if (!current || luaWidgetPanelResize_ != current ||
                        luaWidgetPanelAnimation_.IsClosing() || exitRequested_)
                        return false;
                    const bool keep = motion::RuntimePopupEffect() != motion::NoEffect &&
                        current->Advance(now);
                    if (!keep)
                        luaWidgetPanelResize_.reset();
                    uiAnimationScheduler_.RequestFramePresentation(&luaWidgetPanelResize_, [this] {
                        if (IsLuaPanelHostedByFloatingWindow() && !luaWidgetPanelAnimation_.IsClosing())
                            UpdateFloatingPopupWindowBounds(true, false);
                    });
                    return keep;
                });
            if (!token)
                luaWidgetPanelResize_.reset();
        }
    }
    UpdateFloatingPopupWindowBounds(false);
}

void DesktopApp::FinalizeCloseLuaWidgetPanel(
    bool allowPendingOpen)
{
    if (luaWidgetPanelFinalizing_)
        return;
    snowdesktop::performance::Scope trace("popup", "lua-panel.close.finish");
    luaWidgetPanelFinalizing_ = true;
    const RECT previousPanel = luaWidgetPanelRect_;
    const RECT previousSnapshot = luaWidgetPanelAnimationCacheRect_;
    const std::wstring closingId =
        luaWidgetPanelRequest_.widgetId;
    const size_t sourceIndex = FindWidgetIndexById(closingId);
    const RECT sourceFrame = sourceIndex < widgets_.size()
        ? GetStandaloneWidgetFrameRect(widgets_[sourceIndex]) : RECT{};
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
    luaWidgetPanelResizeOrigin_ = {};
    luaWidgetPanelResize_.reset();
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
    InvalidateLuaPanelDesktopFootprint(hwnd_, previousPanel, previousSnapshot, sourceFrame);

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
    InvalidateLuaPanelDesktopFootprint(hwnd_, luaWidgetPanelRect_, luaWidgetPanelAnimationCacheRect_);
    UpdateFloatingPopupWindowBounds(true);
}
