#include "app.h"
#include "../menu_fluent_glyphs.h"
#include "../widgets/collection_group_rules.h"

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
    FocusDesktopInputWindow();
    DestroyMenu(menu);
    ClearMenuIcons();

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
    FocusDesktopInputWindow();
    DestroyMenu(menu);
    ClearMenuIcons();

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
}

void DesktopApp::ShowWidgetContextMenu(
    POINT screenPoint, size_t widgetIndex,
    std::optional<RECT> dockRenameAnchor)
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
    const std::wstring categoryVisibilityLabel = visibilityLabel(
        _LW("app.interact.file_categories"),
        widget.showFileCategories);
    const std::wstring searchVisibilityLabel = visibilityLabel(
        _LW("app.interact.search_box"),
        widget.showSearchBox);
    const std::wstring dateVisibilityLabel = visibilityLabel(
        _LW("app.interact.date_header"),
        widget.dateHeaders);
    std::vector<LuaWidgetMenuItem> luaMenuItems;
    HMENU displayModeMenu = nullptr;

    if (widget.type == DesktopWidgetType::Collection)
    {
        AppendMenuW(menu, MF_STRING, kContextWidgetOpen, _LW("app.interact.open_all"));
        displayModeMenu = CreatePopupMenu();
        if (displayModeMenu)
        {
            AppendMenuW(displayModeMenu, MF_STRING | (!widget.scrollContainerMode ? MF_CHECKED : 0),
                kContextWidgetCollModeLargeFolder, _LW("app.interact.large_folder"));
            AppendMenuW(displayModeMenu, MF_STRING | (widget.scrollContainerMode ? MF_CHECKED : 0),
                kContextWidgetCollModeScrollContainer, _LW("app.interact.popup_container"));
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(displayModeMenu), _LW("app.interact.display_mode"));
        }
        if (widget.scrollContainerMode)
        {
            AppendMenuW(menu, MF_STRING, kContextWidgetToggleListMode,
                displayTypeLabel.c_str());
        }
    }
    else if (widget.type == DesktopWidgetType::CollectionGroup)
    {
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleListMode,
            displayTypeLabel.c_str());
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleSearchBox,
            searchVisibilityLabel.c_str());
    }
    else if (widget.type == DesktopWidgetType::FileGroup)
    {
        if (effectiveSource.type ==
            DesktopWidgetType::FileCategories)
        {
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
        }
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleListMode,
            displayTypeLabel.c_str());
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
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleDateGroup,
            dateVisibilityLabel.c_str());
    }
    else if (widget.type == DesktopWidgetType::FolderMapping)
    {
        AppendMenuW(menu, MF_STRING, kContextWidgetOpenFolder, _LW("app.interact.open_folder"));
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleListMode,
            displayTypeLabel.c_str());
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
        AppendMenuW(menu, MF_STRING, kContextWidgetEdit, _LW("app.interact.detailed_settings"));
        if (widgetEngine_)
        {
            widgetEngine_->EnsureWidgetLoaded(widget.id, widget.packageId);
            luaMenuItems = widgetEngine_->GetContextMenu(widget.id);
            for (size_t i = 0; i < luaMenuItems.size() &&
                kContextLuaWidgetMenuFirst + static_cast<UINT>(i) <= kContextLuaWidgetMenuLast; ++i)
            {
                const auto& item = luaMenuItems[i];
                if (item.separator)
                {
                    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                    continue;
                }
                UINT flags = MF_STRING | (item.enabled ? 0 : MF_GRAYED);
                AppendMenuW(menu, flags,
                    kContextLuaWidgetMenuFirst + static_cast<UINT>(i),
                    Utf8ToWide(item.label).c_str());
                if (!item.icon.empty())
                {
                    std::wstring icon = Utf8ToWide(item.icon);
                    const bool useFluent =
                        _stricmp(item.iconFont.c_str(), "fluent") == 0 ||
                        _stricmp(item.iconFont.c_str(), "fluent-regular") == 0;
                    SetMenuItemIcon(menu,
                        kContextLuaWidgetMenuFirst + static_cast<UINT>(i),
                        icon.c_str(),
                        useFluent
                            ? MenuIconFont::FluentRegular
                            : MenuIconFont::FontAwesomeSolid);
                }
            }
        }
    }

    HMENU sortMenu = nullptr, wNameMenu = nullptr, wTypeMenu = nullptr, wDateMenu = nullptr;
    if ((widget.type == DesktopWidgetType::FileCategories ||
        widget.type == DesktopWidgetType::FolderMapping ||
        widget.type == DesktopWidgetType::Collection ||
        widget.type == DesktopWidgetType::CollectionGroup ||
        widget.type == DesktopWidgetType::FileGroup) &&
        (widget.type == DesktopWidgetType::CollectionGroup ||
            widget.type == DesktopWidgetType::FileGroup ||
            !widget.dateHeaders))
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
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sortMenu), _LW("app.menu.sort_by"));
        }
    }

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
    const UINT hoverToggleCommand = widget.showOnHoverOnly
        ? kContextWidgetShowOnHoverOff
        : kContextWidgetShowOnHoverOn;
    const UINT keepToggleCommand = widget.keepWhenDesktopHidden
        ? kContextWidgetKeepWhenHiddenOff
        : kContextWidgetKeepWhenHiddenOn;
    const UINT privacyToggleCommand = widget.privacyMode
        ? kContextWidgetPrivacyModeOff
        : kContextWidgetPrivacyModeOn;
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
    AppendMenuW(menu, MF_STRING, kContextWidgetDelete, _LW("app.interact.delete_widget"));

    setFluentIcon(menu, kContextWidgetOpen, L"\uF582");
    setFluentIcon(menu, kContextWidgetManualCollect, L"\uF150");
    setFluentIcon(menu, kContextWidgetToggleAutoCollect, L"\uF190");
    setFluentIcon(menu, kContextWidgetToggleListMode,
        widget.listMode ? L"\uF4ED" : L"\uF462");
    setFluentIcon(menu, kContextWidgetToggleDateGroup,
        snowdesktop::menu_fluent_glyphs::kDateHeader);
    setFluentIcon(menu, kContextWidgetOpenFolder, L"\uF42E");
    setFluentIcon(menu, kContextWidgetToggleFileCategories,
        L"\U000F0129");
    setFluentIcon(menu, kContextWidgetToggleSearchBox, L"\uF68F");
    setFluentIcon(menu, kContextNewMenu,
        snowdesktop::menu_fluent_glyphs::kNewItem);
    setFluentIcon(menu, kContextMoreCommand,
        snowdesktop::menu_fluent_glyphs::kMoreOptions);
    setFluentIcon(menu, kContextWidgetEdit, L"\uF6A9");
    setFluentIcon(menu, kContextWidgetRename, L"\U000F0A39");
    setFluentIcon(menu, kContextWidgetDelete, L"\uF34C");
    SetMenuItemQuickAction(menu, kContextWidgetEdit);
    SetMenuItemQuickAction(menu, kContextNewMenu);
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
    }
    if (displayModeMenu)
    {
        setFluentIcon(menu,
            reinterpret_cast<UINT_PTR>(displayModeMenu), L"\uF133");
        setFluentIcon(displayModeMenu,
            kContextWidgetCollModeLargeFolder, L"\uF418");
        setFluentIcon(displayModeMenu,
            kContextWidgetCollModeScrollContainer, L"\uF8CB");
    }

    SetForegroundWindow(hwnd_);
    UINT command = ShowModernMenu(
        menu, screenPoint, hwnd_, dockRenameAnchor.has_value());
    FocusDesktopInputWindow();
    DestroyMenu(menu);
    ClearMenuIcons();

    if (command >= kContextLuaWidgetMenuFirst && command <= kContextLuaWidgetMenuLast)
    {
        size_t itemIndex = static_cast<size_t>(command - kContextLuaWidgetMenuFirst);
        if (itemIndex < luaMenuItems.size() && widgetEngine_)
        {
            widgetEngine_->InvokeMenu(widgets_[widgetIndex].id, luaMenuItems[itemIndex].id);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }

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
        if (effectiveSource.type ==
                DesktopWidgetType::FolderMapping &&
            !effectiveSource.sourceFolderPath.empty())
            ShellExecuteW(hwnd_, L"open",
                effectiveSource.sourceFolderPath.c_str(),
                nullptr, nullptr, SW_SHOWNORMAL);
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
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
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
                    if (auto* mapping =
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
        if (widgets_[widgetIndex].type == DesktopWidgetType::FolderMapping ||
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
                    if (auto* mapping = dynamic_cast<FolderMapping*>(scrolling))
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
        if (CanRenameWidget(widgets_[widgetIndex]))
        {
            SelectWidgetOnly(widgetIndex);
            BeginRenameSelected(dockRenameAnchor);
        }
        break;
    case kContextWidgetEdit:
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
        break;
    case kContextMoreCommand:
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
        SortWidgetContents(effectiveSourceIndex, 0, true);
        break;
    case kContextWidgetSortByNameDesc:
        SortWidgetContents(effectiveSourceIndex, 0, false);
        break;
    case kContextWidgetSortByType:
        SortWidgetContents(effectiveSourceIndex, 1, true);
        break;
    case kContextWidgetSortByTypeDesc:
        SortWidgetContents(effectiveSourceIndex, 1, false);
        break;
    case kContextWidgetSortByDate:
        SortWidgetContents(effectiveSourceIndex, 2, true);
        break;
    case kContextWidgetSortByDateDesc:
        SortWidgetContents(effectiveSourceIndex, 2, false);
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
    case kContextWidgetCollModeLargeFolder:
        widgets_[widgetIndex].scrollContainerMode = false;
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
    case kContextWidgetCollModeScrollContainer:
        widgets_[widgetIndex].scrollContainerMode = true;
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
}

// Tray implementation lives in app_tray.cpp.
