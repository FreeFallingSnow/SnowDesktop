#pragma once
// Inline implementations for DesktopApp — Context menus.
// This file is included by app_oo.h after the class definition.

// ── Context menus ───────────────────────────────────────────

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

inline void DesktopApp::ClearMenuIcons()
{
    for (HBITMAP bmp : menuIconPool_)
        DeleteObject(bmp);
    menuIconPool_.clear();
}

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

    HMENU gridMenu = CreatePopupMenu();
    if (gridMenu)
    {
        POINT clientPoint = screenPoint;
        ScreenToClient(hwnd_, &clientPoint);
        const GridPage* page = GridPageFromPoint(clientPoint);
        int cols = page ? page->columns : 0;
        int rows = page ? page->rows : 0;
        wchar_t gridLabel[64]{};
        swprintf_s(gridLabel, L"行列调整（%d列 × %d行）", cols, rows);
        AppendMenuW(gridMenu, MF_STRING, kContextGridAddRow, L"增加行");
        AppendMenuW(gridMenu, MF_STRING, kContextGridRemoveRow, L"减少行");
        AppendMenuW(gridMenu, MF_STRING, kContextGridAddColumn, L"增加列");
        AppendMenuW(gridMenu, MF_STRING, kContextGridRemoveColumn, L"减少列");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(gridMenu), gridLabel);
    }

    HMENU widgetMenu = CreatePopupMenu();
    if (widgetMenu)
    {
        AppendMenuW(widgetMenu, MF_STRING, kContextAddCollectionWidget, L"集合");
        AppendMenuW(widgetMenu, MF_STRING, kContextAddFileCategoryWidget, L"桌面文件分类");
        AppendMenuW(widgetMenu, MF_STRING, kContextAddFolderMappingWidget, L"文件夹映射");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(widgetMenu), L"添加组件");
    }

    HMENU zoomMenu = CreatePopupMenu();
    if (zoomMenu)
    {
        const int presets[] = { 50, 70, 80, 90, 100, 110, 120, 130, 150, 200 };
        for (int pct : presets)
        {
            wchar_t label[16]{};
            swprintf_s(label, L"%d%%", pct);
            UINT flags = MF_STRING;
            if (static_cast<int>(gapScale_ * 100) == pct) flags |= MF_CHECKED;
            AppendMenuW(zoomMenu, flags, kContextZoomPresetFirst + static_cast<UINT>(pct), label);
        }
        AppendMenuW(zoomMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(zoomMenu, MF_STRING, kContextZoomIncrease, L"放大 (+10%)");
        AppendMenuW(zoomMenu, MF_STRING, kContextZoomDecrease, L"缩小 (-10%)");
        wchar_t zoomLabel[32]{};
        swprintf_s(zoomLabel, L"缩放：%d%%", static_cast<int>(gapScale_ * 100));
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(zoomMenu), zoomLabel);
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
    if (gridMenu)
    {
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(gridMenu), L"");
        SetMenuItemIcon(gridMenu, kContextGridAddRow, L"");
        SetMenuItemIcon(gridMenu, kContextGridRemoveRow, L"");
        SetMenuItemIcon(gridMenu, kContextGridAddColumn, L"");
        SetMenuItemIcon(gridMenu, kContextGridRemoveColumn, L"");
    }
    if (widgetMenu)
    {
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(widgetMenu), L"");
        SetMenuItemIcon(widgetMenu, kContextAddCollectionWidget, L"");
        SetMenuItemIcon(widgetMenu, kContextAddFileCategoryWidget, L"");
        SetMenuItemIcon(widgetMenu, kContextAddFolderMappingWidget, L"");
    }
    if (zoomMenu)
    {
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(zoomMenu), L"");
        SetMenuItemIcon(zoomMenu, kContextZoomIncrease, L"");
        SetMenuItemIcon(zoomMenu, kContextZoomDecrease, L"");
    }
    SetMenuItemIcon(menu, kContextThisDisplayFirstCommand, L"");
    SetMenuItemIcon(menu, kContextSettingsCommand, L"");

    SetForegroundWindow(hwnd_);
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, hwnd_, nullptr);
    if (sortMenu) DestroyMenu(sortMenu);
    if (gridMenu) DestroyMenu(gridMenu);
    if (widgetMenu) DestroyMenu(widgetMenu);
    if (zoomMenu) DestroyMenu(zoomMenu);
    DestroyMenu(menu);
    newMenuContextMenu_.Reset();
    ClearMenuIcons();
    RestoreDesktopWindowLayer();

    if (command >= kContextZoomPresetFirst && command <= kContextZoomPresetFirst + 200)
    {
        SetZoom(static_cast<float>(command - kContextZoomPresetFirst) / 100.0f);
    }
    else switch (command)
    {
    case kContextRefreshCommand: ReloadItems(); break;
    case kContextSortByNameCommand: SortIconsByName(); break;
    case kContextSortByTypeCommand: SortIconsByType(); break;
    case kContextGridAddRow: AdjustGridRows(1); break;
    case kContextGridRemoveRow: AdjustGridRows(-1); break;
    case kContextGridAddColumn: AdjustGridColumns(1); break;
    case kContextGridRemoveColumn: AdjustGridColumns(-1); break;
    case kContextZoomIncrease: AdjustZoom(+0.1f); break;
    case kContextZoomDecrease: AdjustZoom(-0.1f); break;
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
    case kContextSettingsCommand: break;
    default: break;
    }
}

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
