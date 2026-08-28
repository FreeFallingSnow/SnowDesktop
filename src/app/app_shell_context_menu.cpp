#include "app.h"
#include "../menu_fluent_glyphs.h"
#include "../right_click_contract.h"
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

    PrepareMenuIconsForPoint(screenPoint);

    const std::wstring fullPath =
        widgets_[widgetIndex].folderEntries[memberIndex].fullPath;
    const std::vector<std::wstring> selectedPaths =
        GetSelectedFolderEntryPaths();
    const bool hasSelection = !selectedPaths.empty();
    const bool singleSelection = selectedPaths.size() == 1;
    const bool canRunAsAdministrator =
        singleSelection &&
        IsAdministratorRunnablePath(selectedPaths.front());
    const bool canShowProperties = singleSelection;
    HWND menuOwner = keepQuickNavigationOpen &&
        quickNavigationHwnd_ &&
        IsWindow(quickNavigationHwnd_)
        ? quickNavigationHwnd_
        : hwnd_;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu,
        hasSelection ? MF_STRING : MF_STRING | MF_GRAYED,
        kContextOpenCommand, _LW("app.menu.open"));
    AppendMenuW(menu,
        hasSelection ? MF_STRING : MF_STRING | MF_GRAYED,
        kContextCopyPathCommand,
        _LW("app.menu.copy_path"));
    AppendMenuW(menu,
        singleSelection &&
                snowdesktop::item_location::CanReveal(fullPath)
            ? MF_STRING
            : MF_STRING | MF_GRAYED,
        kContextRevealLocationCommand,
        _LW("app.menu.open_file_location"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu,
        canRunAsAdministrator ? MF_STRING : MF_STRING | MF_GRAYED,
        kContextRunAsAdministratorCommand,
        _LW("app.menu.run_as_administrator"));
    AppendMenuW(menu,
        canShowProperties ? MF_STRING : MF_STRING | MF_GRAYED,
        kContextPropertiesCommand,
        _LW("app.menu.properties"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu,
        singleSelection ? MF_STRING : MF_STRING | MF_GRAYED,
        kContextRenameCommand, _LW("app.menu.rename"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu,
        hasSelection ? MF_STRING : MF_STRING | MF_GRAYED,
        kContextCutCommand, _LW("app.menu.cut"));
    AppendMenuW(menu,
        hasSelection ? MF_STRING : MF_STRING | MF_GRAYED,
        kContextCopyCommand, _LW("app.menu.copy"));
    AppendMenuW(menu,
        hasSelection ? MF_STRING : MF_STRING | MF_GRAYED,
        kContextDeleteCommand, _LW("app.settings.delete"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu,
        singleSelection ? MF_STRING : MF_STRING | MF_GRAYED,
        kContextMoreCommand, _LW("app.menu.more_options"));

    SetMenuItemIcon(menu, kContextOpenCommand, L"");
    SetMenuItemIcon(menu, kContextRevealLocationCommand, L"");
    SetMenuItemIcon(menu, kContextCopyPathCommand,
        snowdesktop::menu_fluent_glyphs::kCopy,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(menu, kContextRunAsAdministratorCommand,
        snowdesktop::menu_fluent_glyphs::kShield,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(menu, kContextPropertiesCommand,
        snowdesktop::menu_fluent_glyphs::kInfo,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(menu, kContextRenameCommand, L"");
    SetMenuItemIcon(menu, kContextCutCommand, L"");
    SetMenuItemIcon(menu, kContextCopyCommand, L"");
    SetMenuItemIcon(menu, kContextDeleteCommand, L"");
    SetMenuItemIcon(menu, kContextMoreCommand,
        snowdesktop::menu_fluent_glyphs::kMoreOptions,
        MenuIconFont::FluentRegular);
    SetMenuItemQuickAction(menu, kContextRenameCommand);
    SetMenuItemQuickAction(menu, kContextCutCommand);
    SetMenuItemQuickAction(menu, kContextCopyCommand);
    SetMenuItemQuickAction(menu, kContextDeleteCommand);

    SetForegroundWindow(menuOwner);
    UINT command = ShowModernMenu(
        menu, screenPoint, menuOwner);
    DestroyMenu(menu);
    ClearMenuIcons();
    bool inlineEditorStarted = false;

    switch (command)
    {
    case kContextOpenCommand:
        for (const auto& path : selectedPaths)
            shellLaunchWorker_.Enqueue(
                hwnd_, path);
        break;
    case kContextRevealLocationCommand:
        if (singleSelection)
            snowdesktop::item_location::Reveal(
                hwnd_, fullPath);
        break;
    case kContextCopyPathCommand:
        CopyPathsToClipboard(selectedPaths);
        break;
    case kContextRunAsAdministratorCommand:
        if (canRunAsAdministrator)
            RunPathAsAdministrator(selectedPaths.front());
        break;
    case kContextPropertiesCommand:
        if (canShowProperties)
            ShowPathProperties(selectedPaths.front());
        break;
    case kContextRenameCommand:
        if (singleSelection)
        {
            if (keepQuickNavigationOpen)
                BeginQuickNavigationFolderEntryRename(
                    widgetIndex, memberIndex);
            else
                BeginRenameFolderEntry(
                    widgetIndex, memberIndex);
            inlineEditorStarted = renameEdit_ != nullptr;
        }
        break;
    case kContextCutCommand:
        CopyCutSelectedFolderEntries(true);
        break;
    case kContextCopyCommand:
        CopyCutSelectedFolderEntries(false);
        break;
    case kContextDeleteCommand:
        DeleteSelectedFolderEntries(false);
        break;
    case kContextMoreCommand:
        if (singleSelection)
        {
            if (keepQuickNavigationOpen)
                SetQuickNavigationTopmost(false);
            ShowShellItemContextMenuForPath(
                fullPath, screenPoint);
            if (keepQuickNavigationOpen)
                SetQuickNavigationTopmost(true);
        }
        break;
    default:
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
