/**
 * @file app_menu.h
 * @brief DesktopApp 的上下文菜单相关内联实现。
 *
 * 该文件在 app_oo.h 中类定义之后被包含，提供桌面背景菜单、图标右键菜单、
 * Shell 扩展菜单、新建菜单以及菜单图标位图创建等功能。
 * 所有菜单均使用 TrackPopupMenuEx 以右键菜单方式弹出，并支持图标渲染。
 */
#pragma once
#include "../crashlog.h"

/**
 * @brief 根据文本创建菜单图标位图（使用 DIB 段）。
 *        将文本渲染到透明位图上，再提取亮度通道作为 alpha 通道，
 *        生成适合菜单图标使用的灰度位图。
 * @param text 要渲染的文本内容（通常为 Segoe MDL2 Assets 图标字符）。
 * @return 成功返回创建的位图句柄，失败返回 nullptr。
 */
inline HBITMAP DesktopApp::CreateMenuIconBitmap(const wchar_t* text)
{
    const int cx = std::max(20, GetSystemMetrics(SM_CXMENUCHECK));
    const int cy = std::max(20, GetSystemMetrics(SM_CYMENUCHECK));
    if (cx <= 0 || cy <= 0 || !text || !*text) return nullptr;

    HDC screenDc = GetDC(nullptr);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = cx;
    bmi.bmiHeader.biHeight = -cy;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp)
    {
        ReleaseDC(nullptr, screenDc);
        return nullptr;
    }

    std::fill_n(static_cast<std::uint32_t*>(bits), cx * cy, 0u);

    HDC memDc = CreateCompatibleDC(screenDc);
    HGDIOBJ oldBmp = SelectObject(memDc, bmp);
    HGDIOBJ fallbackFont = GetStockObject(DEFAULT_GUI_FONT);
    HGDIOBJ oldFont = SelectObject(memDc, faMenuFont_ ? faMenuFont_ : fallbackFont);

    RECT rc{ 0, 0, cx, cy };
    SetBkMode(memDc, TRANSPARENT);
    SetTextColor(memDc, RGB(255, 255, 255));
    DrawTextW(memDc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(memDc, oldFont);
    SelectObject(memDc, oldBmp);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);

    auto* pixels = static_cast<std::uint32_t*>(bits);
    const size_t count = static_cast<size_t>(cx) * static_cast<size_t>(cy);
    for (size_t i = 0; i < count; ++i)
    {
        std::uint32_t p = pixels[i];
        if ((p & 0x00FFFFFF) == 0) continue;
        std::uint8_t lum = static_cast<std::uint8_t>(
            std::max({ (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF }));
        pixels[i] = static_cast<std::uint32_t>(lum) << 24;
    }
    return bmp;
}

/**
 * @brief 为指定菜单项设置位图图标。
 *        通过 CreateMenuIconBitmap 创建图标位图后，使用 MIIM_BITMAP
 *        将位图关联到菜单项上。创建的位图由 menuIconPool_ 统一管理。
 * @param menu    目标菜单句柄。
 * @param command 菜单项的 ID（或子菜单句柄）。
 * @param text    用于生成图标的文本（图标字符）。
 */
inline void DesktopApp::SetMenuItemIcon(HMENU menu, UINT_PTR command, const wchar_t* text)
{
    HBITMAP icon = CreateMenuIconBitmap(text);
    if (!icon) return;

    MENUITEMINFOW mii{ sizeof(mii) };
    mii.fMask = MIIM_BITMAP;
    mii.hbmpItem = icon;

    bool applied = false;
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count && !applied; ++i)
    {
        MENUITEMINFOW probe{ sizeof(probe) };
        probe.fMask = MIIM_ID | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &probe)) continue;
        if (probe.wID == command || reinterpret_cast<UINT_PTR>(probe.hSubMenu) == command)
            applied = SetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &mii) != FALSE;
    }

    if (applied)
        menuIconPool_.push_back(icon);
    else
        DeleteObject(icon);
}

/**
 * @brief 清除所有缓存的菜单图标位图。
 *        遍历 menuIconPool_ 逐一 DeleteObject 释放 GDI 资源，然后清空容器。
 */
