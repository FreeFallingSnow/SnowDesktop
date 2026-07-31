#include "app.h"

// Top-level keyboard command dispatch.

void DesktopApp::OnKeyDown(WPARAM key)
{
    if (key == VK_CONTROL || key == VK_MENU || key == VK_SHIFT)
    {
        RefreshDragHintFromKeyboard();
        return;
    }

    if (renameEdit_ != nullptr) return;

    // Handle searchable widget keyboard input.
    {
        for (auto& c : containers_)
        {
            auto* searchable = dynamic_cast<ScrollingItemWidget*>(c.get());
            if (searchable && searchable->IsSearchFocused())
            {
                if (searchable->HandleSearchKey(key))
                {
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
                break;
            }
        }
    }

    if (quickNavigationOpen_)
    {
        if (key == VK_ESCAPE)
        {
            CloseQuickNavigation();
            return;
        }
    }

    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

    switch (key)
    {
    case VK_F2:
    case 'R':
        if (key == 'R' && !ctrl) break;
        if (key == VK_F2 || ctrl)
            BeginRenameSelected();
        break;
    case VK_F5:
        ReloadItems();
        break;
    case VK_DELETE:
    {
        if (DockContainer* dock = GetDockContainer())
        {
            std::vector<Item*> selectedDockItems = dock->GetSelectedItems();
            if (!selectedDockItems.empty())
            {
                GridCell returnCell;
                if (const GridPage* firstPage = GetFirstPageGridPage())
                    returnCell.pageId = firstPage->id;
                MoveDockItemsToDesktop(selectedDockItems, returnCell);
                SaveLayoutSlots();
                ClearSelection();
                InvalidateRect(hwnd_, nullptr, FALSE);
                break;
            }
        }

        if (DeleteSelectedFolderEntries(shift))
            break;

        cutPaths_.clear();
        std::vector<std::wstring> paths;
        for (const auto& item : items_)
        {
            if (!item.selected || !item.desktopIconClsid.empty()) continue;
            wchar_t path[MAX_PATH]{};
            if (SHGetPathFromIDListW(item.absolutePidl.get(), path))
            {
                cutPaths_.erase(path);
                paths.push_back(path);
            }
        }

        if (!paths.empty())
        {
            std::wstring from;
            for (const auto& path : paths)
            {
                from += path;
                from.push_back(L'\0');
            }
            from.push_back(L'\0');

            SHFILEOPSTRUCTW op{};
            op.hwnd = ShellDialogOwnerHwnd();
            op.wFunc = FO_DELETE;
            op.pFrom = from.c_str();
            op.fFlags = static_cast<FILEOP_FLAGS>(shift
                ? FOF_WANTNUKEWARNING
                : (FOF_ALLOWUNDO | FOF_NOCONFIRMATION));
            if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted)
                ReloadItems();
        }
        break;
    }
    case 'C':
        if (!ctrl) break;
        if (CopyCutSelectedFolderEntries(false))
            break;
        InvokeSelectedShellVerb("copy");
        break;
    case 'X':
        if (!ctrl) break;
    {
        if (CopyCutSelectedFolderEntries(true))
            break;

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

                OleSetClipboard(dataObj.Get());
                OleFlushClipboard();
            }
        }

        for (size_t idx : selectedIndexes)
        {
            wchar_t path[MAX_PATH]{};
            if (SHGetPathFromIDListW(items_[idx].absolutePidl.get(), path))
                cutPaths_.insert(path);
        }

        UpdateCutState();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    break;
    case 'V':
        if (!ctrl) break;
    {
        if (dockFolderPopupOpen_ &&
            dockFolderPopupAvailable_ &&
            PasteClipboardToFolderPath(
                dockFolderPopupWidget_.
                    sourceFolderPath))
            break;
        if (PasteClipboardToFolderMapping(FindFolderMappingShortcutTarget()))
            break;

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
                cutPaths_.clear();
                ReloadItems();
            }
        }
    }
    break;
    case 'A':
        if (!ctrl) break;
    {
        if (IsCollectionPopupInteractive() &&
            dockFolderPopupOpen_)
        {
            ClearSelection();
            for (auto& entry :
                 dockFolderPopupWidget_.
                    folderEntries)
                entry.selected = true;
            InvalidateRect(
                hwnd_, nullptr, FALSE);
            break;
        }
        ClearSelection();
        for (auto& oo : items_oo_)
        {
            auto* icon = dynamic_cast<DesktopIcon*>(oo.get());
            if (!icon) continue;
            DesktopItem* di = icon->GetDesktopItem();
            if (!di || di->name.empty()) continue;
            di->selected = true;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    break;
    case VK_RETURN:
        if (keyboardNavInsideWidget_)
        {
            if (keyboardNavWidgetIndex_ < widgets_.size() &&
                ((widgets_[keyboardNavWidgetIndex_].type ==
                      DesktopWidgetType::CollectionGroup &&
                  keyboardNavCollectionGroupTabs_) ||
                 (widgets_[keyboardNavWidgetIndex_].type ==
                      DesktopWidgetType::FileGroup &&
                  (keyboardNavCollectionGroupTabs_ ||
                   keyboardNavFileGroupCategoryTabs_))))
                NavigateWidgetMembers(VK_DOWN);
            else
                OpenWidgetMember(
                    keyboardNavWidgetIndex_,
                    keyboardNavMemberIndex_);
        }
        else if (std::any_of(widgets_.begin(), widgets_.end(),
            [](const DesktopWidget& w) { return w.selected; }))
            EnterWidget();
        else
            OpenSelectedDesktopItem();
        break;
    case VK_ESCAPE:
        if (!luaWidgetPanelRequest_.widgetId.empty())
            CloseLuaWidgetPanel(
                luaWidgetPanelRequest_.widgetId,
                "escape");
        else if (IsCollectionPopupInteractive())
            CloseCollectionPopup();
        else if (keyboardNavInsideWidget_)
            ExitWidget();
        else
        {
            ClearSelection();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        break;
    case VK_UP:
    case VK_DOWN:
    case VK_LEFT:
    case VK_RIGHT:
        if (keyboardNavInsideWidget_)
            NavigateWidgetMembers(key);
        else
            NavigateDesktopGrid(key);
        break;
    default:
        break;
    }
}

/**
 * @brief 根据键盘修饰键状态刷新拖拽提示信息
 */
