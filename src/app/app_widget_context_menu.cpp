#include "app.h"
#include "../demo_collection_rules.h"
#include "../collection_titleless_rules.h"
#include "../menu_fluent_glyphs.h"
#include "../right_click_contract.h"
#include "../widgets/collection_group_rules.h"

#include <functional>

// Widget editor, group-tab and generic widget context menus.

void DesktopApp::ShowWidgetEditorHost(size_t widgetIndex)
{
    if (!settingsWindow_ || widgetIndex >= widgets_.size()) return;
    const auto& widget = widgets_[widgetIndex];
    if (widget.type != DesktopWidgetType::LuaScript) return;
    settingsWindow_->ShowWidgetEditor(widgetIndex, widget.id.c_str(),
        widget.title.c_str(), widget.packageId.c_str());
}

/**
 * @brief 显示窗口小部件的右键上下文菜单
 * @param screenPoint 屏幕坐标点
 * @param widgetIndex 小部件索引
 */
void DesktopApp::ShowCollectionGroupTabContextMenu(
    POINT screenPoint, size_t groupIndex,
    const std::wstring& collectionId)
{
    if (groupIndex >= widgets_.size() ||
        widgets_[groupIndex].type !=
            DesktopWidgetType::CollectionGroup)
        return;
    const size_t childIndex =
        FindWidgetIndexById(collectionId);
    if (childIndex >= widgets_.size() ||
        widgets_[childIndex].type !=
            DesktopWidgetType::Collection ||
        std::find(
            widgets_[groupIndex].childWidgetIds.begin(),
            widgets_[groupIndex].childWidgetIds.end(),
            collectionId) ==
            widgets_[groupIndex].childWidgetIds.end())
        return;

    PrepareMenuIconsForPoint(screenPoint);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(
        menu, MF_STRING,
        kContextWidgetRename,
        _LW("app.menu.rename"));
    SetMenuItemIcon(
        menu, kContextWidgetRename, L"\U000F0A39",
        MenuIconFont::FluentRegular);
    SetForegroundWindow(hwnd_);
    const UINT command = ShowModernMenu(menu, screenPoint, hwnd_);
    DestroyMenu(menu);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();

    if (command == kContextWidgetRename)
    {
        ClearSelection();
        widgets_[childIndex].selected = true;
        keyboardNavInsideWidget_ = true;
        keyboardNavWidgetIndex_ = groupIndex;
        auto it = std::find(
            widgets_[groupIndex].childWidgetIds.begin(),
            widgets_[groupIndex].childWidgetIds.end(),
            collectionId);
        keyboardNavMemberIndex_ =
            it == widgets_[groupIndex].childWidgetIds.end()
                ? 0
                : static_cast<int>(
                    std::distance(
                        widgets_[groupIndex].childWidgetIds.begin(),
                        it));
        keyboardNavCollectionGroupTabs_ = true;
        BeginRenameSelected();
    }
    else
    {
        RestoreInteractionInputFocus();
    }
}

void DesktopApp::ShowFileGroupSourceTabContextMenu(
    POINT screenPoint, size_t groupIndex,
    const std::wstring& childId)
{
    if (groupIndex >= widgets_.size() ||
        widgets_[groupIndex].type !=
            DesktopWidgetType::FileGroup)
        return;
    const size_t childIndex =
        FindWidgetIndexById(childId);
    if (childIndex >= widgets_.size() ||
        (widgets_[childIndex].type !=
             DesktopWidgetType::FileCategories &&
         widgets_[childIndex].type !=
             DesktopWidgetType::FolderMapping) ||
        std::find(
            widgets_[groupIndex].childWidgetIds.begin(),
            widgets_[groupIndex].childWidgetIds.end(),
            childId) ==
            widgets_[groupIndex].childWidgetIds.end())
        return;

    PrepareMenuIconsForPoint(screenPoint);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING,
        kContextWidgetRename,
        _LW("app.menu.rename"));
    SetMenuItemIcon(
        menu, kContextWidgetRename, L"\U000F0A39",
        MenuIconFont::FluentRegular);
    SetForegroundWindow(hwnd_);
    const UINT command = ShowModernMenu(menu, screenPoint, hwnd_);
    DestroyMenu(menu);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();

    if (command == kContextWidgetRename)
    {
        ClearSelection();
        widgets_[childIndex].selected = true;
        keyboardNavInsideWidget_ = true;
        keyboardNavWidgetIndex_ = groupIndex;
        const auto it = std::find(
            widgets_[groupIndex].childWidgetIds.begin(),
            widgets_[groupIndex].childWidgetIds.end(),
            childId);
        keyboardNavMemberIndex_ =
            it == widgets_[groupIndex].childWidgetIds.end()
                ? 0
                : static_cast<int>(
                    std::distance(
                        widgets_[groupIndex].
                            childWidgetIds.begin(), it));
        keyboardNavCollectionGroupTabs_ = true;
        BeginRenameSelected();
    }
    else
    {
        RestoreInteractionInputFocus();
    }
}

