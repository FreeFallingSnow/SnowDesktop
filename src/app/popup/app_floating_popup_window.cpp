#include "floating_popup_composition.h"

#include "ui/input/private_pointer.h"
#include "app/app.h"
#include "popup_resize_transition.h"
#include "diagnostics/diagnostic_log.h"
#include "common/utils.h"
#include "layout/floating_popup_rules.h"
#include "app/dock/persistent_dock_host.h"
#include "ui/render/ui_presentation_scope.h"
#include "drag_drop/drag_input_rules.h"
#include "popup_window_pair_z_order.h"
#include "layout/popup_round_geometry.h"
#include "ui/render/d2d_drawing.h"
#include "ui/render/blur_sampling_bounds.h"
#include "theme/flat_glass_rim.h"
#include "platform/shell_overlay_window.h"
#include "diagnostics/performance_trace.h"
#include "platform/window_support/window_support_runtime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <dwmapi.h>
#include <sstream>
#include <utility>

namespace
{
constexpr UINT_PTR kPopupMouseLeaveTimerId = 1;
constexpr UINT kPopupMouseLeavePollIntervalMs = 50;
constexpr UINT kStackIndicatorMouseLeavePollIntervalMs = 30;
constexpr UINT_PTR kPopupPresentationRetryTimerId = 2;
constexpr UINT kPopupPresentationRetryIntervalMs = 100;
constexpr unsigned kPopupPresentationRetryLimit = 5;
constexpr UINT kRefreshPopupBoundsMessage = WM_APP + 0x477;

RECT UnionRects(const RECT& first, const RECT& second)
{
    if (IsRectEmpty(&first))
        return second;
    if (IsRectEmpty(&second))
        return first;
    RECT result{};
    UnionRect(&result, &first, &second);
    return result;
}

bool ContainsRect(const RECT& outer, const RECT& inner)
{
    return outer.left <= inner.left && outer.top <= inner.top &&
        outer.right >= inner.right && outer.bottom >= inner.bottom;
}

RECT ResolveFloatingPopupWindowFrame(const RECT& desktop, RECT content)
{
    OffsetRect(&content, desktop.left, desktop.top);
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromRect(&content, MONITOR_DEFAULTTONULL), &monitor) ||
        !ContainsRect(monitor.rcMonitor, content))
        return desktop;

    // The glass helper adds a sampling halo to the HWND. When the content
    // fits, reserve that halo inside this display along the interior edges.
    RECT frame = monitor.rcMonitor;
    constexpr LONG halo = snowdesktop::blur_sampling::kMaximumBackdropMargin;
    if (frame.left > desktop.left) frame.left += halo;
    if (frame.top > desktop.top) frame.top += halo;
    if (frame.right < desktop.right) frame.right -= halo;
    if (frame.bottom < desktop.bottom) frame.bottom -= halo;
    return ContainsRect(frame, content) ? frame : monitor.rcMonitor;
}

struct MenuHostZOrderSnapshot
{
    HWND menu = nullptr;
    HWND host = nullptr;
    HWND menuPrevious = nullptr;
    HWND menuNext = nullptr;
    HWND hostPrevious = nullptr;
    HWND hostNext = nullptr;
    LONG_PTR menuExStyle = 0;
    LONG_PTR hostExStyle = 0;
    bool menuAboveHost = false;

    bool operator==(const MenuHostZOrderSnapshot& other) const
    {
        return menu == other.menu &&
            host == other.host &&
            menuPrevious == other.menuPrevious &&
            menuNext == other.menuNext &&
            hostPrevious == other.hostPrevious &&
            hostNext == other.hostNext &&
            menuExStyle == other.menuExStyle &&
            hostExStyle == other.hostExStyle &&
            menuAboveHost == other.menuAboveHost;
    }
};

MenuHostZOrderSnapshot CaptureMenuHostZOrder(HWND host)
{
    MenuHostZOrderSnapshot snapshot;
    snapshot.menu = snowdesktop::modern_menu::ActiveRootWindow();
    if (!snapshot.menu || !host || !IsWindow(host))
        return snapshot;

    snapshot.host = host;
    snapshot.menuPrevious = GetWindow(snapshot.menu, GW_HWNDPREV);
    snapshot.menuNext = GetWindow(snapshot.menu, GW_HWNDNEXT);
    snapshot.hostPrevious = GetWindow(host, GW_HWNDPREV);
    snapshot.hostNext = GetWindow(host, GW_HWNDNEXT);
    snapshot.menuExStyle =
        GetWindowLongPtrW(snapshot.menu, GWL_EXSTYLE);
    snapshot.hostExStyle = GetWindowLongPtrW(host, GWL_EXSTYLE);
    for (HWND current = snapshot.menu; current;
         current = GetWindow(current, GW_HWNDNEXT))
    {
        if (current == host)
        {
            snapshot.menuAboveHost = true;
            break;
        }
    }
    return snapshot;
}

void TraceMenuHostZOrderTransition(
    const wchar_t* stage,
    const MenuHostZOrderSnapshot& before,
    const MenuHostZOrderSnapshot& after,
    bool force = false)
{
    if ((!before.menu && !after.menu) ||
        (!force && before == after))
    {
        return;
    }

    const MenuHostZOrderSnapshot& current =
        after.menu ? after : before;
    std::wostringstream line;
    line << L"FloatingPopupZOrder stage=" << stage
         << L" menu=" << current.menu
         << L" host=" << current.host
         << L" beforeAbove=" << (before.menuAboveHost ? 1 : 0)
         << L" afterAbove=" << (after.menuAboveHost ? 1 : 0)
         << L" menuTopmost="
         << ((current.menuExStyle & WS_EX_TOPMOST) ? 1 : 0)
         << L" hostTopmost="
         << ((current.hostExStyle & WS_EX_TOPMOST) ? 1 : 0)
         << L" menuPrev=" << current.menuPrevious
         << L" menuNext=" << current.menuNext
         << L" hostPrev=" << current.hostPrevious
         << L" hostNext=" << current.hostNext;
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Menus,
        line.str().c_str(), DiagnosticLogLevel::Debug);
}

}

bool DesktopApp::IsCollectionPopupHostedByFloatingWindow() const
{
    if (folderPreview_ && folderPreview_->inlineBounds) return false;
    return snowdesktop::floating_popup_rules::HostsCollectionPopup(
        GetOpenPopupWidget() != nullptr);
}

bool DesktopApp::IsLuaPanelHostedByFloatingWindow() const
{
    return snowdesktop::floating_popup_rules::HostsLuaPanel(
        !luaWidgetPanelRequest_.widgetId.empty());
}

bool DesktopApp::ShouldShowFloatingPopupWindow() const
{
    return IsLuaStackIndicatorVisible() || snowdesktop::floating_popup_rules::ShouldShow(
        IsCollectionPopupHostedByFloatingWindow(),
        IsLuaPanelHostedByFloatingWindow());
}

