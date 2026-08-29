#include "app.h"

// Floating-Dock window, composition surface and bounds management.

bool DesktopApp::CreateFloatingDockWindow(
    PersistentDockHost& host)
{
    if (host.hwnd && IsWindow(host.hwnd))
        return true;

    host.owner = this;
    host.hwnd = CreateWindowExW(
        snowdesktop::floating_dock_rules::kWindowExStyle,
        kFloatingDockWindowClassName,
        _LW("app.settings.dock_bar"),
        WS_POPUP,
        0, 0, 1, 1,
        nullptr, nullptr, instance_, &host);
    if (!host.hwnd)
        return false;

    OleDragDropAdapter* oleAdapter = EnsureOleDragDropAdapter();
    host.dropTargetRegistered = oleAdapter &&
        SUCCEEDED(RegisterDragDrop(
            host.hwnd,
            static_cast<IDropTarget*>(oleAdapter)));
    const BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(host.hwnd,
        DWMWA_TRANSITIONS_FORCEDISABLED,
        &disableTransitions, sizeof(disableTransitions));
    return true;
}

DesktopApp::PersistentDockHost*
DesktopApp::FindPersistentDockHost(HWND hwnd)
{
    for (const auto& host : persistentDockHosts_)
        if (host && host->hwnd == hwnd)
            return host.get();
    return nullptr;
}

const DesktopApp::PersistentDockHost*
DesktopApp::FindPersistentDockHost(HWND hwnd) const
{
    for (const auto& host : persistentDockHosts_)
        if (host && host->hwnd == hwnd)
            return host.get();
    return nullptr;
}

DesktopApp::PersistentDockHost*
DesktopApp::FindPersistentDockHost(
    DockContainer* container)
{
    for (const auto& host : persistentDockHosts_)
        if (host && host->container == container)
            return host.get();
    return nullptr;
}

const DesktopApp::PersistentDockHost*
DesktopApp::FindPersistentDockHost(
    const DockContainer* container) const
{
    for (const auto& host : persistentDockHosts_)
        if (host && host->container == container)
            return host.get();
    return nullptr;
}

bool DesktopApp::IsPersistentDockHostWindow(HWND hwnd) const
{
    return FindPersistentDockHost(hwnd) != nullptr;
}

bool DesktopApp::IsPersistentDockBackdropWindow(HWND hwnd) const
{
    for (const auto& host : persistentDockHosts_)
        if (host && host->backdrop.IsBackdropWindow(hwnd))
            return true;
    return false;
}

bool DesktopApp::IsDockHostedByPersistentHost(
    const DockContainer* container) const
{
    const PersistentDockHost* host =
        FindPersistentDockHost(container);
    return host && host->active;
}

bool DesktopApp::IsPersistentDockHostPromoted(
    const PersistentDockHost& host) const
{
    return host.active && host.promoted;
}

bool DesktopApp::IsPersistentDockHostEffectivelyFloating(
    const PersistentDockHost& host) const
{
    return host.active &&
        snowdesktop::floating_dock_rules::
            IsDockEffectivelyPromoted(
                host.promoted,
                host.passivelyRevealed,
                dockSettings_.showOnlyWhenSummoned);
}

bool DesktopApp::ShouldShowPersistentDockHost(
    const PersistentDockHost& host) const
{
    return snowdesktop::floating_dock_rules::
        ShouldShowPersistentDockHost(
            host.active,
            IsPersistentDockHostEffectivelyFloating(host),
            dockSettings_.showOnlyWhenSummoned,
            customDesktopVisible_,
            desktopIconsHidden_,
            dockSettings_.keepWhenDesktopHidden);
}

bool DesktopApp::IsDockContainerInteractionVisible(
    const DockContainer* container) const
{
    const PersistentDockHost* host =
        FindPersistentDockHost(container);
    // Before graphics initialization, or after a Host creation failure, the
    // Dock falls back to the desktop foreground surface. Only an active
    // persistent Host can make that same logical container non-interactive.
    return !host || !host->active ||
        ShouldShowPersistentDockHost(*host);
}

