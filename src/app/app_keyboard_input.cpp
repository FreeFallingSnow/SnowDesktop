#include "app.h"
#include "../page_navigation_rules.h"

// Top-level keyboard command dispatch.

bool DesktopApp::TryHandlePageNavigationKey(
    WPARAM key, bool repeated)
{
    if (!generalSettings_.pageNavigationKeyboardEnabled ||
        key == VK_CONTROL || key == VK_MENU || key == VK_SHIFT ||
        renameEdit_ != nullptr || quickNavigationOpen_ ||
        IsCollectionPopupInteractive() || dragSession_.IsActive() ||
        !luaWidgetPanelRequest_.widgetId.empty())
        return false;

    if (widgetEngine_ && widgetEngine_->HasFocusedHostInput())
        return false;
    for (const auto& container : containers_)
    {
        const auto* searchable =
            dynamic_cast<const ScrollingItemWidget*>(container.get());
        if (searchable && searchable->IsSearchFocused())
            return false;
    }

    UINT pressedModifiers = 0;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)
        pressedModifiers |= MOD_CONTROL;
    if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0)
        pressedModifiers |= MOD_ALT;
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
        pressedModifiers |= MOD_SHIFT;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0)
        pressedModifiers |= MOD_WIN;

    using snowdesktop::page_navigation_rules::ShortcutMatches;
    const UINT virtualKey = static_cast<UINT>(key);
    const bool matchesPrevious = ShortcutMatches(
        generalSettings_.pageNavigationPreviousModifiers,
        generalSettings_.pageNavigationPreviousVirtualKey,
        pressedModifiers, virtualKey);
    const bool matchesNext = ShortcutMatches(
        generalSettings_.pageNavigationNextModifiers,
        generalSettings_.pageNavigationNextVirtualKey,
        pressedModifiers, virtualKey);
    if (!matchesPrevious && !matchesNext)
        return false;

    const auto conflictsWith = [&](UINT modifiers,
        UINT configuredVirtualKey) {
        return ShortcutMatches(
            modifiers, configuredVirtualKey,
            pressedModifiers, virtualKey);
    };
    const bool configuredConflict =
        (matchesPrevious && matchesNext) ||
        (navigationSettings_.enabled && conflictsWith(
            navigationSettings_.modifiers,
            navigationSettings_.virtualKey)) ||
        (generalSettings_.desktopPassthroughHotkeyEnabled &&
            conflictsWith(
                generalSettings_.desktopPassthroughHotkeyModifiers,
                generalSettings_.desktopPassthroughHotkeyVirtualKey)) ||
        (generalSettings_.dockEnabled &&
            dockSettings_.floatingShortcutMode &&
            conflictsWith(
                dockSettings_.floatingHotkeyModifiers,
                dockSettings_.floatingHotkeyVirtualKey));
    if (configuredConflict)
        return false;

    // A configured key remains consumed at page boundaries and on repeats,
    // but only a fresh physical press may initiate one page transition.
    if (!repeated)
        NavigatePageOffset(matchesPrevious ? -1 : 1);
    return true;
}

void DesktopApp::DispatchLuaWidgetViewKeyEvent(
    WPARAM key, bool pressed, bool repeated)
{
    if (!widgetEngine_ || quickNavigationOpen_ ||
        IsCollectionPopupInteractive())
        return;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    (void)widgetEngine_->DispatchHostViewKeyEvent(
        key, pressed, repeated, ctrl, shift, alt);
}

