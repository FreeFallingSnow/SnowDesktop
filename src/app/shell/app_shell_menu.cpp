#include "app/app.h"
#include "desktop/desktop_hover_rules.h"
#include "app/popup/popup_window_pair_z_order.h"
#include "app/dock/dock_platform_helpers.h"
#include "app/dock/dock_taskbar_diagnostics.h"
#include "shell/shell_context_menu_invoke.h"
#include "shell/shell_context_menu_site.h"
#include "shell/shell_start_pin.h"
#include "shell/shell_new_item_capture.h"
#include "shell/shell_new_menu.h"
#include "shell/shell_popup_menu_tracker.h"
#include "ui/menu/menu_fluent_glyphs.h"
#include "ui/menu/modern_menu_appearance_rules.h"

// Shell New menu, desktop host restoration and protected-icon handling.

DesktopApp::ShellPopupMenuLayerGuard::
ShellPopupMenuLayerGuard(DesktopApp& app)
    : app_(app),
      active_(!app.ShouldKeepFloatingPopupTopmostForShellMenu())
{
    if (active_)
        app_.BeginShellPopupMenuLayer();
}

DesktopApp::ShellPopupMenuLayerGuard::
~ShellPopupMenuLayerGuard()
{
    if (active_)
        app_.EndShellPopupMenuLayer();
}

UINT DesktopApp::TrackShellPopupMenuWithDesktopPump(
    HMENU menu,
    UINT flags,
    POINT screenPoint,
    HWND owner)
{
    if (!menu || !owner || !IsWindow(owner))
        return 0;

    // Resolve from the same current preference as the custom menu. New/other
    // Shell menus can also reach this entry without first preparing menu icons.
    const bool lightTheme = snowdesktop::modern_menu::appearance_rules::IsLightThemeForCurrentWindows(
        static_cast<snowdesktop::modern_menu::Appearance>(
            std::clamp(CurrentPersonalization().contextMenuStyle, 0, 6)));

    HWND dockOwner = nullptr;
    for (const auto& host : persistentDockHosts_)
    {
        if (!host || !host->active || !host->container || !host->hwnd ||
            !IsWindow(host->hwnd) || !IsWindowVisible(host->hwnd) ||
            !IsDockContainerInteractionVisible(host->container)) continue;
        RECT inputBounds = host->container->GetInteractiveBounds();
        OffsetRect(&inputBounds, virtualLeft_, virtualTop_);
        const bool mergedBarSource = host->container->IsMergedWithStatusBar() &&
            statusBar_ && owner == statusBar_->InteractionWindow(host->monitor);
        if (owner == host->hwnd || mergedBarSource || PtInRect(&inputBounds, screenPoint))
        { dockOwner = host->hwnd; break; }
    }

    // The native menu loop must share the Shell context menu's STA. Keep
    // widget and composition deadlines running through the existing modal
    // animation pump instead of moving TrackPopupMenuEx to another thread.
    snowdesktop::UiAnimationScheduler::MessagePumpScope pump(
        uiAnimationScheduler_, [this]() {
            FlushPendingCompositionCommit();
            FlushPendingQuickNavigationCompositionCommit();
        });
    if (!pump.IsAvailable())
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Shell, L"Shell menu animation pump unavailable", DiagnosticLogLevel::Warning);

    shellPopupTrackerCancelRequested_.store(false, std::memory_order_release);
    const UINT command = snowdesktop::shell_popup_menu_tracker::Track(
        menu, flags, screenPoint, owner,
        ShouldKeepFloatingPopupTopmostForShellMenu(),
        shellPopupTrackerOwnerHwnd_, shellPopupTrackerCancelRequested_, lightTheme, dockOwner);
    shellPopupTrackerCancelRequested_.store(false, std::memory_order_release);
    return command;
}

BOOL DesktopApp::InvokeShellMenuCommand(
    IContextMenu* menu, CMINVOKECOMMANDINFOEX& invoke,
    snowdesktop::ShellContextMenuSite* site)
{
    if (!menu)
        return FALSE;

    // The tracker has already destroyed its foreground window. The tray
    // control is hidden and NOACTIVATE; reuse the persistent input proxy so
    // elevation/extension dialogs have an activatable owner that outlives
    // InvokeCommand, including verbs that open a modeless window.
    const HWND owner = inputHwnd_ && IsWindow(inputHwnd_)
        ? inputHwnd_ : ShellDialogOwnerHwnd();
    invoke.hwnd = owner;
    if (site)
        site->SetInvocationOwner(owner);

    snowdesktop::UiAnimationScheduler::MessagePumpScope pump(
        uiAnimationScheduler_, [this]() {
            FlushPendingCompositionCommit();
            FlushPendingQuickNavigationCompositionCommit();
        });
    if (!pump.IsAvailable())
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Shell, L"Shell invocation animation pump unavailable", DiagnosticLogLevel::Warning);

    const bool foregroundReady = FocusKeyboardWindow(
        owner, true, L"Shell command owner");
    wchar_t message[224]{};
    swprintf_s(message,
        L"Shell command begin owner=%p foregroundReady=%d animationPump=%d verb=%p",
        owner, foregroundReady, pump.IsAvailable(), invoke.lpVerb);
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Shell, message);
    const BOOL result = SafeInvokeCommand(
        menu, reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Shell, L"Shell command returned");
    return result;
}

