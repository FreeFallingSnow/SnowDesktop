# SnowDesktop

Native Win32 proof of concept for replacing the Explorer desktop icon surface.

The current build intentionally focuses on the Explorer replacement foundation:

- hides the original Explorer desktop `SysListView32` icon layer while running
- restores the original Explorer icon layer on normal exit
- renders desktop Shell items in a desktop-level Win32 window
- follows Windows desktop icon visibility settings for common Shell desktop icons
- lays out icons inside the monitor work area, so the taskbar is excluded from the usable desktop grid
- uses a fixed free-arrange slot grid: dragging selected icons only moves them into empty slots and does not insert/shift other icons
- uses Shell/OLE drop dispatch: dragging one desktop icon onto another sends the dragged Shell data object to the target icon, while external Explorer drops can target either an icon or blank desktop space
- redraws only the drag preview and target hint regions during drag, and shows a Chinese floating hint in a true per-pixel layered tooltip window for the action that will happen on release
- persists icon slots to `SnowDesktop.layout.json` beside the executable; newly discovered desktop items are appended after existing slots
- supports click selection, Ctrl multi-select, border-only drag marquee selection, double-click open, native Shell context menu, inline rename with `F2`, `Delete`, `Ctrl+C/X/V/A`, arrow navigation, `F5` reload, and `Esc` exit
- adds a Chinese tray menu with reload, name sorting, native/custom desktop switching, and exit commands

This proof does not move files, shortcuts, folders, or desktop icon coordinates.

Emergency restore:

```powershell
.\.build\native\Release\SnowDesktop.exe --restore-explorer-icons
```
