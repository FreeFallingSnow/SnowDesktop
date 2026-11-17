#include "ui/input/private_pointer.h"
#include "app/app.h"
#include "ui/render/highlight_coverage.h"
#include "popup_resize_transition.h"
#include "diagnostics/diagnostic_log.h"
#include "common/utils.h"
#include "layout/floating_popup_rules.h"
#include "app/dock/persistent_dock_host.h"
#include "ui/render/ui_presentation_scope.h"
#include "drag_drop/drag_input_rules.h"
#include "popup_window_pair_z_order.h"
#include "layout/popup_round_geometry.h"
#include "theme/flat_glass_rim.h"
#include "platform/shell_overlay_window.h"
#include "diagnostics/performance_trace.h"
#include "platform/window_support/window_support_runtime.h"

#include <array>
#include <bit>
#include <dwmapi.h>
#include <sstream>

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
            panelBounds = InflateCopy(GetLuaWidgetPanelRect(), 6);
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
        FAILED(CreateOrResizeFloatingPopupCompositionSurface()))
        return false;
    // A fresh DComp surface has no valid content until EndDraw. Initialize its
    // tiny hidden target without touching popup models, input regions or hooks.
    ComPtr<ID2D1DeviceContext> context;
    POINT offset{};
    const HRESULT draw = floatingPopupDcompSurface_->BeginDraw(nullptr,
        __uuidof(ID2D1DeviceContext), reinterpret_cast<void**>(context.GetAddressOf()), &offset);
    if (FAILED(draw)) return false;
    context->Clear(D2D1::ColorF(0, 0, 0, 0));
    context.Reset();
    if (FAILED(floatingPopupDcompSurface_->EndDraw())) return false;

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
    floatingPopupDcompSurface_.Reset();
    floatingPopupCompWidth_ = 0;
    floatingPopupCompHeight_ = 0;
    if (floatingStackIndicatorVisual_ && floatingPopupDcompVisual_)
        (void)floatingPopupDcompVisual_->RemoveVisual(floatingStackIndicatorVisual_.Get());
    floatingStackIndicatorVisual_.Reset();
    floatingStackIndicatorSurface_.Reset();
    floatingStackIndicatorSurfaceSize_ = {};
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
    if (!floatingPopupCompositionRenderRecoveryPending_ &&
        floatingPopupHwnd_ &&
        IsWindow(floatingPopupHwnd_))
    {
        floatingPopupCompositionRenderRecoveryPending_ = true;
        InvalidateRect(
            floatingPopupHwnd_, nullptr, FALSE);
    }
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
    floatingPopupCompositionRenderRecoveryPending_ = false;
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
        ComPtr<IDCompositionVisual2> root, live;
        HRESULT hr = dcompDevice_->CreateVisual(&root);
        if (SUCCEEDED(hr)) hr = dcompDevice_->CreateVisual(&live);
        if (SUCCEEDED(hr)) hr = root->AddVisual(live.Get(), TRUE, nullptr);
        if (SUCCEEDED(hr)) hr = floatingPopupDcompTarget_->SetRoot(root.Get());
        if (FAILED(hr))
            return hr;
        floatingPopupDcompVisual_ = std::move(root);
        floatingPopupDcompLiveVisual_ = std::move(live);
    }
    return S_OK;
}