std::vector<snowdesktop::modern_menu::Item> DesktopApp::BuildShellNewMenuItems(
    HMENU menu, IContextMenu* context)
{
    for (int index = 0; index < GetMenuItemCount(menu); ++index)
    {
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_ID | MIIM_SUBMENU | MIIM_FTYPE;
        if (!GetMenuItemInfoW(menu, index, TRUE, &item) || (item.fType & MFT_SEPARATOR)) continue;
        if (item.hSubMenu) BuildShellNewMenuItems(item.hSubMenu, context);
        char verb[128]{};
        if (context && item.wID >= snowdesktop::ShellNewMenu::FirstCommand &&
            item.wID <= snowdesktop::ShellNewMenu::LastCommand)
            context->GetCommandString(item.wID - snowdesktop::ShellNewMenu::FirstCommand,
                GCS_VERBA, nullptr, verb, sizeof(verb));
        SetMenuItemIcon(menu, item.hSubMenu ? reinterpret_cast<UINT_PTR>(item.hSubMenu) : item.wID,
            lstrcmpiA(verb, "NewFolder") == 0 ? snowdesktop::menu_fluent_glyphs::kDesktopFiles :
                (lstrcmpiA(verb, "NewLink") == 0 ? snowdesktop::menu_fluent_glyphs::kLinkAdd : L"\uF394"),
            MenuIconFont::FluentRegular);
    }
    return BuildModernMenuItems(menu);
}

void DesktopApp::InvokeNewMenuCommand(IContextMenu* context, UINT command,
    const std::wstring& targetDir)
{
    if (!context || command < snowdesktop::ShellNewMenu::FirstCommand ||
        command > snowdesktop::ShellNewMenu::LastCommand) return;
    CMINVOKECOMMANDINFOEX invoke{};
    invoke.cbSize = sizeof(invoke);
    invoke.fMask = CMIC_MASK_UNICODE;
    invoke.hwnd = ShellDialogOwnerHwnd();
    invoke.lpVerb = MAKEINTRESOURCEA(command - snowdesktop::ShellNewMenu::FirstCommand);
    invoke.lpVerbW = MAKEINTRESOURCEW(command - snowdesktop::ShellNewMenu::FirstCommand);
    std::string invocationDirectoryA;
    snowdesktop::SetShellInvocationDirectory(invoke, targetDir, invocationDirectoryA);
    invoke.nShow = SW_SHOWNORMAL;
    InvokeShellMenuCommand(context, invoke);
}

