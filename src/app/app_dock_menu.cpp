#include "app.h"
#include "../menu_fluent_glyphs.h"
#include "dock_platform_helpers.h"
#include "dock_running_app_pin_rules.h"
#include "../desktop_source.h"
#include "../logical_slot_picker_rules.h"

// Dock and running-application context menus.

namespace
{
constexpr UINT kDockStatusBarSettingsCommand = 1;
constexpr UINT kDockTaskManagerCommand = 2;
}

HMENU DesktopApp::CreateDockContextMenu(bool includeStatusBar)
{
    HMENU menu = CreatePopupMenu();
    HMENU positionMenu = CreatePopupMenu();
    HMENU layoutMenu = CreatePopupMenu();
    if (!menu || !positionMenu || !layoutMenu)
    {
        if (positionMenu) DestroyMenu(positionMenu);
        if (layoutMenu) DestroyMenu(layoutMenu);
        if (menu) DestroyMenu(menu);
        return nullptr;
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
    auto toggleLabel = [](const wchar_t* title, bool enabled) {
        std::wstring label = title;
        label += L"\t";
        label += enabled
            ? _LW("app.interact.on")
            : _LW("app.interact.off");
        return label;
    };
    const std::wstring frequentLabel = toggleLabel(
        _LW("app.dock.show_frequent"),
        dockSettings_.showFrequentItems);
    AppendMenuW(menu, MF_STRING,
        kContextDockShowFrequentItems, frequentLabel.c_str());

    const UINT keepToggleCommand =
        dockSettings_.keepWhenDesktopHidden
            ? kContextDockKeepWhenHiddenOff
            : kContextDockKeepWhenHiddenOn;
    const std::wstring keepLabel = toggleLabel(
        _LW("app.dock.keep_when_hidden"),
        dockSettings_.keepWhenDesktopHidden);
    AppendMenuW(menu, MF_STRING,
        keepToggleCommand, keepLabel.c_str());

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kContextDockDetailedSettings, _LW("app.dock.detailed"));
    if (includeStatusBar)
    {
        AppendMenuW(menu, MF_STRING, kDockStatusBarSettingsCommand, _LW("statusBar.menu.settings"));
        AppendMenuW(menu, MF_STRING, kDockTaskManagerCommand, _LW("statusBar.taskManager"));
    }

    SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(positionMenu), L"");
    SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(layoutMenu), L"");
    SetMenuItemIcon(menu, kContextDockShowFrequentItems,
        dockSettings_.showFrequentItems
            ? snowdesktop::menu_fluent_glyphs::kShowFrequent
            : snowdesktop::menu_fluent_glyphs::kHideFrequent,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(menu, keepToggleCommand,
        snowdesktop::menu_fluent_glyphs::kKeepWhenDesktopHidden,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(menu, kContextDockDetailedSettings, L"");
    if (includeStatusBar)
    {
        SetMenuItemIcon(menu, kDockStatusBarSettingsCommand, L"\uF6A9", MenuIconFont::FluentRegular);
        SetMenuItemIcon(menu, kDockTaskManagerCommand, L"\uE49D", MenuIconFont::FluentRegular);
    }
    return menu;
}

void DesktopApp::ShowDockContextMenu(POINT screenPoint)
{
    POINT clientPoint = screenPoint;
    if (statusBar_ && hwnd_ && ScreenToClient(hwnd_, &clientPoint))
    {
        const DockContainer* dock = GetDockContainerAtPoint(clientPoint);
        if (dock && dock->IsMergedWithStatusBar())
        {
            const HMONITOR monitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
            if (const HWND owner = statusBar_->InteractionWindow(monitor))
            {
                // Both parts of the merged strip share the same activation
                // hold and menu lifetime; never restore the desktop band here.
                ActivateStatusBar(snowdesktop::StatusBarAction::Menu, owner,
                    RECT{screenPoint.x, screenPoint.y, screenPoint.x + 1, screenPoint.y + 1});
            }
            return;
        }
    }
    PrepareMenuIconsForPoint(screenPoint);
    HMENU menu = CreateDockContextMenu(false);
    if (!menu) return;
    RestoreInteractionInputFocus();
    const UINT command = ShowModernMenu(menu, screenPoint, hwnd_, true);
    DestroyMenu(menu);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();
    if (command != kContextDockDetailedSettings)
        RestoreInteractionInputFocus();
    ExecuteDockContextMenuCommand(command, hwnd_);
}

void DesktopApp::ExecuteDockContextMenuCommand(UINT command, HWND owner)
{
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
        if (settingsController_)
        {
            (void)settingsController_->SynchronizeGeneral(generalSettings_);
            (void)settingsController_->SynchronizeDock(dockSettings_);
        }
        ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(
            snowdesktop::SettingsPage::DockAndTaskbar));
        return;
    case kDockStatusBarSettingsCommand:
        ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(
            snowdesktop::SettingsPage::StatusBar));
        return;
    case kDockTaskManagerCommand:
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(owner, L"open", L"taskmgr.exe",
                nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            MessageBeep(MB_ICONWARNING);
        return;
    default:
        return;
    }

    dockSettings_ = updated;
    SaveDockSettings(GetDockSettingsPath().c_str(), dockSettings_);
    if (settingsController_)
        (void)settingsController_->SynchronizeDock(dockSettings_);
    if (layoutChanged)
    {
        UpdateLayoutWorkArea();
        LayoutItems();
        InvalidateDragStaticScene();
    }
    // Visibility preferences also belong to layout backups, even when the
    // current geometry does not change.
    SaveLayoutSlots();
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, TRUE);
}