LRESULT CALLBACK DesktopApp::FloatingPopupMouseHookProc(
    int code, WPARAM message, LPARAM data)
{
    if (code == HC_ACTION &&
        (message == WM_LBUTTONDOWN ||
         message == WM_RBUTTONDOWN ||
         message == WM_MBUTTONDOWN ||
         message == WM_XBUTTONDOWN))
    {
        const auto* event =
            reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
        const HWND notificationWindow =
            floatingPopupMouseHookNotificationWindow_.load();
        const std::uint32_t generation =
            floatingPopupMouseHookActiveGeneration_.load();
        if (event && generation != 0 &&
            notificationWindow &&
            IsWindow(notificationWindow))
        {
            static_assert(sizeof(LPARAM) == sizeof(std::uint64_t),
                "SnowDesktop popup input payload requires the supported x64 build");
            const std::uint64_t screenPointPayload =
                snowdesktop::floating_popup_rules::
                    PackScreenPoint(event->pt);
            PostMessageW(
                notificationWindow,
                kFloatingPopupExternalPointerMessage,
                static_cast<WPARAM>(generation),
                std::bit_cast<LPARAM>(screenPointPayload));
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}

bool DesktopApp::StartFloatingPopupOutsideClickMonitor()
{
    if (snowdesktop::window_support::WindowSupport().Resolve(
            snowdesktop::window_support::Capability::PointerObservation).Available())
    {
        if (!floatingPopupMouseHookNotificationWindow_.load())
        {
            ++floatingPopupMouseHookGeneration_;
            floatingPopupPointerGenerationStartedAt_ = GetTickCount();
            floatingPopupMouseHookNotificationWindow_.store(floatingPopupHwnd_);
        }
        if (floatingPopupMouseHook_) floatingPopupMouseHook_.Stop();
        return true;
    }
    if (floatingPopupMouseHook_)
        return true;
    if (!floatingPopupHwnd_ ||
        !IsWindow(floatingPopupHwnd_))
        return false;

    ++floatingPopupMouseHookGeneration_;
    if (floatingPopupMouseHookGeneration_ == 0)
        ++floatingPopupMouseHookGeneration_;
    floatingPopupMouseHookActiveGeneration_.store(
        floatingPopupMouseHookGeneration_);
    floatingPopupMouseHookNotificationWindow_.store(
        floatingPopupHwnd_);
    if (floatingPopupMouseHook_.Start(
            instance_, &DesktopApp::FloatingPopupMouseHookProc))
        return true;

    floatingPopupMouseHookNotificationWindow_.store(nullptr);
    floatingPopupMouseHookActiveGeneration_.store(0);
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Menus,
        L"Floating popup outside-click hook FAILED", DiagnosticLogLevel::Warning);
    return false;
}

void DesktopApp::StopFloatingPopupOutsideClickMonitor()
{
    floatingPopupMouseHookNotificationWindow_.store(nullptr);
    if (!floatingPopupMouseHook_)
    {
        floatingPopupMouseHook_.Stop();
        floatingPopupMouseHookActiveGeneration_.store(0);
        return;
    }
    ++floatingPopupMouseHookGeneration_;
    if (floatingPopupMouseHookGeneration_ == 0)
        ++floatingPopupMouseHookGeneration_;
    floatingPopupMouseHook_.Stop();
    floatingPopupMouseHookActiveGeneration_.store(0);
}

void DesktopApp::AdvanceFloatingPopupContentGeneration()
{
    floatingPopupPointerGenerationStartedAt_ = GetTickCount();
    floatingPopupPresentationRetryCount_ = 0;
    if (floatingPopupHwnd_)
        KillTimer(floatingPopupHwnd_, kPopupPresentationRetryTimerId);
    popupMouseLeaveController_.Reset();
    ++floatingPopupMouseHookGeneration_;
    if (floatingPopupMouseHookGeneration_ == 0)
        ++floatingPopupMouseHookGeneration_;
    if (floatingPopupMouseHook_)
    {
        // A low-level pointer notification is queued asynchronously. Keep it
        // tied to the popup contents that existed at the physical button-down
        // so it cannot dismiss a Lua panel created by that same click.
        floatingPopupMouseHookActiveGeneration_.store(
            floatingPopupMouseHookGeneration_);
    }
}

void DesktopApp::HandleFloatingPopupExternalPointerDown(
    std::uint32_t generation,
    std::uint64_t screenPointPayload)
{
    if (snowdesktop::private_pointer::Active()) return;
    if (!snowdesktop::floating_popup_rules::
            IsCurrentPointerNotification(
                generation,
                floatingPopupMouseHookGeneration_) ||
        !ShouldShowFloatingPopupWindow())
        return;

    const POINT screenPoint =
        snowdesktop::floating_popup_rules::
            UnpackScreenPoint(screenPointPayload);
    POINT desktopPoint = screenPoint;
    const bool hasDesktopPoint =
        hwnd_ && IsWindow(hwnd_) &&
        ScreenToClient(hwnd_, &desktopPoint);
    const bool pointOnHostedPopup =
        hasDesktopPoint &&
        (snowdesktop::floating_popup_rules::
            IsPointOnHostedPopupSurface(
                desktopPoint,
                floatingPopupCollectionRegion_,
                floatingPopupLuaPanelRegion_) ||
            snowdesktop::floating_popup_rules::ContainsHostedPopupPoint(
                floatingStackIndicatorRegion_, desktopPoint));
    HWND targetWindow = snowdesktop::private_pointer::WindowAtPoint(screenPoint);
    if (!targetWindow && !pointOnHostedPopup)
        return;
    if (!pointOnHostedPopup && IsFolderPreviewSearchImeWindow(targetWindow))
        return;
    const DWORD currentProcessId = GetCurrentProcessId();
    const auto belongsToInternalSurface =
        [this, currentProcessId](HWND window) {
            DWORD processId = 0;
            if (window)
                GetWindowThreadProcessId(window, &processId);
            return snowdesktop::floating_popup_rules::
                IsInternalPointerTarget(
                    processId != 0 &&
                        processId == currentProcessId,
                    IsSettingsApplicationWindow(window));
        };
    // The desktop renderer is a child of Explorer's WorkerW. Test the direct
    // hit first so an in-process desktop/Dock click does not inherit the
    // external process identity of its shell-owned root.
    bool targetBelongsToInternalSurface =
        pointOnHostedPopup ||
        belongsToInternalSurface(targetWindow);
    if (!targetBelongsToInternalSurface)
    {
        targetBelongsToInternalSurface =
            belongsToInternalSurface(
                GetAncestor(targetWindow, GA_ROOTOWNER));
    }
    const bool dragActive =
        dragSession_.IsActive() ||
        dragDropController_.IsExternalDragActive();
    // Every status bar is a separate HWND and never enters Dock pointer-down
    // handling. Use the clicked monitor, not the popup's source monitor: both
    // merged side controls and independent bars are outside this collection.
    // Keep Dock's own HWND internal so its folder button toggles on release.
    const HMONITOR hitMonitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONULL);
    const bool statusBarOutsideCollection = !pointOnHostedPopup && targetWindow &&
        statusBar_ && hitMonitor && targetWindow == statusBar_->InteractionWindow(hitMonitor);
    if (folderPreview_ && !pointOnHostedPopup && !targetBelongsToInternalSurface)
        folderPreview_->keyboardFocused = false;
    const bool dismissCollection =
        snowdesktop::floating_popup_rules::
            ShouldDismissForExternalPointerDown(
                IsCollectionPopupHostedByFloatingWindow(),
                targetBelongsToInternalSurface && !statusBarOutsideCollection,
                dragActive);
    const bool dismissLuaPanel =
        snowdesktop::floating_popup_rules::
            ShouldDismissLuaPanelForExternalPointerDown(
                IsLuaPanelHostedByFloatingWindow(),
                luaWidgetPanelRequest_.dismissOnOutside,
                targetBelongsToInternalSurface,
                dragActive);
    if (dismissCollection || dismissLuaPanel)
    {
        wchar_t message[320]{};
        swprintf_s(
            message,
            L"Popup input trace: external-pointer generation=%u current=%u target=%p owned=%d collection=%d lua=%d dismissCollection=%d dismissLua=%d",
            generation,
            floatingPopupMouseHookGeneration_,
            targetWindow,
            targetBelongsToInternalSurface ? 1 : 0,
            IsCollectionPopupHostedByFloatingWindow() ? 1 : 0,
            IsLuaPanelHostedByFloatingWindow() ? 1 : 0,
            dismissCollection ? 1 : 0,
            dismissLuaPanel ? 1 : 0);
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Menus, message);
    }
    if (dismissCollection)
    {
        pendingCollectionPopupOpen_.reset();
        CloseCollectionPopup();
    }
    if (dismissLuaPanel)
    {
        CloseLuaWidgetPanel(
            luaWidgetPanelRequest_.widgetId,
            "external-pointer");
    }
}

RECT DesktopApp::GetFloatingCollectionPopupRegion() const
{
    if (!IsCollectionPopupHostedByFloatingWindow()) return {};
    const auto* popup = GetOpenPopupWidget();
    if (!popup) return {};
    const RECT region = GetCollectionPopupRect(*popup);
    if (const auto* motion = GetCollectionPopupResizeTransition())
        return motion->Panel(region);
    return region;
}

RECT DesktopApp::CalculateFloatingPopupWindowBounds() const
{
    RECT bounds{};
    if (IsCollectionPopupHostedByFloatingWindow())
    {
        if (const DesktopWidget* popup = GetOpenPopupWidget())
        {
            RECT panel = GetCollectionPopupRect(*popup);
            if (const auto* motion = GetCollectionPopupResizeTransition())
                panel = motion->ReservedPanel(panel);
            bounds = InflateCopy(panel, 6);
        }
    }
    if (IsLuaPanelHostedByFloatingWindow())
    {
        RECT panelBounds{};
        if (luaWidgetPanelRequest_.modal && hwnd_ && IsWindow(hwnd_))
            GetClientRect(hwnd_, &panelBounds);
        else
            panelBounds = InflateCopy(GetLuaWidgetPanelReservedRect(), 6);
        bounds = UnionRects(bounds, panelBounds);
    }
    return bounds;
}

bool DesktopApp::WarmFloatingPopupResources()
{
    if (exitRequested_ || graphicsDeviceRecovery_.Pending() || !dcompDevice_ ||
        ShouldShowFloatingPopupWindow())
        return false;
    snowdesktop::performance::Scope scope("popup", "infrastructure.warmup.ui");
    if (collectionPopupGlassTheme_)
        (void)collectionPopupBackdropCompositor_.PrepareBlurFactoryInBackground(collectionPopupBlurRadius_);
    if (!CreateFloatingPopupWindow() ||
        FAILED(EnsureFloatingPopupCompositionRoot()) ||
        FAILED(floatingPopupSurfaces_.PrepareContent(dcompDevice_.Get(), floatingPopupContentBounds_)))
        return false;
    // A fresh DComp surface has no valid content until EndDraw. Initialize its
    // tiny hidden target without touching popup models, input regions or hooks.
    ComPtr<ID2D1DeviceContext> context;
    POINT offset{};
    const HRESULT draw = floatingPopupSurfaces_.Content()->BeginDraw(nullptr,
        __uuidof(ID2D1DeviceContext), reinterpret_cast<void**>(context.GetAddressOf()), &offset);
    if (FAILED(draw)) return false;
    context->Clear(D2D1::ColorF(0, 0, 0, 0));
    context.Reset();
    if (FAILED(floatingPopupSurfaces_.Content()->EndDraw())) return false;

    return CommitCompositionAnimationFrame() && FlushPendingCompositionCommit();
}

bool DesktopApp::CreateFloatingPopupWindow()
{
    if (floatingPopupHwnd_ && IsWindow(floatingPopupHwnd_))
        return true;
    if (!instance_)
        return false;

    floatingPopupHwnd_ = snowdesktop::CreateShellOverlayWindowEx(
        snowdesktop::floating_popup_rules::kWindowExStyle,
        kFloatingPopupWindowClassName,
        L"SnowDesktopFloatingPopup",
        WS_POPUP,
        virtualLeft_, virtualTop_, std::max(1, virtualWidth_), std::max(1, virtualHeight_),
        nullptr, nullptr, instance_, this);
    if (!floatingPopupHwnd_)
        return false;

    const BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(
        floatingPopupHwnd_,
        DWMWA_TRANSITIONS_FORCEDISABLED,
        &disableTransitions,
        sizeof(disableTransitions));

    OleDragDropAdapter* adapter = EnsureOleDragDropAdapter();
    floatingPopupDropTargetRegistered_ = adapter &&
        SUCCEEDED(RegisterDragDrop(
            floatingPopupHwnd_,
            static_cast<IDropTarget*>(adapter)));
    return true;
}

