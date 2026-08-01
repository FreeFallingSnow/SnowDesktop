#include "app.h"
#include "../shell_context_menu_invoke.h"
#include "../shell_context_menu_site.h"

// Shell context-menu command routing.

bool DesktopApp::IsShellRenameCommand(IContextMenu* contextMenu, UINT commandOffset) const
{
    if (!contextMenu) return false;

    wchar_t verbW[128]{};
    if (SUCCEEDED(contextMenu->GetCommandString(commandOffset, GCS_VERBW, nullptr,
        reinterpret_cast<LPSTR>(verbW), static_cast<UINT>(_countof(verbW)))) &&
        lstrcmpiW(verbW, L"rename") == 0)
        return true;

    char verbA[128]{};
    if (SUCCEEDED(contextMenu->GetCommandString(commandOffset, GCS_VERBA, nullptr,
        verbA, static_cast<UINT>(_countof(verbA)))) &&
        lstrcmpiA(verbA, "rename") == 0)
        return true;

    return false;
}

void DesktopApp::ShowFolderEntryContextMenu(
    POINT screenPoint, size_t widgetIndex,
    size_t memberIndex,
    bool keepQuickNavigationOpen)
{
    if (widgetIndex >= widgets_.size() ||
        widgets_[widgetIndex].type != DesktopWidgetType::FolderMapping ||
        memberIndex >= widgets_[widgetIndex].folderEntries.size())
        return;

    const std::wstring fullPath = widgets_[widgetIndex].folderEntries[memberIndex].fullPath;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(fullPath.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl)
        return;

    IShellFolder* parentFolder = nullptr;
    PCUITEMID_CHILD child = nullptr;
    if (FAILED(SHBindToParent(pidl, IID_IShellFolder,
        reinterpret_cast<void**>(&parentFolder), &child)) || !parentFolder)
    {
        ILFree(pidl);
        return;
    }

    HWND menuOwner = keepQuickNavigationOpen &&
        quickNavigationHwnd_ &&
        IsWindow(quickNavigationHwnd_)
        ? quickNavigationHwnd_
        : hwnd_;
    snowdesktop::ShellContextMenuSite menuSite;
    menuSite.Initialize(parentFolder, menuOwner);
    HWND shellOwner = menuSite.HostWindow()
        ? menuSite.HostWindow() : menuOwner;
    ComPtr<IContextMenu> contextMenu;
    HRESULT hr = parentFolder->GetUIObjectOf(shellOwner, 1, &child, IID_IContextMenu,
        nullptr, reinterpret_cast<void**>(contextMenu.GetAddressOf()));
    if (SUCCEEDED(hr) && contextMenu)
        menuSite.Attach(contextMenu.Get());
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
    hr = contextMenu->QueryContextMenu(menu, 0, kFirstCmd, kLastCmd,
        CMF_NORMAL | CMF_CANRENAME | CMF_SYNCCASCADEMENU);
    if (FAILED(hr))
    {
        DestroyMenu(menu);
        ILFree(pidl);
        RestoreDesktopWindowLayer();
        return;
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu,
        snowdesktop::item_location::CanReveal(fullPath)
            ? MF_STRING
            : MF_STRING | MF_GRAYED,
        kContextRevealLocationCommand,
        _LW("app.menu.open_file_location"));

    contextMenu.As(&activeContextMenu2_);
    contextMenu.As(&activeContextMenu3_);

    SetForegroundWindow(menuOwner);
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, menuOwner, nullptr);
    if (!keepQuickNavigationOpen)
        FocusDesktopInputWindow();

    activeContextMenu2_.Reset();
    activeContextMenu3_.Reset();

    if (command == kContextRevealLocationCommand)
    {
        DestroyMenu(menu);
        RestoreDesktopWindowLayer();
        ILFree(pidl);
        snowdesktop::item_location::Reveal(hwnd_, fullPath);
        return;
    }

    if (command >= kFirstCmd && command <= kLastCmd)
    {
        UINT commandOffset = command - kFirstCmd;
        wchar_t menuText[128]{};
        bool renameCommand = IsShellRenameCommand(contextMenu.Get(), commandOffset);
        if (!renameCommand &&
            GetMenuStringW(menu, command, menuText, static_cast<int>(_countof(menuText)), MF_BYCOMMAND) > 0)
        {
            renameCommand = StrStrIW(menuText, L"重命名") != nullptr || // l10n-allow: match Chinese Windows shell verb
                StrStrIW(menuText, L"Rename") != nullptr;
        }

        DestroyMenu(menu);
        RestoreDesktopWindowLayer();
        ILFree(pidl);

        if (renameCommand)
        {
            if (keepQuickNavigationOpen)
                BeginQuickNavigationFolderEntryRename(
                    widgetIndex, memberIndex);
            else
                BeginRenameFolderEntry(
                    widgetIndex, memberIndex);
            return;
        }

        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
        invoke.hwnd = ShellDialogOwnerHwnd();
        invoke.lpVerb = MAKEINTRESOURCEA(commandOffset);
        invoke.lpVerbW = MAKEINTRESOURCEW(commandOffset);
        const std::wstring invocationDirectory =
            snowdesktop::ShellInvocationDirectoryForItem(fullPath);
        std::string invocationDirectoryA;
        snowdesktop::SetShellInvocationDirectory(
            invoke, invocationDirectory, invocationDirectoryA);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screenPoint;
        SafeInvokeCommand(contextMenu.Get(), reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
        RefreshFolderMappingWidget(widgetIndex);
        RebuildContainersAndItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        return;
    }

    DestroyMenu(menu);
    RestoreDesktopWindowLayer();
    ILFree(pidl);
}

/**
 * @brief 如果同名文件/文件夹已存在，自动添加递增序号生成唯一名称
 *
 * 例如 "test.txt" 已存在时返回 "test (2).txt"，依此类推。
 * @param folderPath  父文件夹路径
 * @param desiredName  期望的文件/文件夹名
 * @return 在 folderPath 中不存在的唯一名称
 */


bool DesktopApp::IsShellDeleteCommand(
    IContextMenu* contextMenu,
    UINT commandOffset) const
{
    if (!contextMenu) return false;

    wchar_t verbW[128]{};
    if (SUCCEEDED(contextMenu->GetCommandString(
            commandOffset, GCS_VERBW, nullptr,
            reinterpret_cast<LPSTR>(verbW),
            static_cast<UINT>(_countof(verbW)))) &&
        lstrcmpiW(verbW, L"delete") == 0)
    {
        return true;
    }

    char verbA[128]{};
    if (SUCCEEDED(contextMenu->GetCommandString(
            commandOffset, GCS_VERBA, nullptr,
            verbA,
            static_cast<UINT>(_countof(verbA)))) &&
        lstrcmpiA(verbA, "delete") == 0)
    {
        return true;
    }
    return false;
}