void DesktopApp::ShowNewMenuAndInvoke(POINT screenPoint, const std::wstring& targetDir,
    bool folderOnly, std::wstring desktopFilesWidgetId)
{
    snowdesktop::ShellNewMenu newMenu;
    const auto attachCapture = [&](IContextMenu* context) -> HRESULT
    {
        if (desktopFilesWidgetId.empty()) return S_OK;
        const size_t target = FindWidgetIndexById(desktopFilesWidgetId);
        if (target >= widgets_.size() ||
            widgets_[target].type != DesktopWidgetType::FileCategories) return E_INVALIDARG;
        auto capture = std::make_shared<snowdesktop::ShellNewItemCapture>(
            targetDir, std::move(desktopFilesWidgetId), widgets_[target].itemKeys.size(),
            hwnd_, kShellChangeMessage);
        const HRESULT hr = snowdesktop::AttachShellNewItemCapture(context, capture);
        if (FAILED(hr))
        {
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Shell, L"New-menu item ownership site unavailable", DiagnosticLogLevel::Warning);
            MessageBeep(MB_ICONWARNING);
            return hr;
        }
        pendingNewItemCaptures_.push_back(std::move(capture));
        return S_OK;
    };
    if (FAILED(newMenu.Initialize(targetDir, attachCapture))) return;

    UINT cmd = 0;
    if (folderOnly)
    {
        // Resolve the canonical Shell verb, never a localized label or a
        // position that a third-party New-menu extension can reorder.
        cmd = newMenu.FolderCommand();
        if (!cmd) MessageBeep(MB_ICONWARNING);
    }
    else if (newMenu.CanDraw())
    {
        PrepareMenuIconsForPoint(screenPoint);
        BuildShellNewMenuItems(newMenu.Menu(), newMenu.Context());
        cmd = ShowModernMenu(newMenu.Menu(), screenPoint, hwnd_);
        ClearMenuIcons();
    }
    else
    {
        // Preserve the established native boundary for unreadable/owner-drawn
        // third-party entries rather than inventing labels or commands.
        newMenu.Context()->QueryInterface(IID_PPV_ARGS(newMenuContextMenu_.GetAddressOf()));
        const HWND menuOwner = ShellDialogOwnerHwnd();
        SetForegroundWindow(menuOwner);
        ShellPopupMenuLayerGuard shellMenuLayer(*this);
        cmd = TrackShellPopupMenuWithDesktopPump(
            newMenu.Menu(),
            TPM_RETURNCMD | TPM_LEFTALIGN | TPM_LEFTBUTTON,
            screenPoint, menuOwner);
    }
    newMenuContextMenu_.Reset();

    if (newMenu.Contains(cmd))
        InvokeNewMenuCommand(newMenu.Context(), cmd, targetDir);
    RestoreDesktopWindowLayer();
    RestoreInteractionInputFocus();
}

/**
 * @brief 通过 Shell IContextMenu 显示桌面的系统背景右键菜单。
 *        使用 desktopFolder_->CreateViewObject 获取桌面文件夹的
 *        IContextMenu 接口，显示系统提供的背景菜单（如显示设置、个性化等）。
 * @param screenPoint 菜单弹出的屏幕坐标。
 */
void DesktopApp::ShowDesktopBackgroundContextMenu(POINT screenPoint)
{
    const UINT extendedFlags = (GetKeyState(VK_SHIFT) & 0x8000) ? CMF_EXTENDEDVERBS : 0;
    // Shell initialization can pump desktop paints before TrackPopupMenuEx.
    // Keep their content commits and hover state covered for the whole handoff.
    ShellPopupMenuLayerGuard shellMenuLayer(*this);
    ComPtr<IShellFolder> backgroundFolder = desktopFolder_;
    if (snowdesktop::debug_profile::Enabled())
    {
        PIDLIST_ABSOLUTE raw = nullptr;
        if (FAILED(SHParseDisplayName(snowdesktop::desktop_source::Directory().c_str(), nullptr, &raw, 0, nullptr))) return;
        Pidl folderId(raw);
        if (FAILED(desktopFolder_->BindToObject(raw, nullptr, IID_PPV_ARGS(&backgroundFolder)))) return;
    }
    const HWND menuOwner = ShellDialogOwnerHwnd();
    snowdesktop::ShellContextMenuSite menuSite;
    ComPtr<IContextMenu> contextMenu;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    constexpr UINT kFirstCmd = 1;
    constexpr UINT kLastCmd = 0x7FFF;
    const HRESULT hr = menuSite.BuildMenu(backgroundFolder.Get(), menuOwner, menu, kFirstCmd, kLastCmd,
        CMF_NORMAL | CMF_SYNCCASCADEMENU | extendedFlags,
        [&](HWND bindOwner, IContextMenu** target) {
            return backgroundFolder->CreateViewObject(bindOwner, IID_IContextMenu, reinterpret_cast<void**>(target));
        }, contextMenu.GetAddressOf());
    if (FAILED(hr)) { DestroyMenu(menu); RestoreDesktopWindowLayer(); return; }

    contextMenu.As(&activeContextMenu2_);
    contextMenu.As(&activeContextMenu3_);

    SetForegroundWindow(menuOwner);
    UINT cmd = TrackShellPopupMenuWithDesktopPump(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint, menuOwner);

    activeContextMenu2_.Reset();
    activeContextMenu3_.Reset();

    if (cmd != 0)
    {
        const std::wstring invocationDirectory =
            snowdesktop::DesktopShellInvocationDirectory();
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
        invoke.hwnd = ShellDialogOwnerHwnd();
        invoke.lpVerb = MAKEINTRESOURCEA(cmd - kFirstCmd);
        invoke.lpVerbW = MAKEINTRESOURCEW(cmd - kFirstCmd);
        std::string invocationDirectoryA;
        snowdesktop::SetShellInvocationDirectory(
            invoke, invocationDirectory, invocationDirectoryA);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screenPoint;
        InvokeShellMenuCommand(contextMenu.Get(), invoke, &menuSite);
        RequestShellRefresh();
    }
    DestroyMenu(menu);
    RestoreDesktopWindowLayer();
    if (cmd == 0)
        RestoreInteractionInputFocus();
}

