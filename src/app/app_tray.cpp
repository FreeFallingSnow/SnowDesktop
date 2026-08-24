#include "app.h"

// ── Tray ────────────────────────────────────────────────────

namespace
{

bool CaptureTraySurfaceAtPoint(POINT screenPoint, RECT& surfaceRect)
{
    HWND window = WindowFromPoint(screenPoint);
    if (!window)
        return false;
    HWND root = GetAncestor(window, GA_ROOT);
    if (!root || !IsWindowVisible(root) ||
        !GetWindowRect(root, &surfaceRect))
        return false;
    return PtInRect(&surfaceRect, screenPoint) != FALSE;
}

} // namespace

/**
 * @brief 添加系统托盘图标
 * @param force 是否强制重新添加
 */
void DesktopApp::AddTrayIcon(bool force)
{
    HWND owner = controlHwnd_ ? controlHwnd_ : hwnd_;
    trayIconController_.Add(owner, force);
}

/**
 * @brief 移除系统托盘图标
 */
void DesktopApp::RemoveTrayIcon()
{
    trayIconController_.Remove(hwnd_);
}

/**
 * @brief 显示系统托盘气泡通知
 * @param title 通知标题
 * @param message 通知内容
 */
void DesktopApp::ShowBalloonNotification(const std::wstring& title, const std::wstring& message)
{
    HWND owner = controlHwnd_ ? controlHwnd_ : hwnd_;
    trayIconController_.ShowBalloon(
        owner, {}, title, message);
}

/**
 * @brief 处理系统托盘回调消息
 * @param lParam 消息参数（含右键点击、双击等事件）
 */
void DesktopApp::OnTrayCallback(LPARAM lParam)
{
    switch (TrayIconController::ClassifyCallback(lParam))
    {
    case TrayCallbackAction::ShowContextMenu:
    {
        POINT pt{};
        GetCursorPos(&pt);
        ShowTrayMenu(pt);
        break;
    }
    case TrayCallbackAction::ReloadItems:
        ReloadItems();
        break;
    default:
        break;
    }
}

/**
 * @brief 显示系统托盘右键菜单
 * @param screenPoint 屏幕坐标点
 */
