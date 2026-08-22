#include "app.h"
#include "../drag_input_rules.h"

#include <array>
#include <bit>

namespace
{
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

}

bool DesktopApp::IsCollectionPopupHostedByFloatingWindow() const
{
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
    return snowdesktop::floating_popup_rules::ShouldShow(
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
    floatingPopupMouseHook_ = SetWindowsHookExW(
        WH_MOUSE_LL,
        &DesktopApp::FloatingPopupMouseHookProc,
        instance_,
        0);
    if (floatingPopupMouseHook_)
        return true;

    floatingPopupMouseHookNotificationWindow_.store(nullptr);
    floatingPopupMouseHookActiveGeneration_.store(0);
    WriteDiagnosticLogEntry(
        L"Floating popup outside-click hook FAILED");
    return false;
}

void DesktopApp::StopFloatingPopupOutsideClickMonitor()
{
    floatingPopupMouseHookNotificationWindow_.store(nullptr);
    if (!floatingPopupMouseHook_)
    {
        floatingPopupMouseHookActiveGeneration_.store(0);
        return;
    }
    ++floatingPopupMouseHookGeneration_;
    if (floatingPopupMouseHookGeneration_ == 0)
        ++floatingPopupMouseHookGeneration_;
    UnhookWindowsHookEx(floatingPopupMouseHook_);
    floatingPopupMouseHook_ = nullptr;
    floatingPopupMouseHookActiveGeneration_.store(0);
}

void DesktopApp::HandleFloatingPopupExternalPointerDown(
    std::uint32_t generation,
    std::uint64_t screenPointPayload)
{
    if (generation == 0 ||
        generation != floatingPopupMouseHookGeneration_ ||
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
        snowdesktop::floating_popup_rules::
            IsPointOnHostedPopupSurface(
                desktopPoint,
                floatingPopupCollectionRegion_,
                floatingPopupLuaPanelRegion_);
    HWND targetWindow = WindowFromPoint(screenPoint);
    if (!targetWindow && !pointOnHostedPopup)
        return;
    const DWORD currentProcessId = GetCurrentProcessId();
    const auto belongsToCurrentProcess =
        [currentProcessId](HWND window) {
            DWORD processId = 0;
            if (window)
                GetWindowThreadProcessId(window, &processId);
            return processId != 0 &&
                processId == currentProcessId;
        };
    // The desktop renderer is a child of Explorer's WorkerW. Test the direct
    // hit first so an in-process desktop/Dock click does not inherit the
    // external process identity of its shell-owned root.
    bool targetBelongsToCurrentProcess =
        pointOnHostedPopup ||
        belongsToCurrentProcess(targetWindow);
    if (!targetBelongsToCurrentProcess)
    {
        targetBelongsToCurrentProcess =
            belongsToCurrentProcess(
                GetAncestor(targetWindow, GA_ROOTOWNER));
    }
    const bool dragActive =
        dragSession_.IsActive() ||
        dragDropController_.IsExternalDragActive();
    const bool dismissCollection =
        snowdesktop::floating_popup_rules::
            ShouldDismissForExternalPointerDown(
                IsCollectionPopupHostedByFloatingWindow(),
                targetBelongsToCurrentProcess,
                dragActive);
    const bool dismissLuaPanel =
        snowdesktop::floating_popup_rules::
            ShouldDismissLuaPanelForExternalPointerDown(
                IsLuaPanelHostedByFloatingWindow(),
                luaWidgetPanelRequest_.dismissOnOutside,
                targetBelongsToCurrentProcess,
                dragActive);
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

RECT DesktopApp::CalculateFloatingPopupWindowBounds() const
{
    RECT bounds{};
    if (IsCollectionPopupHostedByFloatingWindow())
    {
        if (const DesktopWidget* popup = GetOpenPopupWidget())
        {
            bounds = InflateCopy(
                GetCollectionPopupRect(*popup), 6);
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

bool DesktopApp::CreateFloatingPopupWindow()
{
    if (floatingPopupHwnd_ && IsWindow(floatingPopupHwnd_))
        return true;
    if (!instance_)
        return false;

    floatingPopupHwnd_ = CreateWindowExW(
        snowdesktop::floating_popup_rules::kWindowExStyle,
        kFloatingPopupWindowClassName,
        L"SnowDesktopFloatingPopup",
        WS_POPUP,
        0, 0, 1, 1,
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
    if (!floatingPopupDcompVisual_)
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
        IDCompositionVisual2* predecessor = nullptr;
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
                    WriteDiagnosticLogEntry(message);
                }
                return hr;
            }
            removedFromRoot[index] = true;
        }
    }

    IDCompositionVisual2* predecessor = nullptr;
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
        predecessor = nullptr;
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
            WriteDiagnosticLogEntry(message);
        }
        return addFailure;
    }
    return S_OK;
}

void DesktopApp::ResetFloatingPopupCompositionResources()
{
    if (floatingPopupDcompVisual_)
        floatingPopupDcompVisual_->SetContent(nullptr);
    floatingPopupDcompSurface_.Reset();
    floatingPopupCompWidth_ = 0;
    floatingPopupCompHeight_ = 0;
}

void DesktopApp::RecoverFloatingPopupCompositionFailure(
    const wchar_t* stage, HRESULT hr)
{
    wchar_t message[208]{};
    wsprintfW(
        message,
        L"FloatingPopup %s FAILED hr=0x%08X; resetting composition surface",
        stage ? stage : L"Render",
        static_cast<unsigned>(hr));
    WriteDiagnosticLogEntry(message);
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
    if (floatingPopupDropTargetRegistered_ &&
        floatingPopupHwnd_ && IsWindow(floatingPopupHwnd_))
    {
        RevokeDragDrop(floatingPopupHwnd_);
    }
    floatingPopupDropTargetRegistered_ = false;
    ResetFloatingPopupCompositionResources();
    floatingPopupDcompVisual_.Reset();
    floatingPopupDcompTarget_.Reset();
    floatingPopupWindowBounds_ = {};
    floatingPopupCollectionRegion_ = {};
    floatingPopupLuaPanelRegion_ = {};
    floatingPopupModalRegion_ = false;
    floatingPopupCompositionRenderRecoveryPending_ = false;
    if (floatingPopupHwnd_ && IsWindow(floatingPopupHwnd_))
        DestroyWindow(floatingPopupHwnd_);
    floatingPopupHwnd_ = nullptr;
}

HRESULT DesktopApp::CreateOrResizeFloatingPopupCompositionSurface()
{
    if (!dcompDevice_ || !floatingPopupHwnd_ ||
        !IsWindow(floatingPopupHwnd_))
        return E_UNEXPECTED;

    RECT client{};
    GetClientRect(floatingPopupHwnd_, &client);
    const UINT width = static_cast<UINT>(
        std::max<LONG>(1, client.right - client.left));
    const UINT height = static_cast<UINT>(
        std::max<LONG>(1, client.bottom - client.top));

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
        HRESULT hr = dcompDevice_->CreateVisual(
            &floatingPopupDcompVisual_);
        if (FAILED(hr) || !floatingPopupDcompVisual_)
            return FAILED(hr) ? hr : E_FAIL;
        hr = floatingPopupDcompTarget_->SetRoot(
            floatingPopupDcompVisual_.Get());
        if (FAILED(hr))
            return hr;
    }
    if (floatingPopupDcompSurface_ &&
        floatingPopupCompWidth_ == width &&
        floatingPopupCompHeight_ == height)
        return S_OK;

    ComPtr<IDCompositionSurface> surface;
    HRESULT hr = dcompDevice_->CreateSurface(
        width, height,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED,
        &surface);
    if (FAILED(hr) || !surface)
        return FAILED(hr) ? hr : E_FAIL;
    hr = floatingPopupDcompVisual_->SetContent(surface.Get());
    if (FAILED(hr))
        return hr;
    floatingPopupDcompSurface_ = std::move(surface);
    floatingPopupCompWidth_ = width;
    floatingPopupCompHeight_ = height;
    return S_OK;
}

