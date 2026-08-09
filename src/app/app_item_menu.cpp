#include "app.h"
#include "../menu_fluent_glyphs.h"
#include "../right_click_contract.h"
#include "../shell_context_menu_invoke.h"
#include "../shell_context_menu_site.h"

// Desktop-item and Shell-backed context menus.

void DesktopApp::ShowItemContextMenu(
    POINT screenPoint, int itemIndex, bool dockFrequentItem,
    bool keepQuickNavigationOpen,
    std::optional<RECT> dockRenameAnchor,
    std::optional<size_t> dockMappingEntryIndex,
    bool dockApplicationItem)
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
    SetMenuItemQuickAction(menu, kContextRenameCommand);
    SetMenuItemQuickAction(menu, kContextCutCommand);
    SetMenuItemQuickAction(menu, kContextCopyCommand);
    SetMenuItemQuickAction(menu, kContextDeleteCommand);
    SetMenuItemIcon(menu, kContextMoreCommand,
        snowdesktop::menu_fluent_glyphs::kMoreOptions,
        MenuIconFont::FluentRegular);
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
    UINT command = ShowModernMenu(
        menu, screenPoint, menuOwner, placeOutsideDock);
    DestroyMenu(menu);
    ClearMenuIcons();
    bool inlineEditorStarted = false;

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
        if (dockMapping &&
            RemoveDockMappingAt(
                *dockMappingEntryIndex))
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
                        ReloadItems();
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
    if (snowdesktop::right_click_contract::
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

    HWND menuOwner = keepQuickNavigationOpen &&
        quickNavigationHwnd_ &&
        IsWindow(quickNavigationHwnd_)
        ? quickNavigationHwnd_
        : hwnd_;
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
    UINT cmd = TrackPopupMenuEx(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, menuOwner, nullptr);
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
                            ReloadItems();
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
        SafeInvokeCommand(ctxMenu.Get(), reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
        ReloadItems();
    }
    DestroyMenu(menu);
    RestoreDesktopWindowLayer();
    if (!keepQuickNavigationOpen)
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