HRESULT DesktopApp::CreateOrResizeFloatingPopupCompositionSurface()
{
    HRESULT hr = EnsureFloatingPopupCompositionRoot();
    if (FAILED(hr))
        return hr;
    const auto& bounds = floatingPopupContentBounds_;
    const UINT width = static_cast<UINT>(std::max<LONG>(1, bounds.right - bounds.left));
    const UINT height = static_cast<UINT>(std::max<LONG>(1, bounds.bottom - bounds.top));
    if (floatingPopupDcompSurface_ &&
        floatingPopupCompWidth_ == width &&
        floatingPopupCompHeight_ == height)
        return S_OK;

    ComPtr<IDCompositionSurface> surface;
    hr = dcompDevice_->CreateSurface(
        width, height,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED,
        &surface);
    if (FAILED(hr) || !surface)
        return FAILED(hr) ? hr : E_FAIL;
    floatingPopupDcompSurface_ = std::move(surface);
    floatingPopupCompWidth_ = width;
    floatingPopupCompHeight_ = height;
    return S_OK;
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
    if (floatingStackIndicatorSurface_ && floatingStackIndicatorSurfaceSize_.cx == size.cx &&
        floatingStackIndicatorSurfaceSize_.cy == size.cy)
        return S_OK;

    ComPtr<IDCompositionSurface> surface;
    const HRESULT hr = dcompDevice_->CreateSurface(
        static_cast<UINT>(size.cx), static_cast<UINT>(size.cy),
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
    if (FAILED(hr)) return hr;
    floatingStackIndicatorSurface_ = std::move(surface);
    floatingStackIndicatorSurfaceSize_ = size;
    return S_OK;
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
        bool& active;
        ~PaintScope() { active = false; }
    } paintScope{ floatingPopupCompositionPaintInProgress_ };

    // Coalesce arriving folder/icons before BeginDraw on the host surface.
    // The animated child keeps its native timeline while only its pixels change.
    RefreshCollectionPopupAnimationContent();
    // Each surface keeps its own desktop position and display color mode.
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

        snowdesktop::HighlightCoverage coverage;
        snowdesktop::HighlightCoverage::Scope captureHighlights(context.Get(), coverage);
        context->SetDpi(96.0f, 96.0f);
        context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        context->SetTransform(D2D1::Matrix3x2F::Translation(
            static_cast<float>(offset.x - bounds.left),
            static_cast<float>(offset.y - bounds.top)));
        context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        context->Clear(D2D1::ColorF(0, 0, 0, 0));

        brushCache_.clear();
        brushCacheContext_ = context.Get();
        draw(context.Get());
        const bool stagedSrgb = collectionPopupBackdropCompositor_.StageSrgbContent(
            floatingPopupHwnd_, key, bounds, context.Get(), offset);
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context.Reset();
        brushCache_.clear();
        brushCacheContext_ = nullptr;

        // Publish the painted surface and offset together, after EndDraw.
        hr = surface.EndDraw();
        if (SUCCEEDED(hr)) hr = visual.SetOffsetX(static_cast<float>(bounds.left));
        if (SUCCEEDED(hr)) hr = visual.SetOffsetY(static_cast<float>(bounds.top));
        if (FAILED(hr)) return hr;
        const bool srgbPresented = stagedSrgb && collectionPopupBackdropCompositor_.PublishSrgbContent(key);
        if (!srgbPresented) collectionPopupBackdropCompositor_.RemoveSrgbContent(key);
        return visual.SetContent(srgbPresented ? nullptr : &surface);
    };

    HRESULT hr = EnsureFloatingPopupCompositionRoot();
    if (FAILED(hr))
    {
        RecoverFloatingPopupCompositionFailure(L"Create popup root", hr);
        return false;
    }
    popupFanBackdropCandidates_.clear();
    const auto srgbKey = reinterpret_cast<std::uintptr_t>(floatingPopupHwnd_);
    const RECT bounds = floatingPopupContentBounds_;
    if (IsRectEmpty(&bounds))
    {
        collectionPopupBackdropCompositor_.RemoveSrgbContent(srgbKey);
        hr = floatingPopupDcompLiveVisual_->SetContent(nullptr);
    }
    else
    {
        hr = CreateOrResizeFloatingPopupCompositionSurface();
        if (SUCCEEDED(hr))
            hr = paintSurface(*floatingPopupDcompSurface_.Get(), *floatingPopupDcompLiveVisual_.Get(), bounds, srgbKey,
                [this](ID2D1DeviceContext* context) {
                    renderingFloatingPopup_ = true;
                    DrawDynamicOverlays(context, false);
                    renderingFloatingPopup_ = false;
                });
    }
    if (FAILED(hr))
    {
        RecoverFloatingPopupCompositionFailure(L"Paint floating popup", hr);
        return false;
    }

    const auto indicatorKey = reinterpret_cast<std::uintptr_t>(&luaStackIndicator_);
    if (IsRectEmpty(&floatingStackIndicatorRegion_))
    {
        collectionPopupBackdropCompositor_.RemoveSrgbContent(indicatorKey);
        if (floatingStackIndicatorVisual_)
            hr = floatingStackIndicatorVisual_->SetContent(nullptr);
    }
    else
    {
        const RECT indicator = InflateCopy(floatingStackIndicatorRegion_, 6);
        hr = PrepareFloatingStackIndicatorComposition(
            SIZE{indicator.right - indicator.left, indicator.bottom - indicator.top});
        if (SUCCEEDED(hr))
            hr = paintSurface(*floatingStackIndicatorSurface_.Get(), *floatingStackIndicatorVisual_.Get(),
                indicator, indicatorKey, [this](ID2D1DeviceContext* context) { DrawLuaStackIndicator(context); });
    }
    if (FAILED(hr))
    {
        RecoverFloatingPopupCompositionFailure(L"Paint stack indicator", hr);
        return false;
    }
    const std::array order{indicatorKey, srgbKey, reinterpret_cast<std::uintptr_t>(&popupAnimationOverlay_),
        reinterpret_cast<std::uintptr_t>(&luaWidgetPanelAnimationOverlay_)};
    collectionPopupBackdropCompositor_.SetSrgbContentOrder(order);
    PublishCollectionPopupFanBackdrops();

    // A panel callback or a re-entrant provider wake may request a desktop
    // widget update while this popup surface owns BeginDraw. The queue defers
    // that work; consume it only after the popup surface has left Draw state.
    // A widget-local retry must not prevent the popup frame from presenting.
    const bool deferredWidgetsFlushed =
        FlushPendingDesktopWidgetComposition() &&
        FlushPendingWidgetMarqueeComposition() &&
        SyncWidgetMarqueeCompositionVisibility();
    if (!deferredWidgetsFlushed && hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, FALSE);
    if (!CommitCompositionAnimationFrame())
    {
        RecoverFloatingPopupCompositionFailure(
            L"Queue Commit", E_FAIL);
        return false;
    }
    floatingPopupCompositionRenderRecoveryPending_ = false;
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
        const auto srgbKey = reinterpret_cast<std::uintptr_t>(floatingPopupHwnd_);
        collectionPopupBackdropCompositor_.RemoveSrgbContent(srgbKey);
        (void)floatingPopupDcompLiveVisual_->SetContent(
            IsRectEmpty(&floatingPopupContentBounds_) ? nullptr : floatingPopupDcompSurface_.Get());
        collectionPopupBackdropCompositor_.RemoveSrgbContent(
            reinterpret_cast<std::uintptr_t>(&luaStackIndicator_));
        if (floatingStackIndicatorVisual_ && !IsRectEmpty(&floatingStackIndicatorRegion_))
            (void)floatingStackIndicatorVisual_->SetContent(floatingStackIndicatorSurface_.Get());
        (void)CommitCompositionAnimationFrame();
        (void)FlushPendingCompositionCommit();
        (void)collectionPopupBackdropCompositor_.CommitSrgbContent();
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

