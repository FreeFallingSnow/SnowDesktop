#pragma once

#include <memory>

#include <windows.h>
#include <shlobj.h>

namespace snowdesktop
{

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
    HWND HostWindow() const;
    // Verb dialogs must use the same persistent, activatable owner as the
    // invocation structure, rather than the hidden view used to build menus.
    void SetInvocationOwner(HWND owner);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snowdesktop
