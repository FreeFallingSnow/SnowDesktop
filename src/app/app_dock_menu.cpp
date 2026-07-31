#include "app.h"

// Dock and running-application context menus.

void DesktopApp::ShowDockContextMenu(POINT screenPoint)
{
    HMENU menu = CreatePopupMenu();
    HMENU positionMenu = CreatePopupMenu();
    HMENU layoutMenu = CreatePopupMenu();
    if (!menu || !positionMenu || !layoutMenu)
    {
        if (positionMenu) DestroyMenu(positionMenu);
        if (layoutMenu) DestroyMenu(layoutMenu);
        if (menu) DestroyMenu(menu);
        return;
    }

    AppendMenuW(positionMenu, MF_STRING, kContextDockPositionBottom, _LW("app.dock.bottom"));
    AppendMenuW(positionMenu, MF_STRING, kContextDockPositionTop, _LW("app.dock.top"));
    AppendMenuW(positionMenu, MF_STRING, kContextDockPositionLeft, _LW("app.dock.left"));
    AppendMenuW(positionMenu, MF_STRING, kContextDockPositionRight, _LW("app.dock.right"));
    CheckMenuRadioItem(positionMenu,
        kContextDockPositionBottom, kContextDockPositionRight,
        kContextDockPositionBottom + static_cast<UINT>(dockSettings_.position),
        MF_BYCOMMAND);

    AppendMenuW(layoutMenu, MF_STRING, kContextDockLayoutIsland, _LW("app.dock.island"));
    AppendMenuW(layoutMenu, MF_STRING, kContextDockLayoutEdge, _LW("app.dock.edge"));
    CheckMenuRadioItem(layoutMenu,
        kContextDockLayoutIsland, kContextDockLayoutEdge,
        dockSettings_.edgeAttached ? kContextDockLayoutEdge : kContextDockLayoutIsland,
        MF_BYCOMMAND);

    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(positionMenu), _LW("app.dock.position"));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(layoutMenu), _LW("app.dock.layout"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu,
        MF_STRING | (dockSettings_.showFrequentItems ? MF_CHECKED : MF_UNCHECKED),
        kContextDockShowFrequentItems, _LW("app.dock.show_frequent"));

    HMENU keepMenu = CreatePopupMenu();
    if (keepMenu)
    {
        AppendMenuW(keepMenu,
            MF_STRING | (dockSettings_.keepWhenDesktopHidden ? MF_CHECKED : MF_UNCHECKED),
            kContextDockKeepWhenHiddenOn, _LW("app.interact.on"));
        AppendMenuW(keepMenu,
            MF_STRING | (!dockSettings_.keepWhenDesktopHidden ? MF_CHECKED : MF_UNCHECKED),
            kContextDockKeepWhenHiddenOff, _LW("app.interact.off"));
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(keepMenu),
            _LW("app.dock.keep_when_hidden"));
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(keepMenu), L"");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kContextDockDetailedSettings, _LW("app.dock.detailed"));

    SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(positionMenu), L"");
    SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(layoutMenu), L"");
    SetMenuItemIcon(menu, kContextDockDetailedSettings, L"");

    SetForegroundWindow(hwnd_);
    const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, hwnd_, nullptr);
    FocusDesktopInputWindow();
    DestroyMenu(menu);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();

    DockSettings updated = dockSettings_;
    bool layoutChanged = false;
    switch (command)
    {
    case kContextDockPositionBottom:
    case kContextDockPositionTop:
    case kContextDockPositionLeft:
    case kContextDockPositionRight:
        updated.position = static_cast<DockPosition>(
            command - kContextDockPositionBottom);
        layoutChanged = updated.position != dockSettings_.position;
        break;
    case kContextDockLayoutIsland:
        updated.edgeAttached = false;
        layoutChanged = updated.edgeAttached != dockSettings_.edgeAttached;
        break;
    case kContextDockLayoutEdge:
        updated.edgeAttached = true;
        layoutChanged = updated.edgeAttached != dockSettings_.edgeAttached;
        break;
    case kContextDockShowFrequentItems:
        updated.showFrequentItems = !updated.showFrequentItems;
        layoutChanged = true;
        break;
    case kContextDockKeepWhenHiddenOn:
        updated.keepWhenDesktopHidden = true;
        break;
    case kContextDockKeepWhenHiddenOff:
        updated.keepWhenDesktopHidden = false;
        break;
    case kContextDockDetailedSettings:
        if (settingsWindow_)
        {
            settingsWindow_->SyncDockEnabled(generalSettings_.dockEnabled);
            settingsWindow_->SyncDockSettings(dockSettings_);
            settingsWindow_->ShowDockSettings();
        }
        return;
    default:
        return;
    }

    dockSettings_ = updated;
    SaveDockSettings(GetDockSettingsPath().c_str(), dockSettings_);
    if (settingsWindow_)
        settingsWindow_->SyncDockSettings(dockSettings_);
    if (layoutChanged)
    {
        UpdateLayoutWorkArea();
        LayoutItems();
        SaveLayoutSlots();
        InvalidateDragStaticScene();
    }
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, TRUE);
}

void DesktopApp::ShowDockRunningAppContextMenu(
    POINT screenPoint, size_t runningIndex)
{
    if (runningIndex >=
        dockUnpinnedRunningApps_.size())
        return;

    const DockRunningAppInfo& running =
        dockUnpinnedRunningApps_[runningIndex];
    DockAppIdentity identity;
    identity.executablePath =
        running.executablePath;
    identity.appUserModelId =
        running.appUserModelId;
    identity.kind =
        !identity.appUserModelId.empty()
        ? DockAppIdentityKind::Applications
        : DockAppIdentityKind::Executable;
    if (identity.kind ==
            DockAppIdentityKind::Executable &&
        identity.executablePath.empty())
        return;

    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;
    AppendMenuW(
        menu, MF_STRING,
        kContextDockCloseApplication,
        _LW("app.dock.close_application"));
    SetMenuItemIcon(
        menu, kContextDockCloseApplication,
        L"");

    DismissDockWindowPreviewUntilLeave();
    SetForegroundWindow(hwnd_);
    const UINT command = TrackPopupMenuEx(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y,
        hwnd_, nullptr);
    FocusDesktopInputWindow();
    DestroyMenu(menu);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();

    if (command ==
        kContextDockCloseApplication)
        CloseDockApplicationWindows(identity);
}

/**
 * @brief 连续显示行列调整菜单。
 * @details 标准 Win32 菜单执行命令后会结束。这里在每次调整完成后立即按
 *          最新行列数重建菜单，方便用户连续增加或减少行列。
 */