bool DesktopApp::RenderFloatingPopupCompositionFrame()
{
    if (!ShouldShowFloatingPopupWindow() ||
        IsRectEmpty(&floatingPopupWindowBounds_) ||
        floatingPopupCompositionPaintInProgress_)
        return false;

    floatingPopupCompositionPaintInProgress_ = true;
    struct PaintScope final
    {
        bool& active;
        ~PaintScope() { active = false; }
    } paintScope{ floatingPopupCompositionPaintInProgress_ };

    HRESULT hr = CreateOrResizeFloatingPopupCompositionSurface();
    if (FAILED(hr) || !floatingPopupDcompSurface_)
    {
        RecoverFloatingPopupCompositionFailure(
            L"CreateOrResize",
            FAILED(hr) ? hr : E_FAIL);
        return false;
    }

    ID2D1DeviceContext* rawContext = nullptr;
    POINT updateOffset{};
    hr = floatingPopupDcompSurface_->BeginDraw(
        nullptr, __uuidof(ID2D1DeviceContext),
        reinterpret_cast<void**>(&rawContext),
        &updateOffset);
    if (FAILED(hr) || !rawContext)
    {
        RecoverFloatingPopupCompositionFailure(
            L"BeginDraw",
            FAILED(hr) ? hr : E_FAIL);
        return false;
    }

    ComPtr<ID2D1DeviceContext> context;
    context.Attach(rawContext);
    context->SetDpi(96.0f, 96.0f);
    context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    context->SetTransform(
        D2D1::Matrix3x2F::Translation(
            static_cast<float>(
                updateOffset.x -
                floatingPopupWindowBounds_.left),
            static_cast<float>(
                updateOffset.y -
                floatingPopupWindowBounds_.top)));
    context->SetAntialiasMode(
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    context->Clear(D2D1::ColorF(0, 0, 0, 0));

    brushCache_.clear();
    brushCacheContext_ = context.Get();
    renderingFloatingPopup_ = true;
    DrawDynamicOverlays(context.Get(), false);
    renderingFloatingPopup_ = false;
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    context.Reset();
    brushCache_.clear();
    brushCacheContext_ = nullptr;

    hr = floatingPopupDcompSurface_->EndDraw();
    if (FAILED(hr))
    {
        RecoverFloatingPopupCompositionFailure(
            L"EndDraw", hr);
        return false;
    }

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
        return true;
    }
    if (!RenderFloatingPopupCompositionFrame())
        return false;
    ValidateRect(floatingPopupHwnd_, nullptr);
    return FlushPendingCompositionCommit();
}