void DesktopApp::ApplyCollectionPopupBackdropAnimationFrame()
{
    if (!collectionPopupBackdropCompositor_.IsAvailable())
        return;

    const auto* collection = IsCollectionPopupHostedByFloatingWindow() ? GetOpenPopupWidget() : nullptr;
    bool visible = collectionPopupGlassTheme_ && IsLuaStackIndicatorVisible();
    const bool fan = collection && UsesCollectionPopupFan(*collection);
    if (fan)
        visible = visible || (popupFanBackdropCount_ > 0 && popupAnimation_.GetVisual().visible);
    if (collectionPopupGlassTheme_ && collection && !fan)
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
    collectionPopupBackdropCompositor_.CommitVisualChanges();
}

void DesktopApp::UpdateCollectionPopupBackdrop()
{
    snowdesktop::performance::Scope scope("popup", "backdrop.update");
    const auto* popup = IsCollectionPopupHostedByFloatingWindow() ? GetOpenPopupWidget() : nullptr;
    const bool fan = popup && UsesCollectionPopupFan(*popup);
    const bool collectionGlass = collectionPopupGlassTheme_ && popup && !fan;
    const bool luaGlass = collectionPopupGlassTheme_ && IsLuaPanelHostedByFloatingWindow();
    const bool stackGlass = collectionPopupGlassTheme_ && IsLuaStackIndicatorVisible();
    if (!floatingPopupHwnd_ || !IsWindow(floatingPopupHwnd_))
    {
        collectionPopupBackdropCompositor_.Reset();
        popupFanBackdropCount_ = 0;
        return;
    }
    if (!collectionPopupGlassTheme_ && !fan)
    {
        // The sRGB layer can own the full popup frame between paints.
        collectionPopupBackdropCompositor_.ResetBackdrop();
        popupFanBackdropCount_ = 0;
        return;
    }
    if ((!fan && !collectionGlass && !luaGlass && !stackGlass) ||
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
            (stackGlass || (collectionGlass && popupAnimation_.GetVisual().visible) ||
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
    if (collectionGlass)
    {
        RECT panel = GetCollectionPopupRect(*popup);
        if (const auto* motion = GetCollectionPopupResizeTransition()) panel = motion->Panel(panel);
        addPanel(panel, 18.0f * GetCollectionPopupLayoutMetrics(*popup).scale, &popupAnimation_);
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
    ApplyCollectionPopupBackdropAnimationFrame();
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

    if (floatingPopupCompositionPaintInProgress_ && (boundsChanged || regionChanged))
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
    // The HWND and its glass helper use desktop coordinates. Only a display
    // topology change can move them. The input region and compact paint
    // surface still track the actual popup rectangles.
    const int desktopWidth = std::max(1, virtualWidth_);
    const int desktopHeight = std::max(1, virtualHeight_);
    const RECT desktopBounds{
        virtualLeft_, virtualTop_, virtualLeft_ + desktopWidth, virtualTop_ + desktopHeight};
    RECT currentWindow{};
    if (!GetWindowRect(floatingPopupHwnd_, &currentWindow) || !EqualRect(&currentWindow, &desktopBounds))
    {
        if (wasVisible)
            SuspendFloatingPopupPresentation();
        wasVisible = false;
        if (!SetWindowPos(floatingPopupHwnd_, nullptr, virtualLeft_, virtualTop_,
                desktopWidth, desktopHeight, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER))
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
        ((collection && (UsesCollectionPopupFan(*collection) || collectionPopupGlassTheme_)) ||
            (collectionPopupGlassTheme_ &&
                (IsLuaPanelHostedByFloatingWindow() || !IsRectEmpty(&nextStackRegion))));
    const bool refreshContent = refreshStableContent || !wasVisible ||
        boundsChanged || regionChanged || backdropRecovery ||
        !floatingPopupDcompSurface_ || !floatingPopupDcompVisual_ ||
        graphicsDeviceRecovery_.Pending();
    // When the HWND is already visible for another popup, expose a new
    // indicator backdrop only after the indicator's own pixels are ready.
    const bool deferNewIndicatorBackdrop = wasVisible && immediatePresent &&
        stackRegionChanged && !IsRectEmpty(&nextStackRegion);
    if (refreshContent && !deferNewIndicatorBackdrop)
        UpdateCollectionPopupBackdrop();

    if (!wasVisible || boundsChanged || regionChanged)
    {
        HRGN windowRegion = CreateRectRgn(0, 0, 0, 0);
        bool regionReady = windowRegion != nullptr;
        auto appendRegion = [&](const RECT& desktopRect, float radius) {
            if (!windowRegion || IsRectEmpty(&desktopRect))
                return;
            HRGN added = snowdesktop::popup_round_geometry::CreateWindowFence(
                desktopRect, radius, 0, static_cast<float>(snowdesktop::flat_glass_rim::kPanelOverdraw));
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

    const bool rendered = immediatePresent
        ? PresentFloatingPopupComposition()
        : false;
    if (rendered && deferNewIndicatorBackdrop)
        UpdateCollectionPopupBackdrop();
    if (!rendered && refreshContent)
    {
        InvalidateRect(floatingPopupHwnd_, nullptr, FALSE);
        if (immediatePresent)
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
        if ((wp == VK_DELETE || historyShortcut) && !dockTrayPopup_ && IsCollectionPopupInteractive() &&
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
        // The host covers the virtual desktop in physical pixels. Keep the
        // desktop-coordinate regions instead of applying a DPI-scaled HWND.
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