HRESULT DesktopApp::SyncFloatingPopupCompositionRootZOrder()
{
    if (!floatingPopupDcompVisual_ || !floatingPopupDcompLiveVisual_)
        return E_UNEXPECTED;

    const std::array<IDCompositionVisual2*, 2> bottomToTop{
        snowdesktop::widget_composition_layer_rules::
                BelongsToCompositionRoot(
                    popupAnimationOverlay_.host,
                    UiCompositionAnimationHost::FloatingPopup)
            ? popupAnimationOverlay_.visual.Get() : nullptr,
        snowdesktop::widget_composition_layer_rules::
                BelongsToCompositionRoot(
                    luaWidgetPanelAnimationOverlay_.host,
                    UiCompositionAnimationHost::FloatingPopup)
            ? luaWidgetPanelAnimationOverlay_.visual.Get() : nullptr,
    };

    std::array<bool, 2> removedFromRoot{};
    const auto restoreRemovedPrefix = [&](std::size_t end) {
        IDCompositionVisual2* predecessor = floatingPopupDcompLiveVisual_.Get();
        HRESULT restoreHr = S_OK;
        for (std::size_t index = 0; index < end; ++index)
        {
            IDCompositionVisual2* visual = bottomToTop[index];
            if (!visual || !removedFromRoot[index])
                continue;
            const HRESULT hr =
                floatingPopupDcompVisual_->AddVisual(
                    visual, TRUE, predecessor);
            if (SUCCEEDED(hr))
                predecessor = visual;
            else if (SUCCEEDED(restoreHr))
                restoreHr = hr;
        }
        return restoreHr;
    };

    for (std::size_t index = 0;
         index < bottomToTop.size(); ++index)
    {
        IDCompositionVisual2* visual = bottomToTop[index];
        if (visual)
        {
            const HRESULT hr =
                floatingPopupDcompVisual_->RemoveVisual(visual);
            if (FAILED(hr))
            {
                const HRESULT restoreHr =
                    restoreRemovedPrefix(index);
                if (FAILED(restoreHr))
                {
                    wchar_t message[176]{};
                    wsprintfW(
                        message,
                        L"Floating popup root z-order rollback FAILED "
                        L"hr=0x%08X after remove hr=0x%08X",
                        static_cast<unsigned>(restoreHr),
                        static_cast<unsigned>(hr));
                    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Menus, message, DiagnosticLogLevel::Warning);
                }
                return hr;
            }
            removedFromRoot[index] = true;
        }
    }

    IDCompositionVisual2* predecessor = floatingPopupDcompLiveVisual_.Get();
    std::array<bool, 2> attachedToRoot{};
    HRESULT addFailure = S_OK;
    for (std::size_t index = 0;
         index < bottomToTop.size(); ++index)
    {
        IDCompositionVisual2* visual = bottomToTop[index];
        if (!visual)
            continue;
        const HRESULT hr = floatingPopupDcompVisual_->AddVisual(
            visual, TRUE, predecessor);
        if (FAILED(hr))
        {
            if (SUCCEEDED(addFailure))
                addFailure = hr;
            continue;
        }
        attachedToRoot[index] = true;
        predecessor = visual;
    }

    if (FAILED(addFailure))
    {
        predecessor = floatingPopupDcompLiveVisual_.Get();
        HRESULT restoreHr = S_OK;
        for (std::size_t index = 0;
             index < bottomToTop.size(); ++index)
        {
            IDCompositionVisual2* visual = bottomToTop[index];
            if (!visual)
                continue;
            if (attachedToRoot[index])
            {
                predecessor = visual;
                continue;
            }
            const HRESULT hr =
                floatingPopupDcompVisual_->AddVisual(
                    visual, TRUE, predecessor);
            if (SUCCEEDED(hr))
            {
                attachedToRoot[index] = true;
                predecessor = visual;
            }
            else if (SUCCEEDED(restoreHr))
            {
                restoreHr = hr;
            }
        }
        if (FAILED(restoreHr))
        {
            wchar_t message[176]{};
            wsprintfW(
                message,
                L"Floating popup root z-order reattach FAILED "
                L"hr=0x%08X after add hr=0x%08X",
                static_cast<unsigned>(restoreHr),
                static_cast<unsigned>(addFailure));
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Menus, message, DiagnosticLogLevel::Warning);
        }
        return addFailure;
    }
    return S_OK;
}

void DesktopApp::ResetFloatingPopupCompositionResources()
{
    // A transparent DComp frame does not make its native HWND click-through.
    // Retire input before discarding the last usable pixels.
    PreserveFloatingPopupDragCapture();
    collectionPopupBackdropCompositor_.HidePopupWindowPair(floatingPopupHwnd_);
    if (floatingPopupDcompLiveVisual_)
        floatingPopupDcompLiveVisual_->SetContent(nullptr);
    if (floatingStackIndicatorVisual_ && floatingPopupDcompVisual_)
        (void)floatingPopupDcompVisual_->RemoveVisual(floatingStackIndicatorVisual_.Get());
    floatingStackIndicatorVisual_.Reset();
    floatingPopupSurfaces_.Release();
    collectionPopupBackdropCompositor_.RemoveSrgbContent(
        reinterpret_cast<std::uintptr_t>(&luaStackIndicator_));
}

void DesktopApp::SuspendFloatingPopupPresentation()
{
    PreserveFloatingPopupDragCapture();
    collectionPopupBackdropCompositor_.HidePopupWindowPair(floatingPopupHwnd_);
    if (floatingPopupHwnd_ && IsWindow(floatingPopupHwnd_) &&
        ShouldShowFloatingPopupWindow() &&
        floatingPopupPresentationRetryCount_ < kPopupPresentationRetryLimit)
        SetTimer(floatingPopupHwnd_, kPopupPresentationRetryTimerId,
            kPopupPresentationRetryIntervalMs, nullptr);
}

void DesktopApp::PreserveFloatingPopupDragCapture()
{
    const bool dragging = dragSession_.IsActive() ||
        widgetAction_ == WidgetAction::Move ||
        widgetAction_ == WidgetAction::Resize || largeIconGesture_;
    if (dragging && !dragDropController_.IsTransportActive() &&
        floatingPopupHwnd_ &&
        snowdesktop::private_pointer::Capture() == floatingPopupHwnd_ &&
        hwnd_ && IsWindow(hwnd_))
    {
        // Capture changes synchronously notify the old host. Its existing
        // owned-host guard preserves the gesture once the desktop owns it.
        snowdesktop::private_pointer::SetPointerCapture(hwnd_);
    }
}

void DesktopApp::RecoverFloatingPopupCompositionFailure(
    const wchar_t* stage, HRESULT hr)
{
    if (RequestGraphicsDeviceRecovery(stage, hr))
        return;
    wchar_t message[208]{};
    wsprintfW(
        message,
        L"FloatingPopup %s FAILED hr=0x%08X; resetting composition surface",
        stage ? stage : L"Render",
        static_cast<unsigned>(hr));
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Menus, message, DiagnosticLogLevel::Warning);
    ResetFloatingPopupCompositionResources();
    if (floatingPopupHwnd_ && IsWindow(floatingPopupHwnd_) && floatingPopupSurfaces_.BeginLocalRecovery())
        InvalidateRect(floatingPopupHwnd_, nullptr, FALSE);
}

void DesktopApp::DestroyFloatingPopupWindow()
{
    StopFloatingPopupOutsideClickMonitor();
    ForgetLuaWidgetPanelCapture(floatingPopupHwnd_);
    collectionPopupBackdropCompositor_.Reset();
    if (floatingPopupDropTargetRegistered_ &&
        floatingPopupHwnd_ && IsWindow(floatingPopupHwnd_))
    {
        RevokeDragDrop(floatingPopupHwnd_);
    }
    floatingPopupDropTargetRegistered_ = false;
    ResetFloatingPopupCompositionResources();
    floatingPopupDcompLiveVisual_.Reset();
    floatingPopupDcompVisual_.Reset();
    floatingPopupDcompTarget_.Reset();
    floatingPopupContentBounds_ = {};
    floatingPopupCollectionRegion_ = {};
    floatingPopupLuaPanelRegion_ = {};
    floatingStackIndicatorRegion_ = {};
    floatingPopupModalRegion_ = false;
    floatingPopupSurfaces_.EndLocalRecovery();
    if (floatingPopupHwnd_ && IsWindow(floatingPopupHwnd_))
        DestroyWindow(floatingPopupHwnd_);
    floatingPopupHwnd_ = nullptr;
}

HRESULT DesktopApp::EnsureFloatingPopupCompositionRoot()
{
    if (!dcompDevice_ || !floatingPopupHwnd_ ||
        !IsWindow(floatingPopupHwnd_))
        return E_UNEXPECTED;

    if (!floatingPopupDcompTarget_)
    {
        HRESULT hr = dcompDevice_->CreateTargetForHwnd(
            floatingPopupHwnd_, FALSE,
            &floatingPopupDcompTarget_);
        if (FAILED(hr))
            return hr;
    }
    if (!floatingPopupDcompVisual_)
    {
        RECT windowFrame{};
        if (!GetWindowRect(floatingPopupHwnd_, &windowFrame))
            return E_FAIL;
        ComPtr<IDCompositionVisual2> root, live;
        HRESULT hr = dcompDevice_->CreateVisual(&root);
        if (SUCCEEDED(hr)) hr = dcompDevice_->CreateVisual(&live);
        if (SUCCEEDED(hr)) hr = root->SetOffsetX(static_cast<float>(virtualLeft_ - windowFrame.left));
        if (SUCCEEDED(hr)) hr = root->SetOffsetY(static_cast<float>(virtualTop_ - windowFrame.top));
        if (SUCCEEDED(hr)) hr = root->AddVisual(live.Get(), TRUE, nullptr);
        if (SUCCEEDED(hr)) hr = floatingPopupDcompTarget_->SetRoot(root.Get());
        if (FAILED(hr))
            return hr;
        floatingPopupDcompVisual_ = std::move(root);
        floatingPopupDcompLiveVisual_ = std::move(live);
    }
    return S_OK;
}

HRESULT snowdesktop::FloatingPopupCompositionSurfaces::Layer::Prepare(
    IDCompositionDevice2* device, UINT newWidth, UINT newHeight)
{
    if (surface && width == newWidth && height == newHeight)
        return S_OK;

    Microsoft::WRL::ComPtr<IDCompositionSurface> replacement;
    const HRESULT hr = device->CreateSurface(newWidth, newHeight, DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED, &replacement);
    if (FAILED(hr) || !replacement)
        return FAILED(hr) ? hr : E_FAIL;
    surface = std::move(replacement);
    width = newWidth;
    height = newHeight;
    return S_OK;
}

HRESULT snowdesktop::FloatingPopupCompositionSurfaces::PrepareContent(
    IDCompositionDevice2* device, const RECT& bounds)
{
    return content_.Prepare(device,
        static_cast<UINT>(std::max<LONG>(1, bounds.right - bounds.left)),
        static_cast<UINT>(std::max<LONG>(1, bounds.bottom - bounds.top)));
}

HRESULT snowdesktop::FloatingPopupCompositionSurfaces::PrepareIndicator(
    IDCompositionDevice2* device, SIZE size)
{
    return indicator_.Prepare(device, static_cast<UINT>(size.cx), static_cast<UINT>(size.cy));
}

void snowdesktop::FloatingPopupCompositionSurfaces::Release()
{
    content_ = {};
    indicator_ = {};
}

bool snowdesktop::FloatingPopupCompositionSurfaces::BeginLocalRecovery()
{
    return !std::exchange(recovering_, true);
}

HRESULT DesktopApp::PrepareFloatingStackIndicatorComposition(SIZE size)
{
    if (!floatingStackIndicatorVisual_)
    {
        ComPtr<IDCompositionVisual2> visual;
        HRESULT hr = dcompDevice_->CreateVisual(&visual);
        if (SUCCEEDED(hr))
            hr = floatingPopupDcompVisual_->AddVisual(
                visual.Get(), FALSE, floatingPopupDcompLiveVisual_.Get());
        if (FAILED(hr)) return hr;
        floatingStackIndicatorVisual_ = std::move(visual);
    }
    return floatingPopupSurfaces_.PrepareIndicator(dcompDevice_.Get(), size);
}