inline void DesktopApp::ClearMenuIcons()
{
    for (HBITMAP bmp : menuIconPool_)
        DeleteObject(bmp);
    menuIconPool_.clear();
}

inline void DesktopApp::ShowDockContextMenu(POINT screenPoint)
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

inline void DesktopApp::ShowDockRunningAppContextMenu(
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
inline void DesktopApp::ShowGridAdjustmentMenu(POINT screenPoint, UINT initialCommand)
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
inline void DesktopApp::ShowBackgroundContextMenu(POINT screenPoint)
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
inline void DesktopApp::ShowItemContextMenu(
    POINT screenPoint, int itemIndex, bool dockFrequentItem,
    bool keepQuickNavigationOpen,
    std::optional<RECT> dockRenameAnchor,
    std::optional<size_t> dockMappingEntryIndex,
    bool dockApplicationItem)
{
    if (itemIndex < 0 || static_cast<size_t>(itemIndex) >= items_.size()) return;
    ClearMenuIcons();

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

    int selectedCount = 0;
    for (const auto& item : items_) if (item.selected) ++selectedCount;

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
    const bool canCloseDockApplication =
        dockApplicationItem &&
        GetDockWindowVisualState(
            static_cast<size_t>(itemIndex)) !=
            DockWindowVisualState::Closed;

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, selectedCount == 1 ? MF_STRING : MF_STRING | MF_GRAYED, kContextOpenCommand, _LW("app.menu.open"));
    AppendMenuW(menu, canReveal ? MF_STRING : MF_STRING | MF_GRAYED,
        kContextRevealLocationCommand, _LW("app.menu.open_file_location"));
    AppendMenuW(menu, selectedCount == 1 && canFile ? MF_STRING : MF_STRING | MF_GRAYED, kContextRenameCommand, _LW("app.menu.rename"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, canFile ? MF_STRING : MF_STRING | MF_GRAYED, kContextCutCommand, _LW("app.menu.cut"));
    AppendMenuW(menu, canFile ? MF_STRING : MF_STRING | MF_GRAYED, kContextCopyCommand, _LW("app.menu.copy"));
    AppendMenuW(menu,
        (canFile || dockMapping)
            ? MF_STRING
            : MF_STRING | MF_GRAYED,
        kContextDeleteCommand,
        dockMapping
            ? _LW("app.dock.remove_mapping")
            : _LW("app.settings.delete"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kContextMoreCommand, _LW("app.menu.more_options"));
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
    SetMenuItemIcon(menu, kContextRevealLocationCommand, L"");
    SetMenuItemIcon(menu, kContextRenameCommand, L"");
    SetMenuItemIcon(menu, kContextCutCommand, L"");
    SetMenuItemIcon(menu, kContextCopyCommand, L"");
    SetMenuItemIcon(menu, kContextDeleteCommand, L"");
    SetMenuItemIcon(menu, kContextMoreCommand, L"");
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
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, menuOwner, nullptr);
    if (!keepQuickNavigationOpen)
        FocusDesktopInputWindow();
    DestroyMenu(menu);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();

    switch (command)
    {
    case kContextOpenCommand:
    {
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
    case kContextRenameCommand:
        if (keepQuickNavigationOpen)
            BeginQuickNavigationDesktopItemRename(
                static_cast<size_t>(itemIndex));
        else
            BeginRenameSelected(dockRenameAnchor);
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
        if (dockMapping &&
            RemoveDockMappingAt(
                *dockMappingEntryIndex))
        {
            break;
        }
        cutPaths_.clear();
        for (const auto& item : items_)
        {
            if (!item.selected || !item.desktopIconClsid.empty()) continue;
            wchar_t path[MAX_PATH]{};
            if (SHGetPathFromIDListW(item.absolutePidl.get(), path))
            {
                cutPaths_.erase(path);
                SHFILEOPSTRUCTW op{};
                op.wFunc = FO_DELETE;
                op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION;
                wchar_t from[MAX_PATH + 2]{};
                wcscpy_s(from, path);
                from[wcslen(path) + 1] = L'\0';
                op.pFrom = from;
                SHFileOperationW(&op);
            }
        }
        ReloadItems();
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
}

/**
 * @brief 调用 Windows Shell 的 IContextMenu 显示系统右键菜单。
 *        收集所有选中项的 PIDL，通过 desktopFolder_->GetUIObjectOf
 *        获取 IContextMenu 接口，显示 Shell 提供的标准右键菜单。
 * @param screenPoint 菜单弹出的屏幕坐标。
 * @param itemIndex   当前右键点击的项索引（用于确定 PIDL 列表的锚点）。
 */
inline void DesktopApp::ShowShellContextMenu(
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

    ComPtr<IContextMenu> ctxMenu;
    if (FAILED(desktopFolder_->GetUIObjectOf(hwnd_, static_cast<UINT>(pidls.size()), pidls.data(),
        IID_IContextMenu, nullptr, reinterpret_cast<void**>(ctxMenu.GetAddressOf()))) || !ctxMenu)
        return;

    HMENU menu = CreatePopupMenu();
    constexpr UINT kFirstCmd = 1;
    constexpr UINT kLastCmd = 0x7FFF;
    if (FAILED(ctxMenu->QueryContextMenu(menu, 0, kFirstCmd, kLastCmd, CMF_NORMAL | CMF_CANRENAME)))
        { DestroyMenu(menu); return; }

    ctxMenu.As(&activeContextMenu2_);
    ctxMenu.As(&activeContextMenu3_);

    HWND menuOwner = keepQuickNavigationOpen &&
        quickNavigationHwnd_ &&
        IsWindow(quickNavigationHwnd_)
        ? quickNavigationHwnd_
        : hwnd_;
    SetForegroundWindow(menuOwner);
    UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, menuOwner, nullptr);
    if (!keepQuickNavigationOpen)
        FocusDesktopInputWindow();

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
            return;
        }

        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
        invoke.hwnd = ShellDialogOwnerHwnd();
        invoke.lpVerb = MAKEINTRESOURCEA(commandOffset);
        invoke.lpVerbW = MAKEINTRESOURCEW(commandOffset);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screenPoint;
        SafeInvokeCommand(ctxMenu.Get(), reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
        ReloadItems();
    }
    DestroyMenu(menu);
    RestoreDesktopWindowLayer();
}

/**
 * @brief 显示 Windows 的"新建"子菜单并创建对应类型的文件。
 *        通过 CLSID_NewMenu 获取系统"新建"菜单的 IContextMenu 接口，
 *        使用 IShellExtInit 初始化到目标目录，弹出子菜单。
 *        用户选择后调用 InvokeCommand 创建对应类型的文件。
 * @param screenPoint 菜单弹出的屏幕坐标。
 * @param targetDir   新建文件的目标目录路径。
 */
inline void DesktopApp::ShowNewMenuAndInvoke(POINT screenPoint, const std::wstring& targetDir)
{
    ComPtr<IContextMenu> ctxMenu;
    if (FAILED(CoCreateInstance(CLSID_NewMenu, nullptr, CLSCTX_INPROC_SERVER,
        IID_IContextMenu, reinterpret_cast<void**>(ctxMenu.GetAddressOf()))) || !ctxMenu)
        return;

    ComPtr<IShellExtInit> shellExtInit;
    if (FAILED(ctxMenu.As(&shellExtInit)) || !shellExtInit)
        return;

    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(targetDir.c_str(), nullptr, &pidl, 0, nullptr)) || pidl == nullptr)
        return;

    HRESULT hr = shellExtInit->Initialize(pidl, nullptr, 0);
    ILFree(pidl);
    if (FAILED(hr)) return;

    HMENU tmpMenu = CreatePopupMenu();
    if (!tmpMenu) return;
    hr = ctxMenu->QueryContextMenu(tmpMenu, 0, 1, 0x7FFF, CMF_NORMAL);
    if (FAILED(hr)) { DestroyMenu(tmpMenu); return; }

    HMENU newSub = GetSubMenu(tmpMenu, 0);
    if (!newSub) { DestroyMenu(tmpMenu); return; }

    ctxMenu.As(&newMenuContextMenu_);
    SetForegroundWindow(hwnd_);
    UINT cmd = TrackPopupMenuEx(newSub, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_LEFTBUTTON,
        screenPoint.x, screenPoint.y, hwnd_, nullptr);
    FocusDesktopInputWindow();
    newMenuContextMenu_.Reset();

    if (cmd != 0 && cmd >= 1)
    {
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE;
        invoke.hwnd = ShellDialogOwnerHwnd();
        invoke.lpVerb = MAKEINTRESOURCEA(cmd - 1);
        invoke.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
        invoke.nShow = SW_SHOWNORMAL;
        SafeInvokeCommand(ctxMenu.Get(), reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
    }

    for (int i = GetMenuItemCount(tmpMenu) - 1; i >= 0; --i)
    {
        if (GetSubMenu(tmpMenu, i) == nullptr)
            RemoveMenu(tmpMenu, i, MF_BYPOSITION);
    }
    DestroyMenu(tmpMenu);
}