void DesktopApp::ShowDockRunningAppContextMenu(
    POINT screenPoint, size_t runningIndex)
{
    if (runningIndex >=
        dockUnpinnedRunningApps_.size())
        return;

    const DockRunningAppInfo running =
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
    shellVisualWork_.Cancel(L"dock-running-menu:");

    // Own the target across the native menu loop: application indexing and
    // desktop refresh may replace their vectors while the menu is open.
    auto showMenu = [this, running, identity, screenPoint](
        std::shared_ptr<QuickNavigationAppEntry> application) {
        if (!hwnd_ || !IsWindow(hwnd_) ||
            std::none_of(dockUnpinnedRunningApps_.begin(),
                dockUnpinnedRunningApps_.end(), [&](const auto& current) {
                    return current.identityKey == running.identityKey;
                }) || dragSession_.HasContext() ||
            dragDropController_.IsTransportActive())
            return;

        std::wstring matchingDesktopKey;
        if (const auto itemIndex = FindDesktopItemForDockRunningApp(running);
            itemIndex && *itemIndex < items_.size())
            matchingDesktopKey = items_[*itemIndex].layoutKey;

        PrepareMenuIconsForPoint(screenPoint);

        HMENU menu = CreatePopupMenu();
        if (!menu)
            return;
        if (!matchingDesktopKey.empty() || application)
        {
            AppendMenuW(
                menu, MF_STRING,
                kContextDockPinMoveToDock,
                _LW("app.dock.pin_move_to_dock"));
            if (!matchingDesktopKey.empty())
                AppendMenuW(
                    menu, MF_STRING,
                    kContextDockCreateMapping,
                    _LW("app.dock.create_mapping"));
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            SetMenuItemIcon(
                menu, kContextDockPinMoveToDock,
                snowdesktop::menu_fluent_glyphs::kPin,
                MenuIconFont::FluentRegular);
            if (!matchingDesktopKey.empty())
                SetMenuItemIcon(
                    menu, kContextDockCreateMapping,
                    snowdesktop::menu_fluent_glyphs::kLinkAdd,
                    MenuIconFont::FluentRegular);
        }
        AppendMenuW(
            menu, MF_STRING,
            kContextDockCloseApplication,
            _LW("app.dock.close_application"));
        SetMenuItemIcon(
            menu, kContextDockCloseApplication,
            L"");

        DismissDockWindowPreviewUntilLeave();
        RestoreInteractionInputFocus();
        const UINT command = ShowModernMenu(menu, screenPoint, hwnd_, true);
        DestroyMenu(menu);
        ClearMenuIcons();
        RestoreDesktopWindowLayer();
        RestoreInteractionInputFocus();

        // The native menu pumps messages. Prefer a desktop source that appeared
        // while it was open, rather than creating a second shortcut for that app.
        if (command == kContextDockPinMoveToDock)
            if (const auto itemIndex = FindDesktopItemForDockRunningApp(running);
                itemIndex && *itemIndex < items_.size())
                matchingDesktopKey = items_[*itemIndex].layoutKey;

        if ((command == kContextDockPinMoveToDock ||
                command == kContextDockCreateMapping) &&
            !matchingDesktopKey.empty())
        {
            const size_t itemIndex =
                FindItemIndexByKey(matchingDesktopKey);
            POINT clientPoint = screenPoint;
            if (itemIndex < items_.size() &&
                ScreenToClient(hwnd_, &clientPoint))
            {
                if (DockContainer* dock =
                        GetDockContainerAtPoint(clientPoint))
                {
                    DesktopIcon source(
                        &items_[itemIndex], nullptr, this);
                    const int mods =
                        command == kContextDockCreateMapping
                        ? MK_CONTROL : 0;
                    CommitDockDrop(
                        { &source }, nullptr, dock,
                        dock->GetInsertIndexAtPoint(clientPoint),
                        mods);
                    SaveLayoutSlots();
                    ApplyPageMapping();
                    LayoutItems();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
            return;
        }

        if (command == kContextDockPinMoveToDock && application)
        {
            POINT clientPoint = screenPoint;
            if (!ScreenToClient(hwnd_, &clientPoint)) return;
            DockContainer* dock = GetDockContainerAtPoint(clientPoint);
            if (!dock) return;
            const size_t insertIndex = dock->GetInsertIndexAtPoint(clientPoint);
            const bool pinned = snowdesktop::dock_running_app_pin::CreateAndPin(
                [dock] { return dock->HasCapacity(1); },
                [&] {
                    return snowdesktop::dock_running_app_pin::CreateShortcut(
                        snowdesktop::desktop_source::Directory(),
                        SanitizeShortcutFileStem(application->name),
                        application->absolutePidl.get());
                },
                [&](const std::wstring& path) {
                    return AddMaterializedItemsToDock({ path }, insertIndex, false);
                },
                [](const std::wstring& path) { DeleteFileW(path.c_str()); },
                [this] { SaveLayoutSlots(); });
            if (!pinned)
            {
                MessageBeep(MB_ICONWARNING);
                return;
            }
            ReloadItems();
            ApplyPageMapping();
            LayoutItems();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        if (command ==
            kContextDockCloseApplication)
            CloseDockApplicationWindows(identity);
    };

    // Desktop sources retain both existing actions without waiting for Shell
    // providers. Catalog-only sources get one action that creates a new link.
    if (FindDesktopItemForDockRunningApp(running))
    {
        showMenu(nullptr);
        return;
    }

    auto applications = std::make_shared<std::vector<QuickNavigationAppEntry>>();
    if (quickNavigationAppsIndexed_)
    {
        for (const auto& entry : quickNavigationAppEntries_)
        {
            QuickNavigationAppEntry copy;
            copy.name = entry.name;
            copy.parsingName = entry.parsingName;
            copy.absolutePidl.reset(ILCloneFull(entry.absolutePidl.get()));
            if (copy.absolutePidl.get()) applications->push_back(std::move(copy));
        }
    }
    const bool indexed = quickNavigationAppsIndexed_;
    const bool submitted = shellVisualWork_.Submit(
        L"dock-running-menu:" + running.identityKey,
        [applications, indexed, running] {
            if (!indexed)
            {
                HIMAGELIST systemImageList = nullptr;
                *applications = BuildQuickNavigationAppIndex(
                    nullptr, systemImageList, 32);
            }
            std::vector<snowdesktop::dock_running_app_pin::ApplicationIdentity>
                identities;
            identities.reserve(applications->size());
            for (const auto& entry : *applications)
            {
                auto app = snowdesktop::dock_running_app_pin::ReadApplicationIdentity(
                    entry.absolutePidl.get());
                app.appUserModelId = ToUpperInvariant(app.appUserModelId);
                app.executablePath = NormalizeDockExecutablePath(app.executablePath);
                if (app.appUserModelId.empty())
                {
                    const auto target = snowdesktop::logical_slot_picker_rules::
                        NormalizeApplicationLaunchTarget(entry.parsingName);
                    constexpr std::wstring_view appsFolderMarker = L"APPSFOLDER\\";
                    const auto marker = ToUpperInvariant(target).find(appsFolderMarker);
                    if (marker != std::wstring::npos)
                        app.appUserModelId = ToUpperInvariant(
                            target.substr(marker + appsFolderMarker.size()));
                }
                identities.push_back(std::move(app));
            }
            const auto match = snowdesktop::dock_running_app_pin::FindApplication(
                identities, running.executablePath, running.appUserModelId,
                running.ancestorExecutablePaths);
            return match ? std::make_shared<QuickNavigationAppEntry>(
                std::move((*applications)[*match])) :
                std::shared_ptr<QuickNavigationAppEntry>{};
        }, [showMenu, screenPoint, foreground = GetForegroundWindow()](
            std::shared_ptr<QuickNavigationAppEntry> application) {
            POINT cursor{};
            // A slow Shell provider must not open an obsolete context menu
            // after the user has moved to another target or another window.
            if (GetForegroundWindow() != foreground || !GetCursorPos(&cursor) ||
                std::abs(cursor.x - screenPoint.x) > GetSystemMetrics(SM_CXDRAG) ||
                std::abs(cursor.y - screenPoint.y) > GetSystemMetrics(SM_CYDRAG))
                return;
            showMenu(std::move(application));
        }, hwnd_, kBackgroundShellReadyMessage);
    if (!submitted) showMenu(nullptr);
}

/**
 * @brief 连续显示行列调整菜单。
 * @details 标准 Win32 菜单执行命令后会结束。这里在每次调整完成后立即按
 *          最新行列数重建菜单，方便用户连续增加或减少行列。
 */