bool DesktopApp::RenderFloatingPopupCompositionFrame()
{
    if (graphicsDeviceRecovery_.Pending())
        return false;
    if (!ShouldShowFloatingPopupWindow() ||
        (IsRectEmpty(&floatingPopupContentBounds_) && IsRectEmpty(&floatingStackIndicatorRegion_)) ||
        floatingPopupCompositionPaintInProgress_)
        return false;

    floatingPopupCompositionPaintInProgress_ = true;
    struct PaintScope final
    {
        DesktopApp& app;
        ~PaintScope()
        {
            app.floatingPopupCompositionPaintInProgress_ = false;
            app.ScheduleDeferredDesktopWidgetPresentation();
        }
    } paintScope{ *this };
    const bool retireCollection = popupAnimationOverlay_.srgbRetirementBounds.has_value();
    const bool retireLuaPanel = luaWidgetPanelAnimationOverlay_.srgbRetirementBounds.has_value();

    // Coalesce arriving folder/icons before BeginDraw on the host surface.
    // The animated child keeps its native timeline while only its pixels change.
    RefreshCollectionPopupAnimationContent();
    // Each surface keeps its own position in the shared precomposed visual tree.
    const auto paintSurface = [&](IDCompositionSurface& surface, IDCompositionVisual2& visual,
        const RECT& bounds, std::uintptr_t key, const auto& draw) -> HRESULT {
        ComPtr<ID2D1DeviceContext> context;
        POINT offset{};
        HRESULT hr = surface.BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
            reinterpret_cast<void**>(context.GetAddressOf()), &offset);
        if (FAILED(hr)) return hr;
        if (!context)
        {
            (void)surface.EndDraw();
            return E_UNEXPECTED;
        }

        context->SetDpi(96.0f, 96.0f);
        context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        context->SetTransform(D2D1::Matrix3x2F::Translation(
            static_cast<float>(offset.x - bounds.left),
            static_cast<float>(offset.y - bounds.top)));
        context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        context->Clear(D2D1::ColorF(0, 0, 0, 0));

        DesktopBackdropCompositor::SrgbSceneRecording scene(context.Get());
        drawing_.Reset();
        draw(context.Get());
        const HRESULT recorded = scene.Finish();
        const bool srgbPrepared = SUCCEEDED(recorded) && collectionPopupBackdropCompositor_.StageSrgbScene(
            floatingPopupHwnd_, key, bounds, context.Get(), offset, scene.Scene());
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context.Reset();
        drawing_.Reset();

        // Publish the painted surface and offset together, after EndDraw.
        hr = surface.EndDraw();
        if (SUCCEEDED(hr)) hr = visual.SetOffsetX(static_cast<float>(bounds.left));
        if (SUCCEEDED(hr)) hr = visual.SetOffsetY(static_cast<float>(bounds.top));
        if (FAILED(hr)) return hr;
        if (!srgbPrepared) return FAILED(recorded) ? recorded : E_FAIL;
        return visual.SetContent(&surface);
    };

    HRESULT hr = EnsureFloatingPopupCompositionRoot();
    if (FAILED(hr))
    {
        RecoverFloatingPopupCompositionFailure(L"Create popup root", hr);
        return false;
    }
    popupFanBackdropCandidates_.clear();
    auto& backdrop = collectionPopupBackdropCompositor_;
    const auto srgbKey = reinterpret_cast<std::uintptr_t>(floatingPopupHwnd_);
    const RECT bounds = floatingPopupContentBounds_;
    snowdesktop::CollectionPopupIconMaterials::PaintResult liveIcons;
    const auto paintOverlays = [this, &liveIcons](ID2D1DeviceContext* context) {
        renderingFloatingPopup_ = true;
        DrawDynamicOverlays(context, false, nullptr, &liveIcons);
        renderingFloatingPopup_ = false;
    };
    if (IsRectEmpty(&bounds))
    {
        hr = floatingPopupDcompLiveVisual_->SetContent(nullptr);
    }
    else
    {
        const auto* collection = GetOpenPopupWidget();
        bool stagedFan = false;
        // The fan has no snapshot overlay. While its host is visible, reuse the
        // transparent DComp layer and draw the moving scene on its sRGB buffer.
        // The surrounding frame still publishes the matching glass with it.
        if (collection && UsesCollectionPopupFan(*collection) && popupAnimation_.IsAnimating() &&
            IsWindowVisible(floatingPopupHwnd_) && backdrop.HasSrgbContent(srgbKey) &&
            floatingPopupSurfaces_.Content() && !retireCollection && !retireLuaPanel &&
            !popupAnimationOverlay_.active && !IsLuaPanelHostedByFloatingWindow())
        {
            snowdesktop::performance::Scope trace("popup", "paint.fan_scene");
            stagedFan = backdrop.StageSrgbSceneFrame(floatingPopupHwnd_, srgbKey, bounds, d2dDevice_.Get(),
                [&](ID2D1DeviceContext* context, POINT offset) {
                    context->SetDpi(96.0f, 96.0f);
                    context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
                    context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
                    context->SetTransform(D2D1::Matrix3x2F::Translation(
                        static_cast<float>(offset.x - bounds.left),
                        static_cast<float>(offset.y - bounds.top)));
                    // Match the source rectangle of the command-list replay.
                    context->PushAxisAlignedClip(snowdesktop::ToD2DRect(bounds), D2D1_ANTIALIAS_MODE_ALIASED);
                    context->Clear(D2D1::ColorF(0, 0, 0, 0));
                    drawing_.Reset();
                    paintOverlays(context);
                    context->PopAxisAlignedClip();
                    context->SetTransform(D2D1::Matrix3x2F::Identity());
                    drawing_.Reset();
                    return S_OK;
                });
        }
        if (!stagedFan)
        {
            popupFanBackdropCandidates_.clear();
            hr = floatingPopupSurfaces_.PrepareContent(dcompDevice_.Get(), bounds);
            if (SUCCEEDED(hr))
                hr = paintSurface(*floatingPopupSurfaces_.Content(), *floatingPopupDcompLiveVisual_.Get(),
                    bounds, srgbKey, paintOverlays);
        }
    }
    if (FAILED(hr))
    {
        RecoverFloatingPopupCompositionFailure(L"Paint floating popup", hr);
        return false;
    }

    const auto indicatorKey = reinterpret_cast<std::uintptr_t>(&luaStackIndicator_);
    const bool showIndicator = !IsRectEmpty(&floatingStackIndicatorRegion_);
    if (!showIndicator)
    {
        if (floatingStackIndicatorVisual_)
            hr = floatingStackIndicatorVisual_->SetContent(nullptr);
    }
    else
    {
        const RECT indicator = InflateCopy(floatingStackIndicatorRegion_, 6);
        hr = PrepareFloatingStackIndicatorComposition(
            SIZE{indicator.right - indicator.left, indicator.bottom - indicator.top});
        if (SUCCEEDED(hr))
            hr = paintSurface(*floatingPopupSurfaces_.Indicator(), *floatingStackIndicatorVisual_.Get(),
                indicator, indicatorKey, [this](ID2D1DeviceContext* context) { DrawLuaStackIndicator(context); });
    }
    if (FAILED(hr))
    {
        RecoverFloatingPopupCompositionFailure(L"Paint stack indicator", hr);
        return false;
    }

    // The live scene and snapshots share one compositor. Publish only after
    // both draws finish, then detach outgoing sprites before its next commit.
    if ((showIndicator && !backdrop.PublishSrgbContent(indicatorKey)) ||
        (!IsRectEmpty(&bounds) && !backdrop.PublishSrgbContent(srgbKey)))
    {
        RecoverFloatingPopupCompositionFailure(L"Publish floating popup", E_FAIL);
        return false;
    }
    if (liveIcons) PublishCollectionPopupIconBackdrops(std::move(*liveIcons));
    if (!showIndicator) backdrop.RemoveSrgbContent(indicatorKey);
    if (IsRectEmpty(&bounds)) backdrop.RemoveSrgbContent(srgbKey);
    if (retireCollection)
        popupAnimationOverlay_.FinishSrgbRetirement(backdrop, UiCompositionAnimationHost::FloatingPopup);
    if (retireLuaPanel)
        luaWidgetPanelAnimationOverlay_.FinishSrgbRetirement(backdrop, UiCompositionAnimationHost::FloatingPopup);

    const std::array order{indicatorKey, srgbKey, reinterpret_cast<std::uintptr_t>(&popupAnimationOverlay_),
        reinterpret_cast<std::uintptr_t>(&luaWidgetPanelAnimationOverlay_)};
    backdrop.SetSrgbContentOrder(order);
    if (std::exchange(floatingPopupBackdropUpdatePending_, false))
        UpdateCollectionPopupBackdrop(false);
    PublishCollectionPopupFanBackdrops();
    ApplyCollectionPopupBackdropAnimationFrame(false);

    if (!CommitCompositionAnimationFrame())
    {
        RecoverFloatingPopupCompositionFailure(
            L"Queue Commit", E_FAIL);
        return false;
    }
    floatingPopupSurfaces_.EndLocalRecovery();
    return true;
}

bool DesktopApp::PresentFloatingPopupComposition()
{
    if (!ShouldShowFloatingPopupWindow())
        return true;
    if (!floatingPopupHwnd_ ||
        !IsWindow(floatingPopupHwnd_))
        return false;
    if (floatingPopupCompositionPaintInProgress_)
    {
        InvalidateRect(floatingPopupHwnd_, nullptr, FALSE);
        return false;
    }
    if (!RenderFloatingPopupCompositionFrame())
    {
        SuspendFloatingPopupPresentation();
        return false;
    }
    ValidateRect(floatingPopupHwnd_, nullptr);
    if (!FlushPendingCompositionCommit())
    {
        ResetFloatingPopupCompositionResources();
        SuspendFloatingPopupPresentation();
        return false;
    }
    if (!collectionPopupBackdropCompositor_.CommitSrgbContent())
    {
        RecoverFloatingPopupCompositionFailure(L"Precomposed content", E_FAIL);
        return false;
    }
    KillTimer(floatingPopupHwnd_, kPopupPresentationRetryTimerId);
    floatingPopupPresentationRetryCount_ = 0;
    SyncLuaWidgetTooltip();
    return true;
}

