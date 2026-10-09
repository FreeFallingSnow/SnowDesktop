#include "app/app.h"
#include "icons/large_icon_steam.h"
#include "icons/large_icon_preset_rules.h"
#include "icons/large_icon_edit_rules.h"
#include "ui/menu/menu_fluent_glyphs.h"
#include "ui/menu/right_click_contract.h"
#include "app/shell/shell_item_action_rules.h"
#include "shell/shell_context_menu_invoke.h"
#include "shell/shell_context_menu_site.h"
#include "shell/shell_start_pin.h"
#include "shell/namespace_menu_actions.h"
#include "desktop/folder_mapping_visibility_rules.h"
#include "app/shell/shell_icon_request.h"

namespace { constexpr UINT kContextNamespaceActionFirst = 42000; }

// Desktop-item and Shell-backed context menus.

bool DesktopApp::IsAdministratorRunnablePath(
    const std::wstring& path) const
{
    if (path.empty())
        return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return false;
    const wchar_t* extension = PathFindExtensionW(path.c_str());
    if (!extension)
        return false;
    std::wstring normalizedExtension(extension);
    CharLowerBuffW(
        normalizedExtension.data(),
        static_cast<DWORD>(normalizedExtension.size()));
    return
        snowdesktop::shell_item_action_rules::
            IsAdministratorRunnableExtension(normalizedExtension);
}

void DesktopApp::CopyPathsToClipboard(
    const std::vector<std::wstring>& paths)
{
    std::wstring text;
    for (const auto& path : paths)
    {
        if (path.empty())
            continue;
        if (!text.empty())
            text += L"\r\n";
        text += path;
    }
    if (text.empty() || !OpenClipboard(ShellDialogOwnerHwnd()))
        return;

    EmptyClipboard();
    const SIZE_T byteCount =
        (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, byteCount);
    if (!memory)
    {
        CloseClipboard();
        return;
    }

    void* target = GlobalLock(memory);
    if (!target)
    {
        GlobalFree(memory);
        CloseClipboard();
        return;
    }
    CopyMemory(target, text.c_str(), byteCount);
    GlobalUnlock(memory);

    if (!SetClipboardData(CF_UNICODETEXT, memory))
        GlobalFree(memory);
    CloseClipboard();
}

bool DesktopApp::RunPathAsAdministrator(
    const std::wstring& path)
{
    if (!IsAdministratorRunnablePath(path))
        return false;

    // ShellExecuteEx(runas) can wait for the consent UI or a third-party
    // shortcut handler. Keep that wait off the rendering/input thread while
    // using a dedicated queue so a pending prompt cannot delay ordinary Open.
    // The independent input proxy remains activatable after a quick panel or
    // menu closes. Never activate the Explorer-owned desktop render child.
    return shellElevationWorker_.Enqueue(
        ShellLaunchOwnerHwnd(),
        path);
}

bool DesktopApp::RunPathAsAdministratorAfterMenu(
    const std::wstring& path)
{
    if (!IsAdministratorRunnablePath(path))
        return false;

    // The elevation worker can race the tail of the menu's input handler.
    // Defer one UI turn so Windows has retired the active menu before the
    // worker requests foreground access for the consent broker.
    return uiAnimationScheduler_.ScheduleOnce(
        1,
        [this, path](snowdesktop::UiScheduleToken) {
            const HWND owner = ShellLaunchOwnerHwnd();
            if (owner)
                FocusKeyboardWindow(owner, true, L"Administrator launch owner");
            RunPathAsAdministrator(path);
        }) != 0;
}

void DesktopApp::ShowPathProperties(
    const std::wstring& path)
{
    if (path.empty())
        return;
    SHObjectProperties(
        ShellDialogOwnerHwnd(), SHOP_FILEPATH,
        path.c_str(), nullptr);
}

