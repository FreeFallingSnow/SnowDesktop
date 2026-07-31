#include "app.h"

// Grid adjustment and desktop-background context menus.

void DesktopApp::ShowGridAdjustmentMenu(POINT screenPoint, UINT initialCommand)
{
    struct MonitorSizeRange
    {
        const wchar_t* label;
        float representativeInches;
    };
    const MonitorSizeRange kMonitorSizeRanges[] = {
        { _LW("app.menu.monitor_1316"), 15.0f },
        { _LW("app.menu.monitor_1721"), 19.0f },
        { _LW("app.menu.monitor_2225"), 24.0f },
        { _LW("app.menu.monitor_2630"), 27.0f },
        { _LW("app.menu.monitor_31plus"), 34.0f },
    };

    auto isRecommendedCommand = [](UINT value) {
        return (value >= kContextGridRecommended169First &&
                value <= kContextGridRecommended169Last) ||
            (value >= kContextGridRecommended1610First &&
                value <= kContextGridRecommended1610Last);
    };
    auto recommendedDimensions = [&](UINT value) {
        const bool isSixteenTen = value >= kContextGridRecommended1610First;
        const UINT first = isSixteenTen
            ? kContextGridRecommended1610First
            : kContextGridRecommended169First;
        const size_t rangeIndex = static_cast<size_t>(value - first);
        return CalculateRecommendedGridDimensions(
            16, isSixteenTen ? 10 : 9,
            kMonitorSizeRanges[rangeIndex].representativeInches);
    };

    UINT command = initialCommand;
    while (true)
    {
        const bool validCommand =
            command == 0 ||
            command == kContextGridAddRow ||
            command == kContextGridRemoveRow ||
            command == kContextGridAddColumn ||
            command == kContextGridRemoveColumn ||
            isRecommendedCommand(command);
        if (!validCommand) break;

        if (isRecommendedCommand(command))
        {
            const GridSpan recommended = recommendedDimensions(command);
            SetGridDimensions(recommended.columns, recommended.rows);
        }
        switch (command)
        {
        case kContextGridAddRow: AdjustGridRows(1); break;
        case kContextGridRemoveRow: AdjustGridRows(-1); break;
        case kContextGridAddColumn: AdjustGridColumns(1); break;
        case kContextGridRemoveColumn: AdjustGridColumns(-1); break;
        default: break;
        }

        HMENU menu = CreatePopupMenu();
        if (!menu) break;

        POINT clientPoint = lastContextMenuScreenPoint_;
        ScreenToClient(hwnd_, &clientPoint);
        const GridPage* page = GridPageFromPoint(clientPoint);
        const std::wstring status = _LFW("app.menu.grid_current",
            std::to_wstring(page ? page->columns : 0),
            std::to_wstring(page ? page->rows : 0));

        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kContextGridAddRow, _LW("app.menu.add_row"));
        AppendMenuW(menu, MF_STRING, kContextGridRemoveRow, _LW("app.menu.remove_row"));
        AppendMenuW(menu, MF_STRING, kContextGridAddColumn, _LW("app.menu.add_col"));
        AppendMenuW(menu, MF_STRING, kContextGridRemoveColumn, _LW("app.menu.remove_col"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        auto appendRecommendedMenu = [&](HMENU submenu, int aspectHeight, UINT firstCommand) {
            if (!submenu) return;
            for (size_t i = 0; i < std::size(kMonitorSizeRanges); ++i)
            {
                const GridSpan recommended = CalculateRecommendedGridDimensions(
                    16, aspectHeight, kMonitorSizeRanges[i].representativeInches);
                const std::wstring label = _LFW("app.menu.grid_format",
                    kMonitorSizeRanges[i].label,
                    std::to_wstring(recommended.columns),
                    std::to_wstring(recommended.rows));
                UINT flags = MF_STRING;
                if (page &&
                    page->columns == recommended.columns &&
                    page->rows == recommended.rows)
                    flags |= MF_CHECKED;
                AppendMenuW(submenu, flags,
                    firstCommand + static_cast<UINT>(i), label.c_str());
                SetMenuItemIcon(submenu,
                    firstCommand + static_cast<UINT>(i), L"");
            }
        };

        HMENU recommended169Menu = CreatePopupMenu();
        HMENU recommended1610Menu = CreatePopupMenu();
        appendRecommendedMenu(recommended169Menu, 9, kContextGridRecommended169First);
        appendRecommendedMenu(recommended1610Menu, 10, kContextGridRecommended1610First);
        if (recommended169Menu)
            AppendMenuW(menu, MF_POPUP,
                reinterpret_cast<UINT_PTR>(recommended169Menu), _LW("app.menu.recommend_169"));
        if (recommended1610Menu)
            AppendMenuW(menu, MF_POPUP,
                reinterpret_cast<UINT_PTR>(recommended1610Menu), _LW("app.menu.recommend_1610"));

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kContextGridAdjustmentDone, _LW("app.menu.end_adjust"));

        SetMenuItemIcon(menu, kContextGridAddRow, L"");
        SetMenuItemIcon(menu, kContextGridRemoveRow, L"");
        SetMenuItemIcon(menu, kContextGridAddColumn, L"");
        SetMenuItemIcon(menu, kContextGridRemoveColumn, L"");
        SetMenuItemIcon(menu, kContextGridAdjustmentDone, L"");

        SetForegroundWindow(hwnd_);
        command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
            screenPoint.x, screenPoint.y, hwnd_, nullptr);
        FocusDesktopInputWindow();
        DestroyMenu(menu);
        ClearMenuIcons();
        if (command == 0 || command == kContextGridAdjustmentDone) break;
    }
    RestoreDesktopWindowLayer();
}