/**
 * @brief 恢复桌面窗口的 Z 序层次位置。
 *        菜单弹出后可能改变窗口 Z 序，此方法将窗口恢复到正确的位置。
 *        有父窗口时置顶（HWND_TOP），无父窗口时置底（HWND_BOTTOM）。
 */
void DesktopApp::RestoreDesktopWindowLayer()
{
    BeginDesktopInteractionTrace(L"restore-layer");
    ApplyFloatingDockLayerPolicy();
    ApplyFloatingPopupLayerPolicy();
    if (!hwnd_ || !IsWindow(hwnd_))
        return;
    POINT origin{ virtualLeft_, virtualTop_ };
    HWND parent = GetParent(hwnd_);
    if (parent)
    {
        ScreenToClient(parent, &origin);
        SetWindowPos(hwnd_, HWND_TOP, origin.x, origin.y, virtualWidth_, virtualHeight_, SWP_NOACTIVATE);
    }
    else
    {
        SetWindowPos(hwnd_, HWND_BOTTOM, virtualLeft_, virtualTop_, virtualWidth_, virtualHeight_, SWP_NOACTIVATE);
    }
    TraceDesktopInteraction(L"restore-layer-done", hwnd_, 0, 0, 0, true);
}

void DesktopApp::ApplyFloatingDockLayerPolicy()
{
    for (const auto& host : persistentDockHosts_)
        if (host)
            ApplyFloatingDockLayerPolicy(*host);
}