void DesktopApp::ShowItemContextMenu(
    POINT screenPoint, int itemIndex, bool dockFrequentItem,
    bool keepQuickNavigationOpen,
    std::optional<RECT> dockRenameAnchor,
    std::optional<size_t> dockMappingEntryIndex,
    bool dockApplicationItem,
    std::optional<size_t> dockEntryIndex)
{
    if (itemIndex < 0 || static_cast<size_t>(itemIndex) >= items_.size()) return;
    PrepareMenuIconsForPoint(screenPoint);

    const bool dockMapping =
        dockMappingEntryIndex &&
        *dockMappingEntryIndex < dockEntries_.size() &&
        snowdesktop::
            desktop_item_reference_migration::
                IsDockMapping(
                    dockEntries_[*dockMappingEntryIndex]) &&
        snowdesktop::
            desktop_item_reference_migration::
                KeysEqual(
                    dockEntries_[*dockMappingEntryIndex].
                        reference,
                    items_[static_cast<size_t>(
                        itemIndex)].layoutKey);
    const bool directDockFolder =
        dockEntryIndex &&
        *dockEntryIndex < dockEntries_.size() &&
        dockEntries_[*dockEntryIndex].type ==
            DockEntryType::DesktopItem &&
        IsFolderDockEntry(
            dockEntries_[*dockEntryIndex]);
    DockEntry* dockFolderEntry = directDockFolder
        ? &dockEntries_[*dockEntryIndex]
        : nullptr;

    int selectedCount = 0;
    size_t selectedFileCount = 0;
    size_t selectedNamespaceCount = 0;
    for (const auto& item : items_)
    {
        if (!item.selected)
            continue;
        ++selectedCount;
        if (!item.desktopIconClsid.empty())
            ++selectedNamespaceCount;
    }

    std::vector<std::wstring> selectedFilePaths;
    for (const auto& item : items_)
    {
        if (!item.selected)
            continue;
        wchar_t path[MAX_PATH]{};
        if (SHGetPathFromIDListW(item.absolutePidl.get(), path))
        {
            selectedFilePaths.emplace_back(path);
            if (item.desktopIconClsid.empty())
                ++selectedFileCount;
        }
    }

    bool canFile = !items_[itemIndex].desktopIconClsid.empty() ? false : true;
    std::wstring itemPath;
    if (canFile)
    {
        wchar_t path[MAX_PATH]{};
        if (!SHGetPathFromIDListW(items_[itemIndex].absolutePidl.get(), path))
            canFile = false;
        else
            itemPath = path;
    }
    const bool canReveal = selectedCount == 1 && canFile &&
        snowdesktop::item_location::CanReveal(itemPath);
    const bool canCopyPath =
        selectedCount > 0 &&
        selectedFilePaths.size() == static_cast<size_t>(selectedCount);
    const bool canRunAsAdministrator =
        selectedCount == 1 &&
        IsAdministratorRunnablePath(itemPath);
    const bool administratorLaunch =
        selectedCount == 1 &&
        snowdesktop::ShellLaunchWorker::
            PathRequestsAdministrator(itemPath);
    const bool canOpen =
        selectedCount == 1 && !administratorLaunch;
    const bool protectedDesktopIcon = selectedCount == 1 && IsProtectedDesktopIcon(items_[itemIndex]);
    const bool namespaceItem = selectedCount == 1 &&
        (!items_[itemIndex].desktopIconClsid.empty() || protectedDesktopIcon);
    // Query supported verbs without displaying the native popup. Keep its site,
    // menu and COM object alive until a selected command has been invoked.
    snowdesktop::ShellContextMenuSite namespaceSite;
    ComPtr<IContextMenu> namespaceContext;
    struct NativeMenuOwner { HMENU value = nullptr; ~NativeMenuOwner() { if (value) DestroyMenu(value); } } namespaceNative;
    std::vector<std::pair<UINT, snowdesktop::namespace_menu_actions::Command>> namespaceActions;
    if (namespaceItem && desktopFolder_)
    {
        const HWND owner = ShellDialogOwnerHwnd();
        namespaceSite.Initialize(desktopFolder_.Get(), owner);
        PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(items_[itemIndex].childPidl.get());
        if (child && SUCCEEDED(desktopFolder_->GetUIObjectOf(namespaceSite.HostWindow() ? namespaceSite.HostWindow() : owner,
            1, &child, IID_IContextMenu, nullptr, reinterpret_cast<void**>(namespaceContext.GetAddressOf()))))
        {
            namespaceSite.Attach(namespaceContext.Get()); namespaceNative.value = CreatePopupMenu();
            if (SUCCEEDED(namespaceContext->QueryContextMenu(namespaceNative.value, 0, 1, 0x7fff, CMF_NORMAL | CMF_SYNCCASCADEMENU)))
            {
                const wchar_t* verbs[] = {L"manage", L"empty", L"connectNetworkDrive", L"disconnectNetworkDrive", L"properties"};
                for (size_t i = 0; i < std::size(verbs); ++i)
                    if (auto action = snowdesktop::namespace_menu_actions::Find(namespaceContext.Get(), namespaceNative.value, verbs[i]))
                        namespaceActions.emplace_back(i == 4 ? kContextPropertiesCommand : kContextNamespaceActionFirst + static_cast<UINT>(i), std::move(*action));
            }
        }
    }
    const auto namespaceProperty = std::find_if(namespaceActions.begin(), namespaceActions.end(), [](const auto& entry) { return entry.first == kContextPropertiesCommand; });
    const bool canShowProperties = (selectedCount == 1 && canFile && !itemPath.empty()) ||
        (namespaceProperty != namespaceActions.end() && namespaceProperty->second.enabled);
    const auto removalAction =
        snowdesktop::shell_item_action_rules::
            ResolveRemovalAction(
                static_cast<size_t>(selectedCount),
                selectedFileCount,
                selectedNamespaceCount,
                dockMapping,
                protectedDesktopIcon);
    const bool canRemove = removalAction !=
        snowdesktop::shell_item_action_rules::
            RemovalAction::Disabled;
    const bool hidesDesktopNamespace = removalAction ==
        snowdesktop::shell_item_action_rules::
            RemovalAction::HideDesktopNamespace;
    const bool canCloseDockApplication =
        dockApplicationItem &&
        GetDockWindowVisualState(
            static_cast<size_t>(itemIndex)) !=
            DockWindowVisualState::Closed;

    const auto canConvertFolder = [&](size_t index, const std::wstring& path) {
        if (index >= items_.size() || path.empty()) return false;
        const auto& item = items_[index];
        const bool pinnedDock = dockEntryIndex && *dockEntryIndex < dockEntries_.size() &&
            dockEntries_[*dockEntryIndex].type == DockEntryType::DesktopItem &&
            snowdesktop::folder_mapping_visibility::EqualInsensitive(
                dockEntries_[*dockEntryIndex].reference, item.layoutKey);
        const DWORD attributes = item.isShortcut ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
        const bool folderTarget = item.isShortcut ? item.shortcutTarget.directory :
            attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        return snowdesktop::folder_mapping_visibility::CanConvert(
            static_cast<size_t>(std::count_if(items_.begin(), items_.end(),
                [](const DesktopItem& candidate) { return candidate.selected; })),
            folderTarget, !item.desktopIconClsid.empty() || IsProtectedDesktopIcon(item),
            pinnedDock || (!dockFrequentItem && !dockApplicationItem && !dockMapping && !dockEntryIndex &&
                !keepQuickNavigationOpen && item.gridCell.pageId != kDockPageId &&
                !IsItemInAnyWidget(item)),
            snowdesktop::folder_mapping_visibility::HasMappingForItem(item, widgets_));
    };
    const bool canConvertFolderMapping = canFile &&
        canConvertFolder(static_cast<size_t>(itemIndex), itemPath);
    const std::wstring folderMappingSourceKey = items_[itemIndex].layoutKey;

    HMENU menu = CreatePopupMenu();
    HMENU detailsMenu = nullptr;
    const auto largeIconKey = items_[itemIndex].layoutKey;
    std::vector<std::wstring> largeIconKeys{largeIconKey};
    for (const auto& item : items_)
        if (item.selected && item.layoutKey != largeIconKey) largeIconKeys.push_back(item.layoutKey);
    const auto allLargeIcons = [&](auto predicate) {
        for (const auto& key : largeIconKeys)
        {
            const auto index = FindItemIndexByKey(key);
            if (index >= items_.size() || !items_[index].largeIcon || IsItemInAnyWidget(items_[index]) ||
                items_[index].gridCell.pageId == kDockPageId || !predicate(*items_[index].largeIcon)) return false;
        }
        return true;
    };
    const bool largeIconMenu = (selectedCount == 1 || allLargeIcons([](const auto&) { return true; })) && !dockFrequentItem && !dockApplicationItem &&
        !dockMapping && !dockEntryIndex && !keepQuickNavigationOpen &&
        !IsItemInAnyWidget(items_[itemIndex]) && items_[itemIndex].gridCell.pageId != kDockPageId;
    const auto entitlement = steamEntitlementService_
        ? steamEntitlementService_->Current() : snowdesktop::steam_entitlement::Snapshot{};
    using snowdesktop::large_icon_edit_rules::EntryAccess;
    const auto largeIconAccess = snowdesktop::large_icon_edit_rules::ResolveEntryAccess(
        entitlement.bridgeAvailable, entitlement.registered);
    if (largeIconMenu)
    {
        if (items_[itemIndex].largeIcon && largeIconAccess == EntryAccess::Edit)
        {
            namespace presets = snowdesktop::large_icon_preset_rules;
            const bool hasEdge = std::all_of(largeIconKeys.begin(), largeIconKeys.end(), [&](const auto& key) {
                const auto runtime = largeIconRuntime_.find(key);
                return runtime != largeIconRuntime_.end() && runtime->second.asset && runtime->second.asset->hasEdgeColor;
            });
            const bool editable = CanEditLargeIcons();
            HMENU settings = CreatePopupMenu();
            HMENU backgrounds = CreatePopupMenu(), effects = CreatePopupMenu();
            for (size_t i = 0; i < presets::backgrounds.size(); ++i)
            {
                const auto option = presets::backgrounds[i];
                if (!allLargeIcons([&](const auto& c) { return presets::BackgroundVisible(option.value, hasEdge, c); })) continue;
                const auto id = kContextLargeIconBackgroundFirst + static_cast<UINT>(i);
                AppendMenuW(backgrounds, MF_STRING | (editable ? 0 : MF_GRAYED) |
                    (allLargeIcons([&](const auto& c) { return presets::Background(c) == option.value; }) ? MF_CHECKED : 0), id, _LW(option.label));
            }
            for (size_t i = 0; i < presets::effects.size(); ++i)
            {
                const auto option = presets::effects[i];
                const bool enabled = editable && allLargeIcons([&](const auto& c) { return option.value != 2 || !snowdesktop::IsLargeIconFill(c); });
                AppendMenuW(effects, MF_STRING | (enabled ? 0 : MF_GRAYED) |
                    (allLargeIcons([&](const auto& c) { return presets::Effect(c) == option.value; }) ? MF_CHECKED : 0),
                    kContextLargeIconEffectFirst + static_cast<UINT>(i), _LW(option.label));
            }
            AppendMenuW(settings, MF_POPUP | (editable ? 0 : MF_GRAYED), reinterpret_cast<UINT_PTR>(backgrounds), _LW("largeIcon.backgroundSettings"));
            AppendMenuW(settings, MF_POPUP | (editable ? 0 : MF_GRAYED), reinterpret_cast<UINT_PTR>(effects), _LW("largeIcon.effectsSection"));
            AppendMenuW(settings, MF_STRING | (editable ? 0 : MF_GRAYED) | (allLargeIcons([](const auto& c) { return c.showOnHoverOnly; }) ? MF_CHECKED : 0), kContextLargeIconHoverOnly, _LW("app.interact.hover_only"));
            AppendMenuW(settings, MF_STRING | (editable ? 0 : MF_GRAYED) | (allLargeIcons([](const auto& c) { return c.keepWhenDesktopHidden; }) ? MF_CHECKED : 0), kContextLargeIconKeepWhenHidden, _LW("app.interact.keep_when_hidden"));
            SetMenuItemIcon(settings, kContextLargeIconHoverOnly, L"\uF06E");
            SetMenuItemIcon(settings, kContextLargeIconKeepWhenHidden, L"\uF108");
            AppendMenuW(settings, MF_STRING, kContextLargeIconSettings, _LW("largeIcon.detailedSettings"));
            SetMenuItemIcon(settings, reinterpret_cast<UINT_PTR>(backgrounds), L"\uF53F");
            SetMenuItemIcon(settings, reinterpret_cast<UINT_PTR>(effects), L"\uF0D0");
            SetMenuItemIcon(settings, kContextLargeIconSettings, L"\uF013");
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(settings), _LW("largeIcon.settings"));
            SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(settings), L"\uF013");
        }
        else if (largeIconAccess != EntryAccess::Hidden)
        {
            // A locked Steam-capable build opens the unlock settings directly.
            if (items_[itemIndex].largeIcon)
                AppendMenuW(menu, MF_STRING, kContextLargeIconSettings, _LW("largeIcon.settings"));
            else
                AppendMenuW(menu, MF_STRING, kContextLargeIconCreate, _LW("largeIcon.create"));
        }
        // Returning to ordinary icons must remain available without the Bridge.
        if (items_[itemIndex].largeIcon)
            AppendMenuW(menu, MF_STRING, kContextLargeIconRestore, _LW("largeIcon.restore"));
        if (largeIconAccess != EntryAccess::Hidden || items_[itemIndex].largeIcon)
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, canOpen ? MF_STRING : MF_STRING | MF_GRAYED,
        kContextOpenCommand, _LW("app.menu.open"));
    if (canConvertFolderMapping)
        AppendMenuW(menu, MF_STRING, kContextAddFolderMappingWidget,
            _LW("app.menu.convert_folder_mapping"));
    if (!namespaceItem)
    {
        AppendMenuW(menu, canCopyPath ? MF_STRING : MF_STRING | MF_GRAYED,
            kContextCopyPathCommand, _LW("app.menu.copy_path"));
        AppendMenuW(menu, canReveal ? MF_STRING : MF_STRING | MF_GRAYED,
            kContextRevealLocationCommand, _LW("app.menu.open_file_location"));
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (!namespaceItem)
    {
        AppendMenuW(menu,
            canRunAsAdministrator ? MF_STRING : MF_STRING | MF_GRAYED,
            kContextRunAsAdministratorCommand,
            _LW("app.menu.run_as_administrator"));
    }
    AppendMenuW(menu,
        canShowProperties ? MF_STRING : MF_STRING | MF_GRAYED,
        kContextPropertiesCommand, _LW("app.menu.properties"));
    if (selectedCount == 1 && canFile)
        AppendWebsiteIconMenu(menu, itemPath);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (!namespaceItem)
    {
        AppendMenuW(menu,
            selectedCount == 1 && canFile && !dockMapping
                ? MF_STRING : MF_STRING | MF_GRAYED,
            kContextRenameCommand, _LW("app.menu.rename"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu,
            canFile && !dockMapping ? MF_STRING : MF_STRING | MF_GRAYED,
            kContextCutCommand, _LW("app.menu.cut"));
        AppendMenuW(menu,
            canFile && !dockMapping ? MF_STRING : MF_STRING | MF_GRAYED,
            kContextCopyCommand, _LW("app.menu.copy"));
    }
    if (!protectedDesktopIcon || dockMapping)
        AppendMenuW(menu,
            canRemove ? MF_STRING : MF_STRING | MF_GRAYED,
            kContextDeleteCommand,
            dockMapping
                ? _LW("app.dock.remove_mapping")
                : hidesDesktopNamespace
                ? _LW("app.menu.hide_desktop_icon")
                : _LW("app.settings.delete"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kContextMoreCommand, _LW("app.menu.more_options"));
    if (dockFolderEntry)
    {
        std::wstring fanLabel = _LW("app.interact.popup_fan");
        fanLabel += L"\t";
        fanLabel += dockFolderEntry->fanPopup ? _LW("app.interact.on") : _LW("app.interact.off");
        AppendMenuW(menu, MF_STRING, kContextPopupFan, fanLabel.c_str());
        auto optionLabel = [](const wchar_t* title, bool enabled) {
            return std::wstring(title) + L"\t" + (enabled ? _LW("app.interact.on") : _LW("app.interact.off"));
        };
        const auto searchLabel = optionLabel(_LW("app.interact.search_box"), dockFolderEntry->showSearchBox);
        const auto categoriesLabel = optionLabel(_LW("app.interact.file_categories"), dockFolderEntry->showFileCategories);
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleSearchBox, searchLabel.c_str());
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleFileCategories, categoriesLabel.c_str());
        const auto statusLabel = [](
            const wchar_t* title,
            const wchar_t* status) {
            std::wstring label = title;
            label += L"\t";
            label += status;
            return label;
        };
        const std::wstring displayTypeLabel =
            statusLabel(
                _LW("app.interact.display_type"),
                dockFolderEntry->listMode
                    ? _LW("app.interact.list_view_state")
                    : _LW("app.interact.icon_view_state"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(
            menu, MF_STRING,
            kContextWidgetToggleListMode,
            displayTypeLabel.c_str());

        detailsMenu = CreatePopupMenu();
        if (detailsMenu)
        {
            AppendMenuW(
                detailsMenu,
                MF_STRING | MF_CHECKED | MF_GRAYED,
                kContextWidgetDetailName,
                _LW("widget.details.name"));
            AppendMenuW(
                detailsMenu,
                MF_STRING |
                    (dockFolderEntry->detailShowModified
                        ? MF_CHECKED : 0),
                kContextWidgetDetailModified,
                _LW("widget.details.modified"));
            AppendMenuW(
                detailsMenu,
                MF_STRING |
                    (dockFolderEntry->detailShowType
                        ? MF_CHECKED : 0),
                kContextWidgetDetailType,
                _LW("widget.details.type"));
            AppendMenuW(
                detailsMenu,
                MF_STRING |
                    (dockFolderEntry->detailShowSize
                        ? MF_CHECKED : 0),
                kContextWidgetDetailSize,
                _LW("widget.details.size"));
            AppendMenuW(
                menu,
                MF_POPUP |
                    (dockFolderEntry->listMode
                        ? 0 : MF_GRAYED),
                reinterpret_cast<UINT_PTR>(
                    detailsMenu),
                _LW("app.interact.show_details"));
        }
    }
    if (dockFrequentItem)
    {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kContextDockRemoveFrequentItem,
            _LW("app.dock.remove_frequent"));
    }
    if (canCloseDockApplication)
    {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(
            menu, MF_STRING,
            kContextDockCloseApplication,
            _LW("app.dock.close_application"));
    }

    for (const auto& [id, action] : namespaceActions)
    {
        if (id == kContextPropertiesCommand) continue;
        // Keep commonly used system actions next to Open, before generic file commands.
        MENUITEMINFOW info{sizeof(info)}; info.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE;
        info.wID = id; info.dwTypeData = const_cast<wchar_t*>(action.label.c_str());
        info.fState = action.enabled ? MFS_ENABLED : MFS_GRAYED;
        InsertMenuItemW(menu, kContextPropertiesCommand, FALSE, &info);
        SetMenuItemIcon(menu, id, id == kContextNamespaceActionFirst + 1 ? L"\uF2ED" : id == kContextNamespaceActionFirst ? L"\uF085" : L"\uF6FF");
    }
    if (namespaceItem && namespaceProperty == namespaceActions.end()) DeleteMenu(menu, kContextPropertiesCommand, MF_BYCOMMAND);
    SetMenuItemIcon(menu, kContextOpenCommand, L"");
    if (canConvertFolderMapping)
        SetMenuItemIcon(menu, kContextAddFolderMappingWidget,
            snowdesktop::menu_fluent_glyphs::kFolderMapping, MenuIconFont::FluentRegular);
    SetMenuItemIcon(menu, kContextLargeIconCreate, L"\uF0B2");
    SetMenuItemIcon(menu, kContextLargeIconSettings, L"\uF013");
    SetMenuItemIcon(menu, kContextLargeIconRestore,
        snowdesktop::menu_fluent_glyphs::kCompactGrid, MenuIconFont::FluentRegular);
    SetMenuItemIcon(menu, kContextRevealLocationCommand, L"");
    SetMenuItemIcon(menu, kContextCopyPathCommand,
        snowdesktop::menu_fluent_glyphs::kCopy,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(menu, kContextRunAsAdministratorCommand,
        snowdesktop::menu_fluent_glyphs::kShield,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(menu, kContextPropertiesCommand,
        snowdesktop::menu_fluent_glyphs::kInfo,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(menu, kContextRenameCommand, L"");
    SetMenuItemIcon(menu, kContextCutCommand, L"");
    SetMenuItemIcon(menu, kContextCopyCommand, L"");
    SetMenuItemIcon(menu, kContextDeleteCommand, L"");
    SetMenuItemQuickAction(menu, kContextRenameCommand);
    SetMenuItemQuickAction(menu, kContextCutCommand);
    SetMenuItemQuickAction(menu, kContextCopyCommand);
    SetMenuItemQuickAction(menu, kContextDeleteCommand);
    SetMenuItemIcon(menu, kContextPopupFan,
        snowdesktop::menu_fluent_glyphs::kFanExpansion, MenuIconFont::FluentRegular);
    SetMenuItemIcon(menu, kContextMoreCommand,
        snowdesktop::menu_fluent_glyphs::kMoreOptions,
        MenuIconFont::FluentRegular);
    if (dockFolderEntry)
    {
        SetMenuItemIcon(menu, kContextWidgetToggleFileCategories,
            snowdesktop::menu_fluent_glyphs::kCategoryBar, MenuIconFont::FluentRegular);
        SetMenuItemIcon(menu, kContextWidgetToggleSearchBox, L"\uF68F", MenuIconFont::FluentRegular);
        SetMenuItemIcon(
            menu,
            kContextWidgetToggleListMode,
            snowdesktop::menu_fluent_glyphs::
                kContentLayout,
            MenuIconFont::FluentRegular);
        if (detailsMenu)
        {
            SetMenuItemIcon(
                menu,
                reinterpret_cast<UINT_PTR>(
                    detailsMenu),
                L"\uF168",
                MenuIconFont::FluentRegular);
        }
    }
    if (dockFrequentItem)
        SetMenuItemIcon(menu, kContextDockRemoveFrequentItem, L"");
    if (canCloseDockApplication)
        SetMenuItemIcon(
            menu, kContextDockCloseApplication,
            L"");

    HWND menuOwner = keepQuickNavigationOpen &&
        quickNavigationHwnd_ &&
        IsWindow(quickNavigationHwnd_)
        ? quickNavigationHwnd_
        : hwnd_;
    SetForegroundWindow(menuOwner);
    const bool placeOutsideDock = dockRenameAnchor.has_value() ||
        dockFrequentItem || dockApplicationItem;
    namespace presets = snowdesktop::large_icon_preset_rules;
    const auto applyLargeIconCommand = [&](UINT command) {

        const bool backgroundCommand = command >= kContextLargeIconBackgroundFirst && command < kContextLargeIconBackgroundFirst + presets::backgrounds.size();
        const bool effectCommand = command >= kContextLargeIconEffectFirst && command < kContextLargeIconEffectFirst + presets::effects.size();
        const bool visibilityCommand = command == kContextLargeIconHoverOnly || command == kContextLargeIconKeepWhenHidden;
        if (largeIconMenu && (backgroundCommand || effectCommand || visibilityCommand))
        {
            // The menu runs a nested loop: resolve identity and entitlement again at commit time.
            if (!CanEditLargeIcons()) { OpenLargeIconSettings(largeIconKeys); return; }
            std::vector<std::pair<size_t, std::optional<snowdesktop::LargeIconConfig>>> changes;
            const bool showOnHover = !allLargeIcons([](const auto& c) { return c.showOnHoverOnly; });
            const bool keepWhenHidden = !allLargeIcons([](const auto& c) { return c.keepWhenDesktopHidden; });
            for (const auto& key : largeIconKeys)
            {
                const auto index = FindItemIndexByKey(key);
                if (index >= items_.size() || !items_[index].largeIcon) return;
                auto config = *items_[index].largeIcon;
                const auto runtime = largeIconRuntime_.find(key);
                const auto asset = runtime != largeIconRuntime_.end() ? runtime->second.asset : nullptr;
                if (visibilityCommand && CanEditLargeIcons())
                {
                    if (command == kContextLargeIconHoverOnly) config.showOnHoverOnly = showOnHover;
                    else config.keepWhenDesktopHidden = keepWhenHidden;
                }
                const bool changed = visibilityCommand ? CanEditLargeIcons() : backgroundCommand ? presets::ApplyBackground(config,
                    presets::backgrounds[command - kContextLargeIconBackgroundFirst].value, CanEditLargeIcons(),
                    asset && asset->hasEdgeColor, asset ? asset->accent : 0, asset ? asset->edgeColor : 0) :
                    presets::ApplyEffect(config, static_cast<int>(command - kContextLargeIconEffectFirst), CanEditLargeIcons());
                if (!changed) return;
                changes.emplace_back(index, std::move(config));
            }
            if (!SetLargeIconConfigs(changes))
                MessageBoxW(hwnd_, _LW("largeIcon.saveFailed"), _LW("largeIcon.settings"), MB_OK | MB_ICONWARNING);
            else if (backgroundCommand && presets::backgrounds[command - kContextLargeIconBackgroundFirst].value == kAppearancePresetCustom)
                OpenLargeIconSettings(largeIconKeys);
        }
    };
    const auto changeLargeIcon = [&](UINT command, auto& currentItems) {
        const bool background = command >= kContextLargeIconBackgroundFirst && command < kContextLargeIconBackgroundFirst + presets::backgrounds.size();
        const bool effect = command >= kContextLargeIconEffectFirst && command < kContextLargeIconEffectFirst + presets::effects.size();
        const bool visibility = command == kContextLargeIconHoverOnly || command == kContextLargeIconKeepWhenHidden;
        if (!largeIconMenu || (!background && !effect && !visibility)) return false;
        // Custom opens a separate editor after the menu has relinquished focus.
        if (background && presets::backgrounds[command - kContextLargeIconBackgroundFirst].value == kAppearancePresetCustom) return false;
        applyLargeIconCommand(command);
        UpdateLargeIconHover();
        const auto index = FindItemIndexByKey(largeIconKey);
        if (index >= items_.size() || !items_[index].largeIcon) { snowdesktop::modern_menu::DismissActive(); return true; }
        const bool editable = CanEditLargeIcons();
        snowdesktop::modern_menu::VisitItems(currentItems, [&](auto& item) {
            if (item.command == kContextLargeIconHoverOnly) { item.checked = allLargeIcons([](const auto& c) { return c.showOnHoverOnly; }); item.enabled = editable; }
            if (item.command == kContextLargeIconKeepWhenHidden) { item.checked = allLargeIcons([](const auto& c) { return c.keepWhenDesktopHidden; }); item.enabled = editable; }
            if (item.command >= kContextLargeIconBackgroundFirst && item.command < kContextLargeIconBackgroundFirst + presets::backgrounds.size())
            {
                item.checked = allLargeIcons([&](const auto& c) { return presets::Background(c) == presets::backgrounds[item.command - kContextLargeIconBackgroundFirst].value; });
                item.enabled = editable;
            }
            if (item.command >= kContextLargeIconEffectFirst && item.command < kContextLargeIconEffectFirst + presets::effects.size())
            {
                const int value = static_cast<int>(item.command - kContextLargeIconEffectFirst);
                item.checked = allLargeIcons([&](const auto& c) { return presets::Effect(c) == value; });
                item.enabled = editable && allLargeIcons([&](const auto& c) { return value != 2 || !snowdesktop::IsLargeIconFill(c); });
            }
        });
        return true;
    };
    snowdesktop::shell_extensions::Request shellRequest;
    if (canCopyPath) shellRequest.paths = selectedFilePaths;
    shellRequest.extended = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    UINT command = 0;
    {
        LargeIconMenuScope titleScope(*this,
            largeIconMenu && items_[itemIndex].largeIcon ? largeIconKey : std::wstring{});
        command = ShowModernMenu(
            menu, screenPoint, menuOwner, placeOutsideDock, false, nullptr, changeLargeIcon, {}, {}, &shellRequest);
    }
    DestroyMenu(menu);
    ClearMenuIcons();
    bool inlineEditorStarted = false;

    const auto applyDockFolderDisplayChange = [&]() {
        if (!dockFolderEntry || !dockEntryIndex)
            return;
        SaveLayoutSlots();
        const std::wstring sourceId =
            std::to_wstring(static_cast<int>(
                dockFolderEntry->type)) +
            L":" + ToUpperInvariant(
                dockFolderEntry->reference);
        if (!dockFolderPopupOpen_ ||
            dockFolderPopupSourceId_ != sourceId)
            return;

        if (command == kContextPopupFan)
        {
            ResetCollectionPopupFanScroll();
            popupFanShowAll_ = false;
            popupFanActionFocused_ = false;
        }
        if (dockFolderPopupWidget_.fanPopup != dockFolderEntry->fanPopup)
        {
            ResetCollectionPopupAnimationCache();
            popupAnimation_.ShowImmediately();
            popupAnimation_.Configure(
                snowdesktop::animation::RuntimePopupEffect() == snowdesktop::animation::Fade,
                snowdesktop::animation::RuntimeDurationScale() * (dockFolderEntry->fanPopup ? 2.4 : 1.0));
        }
        dockFolderPopupWidget_.listMode =
            dockFolderEntry->listMode;
        dockFolderPopupWidget_.fanPopup = dockFolderEntry->fanPopup;
        dockFolderPopupWidget_.showSearchBox = dockFolderEntry->showSearchBox;
        dockFolderPopupWidget_.showFileCategories = dockFolderEntry->showFileCategories;
        dockFolderPopupWidget_.detailShowModified =
            dockFolderEntry->detailShowModified;
        dockFolderPopupWidget_.detailShowType =
            dockFolderEntry->detailShowType;
        dockFolderPopupWidget_.detailShowSize =
            dockFolderEntry->detailShowSize;
        dockFolderPopupWidget_.detailModifiedPosition =
            dockFolderEntry->detailModifiedPosition;
        dockFolderPopupWidget_.detailTypePosition =
            dockFolderEntry->detailTypePosition;
        dockFolderPopupWidget_.detailSizePosition =
            dockFolderEntry->detailSizePosition;
        dockFolderPopupWidget_.showDetails =
            snowdesktop::list_detail_rules::
                HasMetadataColumns(
                    dockFolderEntry->detailShowModified,
                    dockFolderEntry->detailShowType,
                    dockFolderEntry->detailShowSize);
        popupScrollOffset_ = 0;
        if (dockFolderPopupContainer_)
        {
            if (!dockFolderEntry->showSearchBox) dockFolderPopupContainer_->ClearSearchText();
            if (!dockFolderEntry->showFileCategories) dockFolderPopupContainer_->EndCategoryTabDrag(false);
            dockFolderPopupContainer_->InvalidateFilterCache();
        }
        InvalidateCollectionPopupContent();
        RefreshDockFolderPopupGeometry();
    };

    const auto nativeAction = std::find_if(namespaceActions.begin(), namespaceActions.end(), [command](const auto& entry) { return entry.first == command; });
    if (namespaceContext && nativeAction != namespaceActions.end())
    {
        if (nativeAction->second.enabled)
        {
            RestoreDesktopWindowLayer();
            CMINVOKECOMMANDINFOEX invoke{sizeof(invoke)};
            invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
            invoke.hwnd = ShellDialogOwnerHwnd(); invoke.nShow = SW_SHOWNORMAL; invoke.ptInvoke = screenPoint;
            invoke.lpVerb = MAKEINTRESOURCEA(nativeAction->second.offset);
            invoke.lpVerbW = MAKEINTRESOURCEW(nativeAction->second.offset);
            const auto directory = snowdesktop::DesktopShellInvocationDirectory(); std::string ansiDirectory;
            snowdesktop::SetShellInvocationDirectory(invoke, directory, ansiDirectory);
            // No no-confirmation flags: Windows owns its usual confirmation/UAC UI.
            InvokeShellMenuCommand(namespaceContext.Get(), invoke, &namespaceSite);
            RequestShellRefresh();
        }
        return;
    }
    applyLargeIconCommand(command);
    switch (command)
    {
    case kContextAddFolderMappingWidget:
    {
        // The menu pumps messages. Re-resolve the source identity rather than
        // trusting an index retained across Shell notifications or another menu.
        const size_t sourceIndex = FindItemIndexByKey(folderMappingSourceKey);
        if (!canConvertFolderMapping || !canConvertFolder(sourceIndex, itemPath))
            break;
        const std::wstring sourceStamp = snowdesktop::shell_icon_request::Stamp(items_[sourceIndex]);
        const std::optional<DockEntry> pinnedEntry = dockEntryIndex && *dockEntryIndex < dockEntries_.size()
            ? std::optional<DockEntry>(dockEntries_[*dockEntryIndex]) : std::nullopt;
        // Resolve .lnk targets and verify filesystem availability on the Shell
        // worker. A slow provider must not block the desktop input thread.
        const bool submitted = shellVisualWork_.Submit(L"folder-convert:" + folderMappingSourceKey,
            [itemPath] { return snowdesktop::item_location::ResolveFolderTarget(itemPath); },
            [this, key = folderMappingSourceKey, itemPath, sourceStamp, pinnedEntry, screenPoint](
                snowdesktop::item_location::FolderTarget target) {
                const size_t index = FindItemIndexByKey(key);
                if (!hwnd_ || exitRequested_ || index >= items_.size() ||
                    !snowdesktop::folder_mapping_visibility::EqualInsensitive(items_[index].parsingName, itemPath) ||
                    snowdesktop::shell_icon_request::Stamp(items_[index]) != sourceStamp ||
                    snowdesktop::folder_mapping_visibility::HasMappingForItem(items_[index], widgets_)) return;
                if (!target) { MessageBeep(MB_ICONWARNING); return; }
                DesktopWidget widget;
                widget.id = MakeNewWidgetId();
                widget.type = DesktopWidgetType::FolderMapping;
                widget.title = items_[index].name;
                widget.showTitle = true;
                widget.sourceFolderPath = target.path;
                widget.sourceDesktopItemKey = key;
                const std::wstring createdId = widget.id;
                if (pinnedEntry)
                {
                    auto entry = std::find_if(dockEntries_.begin(), dockEntries_.end(), [&](const DockEntry& current) {
                        return current.type == pinnedEntry->type && current.keepOnDesktop == pinnedEntry->keepOnDesktop &&
                            snowdesktop::folder_mapping_visibility::EqualInsensitive(current.reference, pinnedEntry->reference);
                    });
                    if (entry == dockEntries_.end()) return;
                    DockEntry replacement = *entry;
                    if (!snowdesktop::folder_mapping_visibility::ReplaceDockEntry(replacement, widget)) return;
                    ConfigureWidgetGridLimits(widget);
                    widget.gridSpan = {3, 3};
                    widgets_.push_back(std::move(widget));
                    *entry = std::move(replacement);
                    // Retire the direct-folder popup before rebuilding its source.
                    if (dockFolderPopupOpen_)
                    {
                        CloseCollectionPopup();
                        FinalizeCloseCollectionPopup();
                    }
                    EnsureNavTabOrder();
                    LayoutItems();
                    SaveLayoutSlots();
                    InvalidateDockShellMetadata();
                    InvalidateDockContainers();
                    InvalidateDragStaticScene();
                    InvalidateDockRects();
                }
                else
                {
                    if (IsItemInAnyWidget(items_[index]) || items_[index].gridCell.pageId == kDockPageId) return;
                    lastContextMenuScreenPoint_ = screenPoint;
                    AddWidgetToGrid(std::move(widget), {3, 3});
                }
                const size_t createdIndex = FindWidgetIndexById(createdId);
                if (createdIndex < widgets_.size())
                {
                    EnumerateFolderMappingEntries(widgets_[createdIndex]);
                    ClearWidgetAddedHint();
                    ShowWidgetAddedHint();
                }
            }, hwnd_, kBackgroundShellReadyMessage);
        if (!submitted) MessageBeep(MB_ICONWARNING);
        break;
    }
    case kContextLargeIconCreate:
        if (largeIconMenu && largeIconAccess != EntryAccess::Hidden)
        {
            if (!CanEditLargeIcons()) { OpenLargeIconSettings(itemIndex); break; }
            auto config = MakeLargeIconDefaults(itemIndex);
            const auto& item = items_[itemIndex];
            const auto* page = FindGridPage(gridPages_, item.gridCell.pageId);
            std::unordered_set<std::wstring> occupied;
            for (size_t i = 0; i < items_.size(); ++i)
                if (i != itemIndex && !IsItemInAnyWidget(items_[i])) MarkGridArea(occupied, items_[i].gridCell, items_[i].gridSpan);
            for (const auto& widget : widgets_) if (!IsGroupedWidget(widget)) MarkGridArea(occupied, widget.gridCell, widget.gridSpan);
            const GridSpan span{config.columns, config.rows};
            const bool fits = page && item.gridCell.column + span.columns <= page->columns &&
                item.gridCell.row + span.rows <= page->rows && !AreGridSlotsMarked(occupied, item.gridCell, span);
            if (!fits) BeginLargeIconPlacement(itemIndex, config);
            else if (!SetLargeIconConfig(itemIndex, config))
                MessageBoxW(hwnd_, _LW("largeIcon.saveFailed"), _LW("largeIcon.settings"), MB_OK | MB_ICONWARNING);
        }
        break;
    case kContextLargeIconSettings:
        if (largeIconMenu) OpenLargeIconSettings(largeIconKeys);
        break;
    case kContextLargeIconRestore:
        if (largeIconMenu)
        {
            std::vector<std::pair<size_t, std::optional<snowdesktop::LargeIconConfig>>> changes;
            for (const auto& key : largeIconKeys) changes.emplace_back(FindItemIndexByKey(key), std::nullopt);
            if (!SetLargeIconConfigs(changes)) MessageBoxW(hwnd_, _LW("largeIcon.saveFailed"), _LW("largeIcon.settings"), MB_OK | MB_ICONWARNING);
        }
        break;
    case kContextOpenCommand:
    {
        if (!canOpen)
            break;
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (items_[i].selected)
                LaunchDesktopItem(i);
        }
        break;
    }
    case kContextRevealLocationCommand:
        snowdesktop::item_location::Reveal(hwnd_, itemPath);
        break;
    case kContextCopyPathCommand:
        CopyPathsToClipboard(selectedFilePaths);
        break;
    case kContextRunAsAdministratorCommand:
        if (canRunAsAdministrator)
        {
            if (keepQuickNavigationOpen)
            {
                CloseQuickNavigationThen(
                    [this, itemPath]() {
                        RunPathAsAdministratorAfterMenu(
                            itemPath);
                    });
            }
            else
            {
                RunPathAsAdministratorAfterMenu(itemPath);
            }
        }
        break;
    case kContextPropertiesCommand:
        if (canShowProperties)
            ShowPathProperties(itemPath);
        break;
    case kContextFetchWebsiteIconCommand:
        if (selectedCount == 1 && canFile) FetchWebsiteIcon(itemPath);
        break;
    case kContextPopupFan:
        if (dockFolderEntry)
        {
            dockFolderEntry->fanPopup = !dockFolderEntry->fanPopup;
            applyDockFolderDisplayChange();
        }
        break;
    case kContextWidgetToggleListMode:
        if (dockFolderEntry)
        {
            dockFolderEntry->listMode =
                !dockFolderEntry->listMode;
            applyDockFolderDisplayChange();
        }
        break;
    case kContextWidgetToggleSearchBox:
    case kContextWidgetToggleFileCategories:
        if (dockFolderEntry)
        {
            if (command == kContextWidgetToggleSearchBox) dockFolderEntry->showSearchBox = !dockFolderEntry->showSearchBox;
            else dockFolderEntry->showFileCategories = !dockFolderEntry->showFileCategories;
            applyDockFolderDisplayChange();
        }
        break;
    case kContextWidgetDetailModified:
    case kContextWidgetDetailType:
    case kContextWidgetDetailSize:
        if (dockFolderEntry &&
            dockFolderEntry->listMode)
        {
            if (command ==
                    kContextWidgetDetailModified)
            {
                dockFolderEntry->detailShowModified =
                    !dockFolderEntry->
                        detailShowModified;
            }
            else if (command ==
                    kContextWidgetDetailType)
            {
                dockFolderEntry->detailShowType =
                    !dockFolderEntry->detailShowType;
            }
            else
            {
                dockFolderEntry->detailShowSize =
                    !dockFolderEntry->detailShowSize;
            }
            const auto positions =
                snowdesktop::list_detail_rules::
                    NormalizePositions(
                        dockFolderEntry->
                            detailShowModified,
                        dockFolderEntry->
                            detailShowType,
                        dockFolderEntry->
                            detailShowSize,
                        {
                            dockFolderEntry->
                                detailModifiedPosition,
                            dockFolderEntry->
                                detailTypePosition,
                            dockFolderEntry->
                                detailSizePosition,
                        });
            dockFolderEntry->detailModifiedPosition =
                positions.modified;
            dockFolderEntry->detailTypePosition =
                positions.type;
            dockFolderEntry->detailSizePosition =
                positions.size;
            applyDockFolderDisplayChange();
        }
        break;
    case kContextRenameCommand:
        if (keepQuickNavigationOpen)
            BeginQuickNavigationDesktopItemRename(
                static_cast<size_t>(itemIndex));
        else
            BeginRenameSelected(dockRenameAnchor);
        inlineEditorStarted = renameController_.IsActive();
        break;
    case kContextCutCommand:
    case kContextCopyCommand:
    {
        cutPaths_.clear();

        std::vector<PCUITEMID_CHILD> pidls;
        std::vector<size_t> selectedIndexes;
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (!items_[i].selected || !items_[i].desktopIconClsid.empty()) continue;
            pidls.push_back(reinterpret_cast<PCUITEMID_CHILD>(items_[i].childPidl.get()));
            selectedIndexes.push_back(i);
        }

        if (!pidls.empty())
        {
            ComPtr<IDataObject> dataObj;
            if (SUCCEEDED(desktopFolder_->GetUIObjectOf(hwnd_, static_cast<UINT>(pidls.size()),
                pidls.data(), IID_IDataObject, nullptr,
                reinterpret_cast<void**>(dataObj.GetAddressOf()))) && dataObj)
            {
                if (command == kContextCutCommand)
                {
            CLIPFORMAT cfPreferred = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
                    FORMATETC fmt{ cfPreferred, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
                    STGMEDIUM med{};
                    med.tymed = TYMED_HGLOBAL;
                    med.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
                    if (med.hGlobal)
                    {
                        *static_cast<DWORD*>(GlobalLock(med.hGlobal)) = DROPEFFECT_MOVE;
                        GlobalUnlock(med.hGlobal);
                        dataObj->SetData(&fmt, &med, TRUE);
                    }
                }

                OleSetClipboard(dataObj.Get());
                OleFlushClipboard();
            }
        }

        if (command == kContextCutCommand)
        {
            for (size_t idx : selectedIndexes)
            {
                wchar_t path[MAX_PATH]{};
                if (SHGetPathFromIDListW(items_[idx].absolutePidl.get(), path))
                    cutPaths_.insert(path);
            }
        }

        UpdateCutState();
        InvalidateRect(hwnd_, nullptr, FALSE);
        break;
    }
    case kContextDeleteCommand:
    {
        if (removalAction ==
                snowdesktop::shell_item_action_rules::
                    RemovalAction::RemoveDockMapping &&
            RemoveDockMappingAt(
                *dockMappingEntryIndex))
        {
            break;
        }
        if (removalAction ==
            snowdesktop::shell_item_action_rules::
                RemovalAction::HideDesktopNamespace)
        {
            const std::wstring clsid = ToUpperInvariant(
                items_[static_cast<size_t>(itemIndex)].
                    desktopIconClsid);
            if (!clsid.empty() &&
                snowdesktop::shell_item_visibility::CommitDesktopIconVisibility(
                    clsid, false, settingsIconVisibility_, WriteDesktopIconRegistryValue))
            {
                ReloadItems();
            }
            break;
        }
        if (removalAction !=
            snowdesktop::shell_item_action_rules::
                RemovalAction::DeleteFiles)
        {
            break;
        }
        cutPaths_.clear();
        std::vector<std::wstring> deletePaths;
        for (const auto& item : items_)
        {
            if (!item.selected || !item.desktopIconClsid.empty()) continue;
            wchar_t path[MAX_PATH]{};
            if (SHGetPathFromIDListW(item.absolutePidl.get(), path))
            {
                cutPaths_.erase(path);
                deletePaths.push_back(path);
            }
        }
        if (!deletePaths.empty())
        {
            std::vector<snowdesktop::ShellFileOperationStep> steps;
            steps.push_back({
                FO_DELETE,
                std::move(deletePaths),
                {},
                static_cast<FILEOP_FLAGS>(
                    FOF_ALLOWUNDO |
                    FOF_NOCONFIRMATION) });
            QueueShellFileOperation(
                std::move(steps),
                [this](bool succeeded) {
                    if (succeeded)
                        RequestShellRefresh();
                });
        }
        break;
    }
    case kContextMoreCommand:
        ShowShellContextMenu(
            screenPoint, itemIndex,
            keepQuickNavigationOpen,
            dockRenameAnchor,
            dockMapping
                ? dockMappingEntryIndex
                : std::nullopt);
        break;
    case kContextDockRemoveFrequentItem:
    {
        const DesktopItem& item = items_[static_cast<size_t>(itemIndex)];
        const std::wstring key = ToUpperInvariant(
            item.layoutKey.empty() ? item.parsingName : item.layoutKey);
        if (!key.empty() && dockUsageStats_.erase(key) > 0)
        {
            SaveDockUsageStats();
            InvalidateDockContainers();
            InvalidateDragStaticScene();
        }
        ClearSelection();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    }
    case kContextDockCloseApplication:
        CloseDockApplicationWindows(
            ResolveDockAppIdentity(
                static_cast<size_t>(
                    itemIndex)));
        break;
    }
    RestoreDesktopWindowLayer();
    if (command != kContextMoreCommand &&
        snowdesktop::right_click_contract::
            ShouldRestoreInteractionFocusAfterMenu(
                keepQuickNavigationOpen,
                inlineEditorStarted))
        RestoreInteractionInputFocus();
}

/**
 * @brief 调用 Windows Shell 的 IContextMenu 显示系统右键菜单。
 *        收集所有选中项的 PIDL，通过 desktopFolder_->GetUIObjectOf
 *        获取 IContextMenu 接口，显示 Shell 提供的标准右键菜单。
 * @param screenPoint 菜单弹出的屏幕坐标。
 * @param itemIndex   当前右键点击的项索引（用于确定 PIDL 列表的锚点）。
 */
void DesktopApp::ShowShellContextMenu(
    POINT screenPoint, int itemIndex,
    bool keepQuickNavigationOpen,
    std::optional<RECT> dockRenameAnchor,
    std::optional<size_t> dockMappingEntryIndex)
{
    std::vector<LPCITEMIDLIST> pidls;
    std::vector<std::wstring> invocationPaths;
    const auto collectPath = [&invocationPaths](const DesktopItem& item) {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetNameFromIDList(item.absolutePidl.get(),
                SIGDN_FILESYSPATH, &path)) && path)
            invocationPaths.emplace_back(path);
        else
            invocationPaths.emplace_back();
        CoTaskMemFree(path);
    };
    if (itemIndex >= 0 && static_cast<size_t>(itemIndex) < items_.size())
    {
        for (const auto& item : items_)
            if (item.selected)
            {
                pidls.push_back(reinterpret_cast<LPCITEMIDLIST>(item.childPidl.get()));
                collectPath(item);
            }
        if (pidls.empty())
        {
            pidls.push_back(reinterpret_cast<LPCITEMIDLIST>(items_[itemIndex].childPidl.get()));
            collectPath(items_[itemIndex]);
        }
    }
    if (pidls.empty()) return;

    ShellPopupMenuLayerGuard shellMenuLayer(*this);
    const HWND menuOwner = ShellDialogOwnerHwnd();
    snowdesktop::ShellContextMenuSite menuSite;
    menuSite.Initialize(desktopFolder_.Get(), menuOwner);
    HWND shellOwner = menuSite.HostWindow()
        ? menuSite.HostWindow() : menuOwner;
    ComPtr<IContextMenu> ctxMenu;
    if (FAILED(desktopFolder_->GetUIObjectOf(shellOwner, static_cast<UINT>(pidls.size()), pidls.data(),
        IID_IContextMenu, nullptr, reinterpret_cast<void**>(ctxMenu.GetAddressOf()))) || !ctxMenu)
        return;
    menuSite.Attach(ctxMenu.Get());

    HMENU menu = CreatePopupMenu();
    constexpr UINT kFirstCmd = 1;
    constexpr UINT kLastCmd = 0x7FFF;
    if (FAILED(ctxMenu->QueryContextMenu(menu, 0, kFirstCmd, kLastCmd,
            CMF_NORMAL | CMF_CANRENAME | CMF_SYNCCASCADEMENU)))
        { DestroyMenu(menu); return; }

    ctxMenu.As(&activeContextMenu2_);
    ctxMenu.As(&activeContextMenu3_);

    if (keepQuickNavigationOpen)
        SetQuickNavigationTopmost(false);
    SetForegroundWindow(menuOwner);
    UINT cmd = 0;
    {
        const bool desktopLargeIcon = itemIndex >= 0 && static_cast<size_t>(itemIndex) < items_.size() &&
            items_[itemIndex].largeIcon && !keepQuickNavigationOpen && !dockRenameAnchor && !dockMappingEntryIndex &&
            !IsItemInAnyWidget(items_[itemIndex]) && items_[itemIndex].gridCell.pageId != kDockPageId;
        LargeIconMenuScope titleScope(*this, desktopLargeIcon ? items_[itemIndex].layoutKey : std::wstring{});
        cmd = TrackShellPopupMenuWithDesktopPump(
            menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
            screenPoint, menuOwner);
    }
    if (keepQuickNavigationOpen)
        SetQuickNavigationTopmost(true);

    activeContextMenu2_.Reset();
    activeContextMenu3_.Reset();

    if (cmd != 0)
    {
        UINT commandOffset = cmd - kFirstCmd;
        wchar_t menuText[128]{};
        bool renameCommand = IsShellRenameCommand(ctxMenu.Get(), commandOffset);
        bool deleteCommand = IsShellDeleteCommand(
            ctxMenu.Get(), commandOffset);
        if (!renameCommand &&
            GetMenuStringW(menu, cmd, menuText, static_cast<int>(_countof(menuText)), MF_BYCOMMAND) > 0)
        {
            renameCommand = StrStrIW(menuText, L"重命名") != nullptr || // l10n-allow: match Chinese Windows shell verb
                StrStrIW(menuText, L"Rename") != nullptr;
            deleteCommand = deleteCommand ||
                StrStrIW(menuText, L"删除") != nullptr || // l10n-allow: match Chinese Windows shell verb
                StrStrIW(menuText, L"Delete") != nullptr;
        }

        if (renameCommand)
        {
            DestroyMenu(menu);
            RestoreDesktopWindowLayer();
            if (!keepQuickNavigationOpen)
                RestoreInteractionInputFocus();
            if (keepQuickNavigationOpen)
                BeginQuickNavigationDesktopItemRename(
                    static_cast<size_t>(
                        itemIndex));
            else
                BeginRenameSelected(dockRenameAnchor);
            return;
        }

        if (deleteCommand &&
            dockMappingEntryIndex &&
            RemoveDockMappingAt(
                *dockMappingEntryIndex))
        {
            DestroyMenu(menu);
            RestoreDesktopWindowLayer();
            if (!keepQuickNavigationOpen)
                RestoreInteractionInputFocus();
            return;
        }

        if (deleteCommand)
        {
            std::vector<std::wstring> deletePaths;
            for (const auto& item : items_)
            {
                if (!item.selected ||
                    !item.desktopIconClsid.empty())
                    continue;
                wchar_t path[MAX_PATH]{};
                if (SHGetPathFromIDListW(
                        item.absolutePidl.get(), path))
                    deletePaths.push_back(path);
            }
            if (deletePaths.empty() && itemIndex >= 0 &&
                static_cast<size_t>(itemIndex) < items_.size())
            {
                wchar_t path[MAX_PATH]{};
                if (SHGetPathFromIDListW(
                        items_[static_cast<size_t>(itemIndex)].
                            absolutePidl.get(),
                        path))
                    deletePaths.push_back(path);
            }
            if (!deletePaths.empty())
            {
                DestroyMenu(menu);
                RestoreDesktopWindowLayer();
                std::vector<snowdesktop::ShellFileOperationStep> steps;
                steps.push_back({
                    FO_DELETE,
                    std::move(deletePaths),
                    {},
                    static_cast<FILEOP_FLAGS>(
                        FOF_ALLOWUNDO |
                        FOF_NOCONFIRMATION) });
                QueueShellFileOperation(
                    std::move(steps),
                    [this](bool succeeded) {
                        if (succeeded)
                            RequestShellRefresh();
                    });
                return;
            }
        }

        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
        invoke.hwnd = ShellDialogOwnerHwnd();
        invoke.lpVerb = MAKEINTRESOURCEA(commandOffset);
        invoke.lpVerbW = MAKEINTRESOURCEW(commandOffset);
        const std::wstring invocationDirectory =
            snowdesktop::DesktopShellInvocationDirectory();
        std::string invocationDirectoryA;
        snowdesktop::SetShellInvocationDirectory(
            invoke, invocationDirectory, invocationDirectoryA);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screenPoint;
        const auto startPin = snowdesktop::shell_start_pin::Route(
            ctxMenu.Get(), commandOffset, invocationPaths,
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
            WriteDiagnosticLogEntry(message);
        }
        else
            InvokeShellMenuCommand(ctxMenu.Get(), invoke, &menuSite);
        RequestShellRefresh();
    }
    DestroyMenu(menu);
    RestoreDesktopWindowLayer();
    if (!keepQuickNavigationOpen && cmd == 0)
        RestoreInteractionInputFocus();
}

/**
 * @brief 显示 Windows 的"新建"子菜单并创建对应类型的文件。
 *        通过 CLSID_NewMenu 获取系统"新建"菜单的 IContextMenu 接口，
 *        使用 IShellExtInit 初始化到目标目录，弹出子菜单。
 *        用户选择后调用 InvokeCommand 创建对应类型的文件。
 * @param screenPoint 菜单弹出的屏幕坐标。
 * @param targetDir   新建文件的目标目录路径。
 */