void DesktopApp::InvalidateFloatingPopupWindow(
    bool immediatePresent)
{
    if (folderPreview_ && folderPreview_->inlineBounds && hwnd_)
        InvalidateRect(hwnd_, &*folderPreview_->inlineBounds, FALSE);
    if (!ShouldShowFloatingPopupWindow() ||
        !floatingPopupHwnd_ ||
        !IsWindow(floatingPopupHwnd_) ||
        !IsWindowVisible(floatingPopupHwnd_))
        return;
    InvalidateRect(floatingPopupHwnd_, nullptr, FALSE);
    if (immediatePresent &&
        !floatingPopupCompositionPaintInProgress_)
        UpdateWindow(floatingPopupHwnd_);
}

void DesktopApp::ApplyFloatingPopupLayerPolicy()
{
    if (!floatingPopupHwnd_ ||
        !IsWindow(floatingPopupHwnd_) ||
        !IsWindowVisible(floatingPopupHwnd_))
        return;
    const bool shouldBeTopmost =
        snowdesktop::floating_popup_rules::ShouldBeTopmost(
            true, shellPopupMenuLayerDepth_);
    const MenuHostZOrderSnapshot menuZOrderBefore =
        CaptureMenuHostZOrder(floatingPopupHwnd_);
    HWND preserveAboveWindow =
        snowdesktop::modern_menu::ActiveRootWindow();
    if (preserveAboveWindow &&
        GetWindow(preserveAboveWindow, GW_OWNER) !=
            floatingPopupHwnd_)
    {
        preserveAboveWindow = nullptr;
    }
    HWND insertAfter = shouldBeTopmost
        ? HWND_TOPMOST : HWND_NOTOPMOST;
    if (popupAnchoredToDock_ && collectionPopupDockHost_ &&
        collectionPopupDockHost_->active &&
        collectionPopupDockHost_->hwnd &&
        IsWindowVisible(collectionPopupDockHost_->hwnd) &&
        snowdesktop::popup_window_pair_z_order::IsTopmost(
            collectionPopupDockHost_->hwnd) == shouldBeTopmost)
    {
        const auto& dockHost = *collectionPopupDockHost_;
        const HWND next = GetWindow(dockHost.hwnd, GW_HWNDNEXT);
        insertAfter = dockHost.backdrop.IsBackdropWindow(next)
            ? next : dockHost.hwnd;
        if (dockHost.container &&
            dockHost.container->IsMergedWithStatusBar() && statusBar_)
            if (const HWND stripBottom =
                    statusBar_->MergedPresentationBottomWindow(
                        dockHost.monitor))
                insertAfter = stripBottom;
    }
    if (!collectionPopupBackdropCompositor_.IsAvailable())
    {
        if (insertAfter == HWND_TOPMOST ||
            insertAfter == HWND_NOTOPMOST)
            snowdesktop::popup_window_pair_z_order::MaintainContentBand(
                floatingPopupHwnd_, shouldBeTopmost, preserveAboveWindow);
        else
            snowdesktop::popup_window_pair_z_order::Apply(
                floatingPopupHwnd_, nullptr, insertAfter,
                shouldBeTopmost, POINT{}, SIZE{}, preserveAboveWindow);
    }
    else
        collectionPopupBackdropCompositor_.SetPopupWindowPairZOrder(
            floatingPopupHwnd_, insertAfter,
            shouldBeTopmost, preserveAboveWindow);
    ApplyDragPreviewLayerPolicy();
    TraceMenuHostZOrderTransition(
        L"apply-layer-policy", menuZOrderBefore,
        CaptureMenuHostZOrder(floatingPopupHwnd_));
}

bool DesktopApp::NeedsCollectionPopupMaterialPanel(const DesktopWidget& widget) const
{
    if (UsesCollectionPopupFan(widget)) return false;
    if (collectionPopupGlassTheme_) return true;
    if (UsesCollectionPopupList(widget)) return false;
    if (!CollectionPopupIconBackdrops().empty()) return true;
    // The first snapshot can contain placeholders. Reserve the animated owner
    // when the configured styles allow asynchronous icons to request glass.
    if (iconBeautifySettings_.enabled && iconBeautifySettings_.glassEnabled) return true;
    const auto usesGlass = [](const snowdesktop::IconAppearanceRecord& record) {
        return !record.followGlobalTheme && record.settings.enabled && record.settings.glassEnabled;
    };
    if (const auto* preview = iconAppearancePreview_.Effective(iconAppearanceTarget_, nullptr);
        preview && usesGlass(*preview)) return true;
    return std::any_of(iconAppearanceOverrides_.begin(), iconAppearanceOverrides_.end(),
        [&](const auto& entry) { return usesGlass(entry.second); });
}

void DesktopApp::ApplyCollectionPopupBackdropAnimationFrame(bool submit)
{
    if (floatingPopupBackdropUpdatePending_ || !collectionPopupBackdropCompositor_.IsAvailable())
        return;

    const auto* collection = IsCollectionPopupHostedByFloatingWindow() ? GetOpenPopupWidget() : nullptr;
    bool visible = collectionPopupGlassTheme_ && IsLuaStackIndicatorVisible();
    const bool fan = collection && UsesCollectionPopupFan(*collection);
    if (fan)
        visible = visible || (popupFanBackdropCount_ > 0 && popupAnimation_.GetVisual().visible);
    if (collection && NeedsCollectionPopupMaterialPanel(*collection))
    {
        const auto visual = popupAnimation_.GetVisual();
        const bool animatingNatively = popupAnimationCompositorDriven_ && popupAnimation_.IsAnimating();
        POINT anchor{(popupRect_.left + popupRect_.right) / 2, (popupRect_.top + popupRect_.bottom) / 2};
        if (popupHasAnchor_)
        {
            anchor.x = std::clamp(popupAnchorPoint_.x, popupRect_.left, popupRect_.right);
            anchor.y = std::clamp(popupAnchorPoint_.y, popupRect_.top, popupRect_.bottom);
        }
        collectionPopupBackdropCompositor_.SetPopupPanelTransform(
            reinterpret_cast<std::uintptr_t>(&popupAnimation_),
            visual.scale, visual.visible ? visual.opacity : 0.0f,
            anchor, animatingNatively);
        visible = visible || animatingNatively || visual.visible;
    }
    if (collectionPopupGlassTheme_ && IsLuaPanelHostedByFloatingWindow())
    {
        const auto visual = luaWidgetPanelAnimation_.GetVisual();
        const bool animatingNatively = luaWidgetPanelAnimationCompositorDriven_ &&
            luaWidgetPanelAnimation_.IsAnimating();
        const POINT anchor = GetLuaWidgetPanelAnimationAnchor(GetLuaWidgetPanelRect());
        collectionPopupBackdropCompositor_.SetPopupPanelTransform(
            reinterpret_cast<std::uintptr_t>(&luaWidgetPanelAnimation_),
            visual.scale, visual.visible ? visual.opacity : 0.0f,
            anchor, animatingNatively);
        visible = visible || animatingNatively || visual.visible;
    }
    collectionPopupBackdropCompositor_.SetVisible(
        visible && floatingPopupHwnd_ && IsWindowVisible(floatingPopupHwnd_));
    // Explicitly submit the same logical frame as the content. Relying on
    // WUC's implicit commit cycle can leave the glass behind a busy UI thread.
    if (submit) collectionPopupBackdropCompositor_.CommitVisualChanges();
}

void DesktopApp::UpdateCollectionPopupBackdrop(bool submit)
{
    snowdesktop::performance::Scope scope("popup", "backdrop.update");
    const auto* popup = IsCollectionPopupHostedByFloatingWindow() ? GetOpenPopupWidget() : nullptr;
    const bool fan = popup && UsesCollectionPopupFan(*popup);
    const bool collectionPanel = popup && NeedsCollectionPopupMaterialPanel(*popup);
    const bool luaGlass = collectionPopupGlassTheme_ && IsLuaPanelHostedByFloatingWindow();
    const bool stackGlass = collectionPopupGlassTheme_ && IsLuaStackIndicatorVisible();
    if (!floatingPopupHwnd_ || !IsWindow(floatingPopupHwnd_))
    {
        collectionPopupBackdropCompositor_.Reset();
        popupFanBackdropCount_ = 0;
        return;
    }
    if (!collectionPanel && !collectionPopupGlassTheme_ && !fan)
    {
        if (submit)
            collectionPopupBackdropCompositor_.ResetBackdrop();
        else
        {
            collectionPopupBackdropCompositor_.BeginFrame(true);
            collectionPopupBackdropCompositor_.EndFrame(false);
        }
        popupFanBackdropCount_ = 0;
        return;
    }
    if ((!fan && !collectionPanel && !luaGlass && !stackGlass) ||
        (IsRectEmpty(&floatingPopupContentBounds_) && IsRectEmpty(&floatingStackIndicatorRegion_)))
    {
        collectionPopupBackdropCompositor_.SetVisible(false);
        return;
    }

    const bool topmost =
        snowdesktop::floating_popup_rules::ShouldBeTopmost(
            true, shellPopupMenuLayerDepth_);
    const MenuHostZOrderSnapshot menuZOrderBefore =
        CaptureMenuHostZOrder(floatingPopupHwnd_);
    const wchar_t* zOrderStage =
        collectionPopupBackdropCompositor_.IsAvailable()
            ? L"backdrop-reattach" : L"backdrop-initialize";
    if (!collectionPopupBackdropCompositor_.IsAvailable())
    {
        const bool initiallyVisible =
            IsWindowVisible(floatingPopupHwnd_) != FALSE &&
            (stackGlass || (collectionPanel && popupAnimation_.GetVisual().visible) ||
                (luaGlass && luaWidgetPanelAnimation_.GetVisual().visible));
        if (!collectionPopupBackdropCompositor_.InitializePopup(
                floatingPopupHwnd_, topmost, initiallyVisible))
        {
            std::wstring message =
                L"Collection popup native backdrop unavailable: ";
            message += collectionPopupBackdropCompositor_.LastError();
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Menus, message.c_str(), DiagnosticLogLevel::Warning);
            return;
        }
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Menus,
            L"Collection popup native CompositionBackdropBrush initialized");
    }
    else
    {
        collectionPopupBackdropCompositor_.SetPopupTopmost(topmost);
        collectionPopupBackdropCompositor_.Reattach(
            floatingPopupHwnd_);
    }
    TraceMenuHostZOrderTransition(
        zOrderStage, menuZOrderBefore,
        CaptureMenuHostZOrder(floatingPopupHwnd_));

    collectionPopupBackdropCompositor_.BeginFrame(true);
    if (fan && IsWindowVisible(floatingPopupHwnd_))
    {
        for (std::size_t i = 0; i < popupFanBackdropCount_; ++i)
            (void)collectionPopupBackdropCompositor_.KeepPanel(popupFanBackdropViewport_,
                static_cast<std::uintptr_t>(i + 1));
    }
    else popupFanBackdropCount_ = 0;
    const auto addPanel = [&](const RECT& panel, float radius, const void* owner) {
        collectionPopupBackdropCompositor_.AddPanel(
            panel, radius, collectionPopupBlurRadius_, reinterpret_cast<std::uintptr_t>(owner));
    };
    if (collectionPanel)
    {
        RECT panel = GetCollectionPopupRect(*popup);
        if (const auto* motion = GetCollectionPopupResizeTransition()) panel = motion->Panel(panel);
        const auto key = reinterpret_cast<std::uintptr_t>(&popupAnimation_);
        const bool iconsOnly = !collectionPopupGlassTheme_;
        const float radius = 18.0f * GetCollectionPopupLayoutMetrics(*popup).scale;
        const bool prepared = collectionPopupBackdropCompositor_.AddPanel(
            panel, radius, iconsOnly ? 0.f : collectionPopupBlurRadius_, key);
        if (!UsesCollectionPopupList(*popup))
        {
            // The silhouette and body have the same local coordinates and the
            // same animated parent, including snapshot-to-live replacement.
            if (!prepared || !collectionPopupBackdropCompositor_.SetPanelIconRegions(key, d2dDevice_.Get(),
                    CollectionPopupIconBackdrops(), false, 0.f, iconsOnly))
                (void)collectionPopupBackdropCompositor_.SetPanelIconRegions(key, nullptr, {}, false, 0.f, iconsOnly);
        }
    }
    if (luaGlass)
        addPanel(GetLuaWidgetPanelRect(), kLuaWidgetPanelCornerRadius, &luaWidgetPanelAnimation_);
    if (stackGlass)
    {
        const auto& stack = widgets_[FindWidgetIndexById(luaStackIndicator_.stackId)];
        addPanel(luaStackIndicator_.bounds,
            static_cast<float>(ScaleWidgetCu(12.0f, GetWidgetCellScale(stack))), &luaStackIndicator_);
    }
    // Include geometry and the current pose in one backdrop transaction.
    collectionPopupBackdropCompositor_.EndFrame(false);
    if (submit) ApplyCollectionPopupBackdropAnimationFrame();
}

