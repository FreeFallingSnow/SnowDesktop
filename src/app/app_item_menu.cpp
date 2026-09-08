#include "app.h"
#include "../large_icon_steam.h"
#include "../large_icon_preset_rules.h"
#include "../menu_fluent_glyphs.h"
#include "../right_click_contract.h"
#include "shell_item_action_rules.h"
#include "../shell_context_menu_invoke.h"
#include "../shell_context_menu_site.h"

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
    // Pass the stable desktop HWND instead of a quick panel that closes as
    // soon as this request is accepted; the worker performs the foreground
    // handoff immediately before invoking runas.
    return shellElevationWorker_.Enqueue(
        hwnd_ && IsWindow(hwnd_) ? hwnd_ : ShellDialogOwnerHwnd(),
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
            const HWND owner = hwnd_ && IsWindow(hwnd_)
                ? hwnd_
                : ShellDialogOwnerHwnd();
            if (owner)
                SetForegroundWindow(owner);
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
    const bool administratorShortcut =
        selectedCount == 1 &&
        snowdesktop::ShellLaunchWorker::
            ShortcutRequestsAdministrator(itemPath);
    const bool canOpen =
        selectedCount == 1 && !administratorShortcut;
    const bool canShowProperties =
        selectedCount == 1 && canFile && !itemPath.empty();
    const auto removalAction =
        snowdesktop::shell_item_action_rules::
            ResolveRemovalAction(
                static_cast<size_t>(selectedCount),
                selectedFileCount,
                selectedNamespaceCount,
                dockMapping);
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

    HMENU menu = CreatePopupMenu();
    HMENU detailsMenu = nullptr;
    const auto largeIconKey = items_[itemIndex].layoutKey;
    const bool largeIconMenu = selectedCount == 1 && !dockFrequentItem && !dockApplicationItem &&
        !dockMapping && !dockEntryIndex && !keepQuickNavigationOpen &&
        !IsItemInAnyWidget(items_[itemIndex]) && items_[itemIndex].gridCell.pageId != kDockPageId;
    if (largeIconMenu)
    {
        if (items_[itemIndex].largeIcon)
        {
            namespace presets = snowdesktop::large_icon_preset_rules;
            const auto& config = *items_[itemIndex].largeIcon;
            const auto runtime = largeIconRuntime_.find(largeIconKey);
            const bool hasEdge = runtime != largeIconRuntime_.end() && runtime->second.asset && runtime->second.asset->hasEdgeColor;
            const bool editable = CanEditLargeIcons();
            HMENU settings = CreatePopupMenu();
            HMENU backgrounds = CreatePopupMenu(), effects = CreatePopupMenu();
            for (size_t i = 0; i < presets::backgrounds.size(); ++i)
            {
                const auto option = presets::backgrounds[i];
                if (!presets::BackgroundVisible(option.value, hasEdge, config)) continue;
                const auto id = kContextLargeIconBackgroundFirst + static_cast<UINT>(i);
                AppendMenuW(backgrounds, MF_STRING | (editable ? 0 : MF_GRAYED) |
                    (presets::Background(config) == option.value ? MF_CHECKED : 0), id, _LW(option.label));
            }
            for (size_t i = 0; i < presets::effects.size(); ++i)
            {
                const auto option = presets::effects[i];
                const bool enabled = editable && (option.value != 2 || !snowdesktop::IsLargeIconFill(config));
                AppendMenuW(effects, MF_STRING | (enabled ? 0 : MF_GRAYED) |
                    (presets::Effect(config) == option.value ? MF_CHECKED : 0),
                    kContextLargeIconEffectFirst + static_cast<UINT>(i), _LW(option.label));
            }
            AppendMenuW(settings, MF_POPUP | (editable ? 0 : MF_GRAYED), reinterpret_cast<UINT_PTR>(backgrounds), _LW("largeIcon.backgroundSettings"));
            AppendMenuW(settings, MF_POPUP | (editable ? 0 : MF_GRAYED), reinterpret_cast<UINT_PTR>(effects), _LW("largeIcon.effectsSection"));
            AppendMenuW(settings, MF_STRING | (editable ? 0 : MF_GRAYED) | (config.showOnHoverOnly ? MF_CHECKED : 0), kContextLargeIconHoverOnly, _LW("app.interact.hover_only"));
            AppendMenuW(settings, MF_STRING | (editable ? 0 : MF_GRAYED) | (config.keepWhenDesktopHidden ? MF_CHECKED : 0), kContextLargeIconKeepWhenHidden, _LW("app.interact.keep_when_hidden"));
            SetMenuItemIcon(settings, kContextLargeIconHoverOnly, L"\uF06E");
            SetMenuItemIcon(settings, kContextLargeIconKeepWhenHidden, L"\uF108");
            AppendMenuW(settings, MF_STRING, kContextLargeIconSettings, _LW("largeIcon.detailedSettings"));
            SetMenuItemIcon(settings, reinterpret_cast<UINT_PTR>(backgrounds), L"\uF53F");
            SetMenuItemIcon(settings, reinterpret_cast<UINT_PTR>(effects), L"\uF0D0");
            SetMenuItemIcon(settings, kContextLargeIconSettings, L"\uF013");
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(settings), _LW("largeIcon.settings"));
            SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(settings), L"\uF013");
            AppendMenuW(menu, MF_STRING, kContextLargeIconRestore, _LW("largeIcon.restore"));
        }
        else AppendMenuW(menu, MF_STRING, kContextLargeIconCreate, _LW("largeIcon.create"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, canOpen ? MF_STRING : MF_STRING | MF_GRAYED,
        kContextOpenCommand, _LW("app.menu.open"));
    AppendMenuW(menu, canCopyPath ? MF_STRING : MF_STRING | MF_GRAYED,
        kContextCopyPathCommand, _LW("app.menu.copy_path"));
    AppendMenuW(menu, canReveal ? MF_STRING : MF_STRING | MF_GRAYED,
        kContextRevealLocationCommand, _LW("app.menu.open_file_location"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu,
        canRunAsAdministrator ? MF_STRING : MF_STRING | MF_GRAYED,
        kContextRunAsAdministratorCommand,
        _LW("app.menu.run_as_administrator"));
    AppendMenuW(menu,
        canShowProperties ? MF_STRING : MF_STRING | MF_GRAYED,
        kContextPropertiesCommand, _LW("app.menu.properties"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
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

    SetMenuItemIcon(menu, kContextOpenCommand, L"");
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
    SetMenuItemIcon(menu, kContextMoreCommand,
        snowdesktop::menu_fluent_glyphs::kMoreOptions,
        MenuIconFont::FluentRegular);
    if (dockFolderEntry)
    {
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
            const auto index = FindItemIndexByKey(largeIconKey);
            if (index < items_.size() && items_[index].largeIcon)
            {
                auto config = *items_[index].largeIcon;
                const auto runtime = largeIconRuntime_.find(largeIconKey);
                const auto asset = runtime != largeIconRuntime_.end() ? runtime->second.asset : nullptr;
                if (visibilityCommand && CanEditLargeIcons())
                {
                    if (command == kContextLargeIconHoverOnly) config.showOnHoverOnly = !config.showOnHoverOnly;
                    else config.keepWhenDesktopHidden = !config.keepWhenDesktopHidden;
                }
                const bool changed = visibilityCommand ? CanEditLargeIcons() : backgroundCommand ? presets::ApplyBackground(config,
                    presets::backgrounds[command - kContextLargeIconBackgroundFirst].value, CanEditLargeIcons(),
                    asset && asset->hasEdgeColor, asset ? asset->accent : 0, asset ? asset->edgeColor : 0) :
                    presets::ApplyEffect(config, static_cast<int>(command - kContextLargeIconEffectFirst), CanEditLargeIcons());
                if (changed)
                {
                    if (!SetLargeIconConfig(index, config))
                        MessageBoxW(hwnd_, _LW("largeIcon.saveFailed"), _LW("largeIcon.settings"), MB_OK | MB_ICONWARNING);
                    else if (backgroundCommand && config.backgroundStyle == kAppearancePresetCustom)
                        OpenLargeIconSettings(index);
                }
            }
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
        const auto index = FindItemIndexByKey(largeIconKey);
        if (index >= items_.size() || !items_[index].largeIcon) { snowdesktop::modern_menu::DismissActive(); return true; }
        const auto config = *items_[index].largeIcon;
        const bool editable = CanEditLargeIcons();
        snowdesktop::modern_menu::VisitItems(currentItems, [&](auto& item) {
            if (item.command == kContextLargeIconHoverOnly) { item.checked = config.showOnHoverOnly; item.enabled = editable; }
            if (item.command == kContextLargeIconKeepWhenHidden) { item.checked = config.keepWhenDesktopHidden; item.enabled = editable; }
            if (item.command >= kContextLargeIconBackgroundFirst && item.command < kContextLargeIconBackgroundFirst + presets::backgrounds.size())
            {
                item.checked = presets::Background(config) == presets::backgrounds[item.command - kContextLargeIconBackgroundFirst].value;
                item.enabled = editable;
            }
            if (item.command >= kContextLargeIconEffectFirst && item.command < kContextLargeIconEffectFirst + presets::effects.size())
            {
                const int value = static_cast<int>(item.command - kContextLargeIconEffectFirst);
                item.checked = presets::Effect(config) == value;
                item.enabled = editable && (value != 2 || !snowdesktop::IsLargeIconFill(config));
            }
        });
        return true;
    };
    UINT command = ShowModernMenu(
        menu, screenPoint, menuOwner, placeOutsideDock, false, nullptr, changeLargeIcon);
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

        dockFolderPopupWidget_.listMode =
            dockFolderEntry->listMode;
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
            dockFolderPopupContainer_->InvalidateSlots();
        RefreshDockFolderPopupGeometry();
    };

    applyLargeIconCommand(command);
    switch (command)
    {
    case kContextLargeIconCreate:
        if (largeIconMenu)
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
        if (largeIconMenu) OpenLargeIconSettings(FindItemIndexByKey(largeIconKey));
        break;
    case kContextLargeIconRestore:
        if (largeIconMenu && !SetLargeIconConfig(itemIndex, std::nullopt))
            MessageBoxW(hwnd_, _LW("largeIcon.saveFailed"), _LW("largeIcon.settings"), MB_OK | MB_ICONWARNING);
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
    case kContextWidgetToggleListMode:
        if (dockFolderEntry)
        {
            dockFolderEntry->listMode =
                !dockFolderEntry->listMode;
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
        inlineEditorStarted = renameEdit_ != nullptr;
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
                WriteDesktopIconRegistryValue(
                    clsid, false))
            {
                settingsIconVisibility_[clsid] = false;
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
    if (itemIndex >= 0 && static_cast<size_t>(itemIndex) < items_.size())
    {
        for (const auto& item : items_)
            if (item.selected)
                pidls.push_back(reinterpret_cast<LPCITEMIDLIST>(item.childPidl.get()));
        if (pidls.empty())
            pidls.push_back(reinterpret_cast<LPCITEMIDLIST>(items_[itemIndex].childPidl.get()));
    }
    if (pidls.empty()) return;

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
    ShellPopupMenuLayerGuard shellMenuLayer(*this);
    UINT cmd = TrackShellPopupMenuWithDesktopPump(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint, menuOwner);
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