/**
 * @brief 通过 Shell IContextMenu 显示桌面的系统背景右键菜单。
 *        使用 desktopFolder_->CreateViewObject 获取桌面文件夹的
 *        IContextMenu 接口，显示系统提供的背景菜单（如显示设置、个性化等）。
 * @param screenPoint 菜单弹出的屏幕坐标。
 */
inline void DesktopApp::ShowDesktopBackgroundContextMenu(POINT screenPoint)
{
    ComPtr<IContextMenu> contextMenu;
    HRESULT hr = desktopFolder_->CreateViewObject(hwnd_, IID_IContextMenu,
        reinterpret_cast<void**>(contextMenu.GetAddressOf()));
    if (FAILED(hr) || !contextMenu)
        return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    constexpr UINT kFirstCmd = 1;
    constexpr UINT kLastCmd = 0x7FFF;
    hr = contextMenu->QueryContextMenu(menu, 0, kFirstCmd, kLastCmd, CMF_NORMAL);
    if (FAILED(hr)) { DestroyMenu(menu); RestoreDesktopWindowLayer(); return; }

    contextMenu.As(&activeContextMenu2_);
    contextMenu.As(&activeContextMenu3_);

    SetForegroundWindow(hwnd_);
    UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, hwnd_, nullptr);
    FocusDesktopInputWindow();

    activeContextMenu2_.Reset();
    activeContextMenu3_.Reset();

    if (cmd != 0)
    {
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
        invoke.hwnd = ShellDialogOwnerHwnd();
        invoke.lpVerb = MAKEINTRESOURCEA(cmd - kFirstCmd);
        invoke.lpVerbW = MAKEINTRESOURCEW(cmd - kFirstCmd);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screenPoint;
        SafeInvokeCommand(contextMenu.Get(), reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
        ReloadItems();
    }
    DestroyMenu(menu);
    RestoreDesktopWindowLayer();
}