void DesktopApp::ApplyFloatingDockLayerPolicy(
    PersistentDockHost& host)
{
    if (!host.active || !host.hwnd ||
        !IsWindow(host.hwnd))
        return;
    if (dockWindowTransitionLayerUpdateActive_)
        return;
    const bool wasTopmost =
        snowdesktop::popup_window_pair_z_order::IsTopmost(host.hwnd);
    dockWindowTransitionLayerUpdateActive_ = true;
    struct LayerUpdateScope final
    {
        bool& active;
        ~LayerUpdateScope() { active = false; }
    } layerUpdateScope{dockWindowTransitionLayerUpdateActive_};
    const bool merged = host.container && host.container->IsMergedWithStatusBar();
    host.desktopLayerRestorePending = false;
    host.desktopLayerRestoreRetryTick = GetTickCount64() +
        snowdesktop::floating_dock_rules::kDesktopLayerRestoreRetryMs;
    host.desktopLayerForegroundWindow = GetForegroundWindow();
    if (merged)
    {
        // The visible merged strip is one topmost surface. Summon/hover policy
        // still owns its visibility; menu holds never move it between bands.
        host.mergedPresentation.topmost = true;
        host.mergedPresentation.insertAfter = host.hwnd;
        // Merged chrome belongs to the bar; the center has no separate glass
        // pair. Reapplying TOPMOST would raise it above a side-control menu
        // before that menu's message pump repairs its order on the next turn.
        snowdesktop::popup_window_pair_z_order::MaintainContentBand(host.hwnd, true);
        if (statusBar_ && host.mergedPresentationActive)
            statusBar_->ApplyMergedDockPresentation(host.monitor, host.mergedPresentation);
        // Continue through the shared search/window-animation ordering below.
    }
    const HWND transitionWindow = dockWindowTransition_
        ? dockWindowTransition_->GetPresentationWindow() : nullptr;
    const HWND navigationWindow =
        quickNavigationInvocationSource_ == QuickNavigationInvocationSource::DockSearch &&
        quickNavigationAnimation_.IsAnimating()
            ? quickNavigationHwnd_ : nullptr;
    RECT dockBounds{};
    const bool hasDockBounds = GetWindowRect(host.hwnd, &dockBounds) != FALSE;
    const auto intersectsPresentation = [&dockBounds, hasDockBounds](HWND window) {
        RECT bounds{}, overlap{};
        return hasDockBounds && window && IsWindow(window) &&
            GetWindowRect(window, &bounds) &&
            IntersectRect(&overlap, &bounds, &dockBounds);
    };
    const bool intersectsTransition = intersectsPresentation(transitionWindow);
    const bool intersectsNavigation = intersectsPresentation(navigationWindow);
    if ((intersectsTransition || intersectsNavigation) &&
        IsPersistentDockHostEffectivelyFloating(host) &&
        ShouldShowPersistentDockHost(host) && IsWindowVisible(host.hwnd))
    {
        // Only the Dock's interaction state owns its band. A presentation may
        // lower its target below this pair, but cannot re-promote a Dock already
        // dismissed by an outside click.
        if (!merged) host.backdrop.SetPopupWindowPairZOrder(
            host.hwnd, HWND_TOPMOST, true);
        if (!wasTopmost)
            snowdesktop::dock_taskbar_diagnostics::Record(
                L"dock-promoted-for-animation", host.hwnd);
        const HWND nextWindow = GetWindow(host.hwnd, GW_HWNDNEXT);
        HWND pairEnd = host.backdrop.IsBackdropWindow(nextWindow)
            ? nextWindow : host.hwnd;
        if (merged && statusBar_)
            if (const HWND stripBottom = statusBar_->MergedPresentationBottomWindow(host.monitor)) pairEnd = stripBottom;
        // An already-topmost pair deliberately does not raise itself above
        // menus on policy refresh. Lower the transition below its complete
        // pair instead of inserting it between the content and glass helper.
        if (intersectsTransition &&
            snowdesktop::popup_window_pair_z_order::IsTopmost(pairEnd) &&
            !snowdesktop::popup_window_pair_z_order::IsAbove(
                pairEnd, transitionWindow))
        {
            SetWindowPos(transitionWindow, pairEnd, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                    SWP_NOOWNERZORDER);
        }
        if (intersectsNavigation &&
            snowdesktop::popup_window_pair_z_order::IsTopmost(pairEnd) &&
            !snowdesktop::popup_window_pair_z_order::IsAbove(pairEnd, navigationWindow))
        {
            // Quick Navigation has a separate glass HWND. Lower its complete
            // pair, only when necessary, so another Dock already above it is
            // never crossed while processing a later host. Reordering leaves
            // keyboard focus alone; the native edit is transparent in motion.
            quickNavBackdropCompositor_.SetPopupWindowPairZOrder(
                navigationWindow, pairEnd, true);
        }
        ApplyDragPreviewLayerPolicy();
        return;
    }
    if (merged) { ApplyDragPreviewLayerPolicy(); return; }
    // Retain the floating band until the last closing frame, without feeding
    // presentation visibility back into ShouldShowPersistentDockHost.
    const bool hostsCollectionPopup =
        popupAnchoredToDock_ && collectionPopupDockHost_ == &host &&
        (popupAnimation_.IsInteractive() || !popupAnimation_.IsHidden()) &&
        ShouldShowFloatingPopupWindow();
    const bool promoted = IsPersistentDockHostEffectivelyFloating(host) ||
        hostsCollectionPopup ||
        (host.mergedPresentationActive && host.mergedAnimation.IsClosing());
    const bool systemShowDesktopGuard =
        systemShowDesktopDockLayerGuardActive_ &&
        ShouldShowPersistentDockHost(host);

    if (!promoted && !systemShowDesktopGuard)
    {
        // A desktop-band Dock is still a top-level no-activate window. Place
        // it below ordinary applications and above Explorer's desktop host,
        // while preserving the same HWND and
        // compositor resources used by floating mode.
        const HWND desktopHost =
            desktopWindows_.host && IsWindow(desktopWindows_.host)
                ? desktopWindows_.host : nullptr;
        const bool restored = snowdesktop::floating_dock_rules::RestoreDesktopLayer(
            host.hwnd, desktopHost,
            [](HWND window) {
                // Reject hidden/tool surfaces before the task-window query
                // reaches DWM; background windows cannot cover this Dock.
                return IsWindowVisible(window) && !IsIconic(window) &&
                    snowdesktop::dock_window_rules::IsTaskWindowStyleEligible(
                        GetWindowLongPtrW(window, GWL_EXSTYLE), GetWindow(window, GW_OWNER) != nullptr) &&
                    IsDockTaskWindow(window);
            },
            [this](HWND window) {
                return IsPersistentDockHostWindow(window) || IsPersistentDockBackdropWindow(window);
            },
            [&host](HWND anchor) {
                return host.backdrop.SetPopupWindowPairZOrder(host.hwnd, anchor, false);
            });
        host.desktopLayerRestorePending = !restored;
        host.desktopLayerRestoreRetryTick = GetTickCount64() +
            snowdesktop::floating_dock_rules::kDesktopLayerRestoreRetryMs;
        host.desktopLayerForegroundWindow = GetForegroundWindow();
        if (wasTopmost && restored)
            snowdesktop::dock_taskbar_diagnostics::Record(
                L"dock-returned-to-desktop-band", host.hwnd);
        return;
    }

    const bool shouldBeTopmost = systemShowDesktopGuard ||
        snowdesktop::floating_dock_rules::
            ShouldFloatingDockBeTopmost(
                promoted,
                shellPopupMenuLayerDepth_);
    host.backdrop.SetPopupWindowPairZOrder(
        host.hwnd,
        shouldBeTopmost
            ? HWND_TOPMOST : HWND_NOTOPMOST,
        shouldBeTopmost);
    if (wasTopmost != shouldBeTopmost)
        snowdesktop::dock_taskbar_diagnostics::Record(
            systemShowDesktopGuard ? L"dock-show-desktop-band" : L"dock-normal-layer-policy",
            host.hwnd);
    ApplyDragPreviewLayerPolicy();
}