void DesktopApp::InvalidateFloatingPopupWindow(
    bool immediatePresent)
{
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
    const bool isTopmost =
        (GetWindowLongPtrW(
            floatingPopupHwnd_, GWL_EXSTYLE) &
            WS_EX_TOPMOST) != 0;
    if (isTopmost == shouldBeTopmost)
        return;
    SetWindowPos(
        floatingPopupHwnd_,
        shouldBeTopmost ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void DesktopApp::UpdateFloatingPopupWindowBounds(
    bool immediatePresent)
{
    if (!ShouldShowFloatingPopupWindow())
    {
        StopFloatingPopupOutsideClickMonitor();
        if (floatingPopupHwnd_ &&
            IsWindow(floatingPopupHwnd_) &&
            IsWindowVisible(floatingPopupHwnd_))
            ShowWindow(floatingPopupHwnd_, SW_HIDE);
        floatingPopupWindowBounds_ = {};
        floatingPopupCollectionRegion_ = {};
        floatingPopupLuaPanelRegion_ = {};
        floatingPopupModalRegion_ = false;
        return;
    }
    if (!CreateFloatingPopupWindow())
    {
        WriteDiagnosticLogEntry(
            L"Floating popup window creation FAILED");
        return;
    }
    StartFloatingPopupOutsideClickMonitor();

    const RECT nextBounds =
        CalculateFloatingPopupWindowBounds();
    if (IsRectEmpty(&nextBounds))
        return;
    const bool wasVisible =
        IsWindowVisible(floatingPopupHwnd_) != FALSE;
    const bool boundsChanged =
        !EqualRect(&nextBounds, &floatingPopupWindowBounds_);

    RECT nextCollectionRegion{};
    if (IsCollectionPopupHostedByFloatingWindow())
    {
        if (const DesktopWidget* popup = GetOpenPopupWidget())
            nextCollectionRegion = GetCollectionPopupRect(*popup);
    }
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
        nextModalRegion != floatingPopupModalRegion_;

    floatingPopupWindowBounds_ = nextBounds;
    floatingPopupCollectionRegion_ = nextCollectionRegion;
    floatingPopupLuaPanelRegion_ = nextLuaPanelRegion;
    floatingPopupModalRegion_ = nextModalRegion;

    const bool topmost =
        snowdesktop::floating_popup_rules::ShouldBeTopmost(
            true, shellPopupMenuLayerDepth_);
    if (!wasVisible || boundsChanged)
    {
        SetWindowPos(
            floatingPopupHwnd_,
            topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
            nextBounds.left + virtualLeft_,
            nextBounds.top + virtualTop_,
            std::max<LONG>(
                1, nextBounds.right - nextBounds.left),
            std::max<LONG>(
                1, nextBounds.bottom - nextBounds.top),
            SWP_NOACTIVATE |
                (wasVisible ? SWP_SHOWWINDOW : 0));
    }
    else
    {
        ApplyFloatingPopupLayerPolicy();
    }

    if (!wasVisible || boundsChanged || regionChanged)
    {
        HRGN windowRegion = CreateRectRgn(0, 0, 0, 0);
        auto appendRegion = [&](RECT desktopRect, int radius) {
            if (!windowRegion || IsRectEmpty(&desktopRect))
                return;
            desktopRect = InflateCopy(desktopRect, 3);
            OffsetRect(
                &desktopRect,
                -floatingPopupWindowBounds_.left,
                -floatingPopupWindowBounds_.top);
            HRGN added = radius > 0
                ? CreateRoundRectRgn(
                    desktopRect.left,
                    desktopRect.top,
                    desktopRect.right + 1,
                    desktopRect.bottom + 1,
                    radius * 2,
                    radius * 2)
                : CreateRectRgn(
                    desktopRect.left,
                    desktopRect.top,
                    desktopRect.right + 1,
                    desktopRect.bottom + 1);
            if (added)
            {
                CombineRgn(
                    windowRegion, windowRegion,
                    added, RGN_OR);
                DeleteObject(added);
            }
        };
        appendRegion(floatingPopupCollectionRegion_, 18);
        appendRegion(
            floatingPopupLuaPanelRegion_,
            floatingPopupModalRegion_ ? 0 : 18);
        if (windowRegion &&
            !SetWindowRgn(
                floatingPopupHwnd_, windowRegion, FALSE))
            DeleteObject(windowRegion);
    }

    const bool rendered = immediatePresent
        ? PresentFloatingPopupComposition()
        : false;
    if (!rendered)
        InvalidateRect(floatingPopupHwnd_, nullptr, FALSE);

    if (!wasVisible)
    {
        SetWindowPos(
            floatingPopupHwnd_,
            topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE |
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
        wchar_t message[224]{};
        wsprintfW(
            message,
            L"Floating popup shown rect=(%ld,%ld)-(%ld,%ld) topmost=%d",
            nextBounds.left + virtualLeft_,
            nextBounds.top + virtualTop_,
            nextBounds.right + virtualLeft_,
            nextBounds.bottom + virtualTop_,
            topmost != false);
        WriteDiagnosticLogEntry(message);
    }
    else if (boundsChanged && !immediatePresent)
    {
        InvalidateFloatingPopupWindow(false);
    }
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
    if (ShouldShowFloatingPopupWindow() && !rendered)
        InvalidateRect(hwnd, nullptr, FALSE);
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
            (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        POINT point{};
        if (snowdesktop::drag_input_rules::
                ShouldSampleFloatingWindowPointer(
                    nativeDragActive, primaryButtonDown) &&
            GetCursorPos(&point) && hwnd_ && IsWindow(hwnd_) &&
            ScreenToClient(hwnd_, &point))
        {
            return point;
        }
        return desktopPoint();
    };

    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case kFloatingPopupExternalPointerMessage:
        HandleFloatingPopupExternalPointerDown(
            static_cast<std::uint32_t>(wp),
            std::bit_cast<std::uint64_t>(lp));
        return 0;
    case WM_NCHITTEST:
        return ShouldShowFloatingPopupWindow()
            ? HTCLIENT : HTTRANSPARENT;
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
        TrackMouseEvent(&tracking);
        handlingFloatingPopupInput_ = true;
        bool dragPreviewSynced = false;
        OnMouseMoveAt(
            wp, latestDesktopPointer(),
            &dragPreviewSynced);
        handlingFloatingPopupInput_ = false;
        UpdateFloatingPopupWindowBounds(false);
        PresentPointerInteractionFrame(
            dragPreviewSynced);
        return 0;
    }
    case WM_MOUSELEAVE:
    {
        POINT cursor{};
        if (GetCursorPos(&cursor) &&
            WindowFromPoint(cursor) == hwnd)
            return 0;
        OnMouseLeave();
        return 0;
    }
    case WM_LBUTTONDOWN:
        handlingFloatingPopupInput_ = true;
        OnLeftButtonDown(wp, desktopLParam());
        handlingFloatingPopupInput_ = false;
        InvalidateFloatingPopupWindow(true);
        return 0;
    case WM_LBUTTONUP:
        handlingFloatingPopupInput_ = true;
        OnLeftButtonUpAt(wp, desktopPoint());
        handlingFloatingPopupInput_ = false;
        UpdateFloatingPopupWindowBounds();
        return 0;
    case WM_LBUTTONDBLCLK:
    {
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
    case WM_RBUTTONUP:
        OnRightButtonUp(desktopLParam());
        UpdateFloatingPopupWindowBounds();
        return 0;
    case WM_MOUSEWHEEL:
        handlingFloatingPopupInput_ = true;
        OnMouseWheel(wp, lp);
        handlingFloatingPopupInput_ = false;
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        CloseCollectionPopup(false);
        CloseLuaWidgetPanel(L"", "display-change");
        return 0;
    case WM_CLOSE:
        CloseCollectionPopup(false);
        CloseLuaWidgetPanel(L"", "window-close");
        return 0;
    case WM_DESTROY:
        StopFloatingPopupOutsideClickMonitor();
        if (floatingPopupHwnd_ == hwnd)
            floatingPopupHwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
