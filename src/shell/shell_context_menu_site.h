#pragma once

#include <memory>
#include <functional>
#include <string>
#include <vector>

#include <windows.h>
#include <shlobj.h>

namespace snowdesktop
{
// Private, never invoked. The kernel removes the sample even when a supervised
// menu process is terminated while an extension is loading.
struct ShellMenuInitializationFile
{
    ShellMenuInitializationFile();
    ~ShellMenuInitializationFile();
    ShellMenuInitializationFile(const ShellMenuInitializationFile&) = delete;
    ShellMenuInitializationFile& operator=(const ShellMenuInitializationFile&) = delete;
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::wstring path;
};

/**
 * Supplies folder and selection services while a classic context menu is open.
 *
 * Cascading verbs backed by IExplorerCommand query their site for
 * IServiceProvider/IFolderView/IShellBrowser. Without that site they can
 * leave an empty placeholder submenu even though QueryContextMenu succeeded.
 */
class ShellContextMenuSite
{
public:
    ShellContextMenuSite();
    ~ShellContextMenuSite();

    ShellContextMenuSite(const ShellContextMenuSite&) = delete;
    ShellContextMenuSite& operator=(const ShellContextMenuSite&) = delete;

    bool Initialize(IShellFolder* folder, HWND owner);
    bool Attach(IContextMenu* contextMenu);
    // Backgrounds and selections share one immutable site and one query.
    // Cold missing entries never trigger a primer, retry or aggregate rebuild.
    using MenuFactory = std::function<HRESULT(HWND, IContextMenu**)>;
    HRESULT BuildMenu(IShellFolder* folder, HWND owner, HMENU menu,
        UINT firstCommand, UINT lastCommand, UINT flags,
        const MenuFactory& bind, IContextMenu** contextMenu);
    // A selection-only site supplies folder/selection services without creating
    // a second Windows folder view or querying an unpublished aggregate.
    HRESULT BuildSelectionMenu(IShellFolder* folder, HWND owner, HMENU menu,
        UINT firstCommand, UINT lastCommand, UINT flags, const MenuFactory& bind,
        UINT count, PCUITEMID_CHILD_ARRAY children, IContextMenu** contextMenu);
    // Read built-in namespace verbs for a custom popup. These commands do not
    // need filesystem providers or a folder view; bind their aggregate once.
    HRESULT BuildNamespaceMenu(HWND owner, HMENU menu,
        UINT firstCommand, UINT lastCommand, UINT flags,
        const MenuFactory& bind, IContextMenu** contextMenu);
    HWND HostWindow() const;
    bool HasView() const;
    // Verb dialogs must use the same persistent, activatable owner as the
    // invocation structure, rather than the hidden view used to build menus.
    void SetInvocationOwner(HWND owner);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snowdesktop