bool DesktopApp::ShouldKeepFloatingPopupTopmostForShellMenu() const
{
    const bool fullscreenDock = floatingDockHost_ && floatingDockHost_->active &&
        floatingDockHost_->promoted && floatingDockHost_->hwnd &&
        IsWindowVisible(floatingDockHost_->hwnd) && statusBar_ &&
        statusBar_->IsFullscreen(floatingDockHost_->monitor);
    // Demoting a fullscreen-summoned Dock also leaves its native Shell menu
    // behind a topmost fullscreen application. The tracker must join the same
    // band, just like a menu opened from the floating collection popup.
    return fullscreenDock || (IsCollectionPopupHostedByFloatingWindow() &&
        floatingPopupHwnd_ &&
        IsWindow(floatingPopupHwnd_) &&
        IsWindowVisible(floatingPopupHwnd_));
}

void DesktopApp::BeginShellPopupMenuLayer()
{
    // Flush the frame that opened the native menu before its tracking thread
    // takes pointer capture. The desktop pump remains live for the session.
    FlushPendingCompositionCommit();
    FlushPendingQuickNavigationCompositionCommit();
    ++shellPopupMenuLayerDepth_;
    if (shellPopupMenuLayerDepth_ == 1)
    {
        if (shellHoverTraceActive_)
            FlushShellHoverTrace();
        BeginShellHoverTrace();
    }
    RecordShellHoverTrace(
        ShellHoverTraceEvent::MenuBegin);
    ApplyFloatingDockLayerPolicy();
    ApplyFloatingPopupLayerPolicy();
}

void DesktopApp::EndShellPopupMenuLayer()
{
    RecordShellHoverTrace(
        ShellHoverTraceEvent::MenuEnd);
    if (shellPopupMenuLayerDepth_ > 0)
        --shellPopupMenuLayerDepth_;
    else
        shellPopupMenuLayerDepth_ = 0;
    if (shellPopupMenuLayerDepth_ == 0)
        shellHoverTraceMenuEndTick_ = GetTickCount64();
    ApplyFloatingDockLayerPolicy();
    ApplyFloatingPopupLayerPolicy();
    // A verb may have activated an external dialog or application. Restore
    // Dock keyboard ownership only while the foreground still belongs to us.
    DWORD foregroundProcess = 0;
    if (const HWND foreground = GetForegroundWindow())
        GetWindowThreadProcessId(foreground, &foregroundProcess);
    if (foregroundProcess == GetCurrentProcessId())
        RefocusFloatingDockKeyboardSession();
    if (shellPopupMenuLayerDepth_ == 0)
    {
        ReconcileDesktopHoverState(
            snowdesktop::desktop_hover_rules::
                ShellPopupCloseReconcileMode());
    }
    FlushPendingCompositionCommit();
    FlushPendingQuickNavigationCompositionCommit();
}

/**
 * @brief 判断指定桌面项是否为受保护的系统图标。
 *        通过比较 CLSID 判断是否为此电脑、用户文件、网络、
 *        控制面板或回收站等系统图标。
 * @param item 要检查的桌面项。
 * @return 如果是受保护的系统图标返回 true，否则返回 false。
 */
bool DesktopApp::IsProtectedDesktopIcon(const DesktopItem& item) const
{
    std::wstring clsid = !item.desktopIconClsid.empty()
        ? item.desktopIconClsid
        : ExtractClsidText(item.parsingName);
    return clsid == kDesktopIconClsidThisPC ||
        clsid == kDesktopIconClsidUserFiles ||
        clsid == kDesktopIconClsidNetwork ||
        clsid == kDesktopIconClsidControlPanel ||
        clsid == kDesktopIconClsidRecycleBin;
}

