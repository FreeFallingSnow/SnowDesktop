#pragma once

#include <memory>
#include <functional>
#include <string>

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

class ShellFileMenuPrimer
{
public:
    ShellFileMenuPrimer();
    ~ShellFileMenuPrimer();
    bool Initialize();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * Hosts a minimal hidden Shell view while a classic context menu is open.
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
    // Load providers before creating a Shell view. Rebind the actual menu only
    // after attaching that view, keeping synchronous cascades and final tokens.
    using MenuFactory = std::function<HRESULT(HWND, IContextMenu**)>;
    HRESULT BuildMenu(IShellFolder* folder, HWND owner, HMENU menu,
        UINT firstCommand, UINT lastCommand, UINT flags,
        const MenuFactory& bind, IContextMenu** contextMenu);
    HWND HostWindow() const;
    // Verb dialogs must use the same persistent, activatable owner as the
    // invocation structure, rather than the hidden view used to build menus.
    void SetInvocationOwner(HWND owner);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snowdesktop