/**
 * @brief 恢复桌面窗口的 Z 序层次位置。
 *        菜单弹出后可能改变窗口 Z 序，此方法将窗口恢复到正确的位置。
 *        有父窗口时置顶（HWND_TOP），无父窗口时置底（HWND_BOTTOM）。
 */
inline void DesktopApp::RestoreDesktopWindowLayer()
{
    if (!hwnd_ || !IsWindow(hwnd_)) return;
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
}

/**
 * @brief 判断指定桌面项是否为受保护的系统图标。
 *        通过比较 CLSID 判断是否为此电脑、用户文件、网络、
 *        控制面板或回收站等系统图标。
 * @param item 要检查的桌面项。
 * @return 如果是受保护的系统图标返回 true，否则返回 false。
 */
inline bool DesktopApp::IsProtectedDesktopIcon(const DesktopItem& item) const
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
inline void DesktopApp::ShowShellContextMenuForPath(const std::wstring& folderPath, POINT screenPoint)
{
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

    IShellFolder* folder = nullptr;
    HRESULT bindHr = parentFolder->BindToObject(child, nullptr, IID_IShellFolder, reinterpret_cast<void**>(&folder));
    parentFolder->Release();
    if (FAILED(bindHr) || folder == nullptr)
    {
        ILFree(pidl);
        return;
    }

    ComPtr<IContextMenu> contextMenu;
    HRESULT hr = folder->CreateViewObject(hwnd_, IID_IContextMenu, reinterpret_cast<void**>(contextMenu.GetAddressOf()));
    folder->Release();
    if (FAILED(hr) || !contextMenu)
    {
        ILFree(pidl);
        return;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu) { ILFree(pidl); return; }

    constexpr UINT kFirstCmd = 1;
    constexpr UINT kLastCmd = 0x7FFF;
    contextMenu->QueryContextMenu(menu, 0, kFirstCmd, kLastCmd, CMF_NORMAL | CMF_EXPLORE | CMF_CANRENAME);

    contextMenu.As(&activeContextMenu2_);
    contextMenu.As(&activeContextMenu3_);

    SetForegroundWindow(hwnd_);
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, hwnd_, nullptr);
    FocusDesktopInputWindow();

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
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screenPoint;
        SafeInvokeCommand(contextMenu.Get(), reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
        ReloadItems();
    }

    DestroyMenu(menu);
    RestoreDesktopWindowLayer();
    ILFree(pidl);
}