/**
 * @brief 对指定的文件夹路径显示 Shell 右键菜单。
 *        解析路径的 PIDL，绑定到 IShellFolder，通过 CreateViewObject
 *        获取 IContextMenu 接口后弹出系统右键菜单。
 * @param folderPath  目标文件夹的路径。
 * @param screenPoint 菜单弹出的屏幕坐标。
 */
void DesktopApp::ShowShellContextMenuForPath(const std::wstring& folderPath, POINT screenPoint)
{
    const UINT extendedFlags = (GetKeyState(VK_SHIFT) & 0x8000) ? CMF_EXTENDEDVERBS : 0;
    ShellPopupMenuLayerGuard shellMenuLayer(*this);
    const HWND menuOwner = ShellDialogOwnerHwnd();
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(folderPath.c_str(), nullptr, &pidl, 0, nullptr)))
        return;

    IShellFolder* parentFolder = nullptr;
    PCUITEMID_CHILD child = nullptr;
    if (FAILED(SHBindToParent(pidl, IID_IShellFolder,
        reinterpret_cast<void**>(&parentFolder), &child)))
    {
        ILFree(pidl);
        return;
    }

    ComPtr<IShellFolder> folder;
    HRESULT bindHr = parentFolder->BindToObject(child, nullptr, IID_IShellFolder, reinterpret_cast<void**>(folder.GetAddressOf()));
    parentFolder->Release();
    if (FAILED(bindHr) || !folder)
    {
        ILFree(pidl);
        return;
    }

    snowdesktop::ShellContextMenuSite menuSite;
    ComPtr<IContextMenu> contextMenu;
    HMENU menu = CreatePopupMenu();
    if (!menu) { ILFree(pidl); return; }

    constexpr UINT kFirstCmd = 1;
    constexpr UINT kLastCmd = 0x7FFF;
    if (FAILED(menuSite.BuildMenu(folder.Get(), menuOwner, menu, kFirstCmd, kLastCmd,
            CMF_NORMAL | CMF_EXPLORE | CMF_CANRENAME | CMF_SYNCCASCADEMENU | extendedFlags,
            [&](HWND bindOwner, IContextMenu** target) {
                return folder->CreateViewObject(bindOwner, IID_IContextMenu, reinterpret_cast<void**>(target));
            }, contextMenu.GetAddressOf())))
    {
        DestroyMenu(menu);
        RestoreDesktopWindowLayer();
        ILFree(pidl);
        return;
    }

    contextMenu.As(&activeContextMenu2_);
    contextMenu.As(&activeContextMenu3_);

    SetForegroundWindow(menuOwner);
    UINT command = TrackShellPopupMenuWithDesktopPump(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint, menuOwner);

    activeContextMenu2_.Reset();
    activeContextMenu3_.Reset();

    if (command != 0)
    {
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
        invoke.hwnd = ShellDialogOwnerHwnd();
        invoke.lpVerb = MAKEINTRESOURCEA(command - kFirstCmd);
        invoke.lpVerbW = MAKEINTRESOURCEW(command - kFirstCmd);
        std::string invocationDirectoryA;
        snowdesktop::SetShellInvocationDirectory(
            invoke, folderPath, invocationDirectoryA);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screenPoint;
        InvokeShellMenuCommand(contextMenu.Get(), invoke, &menuSite);
        RequestShellRefresh();
    }

    DestroyMenu(menu);
    RestoreDesktopWindowLayer();
    if (command == 0)
        RestoreInteractionInputFocus();
    ILFree(pidl);
}