void DesktopApp::ShowTrayMenu(POINT screenPoint)
{
    // Capture before SetForegroundWindow closes the hidden-icons flyout.
    RECT traySurface{};
    const bool hasTraySurface =
        CaptureTraySurfaceAtPoint(screenPoint, traySurface);
    PrepareMenuIconsForPoint(screenPoint);
    HMENU menu = CreatePopupMenu();

    const auto statusLabel = [](const wchar_t* title,
                                const wchar_t* status) {
        std::wstring label = title;
        label += L"\t";
        label += status;
        return label;
    };

    HMENU iconMenu = CreatePopupMenu();
    if (iconMenu)
    {
        struct IS { UINT cmd; const wchar_t* clsid; const wchar_t* label; };
        const IS items[] = {
            { kTrayDesktopIconThisPC, kDesktopIconClsidThisPC, _LW("app.interact.computer") },
            { kTrayDesktopIconUserFiles, kDesktopIconClsidUserFiles, _LW("app.interact.user_files") },
            { kTrayDesktopIconNetwork, kDesktopIconClsidNetwork, _LW("app.interact.network") },
            { kTrayDesktopIconControlPanel, kDesktopIconClsidControlPanel, _LW("app.interact.control_panel") },
            { kTrayDesktopIconRecycleBin, kDesktopIconClsidRecycleBin, _LW("app.interact.recycle_bin") },
        };
        for (const auto& s : items)
        {
            DWORD val = 0;
            bool visible = false;
            if (TryReadDesktopIconRegistryValueAnyRoot(s.clsid, val))
                visible = val == 0;
            else
            {
                static const std::unordered_map<std::wstring, bool> defVis = {
                    { kDesktopIconClsidThisPC, false }, { kDesktopIconClsidUserFiles, false },
                    { kDesktopIconClsidNetwork, false }, { kDesktopIconClsidControlPanel, false },
                    { kDesktopIconClsidRecycleBin, true },
                };
                auto it = defVis.find(s.clsid);
                if (it != defVis.end())
                    visible = it->second;
            }
            const std::wstring label = statusLabel(s.label,
                visible ? _LW("app.interact.shown")
                        : _LW("app.interact.hidden"));
            AppendMenuW(iconMenu, MF_STRING, s.cmd, label.c_str());
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(iconMenu), _LW("app.interact.desktop_icon_settings"));
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const bool nativeActive = !customDesktopVisible_;
    {
        const std::wstring modeLabel = statusLabel(
            _LW("app.interact.desktop_mode"),
            nativeActive ? _LW("app.interact.native_desktop")
                         : _LW("app.interact.software_desktop"));
        AppendMenuW(menu, MF_STRING, kTrayToggleDesktopMode,
            modeLabel.c_str());
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTraySettingsCommand, _LW("app.menu.settings"));
    AppendMenuW(menu, MF_STRING, kTrayRestartExplorerCommand, _LW("app.interact.restart_explorer_menu"));
    AppendMenuW(menu, MF_STRING, kTrayRestartCommand, _LW("app.interact.restart_app"));
    AppendMenuW(menu, MF_STRING, kTrayExitCommand, _LW("app.interact.exit_app"));

    if (iconMenu)
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(iconMenu), L"");
    SetMenuItemIcon(menu, kTrayToggleDesktopMode,
        nativeActive ? L"" : L"");
    SetMenuItemIcon(menu, kTraySettingsCommand, L"");
    SetMenuItemIcon(menu, kTrayRestartExplorerCommand, L"");
    SetMenuItemIcon(menu, kTrayRestartCommand, L"");
    SetMenuItemIcon(menu, kTrayExitCommand, L"");
    HWND menuOwner = controlHwnd_ ? controlHwnd_ : hwnd_;
    SetForegroundWindow(menuOwner);
    UINT command = ShowModernMenu(
        menu, screenPoint, menuOwner, false, true,
        hasTraySurface ? &traySurface : nullptr);

    if (iconMenu) DestroyMenu(iconMenu);
    DestroyMenu(menu);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();

    switch (command)
    {
    case kTrayToggleDesktopMode:
        SetSoftwareDesktopEnabled(!customDesktopVisible_, true);
        break;
    case kTraySettingsCommand:
        ShowSettingsWindow();
        break;
    case kTrayRestartCommand:
        (void)RequestRestart();
        break;
    case kTrayRestartExplorerCommand:
        if (!RestartWindowsExplorer())
            MessageBoxW(controlHwnd_ ? controlHwnd_ : hwnd_,
                _LW("app.interact.restart_explorer_fail"),
                L"SnowDesktop", MB_OK | MB_ICONWARNING);
        break;
    case kTrayExitCommand:
        if (!settingsWindow_ || !settingsWindow_->ShowExitConfirm())
            RequestExit();
        break;
    case kTrayDesktopIconThisPC:
    case kTrayDesktopIconUserFiles:
    case kTrayDesktopIconNetwork:
    case kTrayDesktopIconControlPanel:
    case kTrayDesktopIconRecycleBin:
    {
        struct TV { UINT cmd; const wchar_t* clsid; };
        static const TV tv[] = {
            { kTrayDesktopIconThisPC, kDesktopIconClsidThisPC },
            { kTrayDesktopIconUserFiles, kDesktopIconClsidUserFiles },
            { kTrayDesktopIconNetwork, kDesktopIconClsidNetwork },
            { kTrayDesktopIconControlPanel, kDesktopIconClsidControlPanel },
            { kTrayDesktopIconRecycleBin, kDesktopIconClsidRecycleBin },
        };
        for (const auto& t : tv)
        {
            if (t.cmd == command)
            {
                DWORD val = 0;
                bool visible = true;
                if (TryReadDesktopIconRegistryValueAnyRoot(t.clsid, val))
                    visible = (val == 0);
                WriteDesktopIconRegistryValue(t.clsid, !visible);
                ReloadItems();
                break;
            }
        }
        break;
    }
    default:
        break;
    }
}