bool DesktopApp::IsDockContainerPromoted(
    const DockContainer* container) const
{
    const PersistentDockHost* host =
        FindPersistentDockHost(container);
    return host && IsPersistentDockHostPromoted(*host);
}

bool DesktopApp::IsDockContainerEffectivelyFloating(
    const DockContainer* container) const
{
    const PersistentDockHost* host =
        FindPersistentDockHost(container);
    return host &&
        IsPersistentDockHostEffectivelyFloating(*host);
}

bool DesktopApp::
IsSelectedPersistentDockHostPromoted() const
{
    return floatingDockHost_ &&
        IsPersistentDockHostPromoted(*floatingDockHost_);
}

bool DesktopApp::HasPromotedDockHosts() const
{
    return std::any_of(
        persistentDockHosts_.begin(),
        persistentDockHosts_.end(),
        [this](const auto& host) {
            return host &&
                IsPersistentDockHostPromoted(*host);
        });
}

bool DesktopApp::IsPointOnPromotedDock(
    POINT point) const
{
    for (const auto& host : persistentDockHosts_)
    {
        if (host &&
            IsPersistentDockHostPromoted(*host) &&
            PtInRect(&host->dockRect, point))
        {
            return true;
        }
    }
    return false;
}

bool DesktopApp::IsPointInPromotedDockLayer(
    POINT point) const
{
    for (const auto& host : persistentDockHosts_)
    {
        if (host &&
            IsPersistentDockHostPromoted(*host) &&
            snowdesktop::floating_dock_rules::
                IsPointInVisibleLayer(
                    point,
                    host->dockRect,
                    host->popupRect,
                    host->tooltipRect))
        {
            return true;
        }
    }
    return false;
}

void DesktopApp::RefreshFloatingDockVisibilityState()
{
    floatingDockVisible_ = HasPromotedDockHosts();
}

void DesktopApp::SelectPersistentDockHost(
    PersistentDockHost* host)
{
    floatingDockHost_ = host;
    floatingDockHwnd_ = host ? host->hwnd : nullptr;
    floatingDockContainer_ = host ? host->container : nullptr;
    floatingDockMonitor_ = host ? host->monitor : nullptr;
}

void DesktopApp::DestroyPersistentDockHost(
    PersistentDockHost& host)
{
    if (rightButtonDownDockHost_ == &host)
        rightButtonDownDockHost_ = nullptr;
    if (collectionPopupDockHost_ == &host)
        collectionPopupDockHost_ = nullptr;
    if (quickNavigationDockHost_ == &host)
        quickNavigationDockHost_ = nullptr;
    host.active = false;
    host.promoted = false;
    host.passivelyRevealed = false;
    host.passiveRevealTick = 0;
    host.passiveLeaveStartTick = 0;
    host.revealPending = false;
    host.backdrop.Reset();
    if (host.dropTargetRegistered &&
        host.hwnd && IsWindow(host.hwnd))
        RevokeDragDrop(host.hwnd);
    host.dropTargetRegistered = false;
    ResetFloatingDockCompositionResources(host);
    host.dcompVisual.Reset();
    host.dcompTarget.Reset();
    if (host.hwnd && IsWindow(host.hwnd))
        DestroyWindow(host.hwnd);
    host.hwnd = nullptr;
    host.container = nullptr;
    host.monitor = nullptr;
}