void DesktopApp::ShowLuaLogicalSlotItemContextMenu(
    POINT screenPoint, const std::wstring& widgetId,
    const std::string& slotId, const std::string& itemId,
    bool collection, size_t itemIndex, size_t itemCount,
    bool canRemove)
{
    if (!widgetEngine_) return;
    PrepareMenuIconsForPoint(screenPoint);
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    if (collection)
    {
        AppendMenuW(menu,
            MF_STRING | (itemIndex == 0 ? MF_GRAYED : 0),
            kContextLuaLogicalSlotMovePrevious,
            _LW("app.interact.logical_slot_move_previous"));
        AppendMenuW(menu,
            MF_STRING |
                (itemIndex + 1 >= itemCount ? MF_GRAYED : 0),
            kContextLuaLogicalSlotMoveNext,
            _LW("app.interact.logical_slot_move_next"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        SetMenuItemIcon(menu, kContextLuaLogicalSlotMovePrevious,
            L"\uE74A", MenuIconFont::FluentRegular);
        SetMenuItemIcon(menu, kContextLuaLogicalSlotMoveNext,
            L"\uE74B", MenuIconFont::FluentRegular);
    }
    AppendMenuW(menu, MF_STRING | (canRemove ? 0 : MF_GRAYED),
        kContextLuaLogicalSlotRemove,
        _LW("app.interact.logical_slot_remove"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING,
        kContextWidgetOpenComponentPanel,
        _LW("app.interact.open_component_panel"));
    SetMenuItemIcon(menu, kContextLuaLogicalSlotRemove,
        L"\uF34C", MenuIconFont::FluentRegular);
    SetMenuItemIcon(menu, kContextWidgetOpenComponentPanel,
        snowdesktop::menu_fluent_glyphs::kChevronRight,
        MenuIconFont::FluentRegular);

    SetForegroundWindow(hwnd_);
    const UINT command = ShowModernMenu(menu, screenPoint, hwnd_);
    DestroyMenu(menu);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();

    if (command == kContextWidgetOpenComponentPanel)
    {
        const size_t widgetIndex = FindWidgetIndexById(widgetId);
        if (widgetIndex < widgets_.size())
        {
            ShowWidgetContextMenu(screenPoint, widgetIndex,
                std::nullopt, std::nullopt, "desktop", true);
        }
        else
        {
            RestoreInteractionInputFocus();
        }
        return;
    }

    snowdesktop::widget_runtime::LogicalSlotChange change;
    std::string error;
    bool handled = false;
    bool succeeded = true;
    if (command == kContextLuaLogicalSlotMovePrevious && itemIndex > 0)
    {
        handled = true;
        succeeded = widgetEngine_->RuntimeMoveHostLogicalSlotItem(
            widgetId, slotId, itemId, itemIndex - 1, change, error);
    }
    else if (command == kContextLuaLogicalSlotMoveNext &&
        itemIndex + 1 < itemCount)
    {
        handled = true;
        succeeded = widgetEngine_->RuntimeMoveHostLogicalSlotItem(
            widgetId, slotId, itemId, itemIndex + 1, change, error);
    }
    else if (command == kContextLuaLogicalSlotRemove && canRemove)
    {
        handled = true;
        succeeded = widgetEngine_->RuntimeRemoveHostLogicalSlotItem(
            widgetId, slotId, itemId, change, error);
    }
    if (handled && !succeeded)
    {
        widgetEngine_->RuntimeRecordError(widgetId,
            "logical slot host menu: " + error);
        MessageBeep(MB_ICONWARNING);
    }
    if (handled) InvalidateRect(hwnd_, nullptr, FALSE);
    RestoreInteractionInputFocus();
}

void DesktopApp::ShowWidgetContextMenu(
    POINT screenPoint, size_t widgetIndex,
    std::optional<RECT> dockRenameAnchor,
    std::optional<POINT> luaLocalPoint,
    std::string_view luaSurface,
    bool forceComponentMenu)
{
    if (widgetIndex >= widgets_.size()) return;
    PrepareMenuIconsForPoint(screenPoint);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    const auto setFluentIcon = [this](
        HMENU targetMenu, UINT_PTR item, const wchar_t* glyph) {
        SetMenuItemIcon(targetMenu, item, glyph,
            MenuIconFont::FluentRegular);
    };

    const auto& widget = widgets_[widgetIndex];
    size_t effectiveSourceIndex = widgetIndex;
    if (widget.type == DesktopWidgetType::FileGroup)
    {
        effectiveSourceIndex =
            FindWidgetIndexById(widget.activeCategoryId);
        if (effectiveSourceIndex >= widgets_.size())
            effectiveSourceIndex = widgetIndex;
    }
    const DesktopWidget& effectiveSource =
        widgets_[effectiveSourceIndex];
    size_t contentSortTargetIndex = effectiveSourceIndex;
    if (widget.type == DesktopWidgetType::FileGroup)
    {
        for (const auto& container : containers_)
        {
            const auto* group = dynamic_cast<const FileGroup*>(
                container.get());
            if (group && group->GetWidgetData() == &widget &&
                group->IsGroupSearchActive())
            {
                contentSortTargetIndex = widgetIndex;
                break;
            }
        }
    }
    const auto statusLabel = [](const wchar_t* title,
                                const wchar_t* status) {
        std::wstring label = title;
        label += L"\t";
        label += status;
        return label;
    };
    const auto visibilityLabel = [&](const wchar_t* title,
                                     bool visible) {
        return statusLabel(title,
            visible
                ? _LW("app.interact.shown")
                : _LW("app.interact.hidden"));
    };
    const std::wstring displayTypeLabel = statusLabel(
        _LW("app.interact.display_type"),
        widget.listMode
            ? _LW("app.interact.list_view_state")
            : _LW("app.interact.icon_view_state"));
    HMENU detailsMenu = nullptr;
    const auto appendDetailsMenu = [&]() {
        detailsMenu = CreatePopupMenu();
        if (!detailsMenu) return;
        AppendMenuW(detailsMenu,
            MF_STRING | MF_CHECKED | MF_GRAYED,
            kContextWidgetDetailName,
            _LW("widget.details.name"));
        AppendMenuW(detailsMenu,
            MF_STRING |
                (widget.detailShowModified ? MF_CHECKED : 0),
            kContextWidgetDetailModified,
            _LW("widget.details.modified"));
        AppendMenuW(detailsMenu,
            MF_STRING |
                (widget.detailShowType ? MF_CHECKED : 0),
            kContextWidgetDetailType,
            _LW("widget.details.type"));
        AppendMenuW(detailsMenu,
            MF_STRING |
                (widget.detailShowSize ? MF_CHECKED : 0),
            kContextWidgetDetailSize,
            _LW("widget.details.size"));
        AppendMenuW(menu,
            MF_POPUP | (widget.listMode ? 0 : MF_GRAYED),
            reinterpret_cast<UINT_PTR>(detailsMenu),
            _LW("app.interact.show_details"));
    };
    const std::wstring categoryVisibilityLabel = visibilityLabel(
        _LW("app.interact.file_categories"),
        widget.showFileCategories);
    const std::wstring searchVisibilityLabel = visibilityLabel(
        _LW("app.interact.search_box"),
        widget.showSearchBox);
    const std::wstring dateVisibilityLabel = visibilityLabel(
        _LW("app.interact.date_header"),
        widget.dateHeaders);
    const std::wstring collectionModeLabel = statusLabel(
        _LW("app.interact.display_mode"),
        widget.scrollContainerMode
            ? _LW("app.interact.popup_container")
            : _LW("app.interact.large_folder"));
    std::vector<LuaWidgetMenuItem> luaMenuItems;
    std::vector<LuaWidgetMenuItem> luaMenuActions;
    auto luaMenuScope = snowdesktop::right_click_contract::
        LuaWidgetMenuScope::Widget;
    bool luaElementMenu = false;
    HMENU demoCategoryMenu = nullptr;

    if (widget.type == DesktopWidgetType::Collection)
    {
        AppendMenuW(menu, MF_STRING, kContextWidgetOpen, _LW("app.interact.open_all"));
        if (generalSettings_.demoModeEnabled &&
            demoIdentityAssetsAvailable_)
        {
            demoCategoryMenu = CreatePopupMenu();
            if (demoCategoryMenu)
            {
                UINT flags = MF_STRING;
                if (widget.demoIconCategory.empty()) flags |= MF_CHECKED;
                AppendMenuW(demoCategoryMenu, flags,
                    kContextWidgetDemoCategoryFirst,
                    _LW("app.demo_category.auto"));
                for (size_t index = 0;
                    index < snowdesktop::demo_collection_rules::
                        kCategories.size(); ++index)
                {
                    const auto& category =
                        snowdesktop::demo_collection_rules::
                            kCategories[index];
                    flags = MF_STRING;
                    if (snowdesktop::demo_collection_rules::
                            EqualsAsciiInsensitive(
                                widget.demoIconCategory, category.id))
                        flags |= MF_CHECKED;
                    AppendMenuW(demoCategoryMenu, flags,
                        kContextWidgetDemoCategoryFirst + 1 +
                            static_cast<UINT>(index),
                        _LW(category.titleKey));
                }
                AppendMenuW(menu, MF_POPUP,
                    reinterpret_cast<UINT_PTR>(demoCategoryMenu),
                    _LW("app.demo_category.icon_category"));
            }
        }
        const bool compactCollection =
            widget.gridSpan.columns <= 1 && widget.gridSpan.rows <= 1;
        if (!compactCollection)
        {
            AppendMenuW(menu, MF_STRING,
                kContextWidgetToggleCollectionMode,
                collectionModeLabel.c_str());
            if (widget.scrollContainerMode)
            {
                AppendMenuW(menu, MF_STRING, kContextWidgetToggleListMode,
                    displayTypeLabel.c_str());
                appendDetailsMenu();
            }
        }
    }
    else if (widget.type == DesktopWidgetType::CollectionGroup)
    {
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleListMode,
            displayTypeLabel.c_str());
        appendDetailsMenu();
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleSearchBox,
            searchVisibilityLabel.c_str());
    }
    else if (widget.type == DesktopWidgetType::FileGroup)
    {
        if (effectiveSource.type ==
            DesktopWidgetType::FileCategories)
        {
            AppendMenuW(menu,
                HasPasteableFileClipboardData()
                    ? MF_STRING : MF_STRING | MF_GRAYED,
                kContextPasteCommand, _LW("app.menu.paste"));
            AppendMenuW(menu, MF_STRING,
                kContextNewMenu, _LW("app.menu.new"));
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING,
                kContextWidgetManualCollect,
                _LW("app.interact.collect_now"));
            const std::wstring autoCollectLabel = statusLabel(
                _LW("app.interact.auto_collect"),
                effectiveSource.autoCollect
                    ? _LW("app.interact.on")
                    : _LW("app.interact.off"));
            AppendMenuW(menu, MF_STRING,
                kContextWidgetToggleAutoCollect,
                autoCollectLabel.c_str());
        }
        else if (effectiveSource.type ==
                 DesktopWidgetType::FolderMapping)
        {
            AppendMenuW(menu, MF_STRING,
                kContextWidgetOpenFolder,
                _LW("app.interact.open_folder"));
            AppendMenuW(menu,
                HasPasteableFileClipboardData()
                    ? MF_STRING : MF_STRING | MF_GRAYED,
                kContextPasteCommand, _LW("app.menu.paste"));
        }
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleListMode,
            displayTypeLabel.c_str());
        appendDetailsMenu();
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleFileCategories,
            categoryVisibilityLabel.c_str());
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleSearchBox,
            searchVisibilityLabel.c_str());
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleDateGroup,
            dateVisibilityLabel.c_str());
        if (effectiveSource.type ==
            DesktopWidgetType::FolderMapping)
        {
            AppendMenuW(menu, MF_STRING,
                kContextNewMenu, _LW("app.menu.new"));
            AppendMenuW(menu, MF_STRING,
                kContextMoreCommand,
                _LW("app.menu.more_options"));
        }
    }
    else if (widget.type == DesktopWidgetType::FileCategories)
    {
        AppendMenuW(menu,
            HasPasteableFileClipboardData()
                ? MF_STRING : MF_STRING | MF_GRAYED,
            kContextPasteCommand, _LW("app.menu.paste"));
        AppendMenuW(menu, MF_STRING,
            kContextNewMenu, _LW("app.menu.new"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kContextWidgetManualCollect, _LW("app.interact.collect_now"));
        const std::wstring autoCollectLabel = statusLabel(
            _LW("app.interact.auto_collect"),
            widget.autoCollect
                ? _LW("app.interact.on")
                : _LW("app.interact.off"));
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleAutoCollect,
            autoCollectLabel.c_str());
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleListMode,
            displayTypeLabel.c_str());
        appendDetailsMenu();
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleFileCategories,
            categoryVisibilityLabel.c_str());
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleSearchBox,
            searchVisibilityLabel.c_str());
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleDateGroup,
            dateVisibilityLabel.c_str());
    }
    else if (widget.type == DesktopWidgetType::FolderMapping)
    {
        AppendMenuW(menu, MF_STRING, kContextWidgetOpenFolder, _LW("app.interact.open_folder"));
        AppendMenuW(menu,
            HasPasteableFileClipboardData()
                ? MF_STRING : MF_STRING | MF_GRAYED,
            kContextPasteCommand, _LW("app.menu.paste"));
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleListMode,
            displayTypeLabel.c_str());
        appendDetailsMenu();
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleFileCategories,
            categoryVisibilityLabel.c_str());
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleSearchBox,
            searchVisibilityLabel.c_str());
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleDateGroup,
            dateVisibilityLabel.c_str());
        AppendMenuW(menu, MF_STRING, kContextNewMenu, _LW("app.menu.new"));
        AppendMenuW(menu, MF_STRING, kContextMoreCommand, _LW("app.menu.more_options"));
    }
    else if (widget.type == DesktopWidgetType::LuaScript)
    {
        if (widgetEngine_)
        {
            widgetEngine_->EnsureWidgetLoaded(widget.id, widget.packageId);
            if (!forceComponentMenu)
            {
                POINT clientPoint = screenPoint;
                ScreenToClient(hwnd_, &clientPoint);
                const RECT frame = GetStandaloneWidgetFrameRect(widget);
                const POINT localPoint = luaLocalPoint.value_or(POINT{
                    clientPoint.x - frame.left,
                    clientPoint.y - frame.top });
                luaMenuItems = widgetEngine_->GetContextMenu(widget.id,
                    localPoint.x, localPoint.y, luaSurface);
                const bool hasElementAction =
                    snowdesktop::right_click_contract::
                        HasLuaElementMenuAction(luaMenuItems);
                luaMenuScope = snowdesktop::right_click_contract::
                    ResolveLuaWidgetMenuScope(hasElementAction);
                luaElementMenu = luaMenuScope ==
                    snowdesktop::right_click_contract::
                        LuaWidgetMenuScope::Element;
            }
            if (!luaElementMenu)
            {
                AppendMenuW(menu, MF_STRING, kContextWidgetEdit,
                    _LW("app.interact.detailed_settings"));
            }
            const auto setLuaItemIcon = [this, &widget](HMENU targetMenu,
                                                 UINT_PTR itemId,
                                                 const LuaWidgetMenuItem& item) {
                if (!item.imageResourceName.empty())
                {
                    const auto* source = widgetEngine_->
                        RuntimeFindPackageImageSource(widget.id,
                            item.imageResourceName);
                    if (source)
                        SetMenuItemImage(targetMenu, itemId, *source);
                    return;
                }
                if (item.icon.empty()) return;
                const std::wstring icon = Utf8ToWide(item.icon);
                const bool useFluent =
                    _stricmp(item.iconFont.c_str(), "fluent") == 0 ||
                    _stricmp(item.iconFont.c_str(), "fluent-regular") == 0;
                SetMenuItemIcon(targetMenu, itemId, icon.c_str(),
                    useFluent
                        ? MenuIconFont::FluentRegular
                        : MenuIconFont::FontAwesomeSolid);
            };
            std::function<void(HMENU,
                const std::vector<LuaWidgetMenuItem>&)> appendLuaMenuItems;
            appendLuaMenuItems = [&](HMENU targetMenu,
                                     const std::vector<LuaWidgetMenuItem>& items) {
                for (const auto& item : items)
                {
                    if (item.separator)
                    {
                        AppendMenuW(targetMenu, MF_SEPARATOR, 0, nullptr);
                        continue;
                    }
                    UINT flags = (item.enabled ? 0 : MF_GRAYED) |
                        (item.checked ? MF_CHECKED : 0);
                    if (!item.children.empty())
                    {
                        HMENU submenu = CreatePopupMenu();
                        if (!submenu) continue;
                        appendLuaMenuItems(submenu, item.children);
                        const UINT_PTR submenuId =
                            reinterpret_cast<UINT_PTR>(submenu);
                        if (!AppendMenuW(targetMenu, flags | MF_POPUP,
                                submenuId,
                                Utf8ToWide(item.label).c_str()))
                        {
                            DestroyMenu(submenu);
                            continue;
                        }
                        setLuaItemIcon(targetMenu, submenuId, item);
                        continue;
                    }
                    if (kContextLuaWidgetMenuFirst +
                            static_cast<UINT>(luaMenuActions.size()) >
                        kContextLuaWidgetMenuLast)
                        continue;
                    const UINT commandId = kContextLuaWidgetMenuFirst +
                        static_cast<UINT>(luaMenuActions.size());
                    if (!AppendMenuW(targetMenu, flags | MF_STRING,
                            commandId, Utf8ToWide(item.label).c_str()))
                        continue;
                    setLuaItemIcon(targetMenu, commandId, item);
                    luaMenuActions.push_back(item);
                }
            };
            appendLuaMenuItems(menu, luaMenuItems);
            if (snowdesktop::right_click_contract::
                    ShouldOfferComponentPanelShortcut(luaMenuScope))
            {
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING,
                    kContextWidgetOpenComponentPanel,
                    _LW("app.interact.open_component_panel"));
            }
        }
    }

    HMENU sortMenu = nullptr, wNameMenu = nullptr, wTypeMenu = nullptr,
        wDateMenu = nullptr, wSizeMenu = nullptr;
    if ((widget.type == DesktopWidgetType::FileCategories ||
        widget.type == DesktopWidgetType::FolderMapping ||
        widget.type == DesktopWidgetType::Collection ||
        widget.type == DesktopWidgetType::CollectionGroup ||
        widget.type == DesktopWidgetType::FileGroup))
    {
        sortMenu = CreatePopupMenu();
        if (sortMenu)
        {
            wNameMenu = CreatePopupMenu();
            if (wNameMenu)
            {
                AppendMenuW(wNameMenu, MF_STRING, kContextWidgetSortByName, _LW("app.menu.sort_asc"));
                AppendMenuW(wNameMenu, MF_STRING, kContextWidgetSortByNameDesc, _LW("app.menu.sort_desc"));
                AppendMenuW(sortMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(wNameMenu), _LW("app.menu.sort_name"));
            }
            wTypeMenu = CreatePopupMenu();
            if (wTypeMenu)
            {
                AppendMenuW(wTypeMenu, MF_STRING, kContextWidgetSortByType, _LW("app.menu.sort_asc"));
                AppendMenuW(wTypeMenu, MF_STRING, kContextWidgetSortByTypeDesc, _LW("app.menu.sort_desc"));
                AppendMenuW(sortMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(wTypeMenu), _LW("app.menu.sort_type"));
            }
            wDateMenu = CreatePopupMenu();
            if (wDateMenu)
            {
                AppendMenuW(wDateMenu, MF_STRING, kContextWidgetSortByDate, _LW("app.menu.sort_asc"));
                AppendMenuW(wDateMenu, MF_STRING, kContextWidgetSortByDateDesc, _LW("app.menu.sort_desc"));
                AppendMenuW(sortMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(wDateMenu), _LW("app.interact.sort_date"));
            }
            wSizeMenu = CreatePopupMenu();
            if (wSizeMenu)
            {
                AppendMenuW(wSizeMenu, MF_STRING,
                    kContextWidgetSortBySize, _LW("app.menu.sort_asc"));
                AppendMenuW(wSizeMenu, MF_STRING,
                    kContextWidgetSortBySizeDesc, _LW("app.menu.sort_desc"));
                AppendMenuW(sortMenu, MF_POPUP,
                    reinterpret_cast<UINT_PTR>(wSizeMenu),
                    _LW("app.menu.sort_size"));
            }
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sortMenu), _LW("app.menu.sort_by"));
        }
    }

    const UINT hoverToggleCommand = widget.showOnHoverOnly
        ? kContextWidgetShowOnHoverOff
        : kContextWidgetShowOnHoverOn;
    const UINT keepToggleCommand = widget.keepWhenDesktopHidden
        ? kContextWidgetKeepWhenHiddenOff
        : kContextWidgetKeepWhenHiddenOn;
    const UINT privacyToggleCommand = widget.privacyMode
        ? kContextWidgetPrivacyModeOff
        : kContextWidgetPrivacyModeOn;
    const bool showLargeFolderTitlelessOption =
        widget.type == DesktopWidgetType::Collection &&
        snowdesktop::collection_titleless_rules::
            IsLargeFolderMode(
                widget.scrollContainerMode,
                widget.gridSpan.columns,
                widget.gridSpan.rows);
    if (!luaElementMenu)
    {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        if (CanRenameWidget(widget))
        {
            AppendMenuW(menu, MF_STRING, kContextWidgetRename, _LW("app.menu.rename"));
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }
        auto toggleLabel = [](const wchar_t* title, bool enabled) {
            std::wstring label = title;
            label += L"\t";
            label += enabled
                ? _LW("app.interact.on")
                : _LW("app.interact.off");
            return label;
        };
        const std::wstring hoverLabel = toggleLabel(
            _LW("app.interact.hover_only"), widget.showOnHoverOnly);
        const std::wstring keepLabel = toggleLabel(
            _LW("app.interact.keep_when_hidden"),
            widget.keepWhenDesktopHidden);
        AppendMenuW(menu, MF_STRING, hoverToggleCommand, hoverLabel.c_str());
        AppendMenuW(menu, MF_STRING, keepToggleCommand, keepLabel.c_str());
        if (widget.type == DesktopWidgetType::Collection ||
            widget.type == DesktopWidgetType::FileCategories ||
            widget.type == DesktopWidgetType::FolderMapping ||
            widget.type == DesktopWidgetType::CollectionGroup ||
            widget.type == DesktopWidgetType::FileGroup)
        {
            const std::wstring privacyLabel = toggleLabel(
                _LW("app.interact.privacy_mode"), widget.privacyMode);
            AppendMenuW(menu, MF_STRING,
                privacyToggleCommand, privacyLabel.c_str());
        }
        if (showLargeFolderTitlelessOption)
        {
            const std::wstring titlelessLabel = toggleLabel(
                _LW("app.interact.large_folder_titleless"),
                collectionLargeFolderTitleless_);
            AppendMenuW(menu, MF_STRING,
                kContextWidgetToggleLargeFolderTitleless,
                titlelessLabel.c_str());
        }
        AppendMenuW(menu, MF_STRING, kContextWidgetDelete,
            _LW("app.interact.delete_widget"));
    }

    setFluentIcon(menu, kContextWidgetOpen, L"\uF582");
    setFluentIcon(menu, kContextWidgetManualCollect,
        snowdesktop::menu_fluent_glyphs::kCollectItems);
    setFluentIcon(menu, kContextWidgetToggleAutoCollect,
        snowdesktop::menu_fluent_glyphs::kAutoCollect);
    setFluentIcon(menu, kContextWidgetToggleListMode,
        snowdesktop::menu_fluent_glyphs::kContentLayout);
    if (detailsMenu)
        setFluentIcon(menu,
            reinterpret_cast<UINT_PTR>(detailsMenu), L"\uF168");
    setFluentIcon(menu, kContextWidgetToggleDateGroup,
        snowdesktop::menu_fluent_glyphs::kDateHeader);
    setFluentIcon(menu, kContextWidgetOpenFolder, L"\uF42E");
    setFluentIcon(menu, kContextWidgetToggleFileCategories,
        snowdesktop::menu_fluent_glyphs::kCategoryBar);
    setFluentIcon(menu, kContextWidgetToggleSearchBox, L"\uF68F");
    SetMenuItemIcon(menu, kContextPasteCommand, L"");
    setFluentIcon(menu, kContextNewMenu,
        snowdesktop::menu_fluent_glyphs::kNewItem);
    setFluentIcon(menu, kContextMoreCommand,
        snowdesktop::menu_fluent_glyphs::kMoreOptions);
    setFluentIcon(menu, kContextWidgetEdit, L"\uF6A9");
    setFluentIcon(menu, kContextWidgetOpenComponentPanel,
        snowdesktop::menu_fluent_glyphs::kChevronRight);
    setFluentIcon(menu, kContextWidgetRename, L"\U000F0A39");
    setFluentIcon(menu, kContextWidgetDelete, L"\uF34C");
    SetMenuItemQuickAction(menu, kContextWidgetEdit);
    SetMenuItemQuickAction(menu, kContextNewMenu);
    SetMenuItemQuickAction(menu, kContextPasteCommand);
    SetMenuItemQuickAction(menu, kContextWidgetRename);
    SetMenuItemQuickAction(menu, kContextWidgetDelete);
    setFluentIcon(menu, hoverToggleCommand, L"\uE5F2");
    setFluentIcon(menu, keepToggleCommand, L"\uF359");
    if (widget.type == DesktopWidgetType::Collection ||
        widget.type == DesktopWidgetType::FileCategories ||
        widget.type == DesktopWidgetType::FolderMapping ||
        widget.type == DesktopWidgetType::CollectionGroup ||
        widget.type == DesktopWidgetType::FileGroup)
    {
        setFluentIcon(menu, privacyToggleCommand,
            widget.privacyMode ? L"\uE78F" : L"\uE795");
    }
    if (showLargeFolderTitlelessOption)
    {
        setFluentIcon(menu,
            kContextWidgetToggleLargeFolderTitleless,
            snowdesktop::menu_fluent_glyphs::kHideLabels);
    }
    if (sortMenu)
    {
        setFluentIcon(menu, reinterpret_cast<UINT_PTR>(sortMenu),
            snowdesktop::menu_fluent_glyphs::kSort);
        if (wNameMenu)
        {
            setFluentIcon(sortMenu,
                reinterpret_cast<UINT_PTR>(wNameMenu),
                snowdesktop::menu_fluent_glyphs::kSortName);
            setFluentIcon(wNameMenu,
                kContextWidgetSortByName,
                snowdesktop::menu_fluent_glyphs::kSortNameAscending);
            setFluentIcon(wNameMenu,
                kContextWidgetSortByNameDesc,
                snowdesktop::menu_fluent_glyphs::kSortNameDescending);
        }
        if (wTypeMenu)
        {
            setFluentIcon(sortMenu,
                reinterpret_cast<UINT_PTR>(wTypeMenu),
                snowdesktop::menu_fluent_glyphs::kSortType);
            setFluentIcon(wTypeMenu,
                kContextWidgetSortByType,
                snowdesktop::menu_fluent_glyphs::kSortTypeAscending);
            setFluentIcon(wTypeMenu,
                kContextWidgetSortByTypeDesc,
                snowdesktop::menu_fluent_glyphs::kSortTypeDescending);
        }
        if (wDateMenu)
        {
            setFluentIcon(sortMenu,
                reinterpret_cast<UINT_PTR>(wDateMenu),
                snowdesktop::menu_fluent_glyphs::kSortDate);
            setFluentIcon(wDateMenu,
                kContextWidgetSortByDate,
                snowdesktop::menu_fluent_glyphs::kSortDateAscending);
            setFluentIcon(wDateMenu,
                kContextWidgetSortByDateDesc,
                snowdesktop::menu_fluent_glyphs::kSortDateDescending);
        }
        if (wSizeMenu)
        {
            setFluentIcon(sortMenu,
                reinterpret_cast<UINT_PTR>(wSizeMenu),
                snowdesktop::menu_fluent_glyphs::kSort);
            setFluentIcon(wSizeMenu,
                kContextWidgetSortBySize,
                snowdesktop::menu_fluent_glyphs::kSortNameAscending);
            setFluentIcon(wSizeMenu,
                kContextWidgetSortBySizeDesc,
                snowdesktop::menu_fluent_glyphs::kSortNameDescending);
        }
    }
    if (widget.type == DesktopWidgetType::Collection)
        setFluentIcon(menu, kContextWidgetToggleCollectionMode,
            snowdesktop::menu_fluent_glyphs::kCollection);
    if (demoCategoryMenu)
        setFluentIcon(menu,
            reinterpret_cast<UINT_PTR>(demoCategoryMenu), L"\uF18B");

    SetForegroundWindow(hwnd_);
    UINT command = ShowModernMenu(
        menu, screenPoint, hwnd_, dockRenameAnchor.has_value());
    DestroyMenu(menu);
    ClearMenuIcons();

    if (command == kContextWidgetOpenComponentPanel && luaElementMenu)
    {
        RestoreDesktopWindowLayer();
        ShowWidgetContextMenu(screenPoint, widgetIndex,
            dockRenameAnchor, std::nullopt, luaSurface, true);
        return;
    }

    if (command >= kContextLuaWidgetMenuFirst && command <= kContextLuaWidgetMenuLast)
    {
        // Finish restoring the desktop menu owner before dispatching the Lua
        // action. The action may synchronously open another interaction
        // surface (for example the logical-slot picker), which must keep the
        // focus it establishes.
        RestoreDesktopWindowLayer();
        RestoreInteractionInputFocus();
        size_t itemIndex = static_cast<size_t>(command - kContextLuaWidgetMenuFirst);
        if (itemIndex < luaMenuActions.size() && widgetEngine_)
        {
            widgetEngine_->InvokeMenu(
                widgets_[widgetIndex].id, luaMenuActions[itemIndex]);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }

    if (command >= kContextWidgetDemoCategoryFirst &&
        command <= kContextWidgetDemoCategoryLast)
    {
        if (command == kContextWidgetDemoCategoryFirst)
        {
            widgets_[widgetIndex].demoIconCategory.clear();
        }
        else
        {
            const size_t categoryIndex = static_cast<size_t>(
                command - kContextWidgetDemoCategoryFirst - 1);
            if (categoryIndex < snowdesktop::demo_collection_rules::
                    kCategories.size())
            {
                widgets_[widgetIndex].demoIconCategory =
                    snowdesktop::demo_collection_rules::
                        kCategories[categoryIndex].id;
            }
        }
        demoCollectionIdentityCache_.clear();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        InvalidateFloatingDockWindow(false);
        RestoreDesktopWindowLayer();
        RestoreInteractionInputFocus();
        return;
    }

    const auto refreshOpenPopupDisplay = [&]() {
        const DesktopWidget& source =
            widgets_[widgetIndex];
        if (dockFolderPopupOpen_ &&
            dockFolderPopupMappingWidgetId_ ==
                source.id)
        {
            dockFolderPopupWidget_.listMode =
                source.listMode;
            dockFolderPopupWidget_.showDetails =
                source.showDetails;
            dockFolderPopupWidget_.detailShowModified =
                source.detailShowModified;
            dockFolderPopupWidget_.detailShowType =
                source.detailShowType;
            dockFolderPopupWidget_.detailShowSize =
                source.detailShowSize;
            dockFolderPopupWidget_.detailModifiedPosition =
                source.detailModifiedPosition;
            dockFolderPopupWidget_.detailTypePosition =
                source.detailTypePosition;
            dockFolderPopupWidget_.detailSizePosition =
                source.detailSizePosition;
            dockFolderPopupWidget_.contentSortColumn =
                source.contentSortColumn;
            dockFolderPopupWidget_.contentSortAscending =
                source.contentSortAscending;
            popupScrollOffset_ = 0;
            if (dockFolderPopupContainer_)
                dockFolderPopupContainer_->
                    InvalidateSlots();
            RefreshDockFolderPopupGeometry();
            return;
        }
        if (!dockFolderPopupOpen_ &&
            popupWidgetIndex_ == widgetIndex)
        {
            popupScrollOffset_ = 0;
            RefreshOpenCollectionPopupGeometry();
        }
    };

    bool needsDesktopFocus = true;
    switch (command)
    {
    case kContextWidgetOpen:
    {
        POINT clientPoint = screenPoint;
        ScreenToClient(hwnd_, &clientPoint);
        OpenCollectionPopupAt(widgetIndex, clientPoint, L"");
        break;
    }
    case kContextWidgetOpenFolder:
        needsDesktopFocus = false;
        if (effectiveSource.type ==
                DesktopWidgetType::FolderMapping &&
            !effectiveSource.sourceFolderPath.empty())
            shellLaunchWorker_.Enqueue(
                hwnd_, effectiveSource.sourceFolderPath);
        break;
    case kContextWidgetToggleListMode:
        widgets_[widgetIndex].listMode = !widgets_[widgetIndex].listMode;
        for (auto& c : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (wc && wc->GetWidgetData() == &widgets_[widgetIndex])
            {
                if (auto* group =
                        dynamic_cast<FileGroup*>(wc))
                    group->InvalidateHostedView();
                else
                    wc->InvalidateSlots();
                break;
            }
        }
        SaveLayoutSlots();
        refreshOpenPopupDisplay();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetDetailModified:
    case kContextWidgetDetailType:
    case kContextWidgetDetailSize:
    {
        if (!widgets_[widgetIndex].listMode) break;
        auto& detailWidget = widgets_[widgetIndex];
        auto column = snowdesktop::list_detail_rules::Column::Modified;
        bool enabling = false;
        if (command == kContextWidgetDetailModified)
        {
            enabling = !detailWidget.detailShowModified;
            detailWidget.detailShowModified = enabling;
        }
        else if (command == kContextWidgetDetailType)
        {
            column = snowdesktop::list_detail_rules::Column::Type;
            enabling = !detailWidget.detailShowType;
            detailWidget.detailShowType = enabling;
        }
        else
        {
            column = snowdesktop::list_detail_rules::Column::Size;
            enabling = !detailWidget.detailShowSize;
            detailWidget.detailShowSize = enabling;
        }
        if (enabling)
        {
            const snowdesktop::list_detail_rules::DividerPositions positions{
                detailWidget.detailModifiedPosition,
                detailWidget.detailTypePosition,
                detailWidget.detailSizePosition };
            const float adjusted = snowdesktop::list_detail_rules::
                ClampDraggedPosition(
                    column,
                    column == snowdesktop::list_detail_rules::Column::Modified
                        ? detailWidget.detailModifiedPosition
                        : column == snowdesktop::list_detail_rules::Column::Type
                            ? detailWidget.detailTypePosition
                            : detailWidget.detailSizePosition,
                    detailWidget.detailShowModified,
                    detailWidget.detailShowType,
                    detailWidget.detailShowSize,
                    positions);
            if (column == snowdesktop::list_detail_rules::Column::Modified)
                detailWidget.detailModifiedPosition = adjusted;
            else if (column == snowdesktop::list_detail_rules::Column::Type)
                detailWidget.detailTypePosition = adjusted;
            else
                detailWidget.detailSizePosition = adjusted;
        }
        detailWidget.showDetails =
            snowdesktop::list_detail_rules::HasMetadataColumns(
                detailWidget.detailShowModified,
                detailWidget.detailShowType,
                detailWidget.detailShowSize);
        detailWidget.scrollOffset = 0;
        for (auto& container : containers_)
        {
            auto* widgetContainer =
                dynamic_cast<WidgetContainer*>(container.get());
            if (widgetContainer &&
                widgetContainer->GetWidgetData() ==
                    &widgets_[widgetIndex])
            {
                if (auto* group =
                        dynamic_cast<FileGroup*>(widgetContainer))
                    group->InvalidateHostedView();
                else
                    widgetContainer->InvalidateSlots();
                break;
            }
        }
        SaveLayoutSlots();
        refreshOpenPopupDisplay();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    }
    case kContextWidgetToggleAutoCollect:
        if (effectiveSourceIndex >= widgets_.size() ||
            widgets_[effectiveSourceIndex].type !=
                DesktopWidgetType::FileCategories)
            break;
        widgets_[effectiveSourceIndex].autoCollect =
            !widgets_[effectiveSourceIndex].autoCollect;
        if (widgets_[effectiveSourceIndex].autoCollect)
        {
            EnforceSingleAutoCollectFileCategory(
                effectiveSourceIndex);
            CollectFileCategoryWidget(
                effectiveSourceIndex, false);
        }
        RebuildContainersAndItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetToggleDateGroup:
        widgets_[widgetIndex].dateHeaders = !widgets_[widgetIndex].dateHeaders;
        widgets_[widgetIndex].scrollOffset = 0;
        if (widgets_[widgetIndex].type ==
            DesktopWidgetType::FileGroup)
        {
            for (auto& c : containers_)
            {
                auto* group =
                    dynamic_cast<FileGroup*>(c.get());
                if (group &&
                    group->GetWidgetData() ==
                        &widgets_[widgetIndex])
                {
                    group->InvalidateHostedView();
                    break;
                }
            }
        }
        else if (widgets_[widgetIndex].type ==
                 DesktopWidgetType::FolderMapping)
        {
            for (auto& c : containers_)
            {
                auto* mapping = dynamic_cast<FolderMapping*>(c.get());
                if (mapping &&
                    mapping->GetWidgetData() == &widgets_[widgetIndex])
                {
                    mapping->InvalidateFilterCache();
                    break;
                }
            }
        }
        else
        {
            RebuildContainersAndItems();
        }
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetToggleFileCategories:
        if (widgets_[widgetIndex].type ==
                DesktopWidgetType::FileCategories ||
            widgets_[widgetIndex].type ==
                DesktopWidgetType::FolderMapping ||
            widgets_[widgetIndex].type ==
                DesktopWidgetType::FileGroup)
        {
            widgets_[widgetIndex].showFileCategories =
                !widgets_[widgetIndex].showFileCategories;
            widgets_[widgetIndex].scrollOffset = 0;
            widgets_[widgetIndex].tabScrollOffset = 0;
            for (auto& c : containers_)
            {
                auto* scrolling =
                    dynamic_cast<ScrollingItemWidget*>(c.get());
                if (scrolling &&
                    scrolling->GetWidgetData() ==
                        &widgets_[widgetIndex])
                {
                    if (auto* categories =
                            dynamic_cast<FileCategories*>(
                                scrolling))
                        categories->InvalidateCategoryCache();
                    else if (auto* mapping =
                            dynamic_cast<FolderMapping*>(
                                scrolling))
                        mapping->InvalidateFilterCache();
                    else if (auto* fileGroup =
                                 dynamic_cast<FileGroup*>(
                                     scrolling))
                        fileGroup->InvalidateHostedView();
                    break;
                }
            }
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        break;
    case kContextWidgetToggleSearchBox:
        if (widgets_[widgetIndex].type == DesktopWidgetType::FileCategories ||
            widgets_[widgetIndex].type == DesktopWidgetType::FolderMapping ||
            widgets_[widgetIndex].type == DesktopWidgetType::CollectionGroup ||
            widgets_[widgetIndex].type == DesktopWidgetType::FileGroup)
        {
            widgets_[widgetIndex].showSearchBox =
                !widgets_[widgetIndex].showSearchBox;
            widgets_[widgetIndex].scrollOffset = 0;
            for (auto& c : containers_)
            {
                auto* scrolling = dynamic_cast<ScrollingItemWidget*>(c.get());
                if (scrolling &&
                    scrolling->GetWidgetData() == &widgets_[widgetIndex])
                {
                    if (!widgets_[widgetIndex].showSearchBox)
                        scrolling->ClearSearchText();
                    if (auto* categories =
                            dynamic_cast<FileCategories*>(scrolling))
                        categories->InvalidateCategoryCache();
                    else if (auto* mapping = dynamic_cast<FolderMapping*>(scrolling))
                        mapping->InvalidateFilterCache();
                    else if (auto* group = dynamic_cast<CollectionGroup*>(scrolling))
                        group->InvalidateFilterCache();
                    else if (auto* fileGroup =
                                 dynamic_cast<FileGroup*>(
                                     scrolling))
                        fileGroup->InvalidateHostedView();
                    break;
                }
            }
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        break;
    case kContextWidgetManualCollect:
        if (!CollectFileCategoryWidget(
                effectiveSourceIndex, true))
            MessageBeep(MB_ICONINFORMATION);
        break;
    case kContextWidgetRename:
        needsDesktopFocus = false;
        if (CanRenameWidget(widgets_[widgetIndex]))
        {
            SelectWidgetOnly(widgetIndex);
            BeginRenameSelected(dockRenameAnchor);
        }
        break;
    case kContextWidgetEdit:
        needsDesktopFocus = false;
        ShowWidgetEditorHost(widgetIndex);
        break;
    case kContextWidgetDelete:
    {
        // widgets_ 的下标会在删除后整体移动，不能让关闭动画继续引用
        // 即将失效的弹窗数据源。
        if (GetOpenPopupWidget())
        {
            CloseCollectionPopup();
            FinalizeCloseCollectionPopup();
        }
        const std::wstring deletedWidgetId = widgets_[widgetIndex].id;
        if (widgets_[widgetIndex].type == DesktopWidgetType::CollectionGroup)
        {
            if (popupWidgetIndex_ < widgets_.size() &&
                std::find(
                    widgets_[widgetIndex].childWidgetIds.begin(),
                    widgets_[widgetIndex].childWidgetIds.end(),
                    widgets_[popupWidgetIndex_].id) !=
                    widgets_[widgetIndex].childWidgetIds.end())
                CloseCollectionPopup();
            ReleaseCollectionGroupChildren(widgetIndex);
        }
        else if (widgets_[widgetIndex].type ==
                 DesktopWidgetType::FileGroup)
        {
            ReleaseFileGroupChildren(widgetIndex);
        }
        ReleaseDesktopItemsFromWidget(widgetIndex);
        if (widgets_[widgetIndex].type == DesktopWidgetType::LuaScript && widgetEngine_)
            widgetEngine_->DeleteWidgetInstance(widgets_[widgetIndex].id);
        for (auto& group : widgets_)
        {
            if (group.id == deletedWidgetId ||
                (group.type !=
                     DesktopWidgetType::CollectionGroup &&
                 group.type !=
                     DesktopWidgetType::FileGroup))
                continue;
            std::erase(
                group.childWidgetIds,
                deletedWidgetId);
            group.activeCategoryId =
                snowdesktop::collection_group_rules::
                    ResolveActiveItem(
                        group.childWidgetIds,
                        group.activeCategoryId);
        }
        // 成员落点已在删除前逐个分配；移除组件后重新进入桌面容器。
        widgets_.erase(widgets_.begin() + static_cast<std::ptrdiff_t>(widgetIndex));
        std::erase_if(dockEntries_, [&](const DockEntry& entry) {
            return (entry.type == DockEntryType::Collection ||
                    entry.type == DockEntryType::FolderMapping) &&
                entry.reference == deletedWidgetId;
        });
        EnsureNavTabOrder();
        // 删除组件可能使页面变空（溢出区空页后面有非空页时应立即清理顺延）
        ApplyPageMapping();
        LayoutItems();
        RebuildContainersAndItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    }
    case kContextPasteCommand:
        if (effectiveSourceIndex < widgets_.size())
        {
            if (widgets_[effectiveSourceIndex].type ==
                    DesktopWidgetType::FolderMapping &&
                !widgets_[effectiveSourceIndex].
                    sourceFolderPath.empty())
            {
                PasteClipboardToFolderMapping(
                    effectiveSourceIndex);
            }
            else if (widgets_[effectiveSourceIndex].type ==
                     DesktopWidgetType::FileCategories)
            {
                PasteClipboardToDesktop();
            }
        }
        break;
    case kContextNewMenu:
        if (effectiveSourceIndex < widgets_.size() &&
            widgets_[effectiveSourceIndex].type ==
                DesktopWidgetType::FolderMapping &&
            !widgets_[effectiveSourceIndex].
                sourceFolderPath.empty())
        {
            ShowNewMenuAndInvoke(screenPoint,
                widgets_[effectiveSourceIndex].
                    sourceFolderPath);
            RefreshFolderMappingWidget(
                effectiveSourceIndex);
            RebuildContainersAndItems();
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        else if (effectiveSourceIndex < widgets_.size() &&
                 widgets_[effectiveSourceIndex].type ==
                     DesktopWidgetType::FileCategories)
        {
            wchar_t desktopPath[MAX_PATH]{};
            if (SHGetSpecialFolderPathW(
                    nullptr, desktopPath,
                    CSIDL_DESKTOPDIRECTORY, FALSE))
            {
                ShowNewMenuAndInvoke(
                    screenPoint, desktopPath);
                ReloadItems();
            }
        }
        break;
    case kContextMoreCommand:
        needsDesktopFocus = false;
        if (effectiveSourceIndex < widgets_.size() &&
            widgets_[effectiveSourceIndex].type ==
                DesktopWidgetType::FolderMapping &&
            !widgets_[effectiveSourceIndex].
                sourceFolderPath.empty())
        {
            ShowShellContextMenuForPath(
                widgets_[effectiveSourceIndex].
                    sourceFolderPath, screenPoint);
        }
        break;
    case kContextWidgetSortByName:
        SortWidgetContents(contentSortTargetIndex, 0, true);
        break;
    case kContextWidgetSortByNameDesc:
        SortWidgetContents(contentSortTargetIndex, 0, false);
        break;
    case kContextWidgetSortByType:
        SortWidgetContents(contentSortTargetIndex, 1, true);
        break;
    case kContextWidgetSortByTypeDesc:
        SortWidgetContents(contentSortTargetIndex, 1, false);
        break;
    case kContextWidgetSortByDate:
        SortWidgetContents(contentSortTargetIndex, 2, true);
        break;
    case kContextWidgetSortByDateDesc:
        SortWidgetContents(contentSortTargetIndex, 2, false);
        break;
    case kContextWidgetSortBySize:
        SortWidgetContents(contentSortTargetIndex, 3, true);
        break;
    case kContextWidgetSortBySizeDesc:
        SortWidgetContents(contentSortTargetIndex, 3, false);
        break;
    case kContextWidgetShowOnHoverOn:
        widgets_[widgetIndex].showOnHoverOnly = true;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetShowOnHoverOff:
        widgets_[widgetIndex].showOnHoverOnly = false;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetKeepWhenHiddenOn:
        widgets_[widgetIndex].keepWhenDesktopHidden = true;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetKeepWhenHiddenOff:
        widgets_[widgetIndex].keepWhenDesktopHidden = false;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetPrivacyModeOn:
        widgets_[widgetIndex].privacyMode = true;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetPrivacyModeOff:
        widgets_[widgetIndex].privacyMode = false;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetToggleLargeFolderTitleless:
        if (widgets_[widgetIndex].type ==
            DesktopWidgetType::Collection)
        {
            collectionLargeFolderTitleless_ =
                !collectionLargeFolderTitleless_;
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        break;
    case kContextWidgetToggleCollectionMode:
        widgets_[widgetIndex].scrollContainerMode =
            !widgets_[widgetIndex].scrollContainerMode;
        if (!widgets_[widgetIndex].scrollContainerMode)
            widgets_[widgetIndex].scrollOffset = 0;
        for (auto& c : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (wc && wc->GetWidgetData() == &widgets_[widgetIndex])
            { wc->InvalidateSlots(); break; }
        }
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    default:
        break;
    }
    RestoreDesktopWindowLayer();
    if (needsDesktopFocus)
        RestoreInteractionInputFocus();
}

// Tray implementation lives in app_tray.cpp.