void DesktopApp::UpdateFloatingPopupWindowBounds(
    bool immediatePresent, bool refreshStableContent)
{
    SyncPopupMouseLeaveTimer();
    if (!ShouldShowFloatingPopupWindow())
    {
        luaWidgetTooltip_.Hide();
        PreserveFloatingPopupDragCapture();
        if (floatingPopupHwnd_) KillTimer(floatingPopupHwnd_, kPopupPresentationRetryTimerId);
        floatingPopupPresentationRetryCount_ = 0;
        StopFloatingPopupOutsideClickMonitor();
        collectionPopupBackdropCompositor_.HidePopupWindowPair(
            floatingPopupHwnd_);
        popupAnimationOverlay_.FinishSrgbRetirement(
            collectionPopupBackdropCompositor_, UiCompositionAnimationHost::FloatingPopup);
        luaWidgetPanelAnimationOverlay_.FinishSrgbRetirement(
            collectionPopupBackdropCompositor_, UiCompositionAnimationHost::FloatingPopup);
        floatingPopupBackdropUpdatePending_ = false;
        floatingPopupContentBounds_ = {};
        floatingPopupCollectionRegion_ = {};
        floatingPopupLuaPanelRegion_ = {};
        floatingStackIndicatorRegion_ = {};
        floatingPopupModalRegion_ = false;
        return;
    }
    if (!CreateFloatingPopupWindow())
    {
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Menus,
            L"Floating popup window creation FAILED", DiagnosticLogLevel::Warning);
        return;
    }
    if (IsCollectionPopupHostedByFloatingWindow() || IsLuaPanelHostedByFloatingWindow())
        StartFloatingPopupOutsideClickMonitor();
    else StopFloatingPopupOutsideClickMonitor();

    const RECT nextBounds =
        CalculateFloatingPopupWindowBounds();
    const RECT nextStackRegion = IsLuaStackIndicatorVisible() ? luaStackIndicator_.bounds : RECT{};
    if (IsRectEmpty(&nextBounds) && IsRectEmpty(&nextStackRegion))
        return;
    bool wasVisible =
        IsWindowVisible(floatingPopupHwnd_) != FALSE;
    const bool boundsChanged =
        !EqualRect(&nextBounds, &floatingPopupContentBounds_);

    const RECT nextCollectionRegion = GetFloatingCollectionPopupRegion();
    const bool stackRegionChanged = !EqualRect(&nextStackRegion, &floatingStackIndicatorRegion_);
    RECT nextLuaPanelRegion{};
    const bool nextModalRegion =
        IsLuaPanelHostedByFloatingWindow() &&
        luaWidgetPanelRequest_.modal;
    if (IsLuaPanelHostedByFloatingWindow())
    {
        nextLuaPanelRegion = nextModalRegion
            ? nextBounds : GetLuaWidgetPanelRect();
    }
    const bool regionChanged =
        !EqualRect(
            &nextCollectionRegion,
            &floatingPopupCollectionRegion_) ||
        !EqualRect(
            &nextLuaPanelRegion,
            &floatingPopupLuaPanelRegion_) ||
        stackRegionChanged ||
        nextModalRegion != floatingPopupModalRegion_;

    const RECT desktopFrame{
        virtualLeft_, virtualTop_, virtualLeft_ + std::max(1, virtualWidth_), virtualTop_ + std::max(1, virtualHeight_)};
    RECT coverage = nextBounds;
    if (!IsRectEmpty(&nextStackRegion))
        coverage = UnionRects(coverage, InflateCopy(nextStackRegion, 6));
    RECT windowFrame = ResolveFloatingPopupWindowFrame(desktopFrame, coverage);
    RECT currentWindow{};
    const bool windowKnown = GetWindowRect(floatingPopupHwnd_, &currentWindow) != FALSE;
    // A monitor-local allocation can stay stable; a desktop-wide host must
    // shrink once the reserved popups and stack indicator fit on one output.
    if (wasVisible && windowKnown && !EqualRect(&currentWindow, &desktopFrame) &&
        ContainsRect(desktopFrame, currentWindow) &&
        ContainsRect(currentWindow, windowFrame))
        windowFrame = currentWindow;
    const bool windowChanged = !windowKnown || !EqualRect(&currentWindow, &windowFrame);
    const POINT sceneOffset{virtualLeft_ - windowFrame.left, virtualTop_ - windowFrame.top};
    const POINT previousOffset = collectionPopupBackdropCompositor_.PopupSceneOffset();
    const bool viewportChanged = windowChanged ||
        previousOffset.x != sceneOffset.x || previousOffset.y != sceneOffset.y;

    if (floatingPopupCompositionPaintInProgress_ && (boundsChanged || regionChanged || viewportChanged))
    {
        // A Lua draw callback can replace the popup while BeginDraw owns the
        // surface. Leave its geometry intact until that frame has ended.
        PostMessageW(floatingPopupHwnd_, kRefreshPopupBoundsMessage, 0, 0);
        return;
    }

    floatingPopupContentBounds_ = nextBounds;
    floatingPopupCollectionRegion_ = nextCollectionRegion;
    floatingPopupLuaPanelRegion_ = nextLuaPanelRegion;
    floatingStackIndicatorRegion_ = nextStackRegion;
    floatingPopupModalRegion_ = nextModalRegion;

    const bool topmost =
        snowdesktop::floating_popup_rules::ShouldBeTopmost(
            true, shellPopupMenuLayerDepth_);
    const LONG windowWidth = windowFrame.right - windowFrame.left;
    const LONG windowHeight = windowFrame.bottom - windowFrame.top;
    if (viewportChanged)
    {
        if (wasVisible && (!windowKnown || !ContainsRect(desktopFrame, currentWindow)))
        {
            // Retire the old presentation when its display has moved or
            // disappeared. Ordinary reallocations keep native focus and capture.
            SuspendFloatingPopupPresentation();
            wasVisible = false;
        }
        HRESULT placed = S_OK;
        if (floatingPopupDcompVisual_)
        {
            placed = floatingPopupDcompVisual_->SetOffsetX(static_cast<float>(sceneOffset.x));
            if (SUCCEEDED(placed))
                placed = floatingPopupDcompVisual_->SetOffsetY(static_cast<float>(sceneOffset.y));
        }
        if (FAILED(placed))
        {
            RecoverFloatingPopupCompositionFailure(L"Position popup composition root", placed);
            return;
        }
        if (!collectionPopupBackdropCompositor_.SetPopupSceneOffset(sceneOffset) ||
            (windowChanged && !SetWindowPos(floatingPopupHwnd_, nullptr, windowFrame.left, windowFrame.top,
                windowWidth, windowHeight, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER)))
        {
            floatingPopupContentBounds_ = {};
            SuspendFloatingPopupPresentation();
            return;
        }
    }
    if (wasVisible)
        ApplyFloatingPopupLayerPolicy();

    const auto* collection = IsCollectionPopupHostedByFloatingWindow() ? GetOpenPopupWidget() : nullptr;
    const bool backdropRecovery = !collectionPopupBackdropCompositor_.IsAvailable() &&
        ((collection && (UsesCollectionPopupFan(*collection) || NeedsCollectionPopupMaterialPanel(*collection))) ||
            (collectionPopupGlassTheme_ &&
                (IsLuaPanelHostedByFloatingWindow() || !IsRectEmpty(&nextStackRegion))));
    const bool refreshContent = refreshStableContent || !wasVisible ||
        boundsChanged || regionChanged || viewportChanged || backdropRecovery ||
        !floatingPopupSurfaces_.Content() || !floatingPopupDcompVisual_ ||
        graphicsDeviceRecovery_.Pending();
    if (refreshContent)
    {
        // An existing helper keeps its current glass until the new body and
        // silhouettes are painted, including the fan-to-panel change.
        if (collectionPopupBackdropCompositor_.IsAvailable())
            floatingPopupBackdropUpdatePending_ = true;
        else
            UpdateCollectionPopupBackdrop();
    }

    if (!wasVisible || boundsChanged || regionChanged || viewportChanged)
    {
        HRGN windowRegion = CreateRectRgn(0, 0, 0, 0);
        bool regionReady = windowRegion != nullptr;
        auto appendRegion = [&](RECT rect, float radius) {
            if (!windowRegion || IsRectEmpty(&rect))
                return;
            OffsetRect(&rect, sceneOffset.x, sceneOffset.y);
            HRGN added = snowdesktop::popup_round_geometry::CreateWindowFence(
                rect, radius, 0, static_cast<float>(snowdesktop::flat_glass_rim::kPanelOverdraw));
            if (added)
            {
                if (CombineRgn(
                    windowRegion, windowRegion,
                    added, RGN_OR) == ERROR)
                    regionReady = false;
                DeleteObject(added);
            }
            else regionReady = false;
        };
        const auto* popup = IsCollectionPopupHostedByFloatingWindow() ? GetOpenPopupWidget() : nullptr;
        appendRegion(floatingPopupCollectionRegion_, popup ?
            18.f * GetCollectionPopupLayoutMetrics(*popup).scale : 18.f);
        appendRegion(
            floatingPopupLuaPanelRegion_,
            floatingPopupModalRegion_ ? 0.f : kLuaWidgetPanelCornerRadius);
        if (!IsRectEmpty(&floatingStackIndicatorRegion_))
        {
            const size_t index = FindWidgetIndexById(luaStackIndicator_.stackId);
            if (index < widgets_.size())
                appendRegion(floatingStackIndicatorRegion_,
                    static_cast<float>(ScaleWidgetCu(12.0f, GetWidgetCellScale(widgets_[index]))));
        }
        if (!regionReady || !SetWindowRgn(floatingPopupHwnd_, windowRegion, FALSE))
        {
            if (windowRegion) DeleteObject(windowRegion);
            // Retry identical geometry too; failed native clipping must not
            // become a successful cached input region.
            floatingPopupContentBounds_ = {};
            SuspendFloatingPopupPresentation();
            return;
        }
    }

    const bool presentNow = immediatePresent || (wasVisible && viewportChanged);
    const bool rendered = presentNow
        ? PresentFloatingPopupComposition()
        : false;
    if (!rendered && refreshContent)
    {
        InvalidateRect(floatingPopupHwnd_, nullptr, FALSE);
        if (presentNow)
        {
            // A nested provider callback can ask for presentation while the
            // outer frame owns BeginDraw. It is deferred, not failed: preserve
            // an existing usable frame and let the outer presenter reveal a
            // hidden host only after that frame completes.
            if (!floatingPopupCompositionPaintInProgress_)
                SuspendFloatingPopupPresentation();
            return;
        }
    }

    if (snowdesktop::floating_popup_rules::ShouldRevealHost(
            wasVisible, immediatePresent) && ShouldShowFloatingPopupWindow())
    {
        const MenuHostZOrderSnapshot menuZOrderBefore =
            CaptureMenuHostZOrder(floatingPopupHwnd_);
        const BOOL positioned = SetWindowPos(
            floatingPopupHwnd_,
            topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE |
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
        TraceMenuHostZOrderTransition(
            positioned
                ? L"reveal-set-window-pos"
                : L"reveal-set-window-pos-failed",
            menuZOrderBefore,
            CaptureMenuHostZOrder(floatingPopupHwnd_),
            positioned == FALSE);
        if (!positioned)
        {
            SuspendFloatingPopupPresentation();
            return;
        }
        ApplyCollectionPopupBackdropAnimationFrame();
        snowdesktop::performance::Value("popup", "host.width", {}, windowWidth);
        snowdesktop::performance::Value("popup", "host.height", {}, windowHeight);
        wchar_t message[224]{};
        wsprintfW(
            message,
            L"Floating popup shown rect=(%ld,%ld)-(%ld,%ld) topmost=%d",
            nextBounds.left + virtualLeft_,
            nextBounds.top + virtualTop_,
            nextBounds.right + virtualLeft_,
            nextBounds.bottom + virtualTop_,
            topmost != false);
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Menus, message);
    }
    else if ((boundsChanged || regionChanged) && !immediatePresent)
    {
        InvalidateFloatingPopupWindow(false);
    }
    if (popupAnchoredToDock_ && collectionPopupDockHost_)
    {
        ApplyFloatingDockLayerPolicy();
        ApplyFloatingPopupLayerPolicy();
    }
    ApplyDragPreviewLayerPolicy();
    SyncPopupMouseLeaveTimer();
}

