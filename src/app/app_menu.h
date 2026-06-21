/**
 * @file app_menu.h
 * @brief DesktopApp 的上下文菜单相关内联实现。
 *
 * 该文件在 app_oo.h 中类定义之后被包含，提供桌面背景菜单、图标右键菜单、
 * Shell 扩展菜单、新建菜单以及菜单图标位图创建等功能。
 * 所有菜单均使用 TrackPopupMenuEx 以右键菜单方式弹出，并支持图标渲染。
 */
#pragma once

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

/**
 * @brief 连续显示行列调整菜单。
 * @details 标准 Win32 菜单执行命令后会结束。这里在每次调整完成后立即按
 *          最新行列数重建菜单，方便用户连续增加或减少行列。
 */
inline void DesktopApp::ShowGridAdjustmentMenu(POINT screenPoint, UINT initialCommand)
{
    UINT command = initialCommand;
    while (true)
    {
        const bool validCommand =
            command == 0 ||
            command == kContextGridAddRow ||
            command == kContextGridRemoveRow ||
            command == kContextGridAddColumn ||
            command == kContextGridRemoveColumn;
        if (!validCommand) break;

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
        wchar_t status[64]{};
        swprintf_s(status, L"当前：%d列 × %d行",
            page ? page->columns : 0, page ? page->rows : 0);

        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kContextGridAddRow, L"增加行");
        AppendMenuW(menu, MF_STRING, kContextGridRemoveRow, L"减少行");
        AppendMenuW(menu, MF_STRING, kContextGridAddColumn, L"增加列");
        AppendMenuW(menu, MF_STRING, kContextGridRemoveColumn, L"减少列");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kContextGridAdjustmentDone, L"结束调整");

        SetMenuItemIcon(menu, kContextGridAddRow, L"");
        SetMenuItemIcon(menu, kContextGridRemoveRow, L"");
        SetMenuItemIcon(menu, kContextGridAddColumn, L"");
        SetMenuItemIcon(menu, kContextGridRemoveColumn, L"");
        SetMenuItemIcon(menu, kContextGridAdjustmentDone, L"");

        SetForegroundWindow(hwnd_);
        command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
            screenPoint.x, screenPoint.y, hwnd_, nullptr);
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
    AppendMenuW(menu, MF_STRING, kContextPasteCommand, L"粘贴");
    AppendMenuW(menu, MF_STRING, kContextNewMenu, L"新建");
    AppendMenuW(menu, MF_STRING, kContextRefreshCommand, L"刷新");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kContextMoreCommand, L"展开更多选项");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU sortMenu = CreatePopupMenu();
    if (sortMenu)
    {
        AppendMenuW(sortMenu, MF_STRING, kContextSortByNameCommand, L"名称");
        AppendMenuW(sortMenu, MF_STRING, kContextSortByTypeCommand, L"类型");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sortMenu), L"排序方式");
    }

    POINT clientPoint = screenPoint;
    ScreenToClient(hwnd_, &clientPoint);
    const GridPage* gridPage = GridPageFromPoint(clientPoint);
    wchar_t gridLabel[64]{};
    swprintf_s(gridLabel, L"行列调整（%d列 × %d行）",
        gridPage ? gridPage->columns : 0, gridPage ? gridPage->rows : 0);
    AppendMenuW(menu, MF_STRING, kContextGridAdjustmentMenu, gridLabel);

    std::vector<std::wstring> luaWidgets = WidgetEngine::ListAvailable();
    HMENU widgetMenu = CreatePopupMenu();
    if (widgetMenu)
    {
        AppendMenuW(widgetMenu, MF_STRING, kContextAddCollectionWidget, L"集合");
        AppendMenuW(widgetMenu, MF_STRING, kContextAddFileCategoryWidget, L"桌面文件分类");
        AppendMenuW(widgetMenu, MF_STRING, kContextAddFolderMappingWidget, L"文件夹映射");
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
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(widgetMenu), L"添加组件");
    }

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
        AppendMenuW(spacingMenu, MF_STRING, kContextSpacingIncrease, L"增加间距 (+10%)");
        AppendMenuW(spacingMenu, MF_STRING, kContextSpacingDecrease, L"减少间距 (-10%)");
        wchar_t spacingLabel[32]{};
        swprintf_s(spacingLabel, L"图标间距：%d%%", currentSpacingPercent);
        AppendMenuW(menu, MF_POPUP,
            reinterpret_cast<UINT_PTR>(spacingMenu), spacingLabel);
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
        addFontSizeItem(kContextFontSizeSmall, L"小 (12pt)", 12);
        addFontSizeItem(kContextFontSizeMedium, L"中 (14pt)", 14);
        addFontSizeItem(kContextFontSizeLarge, L"大 (16pt)", 16);
        wchar_t fontSizeLabel[32]{};
        swprintf_s(fontSizeLabel, L"标题字号：%dpt", currentFontSize);
        AppendMenuW(menu, MF_POPUP,
            reinterpret_cast<UINT_PTR>(fontSizeMenu), fontSizeLabel);
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kContextThisDisplayFirstCommand, L"当前显示器显示首屏");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kContextSettingsCommand, L"设置");

    SetMenuItemIcon(menu, kContextNewMenu, L"");
    SetMenuItemIcon(menu, kContextRefreshCommand, L"");
    SetMenuItemIcon(menu, kContextPasteCommand, L"");
    SetMenuItemIcon(menu, kContextMoreCommand, L"");
    if (sortMenu)
    {
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(sortMenu), L"");
        SetMenuItemIcon(sortMenu, kContextSortByNameCommand, L"");
        SetMenuItemIcon(sortMenu, kContextSortByTypeCommand, L"");
    }
    SetMenuItemIcon(menu, kContextGridAdjustmentMenu, L"");
    if (widgetMenu)
    {
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(widgetMenu), L"");
        SetMenuItemIcon(widgetMenu, kContextAddCollectionWidget, L"");
        SetMenuItemIcon(widgetMenu, kContextAddFileCategoryWidget, L"");
        SetMenuItemIcon(widgetMenu, kContextAddFolderMappingWidget, L"");
    }
    if (spacingMenu)
    {
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(spacingMenu), L"");
        SetMenuItemIcon(spacingMenu, kContextSpacingIncrease, L"");
        SetMenuItemIcon(spacingMenu, kContextSpacingDecrease, L"");
    }
    if (fontSizeMenu)
    {
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(fontSizeMenu), L"");
    }
    SetMenuItemIcon(menu, kContextThisDisplayFirstCommand, L"");
    SetMenuItemIcon(menu, kContextSettingsCommand, L"");

    gridAdjustmentParentMenu_ = menu;
    gridAdjustmentMenuAnchorValid_ = false;
    SetForegroundWindow(hwnd_);
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, hwnd_, nullptr);

    gridAdjustmentParentMenu_ = nullptr;
    POINT adjustmentMenuPoint = gridAdjustmentMenuAnchorValid_
        ? gridAdjustmentMenuAnchor_ : screenPoint;

    if (sortMenu) DestroyMenu(sortMenu);
    if (widgetMenu) DestroyMenu(widgetMenu);
    if (spacingMenu) DestroyMenu(spacingMenu);
    if (fontSizeMenu) DestroyMenu(fontSizeMenu);
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
    else switch (command)
    {
    case kContextRefreshCommand: ReloadItems(); break;
    case kContextSortByNameCommand: SortIconsByName(); break;
    case kContextSortByTypeCommand: SortIconsByType(); break;
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
    case kContextThisDisplayFirstCommand: SetFirstPageMonitorFromPoint(screenPoint); break;
    case kContextAddCollectionWidget: AddCollectionWidgetAt(screenPoint); break;
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
                info.hwnd = hwnd_;
                info.lpVerb = "paste";
                info.nShow = SW_SHOWNORMAL;
                bgMenu->InvokeCommand(&info);
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
    case kContextFontSizeMedium: SetItemFontSize(14.0f); break;
    case kContextFontSizeLarge: SetItemFontSize(16.0f); break;
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
inline void DesktopApp::ShowItemContextMenu(POINT screenPoint, int itemIndex)
{
    if (itemIndex < 0 || static_cast<size_t>(itemIndex) >= items_.size()) return;
    ClearMenuIcons();

    int selectedCount = 0;
    for (const auto& item : items_) if (item.selected) ++selectedCount;

    bool canFile = !items_[itemIndex].desktopIconClsid.empty() ? false : true;
    if (canFile)
    {
        wchar_t path[MAX_PATH]{};
        if (!SHGetPathFromIDListW(items_[itemIndex].absolutePidl.get(), path))
            canFile = false;
    }

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, selectedCount == 1 ? MF_STRING : MF_STRING | MF_GRAYED, kContextOpenCommand, L"打开");
    AppendMenuW(menu, selectedCount == 1 && canFile ? MF_STRING : MF_STRING | MF_GRAYED, kContextRenameCommand, L"重命名");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, canFile ? MF_STRING : MF_STRING | MF_GRAYED, kContextCutCommand, L"剪切");
    AppendMenuW(menu, canFile ? MF_STRING : MF_STRING | MF_GRAYED, kContextCopyCommand, L"复制");
    AppendMenuW(menu, canFile ? MF_STRING : MF_STRING | MF_GRAYED, kContextDeleteCommand, L"删除");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kContextMoreCommand, L"展开更多选项");

    SetMenuItemIcon(menu, kContextOpenCommand, L"");
    SetMenuItemIcon(menu, kContextRenameCommand, L"");
    SetMenuItemIcon(menu, kContextCutCommand, L"");
    SetMenuItemIcon(menu, kContextCopyCommand, L"");
    SetMenuItemIcon(menu, kContextDeleteCommand, L"");
    SetMenuItemIcon(menu, kContextMoreCommand, L"");

    SetForegroundWindow(hwnd_);
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, hwnd_, nullptr);
    DestroyMenu(menu);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();

    switch (command)
    {
    case kContextOpenCommand:
    {
        for (const auto& item : items_)
        {
            if (item.selected)
                ShellExecuteW(nullptr, L"open", item.parsingName.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        break;
    }
    case kContextRenameCommand:
        BeginRenameSelected();
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
        ShowShellContextMenu(screenPoint, itemIndex);
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
inline void DesktopApp::ShowShellContextMenu(POINT screenPoint, int itemIndex)
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

    SetForegroundWindow(hwnd_);
    UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, hwnd_, nullptr);

    activeContextMenu2_.Reset();
    activeContextMenu3_.Reset();

    if (cmd != 0)
    {
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
        invoke.hwnd = hwnd_;
        invoke.lpVerb = MAKEINTRESOURCEA(cmd - kFirstCmd);
        invoke.lpVerbW = MAKEINTRESOURCEW(cmd - kFirstCmd);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screenPoint;
        ctxMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
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
    newMenuContextMenu_.Reset();

    if (cmd != 0 && cmd >= 1)
    {
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE;
        invoke.hwnd = hwnd_;
        invoke.lpVerb = MAKEINTRESOURCEA(cmd - 1);
        invoke.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
        invoke.nShow = SW_SHOWNORMAL;
        ctxMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
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

    activeContextMenu2_.Reset();
    activeContextMenu3_.Reset();

    if (cmd != 0)
    {
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
        invoke.hwnd = hwnd_;
        invoke.lpVerb = MAKEINTRESOURCEA(cmd - kFirstCmd);
        invoke.lpVerbW = MAKEINTRESOURCEW(cmd - kFirstCmd);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screenPoint;
        contextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
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

    activeContextMenu2_.Reset();
    activeContextMenu3_.Reset();

    if (command != 0)
    {
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
        invoke.hwnd = hwnd_;
        invoke.lpVerb = MAKEINTRESOURCEA(command - kFirstCmd);
        invoke.lpVerbW = MAKEINTRESOURCEW(command - kFirstCmd);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screenPoint;
        contextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
        ReloadItems();
    }

    DestroyMenu(menu);
    RestoreDesktopWindowLayer();
    ILFree(pidl);
}