void DesktopApp::
ShowShellItemContextMenuForPaths(
    const std::vector<std::wstring>& itemPaths,
    POINT screenPoint)
{
    if (itemPaths.empty()) return;
    const UINT extendedFlags = (GetKeyState(VK_SHIFT) & 0x8000) ? CMF_EXTENDEDVERBS : 0;
    ShellPopupMenuLayerGuard shellMenuLayer(*this);
    const HWND menuOwner = ShellDialogOwnerHwnd();
    ComPtr<IShellFolder> boundParent;
    Pidl parentSnapshot;
    std::vector<Pidl> selectionSnapshot;
    std::vector<PCUITEMID_CHILD> children;
    // Mapped-folder and Dock popup selections belong to one source folder.
    // Retain every PIDL through Shell reentrancy and reject a mixed parent.
    for (const auto& path : itemPaths)
    {
        PIDLIST_ABSOLUTE raw = nullptr;
        if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &raw, 0, nullptr)) || !raw)
            return;
        Pidl item(raw);
        Pidl parent(ILCloneFull(raw));
        if (!parent.get() || !ILRemoveLastID(parent.get())) return;
        if (!boundParent)
        {
            PCUITEMID_CHILD child = nullptr;
            if (FAILED(SHBindToParent(raw, IID_PPV_ARGS(&boundParent), &child))) return;
            parentSnapshot = std::move(parent);
        }
        else if (!ILIsEqual(parentSnapshot.get(), parent.get()))
            return;
        children.push_back(ILFindLastID(raw));
        selectionSnapshot.push_back(std::move(item));
    }

    snowdesktop::ShellContextMenuSite menuSite;
    ComPtr<IContextMenu> contextMenu;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    constexpr UINT kFirstCmd = 1;
    constexpr UINT kLastCmd = 0x7FFF;
    if (FAILED(
            menuSite.BuildMenu(boundParent.Get(), menuOwner, menu, kFirstCmd, kLastCmd,
                CMF_NORMAL | CMF_EXPLORE | CMF_CANRENAME | CMF_SYNCCASCADEMENU | extendedFlags,
                [&](HWND bindOwner, IContextMenu** target) {
                    return boundParent->GetUIObjectOf(bindOwner, static_cast<UINT>(children.size()), children.data(), IID_IContextMenu,
                        nullptr, reinterpret_cast<void**>(target));
                }, contextMenu.GetAddressOf())))
    {
        DestroyMenu(menu);
        return;
    }

    contextMenu.As(
        &activeContextMenu2_);
    contextMenu.As(
        &activeContextMenu3_);
    SetForegroundWindow(menuOwner);
    const UINT command =
        TrackShellPopupMenuWithDesktopPump(
            menu,
            TPM_RETURNCMD |
                TPM_RIGHTBUTTON,
            screenPoint,
            menuOwner);
    activeContextMenu2_.Reset();
    activeContextMenu3_.Reset();

    if (command >= kFirstCmd &&
        command <= kLastCmd)
    {
        const UINT commandOffset =
            command - kFirstCmd;
        if (IsShellDeleteCommand(
                contextMenu.Get(), commandOffset))
        {
            DestroyMenu(menu);
            RestoreDesktopWindowLayer();
            std::vector<snowdesktop::ShellFileOperationStep> steps;
            steps.push_back({
                FO_DELETE,
                itemPaths,
                {},
                static_cast<FILEOP_FLAGS>(
                    FOF_ALLOWUNDO |
                    FOF_NOCONFIRMATION) });
            QueueShellFileOperation(
                std::move(steps),
                [this](bool succeeded) {
                    if (!succeeded)
                        return;
                    RequestShellRefresh();

                });
            return;
        }
        const std::wstring invocationDirectory =
            snowdesktop::ShellInvocationDirectoryForItem(itemPaths.front());
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask =
            CMIC_MASK_UNICODE |
            CMIC_MASK_PTINVOKE;
        invoke.hwnd =
            ShellDialogOwnerHwnd();
        invoke.lpVerb =
            MAKEINTRESOURCEA(
                commandOffset);
        invoke.lpVerbW =
            MAKEINTRESOURCEW(
                commandOffset);
        std::string invocationDirectoryA;
        snowdesktop::SetShellInvocationDirectory(
            invoke, invocationDirectory, invocationDirectoryA);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screenPoint;
        const auto startPin = snowdesktop::shell_start_pin::Route(
            contextMenu.Get(), commandOffset, itemPaths,
            [&](auto action, const auto& path) {
                snowdesktop::UiAnimationScheduler::MessagePumpScope pump(
                    uiAnimationScheduler_, [this]() { FlushPendingCompositionCommit(); });
                return snowdesktop::shell_start_pin::Invoke(
                    snowdesktop::deployment::GetTaskbarHookPath(), action,
                    path, invoke.hwnd, screenPoint);
            });
        if (startPin)
        {
            wchar_t message[96]{};
            swprintf_s(message, L"Shell Start pin command result=0x%08lX",
                static_cast<unsigned long>(*startPin));
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Shell, message, FAILED(*startPin) ? DiagnosticLogLevel::Warning : DiagnosticLogLevel::Info);
        }
        else
            InvokeShellMenuCommand(contextMenu.Get(), invoke, &menuSite);
        for (size_t i = 0;
            i < widgets_.size(); ++i)
        {
            if (widgets_[i].type ==
                DesktopWidgetType::
                    FolderMapping)
                RefreshFolderMappingWidget(
                    i);
        }
        RequestShellRefresh();

    }

    DestroyMenu(menu);
    RestoreDesktopWindowLayer();
    if (command == 0)
        RestoreInteractionInputFocus();
}