bool DesktopApp::SyncPersistentDockHosts()
{
    // Window creation can trigger an early layout pass before InitGraphics.
    // Defer the first top-level DockHost allocation until both rendering
    // devices exist, so startup never creates, hides, and then recovers a
    // glass/content pair around an expected E_UNEXPECTED render failure.
    if (!d2dDevice_ || !dcompDevice_)
        return false;

    const HMONITOR previouslySelectedMonitor =
        floatingDockMonitor_;
    SelectPersistentDockHost(nullptr);

    for (auto& host : persistentDockHosts_)
        if (host)
            host->container = nullptr;

    if (generalSettings_.dockEnabled)
    {
        for (const auto& container : containers_)
        {
            auto* dock =
                dynamic_cast<DockContainer*>(container.get());
            if (!dock)
                continue;
            const RECT bounds = dock->GetBounds();
            const POINT centerScreen{
                (bounds.left + bounds.right) / 2 + virtualLeft_,
                (bounds.top + bounds.bottom) / 2 + virtualTop_
            };
            const HMONITOR monitor = MonitorFromPoint(
                centerScreen, MONITOR_DEFAULTTONEAREST);
            PersistentDockHost* host = nullptr;
            for (const auto& existing : persistentDockHosts_)
            {
                if (existing && existing->monitor == monitor)
                {
                    host = existing.get();
                    break;
                }
            }
            if (!host)
            {
                auto created =
                    std::make_unique<PersistentDockHost>();
                created->owner = this;
                created->monitor = monitor;
                host = created.get();
                persistentDockHosts_.push_back(
                    std::move(created));
            }
            host->container = dock;
            host->sourceRect = {};
            host->dockRect = {};
            host->popupRect = {};
            host->tooltipRect = {};
        }
    }

    for (auto it = persistentDockHosts_.begin();
         it != persistentDockHosts_.end();)
    {
        if (!*it || !(*it)->container)
        {
            if (*it)
                DestroyPersistentDockHost(*(*it));
            it = persistentDockHosts_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    floatingDockHostActive_ = !persistentDockHosts_.empty();
    persistentDockHostOwnsVisual_ =
        floatingDockHostActive_;
    if (!floatingDockHostActive_)
    {
        RefreshFloatingDockVisibilityState();
        return false;
    }

    PersistentDockHost* selected = nullptr;
    for (const auto& host : persistentDockHosts_)
    {
        if (host &&
            host->monitor == previouslySelectedMonitor)
        {
            selected = host.get();
            break;
        }
    }
    if (!selected)
        selected = persistentDockHosts_.front().get();
    SelectPersistentDockHost(selected);

    floatingDockPersonalization_ =
        CurrentPersonalization();
    for (const auto& ownedHost : persistentDockHosts_)
    {
        PersistentDockHost& host = *ownedHost;
        if (!CreateFloatingDockWindow(host))
        {
            host.active = false;
            continue;
        }
        host.active = true;
        host.revealPending =
            !IsWindowVisible(host.hwnd);
        UpdateFloatingDockWindowBounds(host, false);
        if (!RenderFloatingDockCompositionFrame(host) ||
            !host.frameReady)
        {
            WriteDiagnosticLogEntry(
                L"Persistent DockHost initial frame unavailable; host disabled");
            host.active = false;
            host.revealPending = false;
            host.backdrop.HidePopupWindowPair(host.hwnd);
            continue;
        }
        ValidateRect(host.hwnd, nullptr);
        host.revealPending = false;
    }
    floatingDockHostActive_ = std::any_of(
        persistentDockHosts_.begin(),
        persistentDockHosts_.end(),
        [](const auto& host) {
            return host && host->active;
        });
    persistentDockHostOwnsVisual_ =
        floatingDockHostActive_;
    CommitCompositionAnimationFrame();
    FlushPendingCompositionCommit();

    if (!selected->active)
    {
        selected = nullptr;
        for (const auto& host : persistentDockHosts_)
        {
            if (host && host->active)
            {
                selected = host.get();
                break;
            }
        }
        SelectPersistentDockHost(selected);
    }
    RefreshFloatingDockVisibilityState();
    UpdatePersistentDockHostVisibility();
    InvalidateDragStaticScene();
    return selected != nullptr;
}

bool DesktopApp::SyncPersistentDockHost(
    HMONITOR preferredMonitor)
{
    bool requiresRebuild = persistentDockHosts_.empty();
    for (const auto& host : persistentDockHosts_)
    {
        if (!host || !host->active ||
            !host->container || !host->hwnd ||
            !IsWindow(host->hwnd))
        {
            requiresRebuild = true;
            break;
        }
    }
    if (requiresRebuild &&
        !SyncPersistentDockHosts())
        return false;
    PersistentDockHost* selected = nullptr;
    if (preferredMonitor)
    {
        for (const auto& host : persistentDockHosts_)
        {
            if (host && host->active &&
                host->monitor == preferredMonitor)
            {
                selected = host.get();
                break;
            }
        }
    }
    if (!selected)
        selected = floatingDockHost_;
    if (!selected && !persistentDockHosts_.empty())
        selected = persistentDockHosts_.front().get();
    SelectPersistentDockHost(selected);
    return selected != nullptr;
}

void DesktopApp::ApplyPersistentDockHostAppearance()
{
    floatingDockPersonalization_ =
        CurrentPersonalization();
    for (const auto& ownedHost : persistentDockHosts_)
    {
        if (!ownedHost || !ownedHost->active)
            continue;
        UpdateFloatingDockWindowBounds(
            *ownedHost, false, true);
    }
}

void DesktopApp::UpdatePersistentDockHostVisibility(
    PersistentDockHost& host)
{
    if (!host.hwnd || !IsWindow(host.hwnd))
        return;
    const bool shouldShow =
        ShouldShowPersistentDockHost(host);
    if (!shouldShow)
    {
        // Hide the content/backdrop pair before leaving the floating band.
        // A visible TOPMOST-to-desktop restack can otherwise expose one native
        // backdrop frame between the two states.
        host.backdrop.HidePopupWindowPair(host.hwnd);
        ApplyFloatingDockLayerPolicy(host);
        return;
    }

    ApplyFloatingDockLayerPolicy(host);
    // Desktop/floating transitions keep the pair visible and only change its
    // Z-order band. Avoid issuing a redundant SHOWWINDOW transaction, which
    // can make DWM re-evaluate the native backdrop source for one frame.
    if (IsWindowVisible(host.hwnd))
        return;
    if (host.backdrop.IsAvailable())
        host.backdrop.ShowPopupWindowPair(host.hwnd);
    else
        ShowWindow(host.hwnd, SW_SHOWNOACTIVATE);
}

void DesktopApp::UpdatePersistentDockHostVisibility()
{
    for (const auto& host : persistentDockHosts_)
        if (host)
            UpdatePersistentDockHostVisibility(*host);
}

void DesktopApp::ResetFloatingDockCompositionResources(
    PersistentDockHost& host)
{
    host.frameReady = false;
    brushCache_.clear();
    brushCacheContext_ = nullptr;
    if (host.dcompVisual)
        host.dcompVisual->SetContent(nullptr);
    host.dcompSurface.Reset();
    host.compWidth = 0;
    host.compHeight = 0;
}

void DesktopApp::DestroyFloatingDockWindow()
{
    if (floatingDockHoverTailToken_)
    {
        uiAnimationScheduler_.Cancel(
            floatingDockHoverTailToken_);
        floatingDockHoverTailToken_ = 0;
    }
    floatingDockHoverTargetOwner_ = nullptr;
    floatingDockHoverTargetIndex_ = 0;
    floatingDockHoverTargetKind_ = 0;
    EndFloatingDockKeyboardSession(
        FloatingDockCloseFocusPolicy::PreserveCurrent);
    floatingDockPostCloseAction_ = {};
    floatingDockPointerPresentPending_ = false;
    shellPopupMenuLayerDepth_ = 0;
    floatingDockHoverHandoffPending_ = false;
    floatingDockHoverHandoffRect_ = {};
    floatingDockVisible_ = false;
    handlingPersistentDockHost_ = nullptr;
    renderingPersistentDockHost_ = nullptr;
    SelectPersistentDockHost(nullptr);
    for (const auto& host : persistentDockHosts_)
        if (host)
            DestroyPersistentDockHost(*host);
    persistentDockHosts_.clear();
    floatingDockHostActive_ = false;
    persistentDockHostOwnsVisual_ = false;
    if (floatingDockInputHwnd_ &&
        IsWindow(floatingDockInputHwnd_))
        DestroyWindow(floatingDockInputHwnd_);
    floatingDockInputHwnd_ = nullptr;
}

DockContainer*
DesktopApp::SelectFloatingDockContainerAtCursor() const
{
    POINT cursorScreen{};
    GetCursorPos(&cursorScreen);
    const HMONITOR cursorMonitor =
        MonitorFromPoint(cursorScreen, MONITOR_DEFAULTTONEAREST);
    return SelectFloatingDockContainerForMonitor(
        cursorMonitor);
}

DockContainer*
DesktopApp::SelectFloatingDockContainerForMonitor(
    HMONITOR monitor) const
{
    DockContainer* fallback = nullptr;
    for (const auto& container : containers_)
    {
        auto* dock =
            dynamic_cast<DockContainer*>(container.get());
        if (!dock)
            continue;
        if (!fallback)
            fallback = dock;
        const RECT bounds = dock->GetBounds();
        POINT centerScreen{
            (bounds.left + bounds.right) / 2 + virtualLeft_,
            (bounds.top + bounds.bottom) / 2 + virtualTop_
        };
        if (MonitorFromPoint(centerScreen,
                MONITOR_DEFAULTTONEAREST) == monitor)
            return dock;
    }
    return fallback;
}

RECT DesktopApp::
CalculateFloatingDockStableSourceRect(
    const PersistentDockHost& host) const
{
    if (!host.container)
        return RECT{};

    const RECT dockRect =
        host.container->
            GetInteractiveBounds();
    RECT sourceRect =
        snowdesktop::floating_dock_rules::
            ExpandForBorderOverdraw(
                snowdesktop::
                    floating_dock_rules::
                        ExpandHostForTitleLayer(
                            dockRect,
                            dockSettings_.position));

    return sourceRect;
}

void DesktopApp::UpdateFloatingDockWindowBounds(
    bool immediatePresent)
{
    for (const auto& host : persistentDockHosts_)
        if (host)
            UpdateFloatingDockWindowBounds(
                *host, immediatePresent);
}

void DesktopApp::UpdateFloatingDockWindowBounds(
    PersistentDockHost& host,
    bool immediatePresent,
    bool forceRegionRefresh)
{
    if (!host.active || !host.hwnd ||
        !IsWindow(host.hwnd) ||
        !host.container)
        return;
    const bool promoted =
        IsPersistentDockHostEffectivelyFloating(host);
    const HWND dockHostHwnd = host.hwnd;
    RECT& floatingDockSourceRect_ = host.sourceRect;
    RECT& floatingDockRect_ = host.dockRect;
    RECT& floatingDockPopupRect_ = host.popupRect;
    RECT& floatingDockTooltipRect_ = host.tooltipRect;
    bool& floatingDockRevealPending_ =
        host.revealPending;
    DesktopBackdropCompositor&
        floatingDockBackdropCompositor_ =
            host.backdrop;
    const bool floatingLayerTopmost =
        snowdesktop::floating_dock_rules::
            ShouldFloatingDockBeTopmost(
                promoted,
                shellPopupMenuLayerDepth_);

    const RECT nextDockRect =
        host.container->GetInteractiveBounds();
    const RECT nextTooltipRect =
        host.container->
            GetHoveredTitleBounds(
                lastMousePoint_);
    const RECT nextPopupRect{};
    // Collection and folder popups live in the shared floating popup host.
    // This window retains only the Dock and its hover-title layer.
    RECT requiredSourceRect =
        CalculateFloatingDockStableSourceRect(host);
    const RECT previousSourceRect =
        floatingDockSourceRect_;
    if (IsRectEmpty(&floatingDockSourceRect_))
        floatingDockSourceRect_ = requiredSourceRect;
    else
    {
        const RECT expandedSourceRect =
            snowdesktop::floating_dock_rules::
                UnionNonEmptyRects(
                    floatingDockSourceRect_,
                    requiredSourceRect);
        if (!EqualRect(&expandedSourceRect,
                &floatingDockSourceRect_))
            floatingDockSourceRect_ =
                expandedSourceRect;
    }
    if (IsRectEmpty(&floatingDockSourceRect_))
    {
        host.active = false;
        UpdatePersistentDockHostVisibility(host);
        return;
    }
    const bool dockGeometryChanged =
        !EqualRect(&nextDockRect,
            &floatingDockRect_);
    const bool popupRegionChanged =
        !EqualRect(&nextPopupRect,
            &floatingDockPopupRect_);
    const bool titleRegionChanged =
        !EqualRect(&nextTooltipRect,
            &floatingDockTooltipRect_);
    const bool sourceRectChanged =
        !EqualRect(&previousSourceRect,
            &floatingDockSourceRect_);
    if (!dockGeometryChanged &&
        !popupRegionChanged &&
        !titleRegionChanged &&
        !sourceRectChanged &&
        !forceRegionRefresh)
    {
        InvalidateFloatingDockWindow(host);
        return;
    }
    floatingDockRect_ = nextDockRect;
    floatingDockPopupRect_ = nextPopupRect;
    floatingDockTooltipRect_ = nextTooltipRect;

    const int width = std::max<LONG>(
        1, floatingDockSourceRect_.right -
            floatingDockSourceRect_.left);
    const int height = std::max<LONG>(
        1, floatingDockSourceRect_.bottom -
            floatingDockSourceRect_.top);

    const RECT dockRegionRect =
        snowdesktop::floating_dock_rules::
            ExpandForBorderOverdraw(
                floatingDockRect_);
    const RECT dockLocal =
        snowdesktop::floating_dock_rules::
            DesktopRectToWindowRect(
                dockRegionRect,
                floatingDockSourceRect_);
    const int radius = std::max(1,
        static_cast<int>(std::round(
            promoted
                ? floatingDockPersonalization_.cornerRadius
                : CurrentPersonalization().cornerRadius)));
    constexpr int borderOverdraw = 2;
    HRGN windowRegion = CreateRoundRectRgn(
        dockLocal.left, dockLocal.top,
        dockLocal.right + 1, dockLocal.bottom + 1,
        (radius + borderOverdraw) * 2,
        (radius + borderOverdraw) * 2);
    auto appendRegion = [&](
        const RECT& desktopRect,
        int cornerRadius) {
        if (!windowRegion ||
            IsRectEmpty(&desktopRect))
            return;
        const RECT overdrawRect =
            snowdesktop::
                floating_dock_rules::
                    ExpandForBorderOverdraw(
                        desktopRect);
        const RECT local =
            snowdesktop::floating_dock_rules::
                DesktopRectToWindowRect(
                    overdrawRect,
                    floatingDockSourceRect_);
        HRGN addedRegion = CreateRoundRectRgn(
            local.left, local.top,
            local.right + 1,
            local.bottom + 1,
            (cornerRadius +
                borderOverdraw) * 2,
            (cornerRadius +
                borderOverdraw) * 2);
        if (addedRegion)
        {
            CombineRgn(windowRegion, windowRegion,
                addedRegion, RGN_OR);
            DeleteObject(addedRegion);
        }
    };
    appendRegion(
        floatingDockPopupRect_, radius);
    appendRegion(
        floatingDockTooltipRect_, 7);
    if (windowRegion &&
        !SetWindowRgn(
            dockHostHwnd, windowRegion, FALSE))
        DeleteObject(windowRegion);

    // Popup and title changes only alter the visible/input region inside the
    // stable host allocation. Do not resize the HWND or recreate its DComp
    // surface after the floating Dock has been revealed.
    if (!dockGeometryChanged &&
        !sourceRectChanged &&
        IsWindowVisible(dockHostHwnd))
    {
        InvalidateFloatingDockWindow(
            host, immediatePresent);
        return;
    }

    const bool firstReveal =
        floatingDockRevealPending_ ||
        !IsWindowVisible(dockHostHwnd);
    if (firstReveal)
    {
        // Create the hidden host in its eventual Z-order band. Desktop mode
        // must never transiently enter the topmost band during first layout;
        // promoted mode remains topmost so a no-activate popup is not buried.
        SetWindowPos(
            dockHostHwnd,
            floatingLayerTopmost
                ? HWND_TOPMOST : HWND_NOTOPMOST,
            floatingDockSourceRect_.left +
                virtualLeft_,
            floatingDockSourceRect_.top +
                virtualTop_,
            width, height,
            SWP_NOACTIVATE);
    }
    else
    {
        // Resize within the current Z band. Popup-only changes normally keep
        // the stable allocation, while a larger data set can expand it.
        SetWindowPos(
            dockHostHwnd, nullptr,
            floatingDockSourceRect_.left +
                virtualLeft_,
            floatingDockSourceRect_.top +
                virtualTop_,
            width, height,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    bool renderedResizeFrame = false;
    if (!firstReveal && sourceRectChanged)
    {
        // The host is now resized to the expanded source rect. Submit the
        // replacement frame synchronously so the new region never presents
        // the stale pre-resize surface (blank/empty panel) before the next
        // paint. The frame renderer is reentrancy-guarded; on failure the
        // ordinary invalidation below remains the fallback.
        renderedResizeFrame =
            RenderFloatingDockCompositionFrame(host) &&
            FlushPendingCompositionCommit();
    }

    if (sourceRectChanged &&
        floatingDockBackdropCompositor_.IsAvailable())
    {
        // The native backdrop helper is a sibling sized to the content HWND.
        // Keep its window placement in sync after the host expands so glass
        // panels are not clipped to the old host bounds.
        floatingDockBackdropCompositor_.
            SetPopupTopmost(floatingLayerTopmost);
        floatingDockBackdropCompositor_.
            Reattach(dockHostHwnd);
    }

    if (!floatingDockBackdropCompositor_.IsAvailable())
    {
        if (!floatingDockBackdropCompositor_.
                InitializePopup(
                    dockHostHwnd,
                    floatingLayerTopmost,
                    false))
        {
            std::wstring message =
                L"Floating Dock native backdrop unavailable: ";
            message += floatingDockBackdropCompositor_.
                LastError();
            WriteDiagnosticLogEntry(message.c_str());
        }
    }
    if (firstReveal)
    {
        SetWindowPos(
            dockHostHwnd,
            floatingLayerTopmost
                ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE |
                SWP_NOACTIVATE);
    }
    RECT loggedRect{};
    GetWindowRect(dockHostHwnd, &loggedRect);
    wchar_t message[224]{};
    wsprintfW(message,
        L"Floating Dock shown rect=(%ld,%ld)-(%ld,%ld) exStyle=0x%08X topmost=%d",
        loggedRect.left, loggedRect.top,
        loggedRect.right, loggedRect.bottom,
        static_cast<unsigned>(GetWindowLongPtrW(
            dockHostHwnd, GWL_EXSTYLE)),
        (GetWindowLongPtrW(
            dockHostHwnd, GWL_EXSTYLE) &
            WS_EX_TOPMOST) != 0);
    WriteDiagnosticLogEntry(message);
    if (!renderedResizeFrame)
        InvalidateFloatingDockWindow(
            host, immediatePresent);
    // Bounds and backdrop refreshes only prepare hidden geometry. One gated
    // visibility path owns showing the pair, so an idle auto-hidden Host can
    // never be revived by a later layout update.
    if (!floatingDockRevealPending_)
        UpdatePersistentDockHostVisibility(host);
}