void DesktopApp::RequestCollectionPopupResizePresentation(bool animating)
{
    uiAnimationScheduler_.RequestFramePresentation(&floatingPopupHwnd_, [this, animating] {
        if (!IsCollectionPopupHostedByFloatingWindow()) return;
        snowdesktop::performance::Scope trace("popup", "resize.present");
        UpdateFloatingPopupWindowBounds(!animating, false);
        // A change of integer bounds dirties the popup in UpdateBounds. Paint
        // this same pose before the scheduler returns to the message queue.
        // UpdateWindow consumes a pending paint only when there is damage.
        if (animating && floatingPopupHwnd_ && IsWindowVisible(floatingPopupHwnd_) &&
            !floatingPopupCompositionPaintInProgress_)
            UpdateWindow(floatingPopupHwnd_);
    });
}

void DesktopApp::PresentFloatingPopupPointerFrame(
    bool dragPreviewAlreadySynced)
{
    const auto* view = GetCategorizedPopupView();
    const bool stableCategorizedInput = view &&
        (view->HasCategoryTabPress() || view->IsSearchPointerSelecting()) &&
        !dragSession_.IsActive() && !dragDropController_.IsTransportActive() &&
        widgetAction_ != WidgetAction::Move && widgetAction_ != WidgetAction::Resize &&
        !marqueeActive_ && !popupAnimation_.IsAnimating() &&
        !luaWidgetPanelAnimation_.IsAnimating();
    // Pointer mutation already queued any changed content. Still reconcile
    // geometry, native regions and layer policy before the synchronous paint.
    UpdateFloatingPopupWindowBounds(false, !stableCategorizedInput);
    PresentPointerInteractionFrame(dragPreviewAlreadySynced);
}

void DesktopApp::SyncPopupMouseLeaveTimer()
{
    const bool shouldPoll = ((!luaStackIndicator_.stackId.empty() && !IsRectEmpty(&luaStackIndicator_.bounds)) ||
        (!folderPreview_ && personalizationSettings_.popupCloseOnMouseLeave && IsCollectionPopupInteractive())) &&
        floatingPopupHwnd_ && IsWindow(floatingPopupHwnd_);
    if (shouldPoll)
    {
        if (!popupMouseLeaveTimerArmed_)
            popupMouseLeaveTimerArmed_ = SetTimer(floatingPopupHwnd_,
                kPopupMouseLeaveTimerId,
                IsLuaStackIndicatorVisible() ? kStackIndicatorMouseLeavePollIntervalMs : kPopupMouseLeavePollIntervalMs,
                nullptr) != 0;
    }
    else
    {
        if (popupMouseLeaveTimerArmed_ && floatingPopupHwnd_)
            KillTimer(floatingPopupHwnd_, kPopupMouseLeaveTimerId);
        popupMouseLeaveTimerArmed_ = false;
        popupMouseLeaveController_.Reset();
    }
}

void DesktopApp::UpdatePopupCloseOnMouseLeave(POINT point)
{
    if (folderPreview_ || !personalizationSettings_.popupCloseOnMouseLeave ||
        !IsCollectionPopupInteractive())
    {
        popupMouseLeaveController_.Reset();
        return;
    }
    const bool pointerInside = IsPointInsideOpenPopup(point);
    if (!popupMouseLeaveController_.Observe(
            floatingPopupMouseHookGeneration_, pointerInside))
        return;

    // OLE transport can outlive the logical drag during a synchronous Drop.
    // Its retained context must block dismissal until the commit is detached.
    const bool dragging = dragSession_.IsActive() ||
        widgetAction_ == WidgetAction::Move ||
        widgetAction_ == WidgetAction::Resize || largeIconGesture_;
    const HWND dialogOwner = ShellDialogOwnerHwnd();
    const auto observedButtons = snowdesktop::window_support::WindowSupport().PointerButtons();
    const bool pointerPressed = snowdesktop::private_pointer::Capture() || mouseDown_ ||
        dragSession_.HasContext() || middleButtonWidgetMove_ ||
        widgetAction_ != WidgetAction::None ||
        (observedButtons ? (*observedButtons & 7u) != 0 :
            ((snowdesktop::private_pointer::AsyncKeyState(VK_LBUTTON) | snowdesktop::private_pointer::AsyncKeyState(VK_RBUTTON) |
                snowdesktop::private_pointer::AsyncKeyState(VK_MBUTTON)) & 0x8000) != 0);
    const bool interactionBusy = HasActiveContextMenuSession() ||
        renameController_.IsActive() ||
        detailColumnResizeActive_ || luaWidgetPanelMouseDown_ ||
        (dialogOwner && !IsWindowEnabled(dialogOwner)) ||
        snowdesktop::floating_popup_rules::ShouldBlockMouseLeaveForPointerPress(
            dragging, pointerPressed);
    if (!snowdesktop::floating_popup_rules::ShouldCloseOnMouseLeave(
            personalizationSettings_.popupCloseOnMouseLeave,
            IsCollectionPopupInteractive(), pointerInside, interactionBusy))
        return;

    // Hiding the source HWND must not emit capture loss that cancels a native
    // drag. The existing close transition snapshots a Dock-folder source and
    // detaches only the outgoing popup target; keep the drag selection intact.
    popupMouseLeaveController_.Reset();
    CloseCollectionPopup(!dragging);
}

void DesktopApp::PaintFloatingPopupWindow(HWND hwnd)
{
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd, &paint);
    if (!dc)
        return;
    const bool rendered =
        ShouldShowFloatingPopupWindow() &&
        RenderFloatingPopupCompositionFrame();
    EndPaint(hwnd, &paint);
    if (rendered && FlushPendingCompositionCommit())
        SyncLuaWidgetTooltip();
    else if (ShouldShowFloatingPopupWindow())
        SuspendFloatingPopupPresentation();
}

POINT DesktopApp::FloatingPopupClientToDesktop(
    POINT point) const
{
    if (floatingPopupHwnd_ &&
        IsWindow(floatingPopupHwnd_) &&
        hwnd_ && IsWindow(hwnd_))
    {
        ClientToScreen(floatingPopupHwnd_, &point);
        ScreenToClient(hwnd_, &point);
    }
    return point;
}