/**
 * @brief 显示桌面背景右键菜单。
 *        在屏幕坐标处弹出菜单，包含粘贴、新建、刷新、排序方式、
 *        行列调整、添加组件、图标间距等选项。菜单项均带图标。
 *        选中 Lua 组件或间距预设时直接处理，其余通过命令 ID 分发。
 * @param screenPoint 菜单弹出的屏幕坐标。
 */
void DesktopApp::ShowBackgroundContextMenu(POINT screenPoint)
{
    lastContextMenuScreenPoint_ = screenPoint;
    ClearMenuIcons();

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kContextPasteCommand, _LW("app.menu.paste"));
    AppendMenuW(menu, MF_STRING, kContextNewMenu, _LW("app.menu.new"));
    AppendMenuW(menu, MF_STRING, kContextRefreshCommand, _LW("app.menu.refresh"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kContextMoreCommand, _LW("app.menu.more_options"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU sortMenu = CreatePopupMenu();
    HMENU nameSortMenu = nullptr, typeSortMenu = nullptr;
    if (sortMenu)
    {
        nameSortMenu = CreatePopupMenu();
        if (nameSortMenu)
        {
            AppendMenuW(nameSortMenu, MF_STRING, kContextSortByNameCommand,     _LW("app.menu.sort_asc"));
            AppendMenuW(nameSortMenu, MF_STRING, kContextSortByNameDescCommand,     _LW("app.menu.sort_desc"));
            AppendMenuW(sortMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(nameSortMenu), _LW("app.menu.sort_name"));
        }
        typeSortMenu = CreatePopupMenu();
        if (typeSortMenu)
        {
            AppendMenuW(typeSortMenu, MF_STRING, kContextSortByTypeCommand, _LW("app.menu.sort_asc"));
            AppendMenuW(typeSortMenu, MF_STRING, kContextSortByTypeDescCommand, _LW("app.menu.sort_desc"));
            AppendMenuW(sortMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(typeSortMenu), _LW("app.menu.sort_type"));
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sortMenu), _LW("app.menu.sort_by"));
    }

    POINT clientPoint = screenPoint;
    ScreenToClient(hwnd_, &clientPoint);
    const GridPage* gridPage = GridPageFromPoint(clientPoint);

    HMENU displaySettingsMenu = CreatePopupMenu();
    if (displaySettingsMenu)
    {
        const std::wstring gridLabel = _LFW("app.menu.grid_adjust",
            std::to_wstring(gridPage ? gridPage->columns : 0),
            std::to_wstring(gridPage ? gridPage->rows : 0));
        AppendMenuW(displaySettingsMenu, MF_STRING, kContextGridAdjustmentMenu,
            gridLabel.c_str());

        HMENU spacingMenu = CreatePopupMenu();
        if (spacingMenu)
        {
            const int presets[] = { 50, 70, 80, 90, 100, 110, 120, 130, 150, 200 };
            const int currentSpacingPercent = static_cast<int>(
                std::round(iconSpacingScale_ * 100.0f));
            for (int pct : presets)
            {
                wchar_t label[16]{};
                swprintf_s(label, L"%d%%", pct);
                UINT flags = MF_STRING;
                if (currentSpacingPercent == pct) flags |= MF_CHECKED;
                AppendMenuW(spacingMenu, flags,
                    kContextSpacingPresetFirst + static_cast<UINT>(pct), label);
            }
            AppendMenuW(spacingMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(spacingMenu, MF_STRING, kContextSpacingIncrease, _LW("app.menu.inc_spacing"));
            AppendMenuW(spacingMenu, MF_STRING, kContextSpacingDecrease, _LW("app.menu.dec_spacing"));
            const std::wstring spacingLabel = _LFW("app.menu.icon_spacing_pct",
                std::to_wstring(currentSpacingPercent));
            AppendMenuW(displaySettingsMenu, MF_POPUP,
                reinterpret_cast<UINT_PTR>(spacingMenu), spacingLabel.c_str());
            SetMenuItemIcon(displaySettingsMenu, reinterpret_cast<UINT_PTR>(spacingMenu), L"");
            SetMenuItemIcon(spacingMenu, kContextSpacingIncrease, L"");
            SetMenuItemIcon(spacingMenu, kContextSpacingDecrease, L"");
        }

        HMENU fontSizeMenu = CreatePopupMenu();
        if (fontSizeMenu)
        {
            const int currentFontSize = static_cast<int>(std::round(itemFontSize_));
            auto addFontSizeItem = [&](UINT id, const wchar_t* label, int size) {
                UINT flags = MF_STRING;
                if (currentFontSize == size) flags |= MF_CHECKED;
                AppendMenuW(fontSizeMenu, flags, id, label);
            };
            addFontSizeItem(kContextFontSizeSmall, _LW("app.menu.font_small"), 12);
            addFontSizeItem(kContextFontSizeMedium, _LW("app.menu.font_medium"), 15);
            addFontSizeItem(kContextFontSizeLarge, _LW("app.menu.font_large"), 16);
            const std::wstring fontSizeLabel = _LFW("app.menu.title_font_size_pt",
                std::to_wstring(currentFontSize));
            AppendMenuW(displaySettingsMenu, MF_POPUP,
                reinterpret_cast<UINT_PTR>(fontSizeMenu), fontSizeLabel.c_str());
            SetMenuItemIcon(displaySettingsMenu, reinterpret_cast<UINT_PTR>(fontSizeMenu), L"");
        }

        HMENU fontWeightMenu = CreatePopupMenu();
        if (fontWeightMenu)
        {
            auto addWeightItem = [&](UINT id, const wchar_t* label, DWRITE_FONT_WEIGHT weight) {
                UINT flags = MF_STRING;
                if (itemFontWeight_ == weight) flags |= MF_CHECKED;
                AppendMenuW(fontWeightMenu, flags, id, label);
            };
            addWeightItem(kContextFontWeightBold, _LW("app.menu.font_weight_bold_label"), DWRITE_FONT_WEIGHT_BOLD);
            addWeightItem(kContextFontWeightMedium, _LW("app.menu.font_weight_medium_label"), DWRITE_FONT_WEIGHT_SEMI_BOLD);
            addWeightItem(kContextFontWeightFine, _LW("app.menu.font_weight_light_label"), DWRITE_FONT_WEIGHT_NORMAL);
            const wchar_t* weightLabel = _LW("app.menu.font_weight_medium");
            if (itemFontWeight_ == DWRITE_FONT_WEIGHT_BOLD)
                weightLabel = _LW("app.menu.font_weight_bold");
            else if (itemFontWeight_ == DWRITE_FONT_WEIGHT_NORMAL)
                weightLabel = _LW("app.menu.font_weight_light");
            AppendMenuW(displaySettingsMenu, MF_POPUP,
                reinterpret_cast<UINT_PTR>(fontWeightMenu), weightLabel);
            SetMenuItemIcon(displaySettingsMenu, reinterpret_cast<UINT_PTR>(fontWeightMenu), L"");
        }

        AppendMenuW(menu, MF_POPUP,
            reinterpret_cast<UINT_PTR>(displaySettingsMenu), _LW("app.menu.display_settings"));
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(displaySettingsMenu), L"");
        SetMenuItemIcon(displaySettingsMenu, kContextGridAdjustmentMenu, L"");
    }

    std::vector<std::wstring> luaWidgets = WidgetEngine::ListAvailable();
    HMENU widgetMenu = CreatePopupMenu();
    if (widgetMenu)
    {
        AppendMenuW(widgetMenu, MF_STRING, kContextAddCollectionWidget, _LW("app.menu.collection"));
        AppendMenuW(widgetMenu, MF_STRING, kContextAddFileCategoryWidget, _LW("app.menu.file_categories"));
        AppendMenuW(widgetMenu, MF_STRING, kContextAddFolderMappingWidget, _LW("app.menu.folder_mapping"));
        AppendMenuW(widgetMenu, MF_STRING, kContextAddCollectionGroupWidget, _LW("app.menu.collection_group"));
        AppendMenuW(widgetMenu, MF_STRING, kContextAddFileGroupWidget, _LW("app.menu.file_group"));
        if (!luaWidgets.empty())
        {
            AppendMenuW(widgetMenu, MF_SEPARATOR, 0, nullptr);
            for (size_t i = 0; i < luaWidgets.size() && i < 48; ++i)
            {
                std::wstring label = WidgetEngine::GetWidgetDisplayName(luaWidgets[i]);
                if (label.empty()) label = luaWidgets[i];
                AppendMenuW(widgetMenu, MF_STRING,
                    kContextAddLuaWidgetFirst + static_cast<UINT>(i), label.c_str());
            }
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(widgetMenu), _LW("app.menu.add_widget"));
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // ── 条件：分页导航 ──
    const GridPage* clickedPage = GridPageFromPoint(clientPoint);
    std::wstring clickedPageId = clickedPage ? clickedPage->id : L"";
    std::wstring clickedMonitorId = clickedPage ? clickedPage->monitorId : L"";
    std::wstring firstPageId = savedPageIds_.empty() ? L"" : savedPageIds_[0];
    bool isFirstPage = !clickedPageId.empty() && clickedPageId == firstPageId;
    int maxOff = MaxPageOffset();
    const size_t monitorCount = gridPages_.size();
    // 单物理屏同时承担首屏和末屏，也应提供末屏的分页导航菜单。
    const bool showPageNavigation = !isFirstPage || monitorCount == 1;

    // ── 首屏/末屏锁定开关（持久化、互斥，仅多屏时显示） ──
    HMENU pinPageMenu = nullptr;
    if (monitorCount >= 2)
    {
        pinPageMenu = CreatePopupMenu();
        if (pinPageMenu)
        {
            UINT fFlags = MF_STRING;
            if (!firstPageMonitorId_.empty() && firstPageMonitorId_ == clickedMonitorId)
                fFlags |= MF_CHECKED;
            AppendMenuW(pinPageMenu, fFlags, kContextPinFirstPage, _LW("app.menu.page_pin_first"));

            UINT lFlags = MF_STRING;
            if (!lastPageMonitorId_.empty() && lastPageMonitorId_ == clickedMonitorId)
                lFlags |= MF_CHECKED;
            AppendMenuW(pinPageMenu, lFlags, kContextPinLastPage, _LW("app.menu.page_pin_last"));

            AppendMenuW(menu, MF_POPUP,
                reinterpret_cast<UINT_PTR>(pinPageMenu), _LW("app.menu.page_pin_both"));
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    HMENU jumpMenu = nullptr;
    if (showPageNavigation)
    {
        // 当前右键点击的显示器显示的页索引
        int clickedPageIdx = 0;
        {
            auto it = std::ranges::find(savedPageIds_, clickedPageId);
            if (it != savedPageIds_.end())
                clickedPageIdx = static_cast<int>(it - savedPageIds_.begin());
        }

        if (pageOffset_ > 0)
            AppendMenuW(menu, MF_STRING, kContextPagePrev, _LW("app.menu.prev_page"));
        if (pageOffset_ < maxOff)
            AppendMenuW(menu, MF_STRING, kContextPageNext, _LW("app.menu.next_page"));

        jumpMenu = CreatePopupMenu();
        if (jumpMenu)
        {
            // 计算当前所有显示器上显示的页面索引
            std::vector<size_t> monitorOrder = BuildMonitorRenderOrder();
            std::unordered_set<int> pagesOnMonitors;
            for (size_t mi = 0; mi < monitorOrder.size(); ++mi)
            {
                int displayIdx = static_cast<int>(mi);
                if (mi == monitorOrder.size() - 1)
                    displayIdx += pageOffset_;
                if (displayIdx < static_cast<int>(savedPageIds_.size()))
                    pagesOnMonitors.insert(displayIdx);
            }


            for (int i = 0; static_cast<size_t>(i) < savedPageIds_.size(); ++i)
            {
                if (!PageHasContent(savedPageIds_[i]) && !pagesOnMonitors.contains(i)) continue;
                std::wstring label = GetPageDisplayName(i);
                UINT flags = MF_STRING;
                if (pagesOnMonitors.contains(i))
                    flags |= MF_GRAYED;
                if (i == clickedPageIdx)
                    flags |= MF_CHECKED;
                AppendMenuW(jumpMenu, flags,
                    kContextPageJumpFirst + static_cast<UINT>(i), label.c_str());
            }
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(jumpMenu), _LW("app.menu.goto_page"));
        }

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    AppendMenuW(menu, MF_STRING, kContextPageAdd, _LW("app.menu.add_page"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kContextSettingsCommand, _LW("app.menu.settings"));

    SetMenuItemIcon(menu, kContextNewMenu, L"");
    SetMenuItemIcon(menu, kContextRefreshCommand, L"");
    SetMenuItemIcon(menu, kContextPasteCommand, L"");
    SetMenuItemIcon(menu, kContextMoreCommand, L"");
    if (sortMenu)
    {
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(sortMenu), L"");
        if (nameSortMenu)
        {
            SetMenuItemIcon(sortMenu, reinterpret_cast<UINT_PTR>(nameSortMenu), L"");
            SetMenuItemIcon(nameSortMenu, kContextSortByNameCommand, L"");
            SetMenuItemIcon(nameSortMenu, kContextSortByNameDescCommand, L"");
        }
        if (typeSortMenu)
        {
            SetMenuItemIcon(sortMenu, reinterpret_cast<UINT_PTR>(typeSortMenu), L"");
            SetMenuItemIcon(typeSortMenu, kContextSortByTypeCommand, L"");
            SetMenuItemIcon(typeSortMenu, kContextSortByTypeDescCommand, L"");
        }
    }
    if (widgetMenu)
    {
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(widgetMenu), L"");
        SetMenuItemIcon(widgetMenu, kContextAddCollectionWidget, L"");
        SetMenuItemIcon(widgetMenu, kContextAddCollectionGroupWidget, L"");
        SetMenuItemIcon(widgetMenu, kContextAddFileGroupWidget, L"");
        SetMenuItemIcon(widgetMenu, kContextAddFileCategoryWidget, L"");
        SetMenuItemIcon(widgetMenu, kContextAddFolderMappingWidget, L"");
    }
    if (pinPageMenu)
    {
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(pinPageMenu), L"");
    }
    SetMenuItemIcon(menu, kContextSettingsCommand, L"");
    if (pageOffset_ > 0)
        SetMenuItemIcon(menu, kContextPagePrev, L"");
    if (pageOffset_ < maxOff)
        SetMenuItemIcon(menu, kContextPageNext, L"");
    SetMenuItemIcon(menu, kContextPageAdd, L"");
    if (jumpMenu)
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(jumpMenu), L"");

    gridAdjustmentParentMenu_ = displaySettingsMenu;
    gridAdjustmentMenuAnchorValid_ = false;
    SetForegroundWindow(hwnd_);
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, hwnd_, nullptr);
    FocusDesktopInputWindow();

    gridAdjustmentParentMenu_ = nullptr;
    POINT adjustmentMenuPoint = gridAdjustmentMenuAnchorValid_
        ? gridAdjustmentMenuAnchor_ : screenPoint;

    if (sortMenu) DestroyMenu(sortMenu);
    if (widgetMenu) DestroyMenu(widgetMenu);
    if (displaySettingsMenu) DestroyMenu(displaySettingsMenu);
    if (pinPageMenu) DestroyMenu(pinPageMenu);
    if (jumpMenu) DestroyMenu(jumpMenu);
    DestroyMenu(menu);
    newMenuContextMenu_.Reset();
    ClearMenuIcons();
    RestoreDesktopWindowLayer();

    if (command >= kContextAddLuaWidgetFirst &&
        command < kContextAddLuaWidgetFirst + static_cast<UINT>(std::min<size_t>(luaWidgets.size(), 48)))
    {
        size_t scriptIndex = static_cast<size_t>(command - kContextAddLuaWidgetFirst);
        AddLuaWidgetAt(screenPoint, luaWidgets[scriptIndex]);
    }
    else if (command >= kContextSpacingPresetFirst &&
        command <= kContextSpacingPresetFirst + 200)
    {
        SetIconSpacing(
            static_cast<float>(command - kContextSpacingPresetFirst) / 100.0f);
    }
    else if (command >= kContextPageJumpFirst && command <= kContextPageJumpLast)
    {
        int pageIdx = static_cast<int>(command - kContextPageJumpFirst);
        if (pageIdx >= 0 && static_cast<size_t>(pageIdx) < savedPageIds_.size())
        {
            int visiblePageCount = static_cast<int>(std::min(savedPageIds_.size(), gridPages_.size()));
            int targetOffset = pageIdx - (visiblePageCount - 1);
            JumpToPageOffset(targetOffset);
        }
    }
    else switch (command)
    {
    case kContextRefreshCommand: ReloadItems(); break;
    case kContextSortByNameCommand: SortIconsByName(true); break;
    case kContextSortByNameDescCommand: SortIconsByName(false); break;
    case kContextSortByTypeCommand: SortIconsByType(true); break;
    case kContextSortByTypeDescCommand: SortIconsByType(false); break;
    case kContextGridAdjustmentMenu:
        ShowGridAdjustmentMenu(adjustmentMenuPoint, 0);
        break;
    case kContextGridAddRow:
    case kContextGridRemoveRow:
    case kContextGridAddColumn:
    case kContextGridRemoveColumn:
    {
        POINT legacyAdjustmentMenuPoint{};
        GetCursorPos(&legacyAdjustmentMenuPoint);
        ShowGridAdjustmentMenu(legacyAdjustmentMenuPoint, command);
        break;
    }
    case kContextSpacingIncrease: AdjustIconSpacing(+0.1f); break;
    case kContextSpacingDecrease: AdjustIconSpacing(-0.1f); break;
    case kContextPinFirstPage: ToggleFirstPagePin(screenPoint); break;
    case kContextPinLastPage:  ToggleLastPagePin(screenPoint);  break;
    case kContextAddCollectionWidget: AddCollectionWidgetAt(screenPoint); break;
    case kContextAddCollectionGroupWidget: AddCollectionGroupWidgetAt(screenPoint); break;
    case kContextAddFileGroupWidget: AddFileGroupWidgetAt(screenPoint); break;
    case kContextAddFileCategoryWidget: AddFileCategoryWidgetAt(screenPoint); break;
    case kContextAddFolderMappingWidget: AddFolderMappingWidgetAt(screenPoint); break;
    case kContextNewMenu:
    {
        wchar_t desktopPath[MAX_PATH]{};
        if (SHGetSpecialFolderPathW(nullptr, desktopPath, CSIDL_DESKTOPDIRECTORY, FALSE))
        {
            ShowNewMenuAndInvoke(screenPoint, desktopPath);
            ReloadItems();
        }
        break;
    }
    case kContextPasteCommand:
    {
        bool fromDesktop = false;
        std::unordered_set<std::wstring> clipPaths;

        ComPtr<IDataObject> clipObj;
        if (SUCCEEDED(OleGetClipboard(&clipObj)) && clipObj)
        {
            CLIPFORMAT cfPreferred = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
            FORMATETC fmtPref{ cfPreferred, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
            STGMEDIUM medPref{};
            if (SUCCEEDED(clipObj->GetData(&fmtPref, &medPref)) && medPref.hGlobal)
            {
                DWORD* pEffect = static_cast<DWORD*>(GlobalLock(medPref.hGlobal));
                bool isMove = pEffect && (*pEffect & DROPEFFECT_MOVE);
                if (pEffect) GlobalUnlock(medPref.hGlobal);
                if (isMove)
                {
                    FORMATETC fmtDrop{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
                    STGMEDIUM medDrop{};
                    if (SUCCEEDED(clipObj->GetData(&fmtDrop, &medDrop)) && medDrop.hGlobal)
                    {
                        HDROP hDrop = static_cast<HDROP>(medDrop.hGlobal);
                        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
                        for (UINT i = 0; i < count; ++i)
                        {
                            wchar_t path[MAX_PATH]{};
                            if (DragQueryFileW(hDrop, i, path, MAX_PATH) > 0)
                                clipPaths.insert(path);
                        }
                        ReleaseStgMedium(&medDrop);
                    }
                }
                ReleaseStgMedium(&medPref);
            }
        }

        if (!clipPaths.empty())
        {
            for (const auto& item : items_)
            {
                wchar_t path[MAX_PATH]{};
                if (SHGetPathFromIDListW(item.absolutePidl.get(), path) && clipPaths.contains(path))
                {
                    fromDesktop = true;
                    break;
                }
            }
        }

        if (fromDesktop)
        {
            cutPaths_.clear();
            if (OpenClipboard(hwnd_))
            {
                EmptyClipboard();
                CloseClipboard();
            }
            UpdateCutState();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        else
        {
            ComPtr<IContextMenu> bgMenu;
            if (SUCCEEDED(desktopFolder_->CreateViewObject(hwnd_, IID_IContextMenu,
                reinterpret_cast<void**>(bgMenu.GetAddressOf()))) && bgMenu)
            {
                CMINVOKECOMMANDINFO info{};
                info.cbSize = sizeof(info);
                info.hwnd = ShellDialogOwnerHwnd();
                info.lpVerb = "paste";
                info.nShow = SW_SHOWNORMAL;
                SafeInvokeCommand(bgMenu.Get(), &info);
                ReloadItems();
            }
        }
        break;
    }
    case kContextMoreCommand:
        ShowDesktopBackgroundContextMenu(screenPoint);
        break;
    case kContextSettingsCommand: ShowSettingsWindow(); break;
    case kContextFontSizeSmall: SetItemFontSize(12.0f); break;
    case kContextFontSizeMedium: SetItemFontSize(15.0f); break;
    case kContextFontSizeLarge: SetItemFontSize(16.0f); break;
    case kContextFontWeightBold: SetItemFontWeight(DWRITE_FONT_WEIGHT_BOLD); break;
    case kContextFontWeightMedium: SetItemFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD); break;
    case kContextFontWeightFine: SetItemFontWeight(DWRITE_FONT_WEIGHT_NORMAL); break;
    case kContextPagePrev: NavigatePageOffset(-1); break;
    case kContextPageNext: NavigatePageOffset(1); break;
    case kContextPageAdd: AddNewPage(); break;
    default: break;
    }
}

/**
 * @brief 显示桌面图标（文件/快捷方式）的右键菜单。
 *        包含打开、重命名、剪切、复制、删除及"展开更多选项"。
 *        仅在选中项为文件系统项时可操作（非系统图标如此电脑等）。
 *        剪切/复制通过 IDataObject 与 OLE 剪贴板交互。
 * @param screenPoint 菜单弹出的屏幕坐标。
 * @param itemIndex   当前右键点击的桌面项索引。
 */