inline void DesktopApp::
ShowShellItemContextMenuForPath(
    const std::wstring& itemPath,
    POINT screenPoint)
{
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(
            itemPath.c_str(), nullptr,
            &pidl, 0, nullptr)) ||
        !pidl)
        return;

    IShellFolder* parentFolder = nullptr;
    PCUITEMID_CHILD child = nullptr;
    if (FAILED(SHBindToParent(
            pidl, IID_IShellFolder,
            reinterpret_cast<void**>(
                &parentFolder),
            &child)) ||
        !parentFolder)
    {
        ILFree(pidl);
        return;
    }

    ComPtr<IContextMenu> contextMenu;
    const HRESULT hr =
        parentFolder->GetUIObjectOf(
            hwnd_, 1, &child,
            IID_IContextMenu, nullptr,
            reinterpret_cast<void**>(
                contextMenu.
                    GetAddressOf()));
    parentFolder->Release();
    if (FAILED(hr) || !contextMenu)
    {
        ILFree(pidl);
        return;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu)
    {
        ILFree(pidl);
        return;
    }
    constexpr UINT kFirstCmd = 1;
    constexpr UINT kLastCmd = 0x7FFF;
    if (FAILED(
            contextMenu->QueryContextMenu(
                menu, 0, kFirstCmd,
                kLastCmd,
                CMF_NORMAL |
                    CMF_EXPLORE |
                    CMF_CANRENAME)))
    {
        DestroyMenu(menu);
        ILFree(pidl);
        return;
    }

    contextMenu.As(
        &activeContextMenu2_);
    contextMenu.As(
        &activeContextMenu3_);
    SetForegroundWindow(hwnd_);
    const UINT command =
        TrackPopupMenuEx(
            menu,
            TPM_RETURNCMD |
                TPM_RIGHTBUTTON,
            screenPoint.x,
            screenPoint.y,
            hwnd_, nullptr);
    FocusDesktopInputWindow();
    activeContextMenu2_.Reset();
    activeContextMenu3_.Reset();

    if (command >= kFirstCmd &&
        command <= kLastCmd)
    {
        const UINT commandOffset =
            command - kFirstCmd;
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
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screenPoint;
        SafeInvokeCommand(
            contextMenu.Get(),
            reinterpret_cast<
                LPCMINVOKECOMMANDINFO>(
                    &invoke));
        for (size_t i = 0;
            i < widgets_.size(); ++i)
        {
            if (widgets_[i].type ==
                DesktopWidgetType::
                    FolderMapping)
                RefreshFolderMappingWidget(
                    i);
        }
        ReloadItems(false);
        if (dockFolderPopupOpen_)
            RefreshDockFolderPopup();
    }

    DestroyMenu(menu);
    RestoreDesktopWindowLayer();
    ILFree(pidl);
}