bool DesktopApp::OnKeyDown(WPARAM key, bool repeated)
{
    if (key == VK_CONTROL || key == VK_MENU || key == VK_SHIFT)
    {
        RefreshDragHintFromKeyboard();
        return false;
    }

    if (renameEdit_ != nullptr) return false;

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
                    return true;
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
            return true;
        }
    }

    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    bool restoreFloatingDockLayer = false;

    if (widgetEngine_ && !quickNavigationOpen_ &&
        !luaWidgetPanelRequest_.widgetId.empty() &&
        widgetEngine_->HandleHostViewKey(
            luaWidgetPanelRequest_.widgetId,
            key, ctrl, shift, alt,
            luaWidgetPanelRequest_.surface, repeated))
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
        RestoreInteractionInputFocus();
        return true;
    }
    if (!luaWidgetPanelRequest_.widgetId.empty() &&
        luaWidgetPanelRequest_.modal && key != VK_ESCAPE)
        return true;

    if (ctrl && (key == 'Z' || key == 'Y') && widgetEngine_)
    {
        size_t selectedLua = static_cast<size_t>(-1);
        bool multipleSelected = false;
        for (size_t index = 0; index < widgets_.size(); ++index)
        {
            if (!widgets_[index].selected) continue;
            if (selectedLua != static_cast<size_t>(-1) ||
                widgets_[index].type != DesktopWidgetType::LuaScript)
            {
                multipleSelected = true;
                break;
            }
            selectedLua = index;
        }
        if (!multipleSelected && selectedLua < widgets_.size())
        {
            const bool redo = key == 'Y' || shift;
            const auto& widgetId = widgets_[selectedLua].id;
            widgetEngine_->EnsureWidgetLoaded(
                widgetId, widgets_[selectedLua].packageId);
            const bool available = redo
                ? widgetEngine_->RuntimeCanRedoHostLogicalSlot(widgetId)
                : widgetEngine_->RuntimeCanUndoHostLogicalSlot(widgetId);
            if (available)
            {
                snowdesktop::widget_runtime::LogicalSlotChange change;
                std::string error;
                const bool succeeded = redo
                    ? widgetEngine_->RuntimeRedoHostLogicalSlot(
                        widgetId, change, error)
                    : widgetEngine_->RuntimeUndoHostLogicalSlot(
                        widgetId, change, error);
                if (!succeeded)
                {
                    widgetEngine_->RuntimeRecordError(widgetId,
                        "logical slot host history: " + error);
                    MessageBeep(MB_ICONWARNING);
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                RestoreInteractionInputFocus();
                return true;
            }
        }
    }

    if (widgetEngine_ && !quickNavigationOpen_ &&
        luaWidgetPanelRequest_.widgetId.empty() &&
        !IsCollectionPopupInteractive())
    {
        size_t selectedLua = static_cast<size_t>(-1);
        bool multipleSelected = false;
        for (size_t index = 0; index < widgets_.size(); ++index)
        {
            if (!widgets_[index].selected) continue;
            if (selectedLua != static_cast<size_t>(-1) ||
                widgets_[index].type != DesktopWidgetType::LuaScript)
            {
                multipleSelected = true;
                break;
            }
            selectedLua = index;
        }
        if (!multipleSelected && selectedLua < widgets_.size())
        {
            const auto& widgetId = widgets_[selectedLua].id;
            widgetEngine_->EnsureWidgetLoaded(
                widgetId, widgets_[selectedLua].packageId);
            if (widgetEngine_->HandleHostViewKey(
                    widgetId, key, ctrl, shift, alt,
                    "desktop", repeated))
            {
                InvalidateRect(hwnd_, nullptr, FALSE);
                RestoreInteractionInputFocus();
                return true;
            }
        }
    }

    bool handled = false;
    switch (key)
    {
    case VK_F2:
    case 'R':
        if (key == 'R' && !ctrl) break;
        if (key == VK_F2 || ctrl)
        {
            BeginRenameSelected();
            handled = true;
        }
        break;
    case VK_F5:
        handled = true;
        restoreFloatingDockLayer = true;
        ReloadItems();
        break;
    case VK_DELETE:
    {
        handled = true;
        restoreFloatingDockLayer = true;
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
            std::vector<snowdesktop::ShellFileOperationStep> steps;
            steps.push_back({
                FO_DELETE,
                std::move(paths),
                {},
                static_cast<FILEOP_FLAGS>(shift
                    ? FOF_WANTNUKEWARNING
                    : (FOF_ALLOWUNDO |
                       FOF_NOCONFIRMATION)) });
            QueueShellFileOperation(
                std::move(steps),
                [this](bool succeeded) {
                    if (succeeded)
                        ReloadItems();
                });
        }
        break;
    }
    case 'C':
        if (!ctrl) break;
        handled = true;
        restoreFloatingDockLayer = true;
        if (CopyCutSelectedFolderEntries(false))
            break;
        InvokeSelectedShellVerb("copy");
        break;
    case 'X':
        if (!ctrl) break;
    {
        handled = true;
        restoreFloatingDockLayer = true;
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
        handled = true;
        restoreFloatingDockLayer = true;
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
        handled = true;
        restoreFloatingDockLayer = true;
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
        handled = true;
        restoreFloatingDockLayer = true;
        if (keyboardNavInsideWidget_)
        {
            if (keyboardNavSearchBox_ &&
                keyboardNavWidgetIndex_ < widgets_.size())
            {
                for (auto& container : containers_)
                {
                    auto* searchable =
                        dynamic_cast<ScrollingItemWidget*>(
                            container.get());
                    if (!searchable ||
                        searchable->GetWidgetData() !=
                            &widgets_[keyboardNavWidgetIndex_])
                        continue;
                    searchable->SetSearchFocused(true);
                    UpdateHostInputImePosition();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    break;
                }
            }
            else if (keyboardNavWidgetIndex_ < widgets_.size() &&
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
        handled = true;
        restoreFloatingDockLayer = true;
        if (dragSession_.IsActive())
        {
            CancelActiveItemDrag();
            ClearSelection();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        else if (!luaWidgetPanelRequest_.widgetId.empty())
        {
            if (luaWidgetPanelRequest_.dismissOnEscape)
                CloseLuaWidgetPanel(
                    luaWidgetPanelRequest_.widgetId,
                    "escape");
        }
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
        handled = true;
        if (keyboardNavInsideWidget_)
            NavigateWidgetMembers(key);
        else
            NavigateDesktopGrid(key);
        break;
    default:
        break;
    }

    if (restoreFloatingDockLayer)
    {
        // Asynchronous Shell operations restore the floating input proxy
        // after the final completion. Synchronous actions can retain it now.
        if (shellFileOperationInFlight_ == 0)
            RestoreInteractionInputFocus();
    }
    return handled;
}

/**
 * @brief 根据键盘修饰键状态刷新拖拽提示信息
 */