LRESULT DesktopApp::HandleFloatingPopupMessage(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!snowdesktop::private_pointer::MessageAllowed(hwnd, msg, wp, lp)) return 0;
    const snowdesktop::UiImmediatePresentationScope immediatePresentation;
    struct NativeMenuPresentationScope final
    {
        DesktopApp& app;
        ~NativeMenuPresentationScope()
        {
            app.FlushNativeMenuPresentation();
        }
    } nativeMenuPresentationScope{ *this };

    auto desktopPoint = [&]() {
        return FloatingPopupClientToDesktop(
            POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
    };
    auto desktopLParam = [&]() {
        const POINT point = desktopPoint();
        return MAKELPARAM(point.x, point.y);
    };
    auto latestDesktopPointer = [&]() {
        const bool nativeDragActive =
            snowdesktop::drag_input_rules::IsNativeDragActive(
                dragSession_.IsActive(),
                dragDropController_.IsTransportActive());
        const bool primaryButtonDown =
            (snowdesktop::private_pointer::AsyncKeyState(PointerGestureVirtualKey()) & 0x8000) != 0;
        POINT point{};
        if (snowdesktop::drag_input_rules::
                ShouldSampleFloatingWindowPointer(
                    nativeDragActive, primaryButtonDown) &&
            snowdesktop::private_pointer::CursorPosition(&point) && hwnd_ && IsWindow(hwnd_) &&
            ScreenToClient(hwnd_, &point))
        {
            return point;
        }
        return desktopPoint();
    };

    switch (msg)
    {
    case WM_WINDOWPOSCHANGING:
        if (lp)
            PreserveModernMenuHostZOrder(
                hwnd, *reinterpret_cast<WINDOWPOS*>(lp));
        break;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    {
        // With no focused window, User32 sends WM_SYSKEYDOWN without Alt to
        // the active popup. Keep file commands in the common input handler.
        const bool historyShortcut = folderPreview_ && IsCollectionPopupKeyboardTarget() && (wp == 'Z' || wp == 'Y') &&
            (snowdesktop::private_pointer::AsyncKeyState(VK_CONTROL) & 0x8000);
        const bool previewFind = folderPreview_ && wp == 'F' && IsCollectionPopupKeyboardTarget() &&
            (snowdesktop::private_pointer::AsyncKeyState(VK_CONTROL) & 0x8000);
        const bool popupNavigation = wp == VK_UP || wp == VK_DOWN || wp == VK_LEFT ||
            wp == VK_RIGHT || wp == VK_HOME || wp == VK_END || wp == VK_TAB ||
            wp == VK_RETURN || wp == VK_ESCAPE;
        if (((!dockTrayPopup_ && (wp == VK_DELETE || historyShortcut || previewFind)) ||
                (popupNavigation && IsCollectionPopupKeyboardTarget())) &&
            IsCollectionPopupInteractive() &&
            (static_cast<ULONG_PTR>(lp) & (ULONG_PTR{1} << 29)) == 0)
        {
            const snowdesktop::private_pointer::ForwardScope forward(hwnd, WM_KEYDOWN);
            return HandleInputMessage(hwnd, WM_KEYDOWN, wp, lp);
        }
        break;
    }
    case kFloatingPopupExternalPointerMessage:
        HandleFloatingPopupExternalPointerDown(
            static_cast<std::uint32_t>(wp),
            std::bit_cast<std::uint64_t>(lp));
        return 0;
    case kRefreshPopupBoundsMessage:
        UpdateFloatingPopupWindowBounds(true);
        return 0;
    case WM_NCHITTEST:
        return ShouldShowFloatingPopupWindow()
            ? HTCLIENT : HTTRANSPARENT;
    case WM_SETCURSOR:
    {
        POINT point{};
        if (IsLuaStackIndicatorVisible() &&
            snowdesktop::private_pointer::CursorPosition(&point) &&
            hwnd_ && ScreenToClient(hwnd_, &point) && PtInRect(&luaStackIndicator_.bounds, point))
        {
            snowdesktop::private_pointer::Cursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        const snowdesktop::private_pointer::ForwardScope forward(hwnd_, msg);
        return HandleMessage(hwnd_, msg, wp, lp);
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintFloatingPopupWindow(hwnd);
        return 0;
    case WM_MOUSEMOVE:
    {
        TRACKMOUSEEVENT tracking{ sizeof(tracking) };
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = hwnd;
        snowdesktop::private_pointer::TrackLeave(&tracking);
        handlingFloatingPopupInput_ = true;
        bool dragPreviewSynced = false;
        const bool pointerUpdated = OnNativeMouseMoveAt(
            wp, latestDesktopPointer(),
            &dragPreviewSynced);
        handlingFloatingPopupInput_ = false;
        if (!pointerUpdated) return 0;
        PresentFloatingPopupPointerFrame(
            dragPreviewSynced);
        return 0;
    }
    case WM_MOUSELEAVE:
    {
        POINT cursor{};
        if (snowdesktop::private_pointer::CursorPosition(&cursor) &&
            snowdesktop::private_pointer::WindowAtPoint(cursor) == hwnd)
            return 0;
        OnMouseLeave();
        return 0;
    }
    case WM_TIMER:
        if (wp == kPopupPresentationRetryTimerId)
        {
            KillTimer(hwnd, kPopupPresentationRetryTimerId);
            if (ShouldShowFloatingPopupWindow() &&
                floatingPopupPresentationRetryCount_ < kPopupPresentationRetryLimit)
            {
                ++floatingPopupPresentationRetryCount_;
                UpdateFloatingPopupWindowBounds(true);
            }
            return 0;
        }
        if (wp == kPopupMouseLeaveTimerId)
        {
            SyncPopupMouseLeaveTimer();
            POINT point{};
            if (popupMouseLeaveTimerArmed_ && hwnd_ && IsWindow(hwnd_) &&
                snowdesktop::private_pointer::CursorPosition(&point) && ScreenToClient(hwnd_, &point))
            {
                UpdatePopupCloseOnMouseLeave(point);
                if (!luaStackIndicator_.stackId.empty())
                {
                    POINT screen = point;
                    ClientToScreen(hwnd_, &screen);
                    const HWND hit = snowdesktop::private_pointer::WindowAtPoint(screen);
                    const bool onDesktop = IsSameWindowTree(hwnd_, hit) ||
                        desktopBackdropCompositor_.IsBackdropWindow(hit);
                    const bool onFloatingPopup = IsSameWindowTree(floatingPopupHwnd_, hit) ||
                        collectionPopupBackdropCompositor_.IsBackdropWindow(hit);
                    if (!onDesktop && !onFloatingPopup) HideLuaStackIndicator();
                    else
                    {
                        const size_t owner = FindWidgetIndexById(luaStackIndicator_.stackId);
                        const RECT frame = owner < widgets_.size() ? GetStandaloneWidgetFrameRect(widgets_[owner]) : RECT{};
                        const bool onOwner = onDesktop && PtInRect(&frame, point) &&
                            HitTestStandaloneWidgetIndex(point) == owner;
                        const bool onIndicator = PtInRect(&luaStackIndicator_.bounds, point) ||
                            PtInRect(&luaStackIndicator_.bridge, point);
                        const RECT cursor{point.x, point.y, point.x + 1, point.y + 1};
                        if (IsLuaStackIndicatorVisible() && (onOwner || onIndicator) &&
                            !IsLuaStackIndicatorOccluded(cursor))
                            luaStackIndicator_.hideAfter = 0;
                        else UpdateLuaStackIndicator(point);
                    }
                }
            }
            else if (!luaStackIndicator_.stackId.empty()) HideLuaStackIndicator();
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        if (HandleLuaStackIndicatorPointer(desktopPoint(), true)) return 0;
        handlingFloatingPopupInput_ = true;
        OnLeftButtonDown(wp, desktopLParam());
        handlingFloatingPopupInput_ = false;
        InvalidateFloatingPopupWindow(true);
        return 0;
    case WM_LBUTTONUP:
        if (!luaStackIndicator_.pressedMember.empty())
        {
            if (!HandleLuaStackIndicatorPointer(desktopPoint(), false)) snowdesktop::private_pointer::ReleasePointerCapture();
            return 0;
        }
        handlingFloatingPopupInput_ = true;
        OnLeftButtonUpAt(wp, desktopPoint());
        handlingFloatingPopupInput_ = false;
        UpdateFloatingPopupWindowBounds();
        return 0;
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        luaStackIndicator_.pressedMember.clear();
        if (msg == WM_CANCELMODE) CancelRenameClick();
        ForgetLuaWidgetPanelCapture(hwnd);
        {
            const HWND currentCapture = snowdesktop::private_pointer::Capture();
            const HWND nextCapture =
                reinterpret_cast<HWND>(lp);
            const bool shouldCancel =
                snowdesktop::floating_popup_rules::
                    ShouldCancelPointerPressForHostMessage(
                        msg == WM_CANCELMODE,
                        hwnd,
                        currentCapture,
                        IsOwnedPointerCaptureWindow(
                            currentCapture),
                        IsOwnedPointerCaptureWindow(
                            nextCapture));
            if (!shouldCancel)
                return 0;
        }
        if (CanCancelPointerPressAfterCaptureLoss())
        {
            CancelPointerPressWithoutCaptureRelease();
        }
        if (msg == WM_CANCELMODE && snowdesktop::private_pointer::Capture() == hwnd)
            ReleaseCapturePreservingPointerState();
        return 0;
    case WM_LBUTTONDBLCLK:
    {
        const snowdesktop::private_pointer::ForwardScope forward(hwnd_, msg);
        handlingFloatingPopupInput_ = true;
        const LRESULT result =
            HandleMessage(hwnd_, msg, wp, desktopLParam());
        handlingFloatingPopupInput_ = false;
        InvalidateFloatingPopupWindow(true);
        return result;
    }
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
        OnMiddleButtonDown(wp, desktopLParam());
        return 0;
    case WM_MBUTTONUP:
        OnMiddleButtonUpAt(wp, desktopPoint());
        return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        OnRightButtonDown(nullptr, desktopPoint());
        return 0;
    case WM_RBUTTONUP:
        OnRightButtonUp(desktopLParam());
        UpdateFloatingPopupWindowBounds();
        return 0;
    case WM_MOUSEWHEEL:
        handlingFloatingPopupInput_ = true;
        OnMouseWheel(wp, lp);
        handlingFloatingPopupInput_ = false;
        return 0;
    case WM_DPICHANGED:
        // Placement and regions use physical pixels from display topology.
        InvalidateFloatingPopupWindow(false);
        return 0;
    case WM_DISPLAYCHANGE:
        CloseCollectionPopup(false);
        CloseLuaWidgetPanel(L"", "display-change");
        return 0;
    case WM_CLOSE:
        CloseCollectionPopup(false);
        CloseLuaWidgetPanel(L"", "window-close");
        return 0;
    case WM_DESTROY:
        popupMouseLeaveTimerArmed_ = false;
        popupMouseLeaveController_.Reset();
        StopFloatingPopupOutsideClickMonitor();
        if (floatingPopupHwnd_ == hwnd)
            floatingPopupHwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
