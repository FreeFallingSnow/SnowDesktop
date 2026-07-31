#include "app.h"

// ── Tray ────────────────────────────────────────────────────

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
        owner, title, message);
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
    ClearMenuIcons();
    HMENU menu = CreatePopupMenu();

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
            UINT flags = MF_STRING;
            DWORD val = 0;
            if (TryReadDesktopIconRegistryValueAnyRoot(s.clsid, val))
            { if (val == 0) flags |= MF_CHECKED; }
            else
            {
                static const std::unordered_map<std::wstring, bool> defVis = {
                    { kDesktopIconClsidThisPC, false }, { kDesktopIconClsidUserFiles, false },
                    { kDesktopIconClsidNetwork, false }, { kDesktopIconClsidControlPanel, false },
                    { kDesktopIconClsidRecycleBin, true },
                };
                auto it = defVis.find(s.clsid);
                if (it != defVis.end() && it->second) flags |= MF_CHECKED;
            }
            AppendMenuW(iconMenu, flags, s.cmd, s.label);
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(iconMenu), _LW("app.interact.desktop_icon_settings"));
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    {
        bool nativeActive = !customDesktopVisible_;
        AppendMenuW(menu, MF_STRING | (nativeActive ? MF_CHECKED : 0),
            kTraySwitchNativeCommand, _LW("app.interact.switch_native_desktop"));
        AppendMenuW(menu, MF_STRING | (nativeActive ? 0 : MF_CHECKED),
            kTraySwitchCustomCommand, _LW("app.interact.switch_software_desktop"));
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTraySettingsCommand, _LW("app.menu.settings"));
    AppendMenuW(menu, MF_STRING, kTrayRestartExplorerCommand, _LW("app.interact.restart_explorer_menu"));
    AppendMenuW(menu, MF_STRING, kTrayRestartCommand, _LW("app.interact.restart_app"));
    AppendMenuW(menu, MF_STRING, kTrayExitCommand, _LW("app.interact.exit_app"));

    SetForegroundWindow(controlHwnd_ ? controlHwnd_ : hwnd_);
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, controlHwnd_ ? controlHwnd_ : hwnd_, nullptr);

    if (iconMenu) DestroyMenu(iconMenu);
    DestroyMenu(menu);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();

    switch (command)
    {
    case kTraySwitchNativeCommand:
        SetSoftwareDesktopEnabled(false, true);
        break;
    case kTraySwitchCustomCommand:
        SetSoftwareDesktopEnabled(true, true);
        break;
    case kTraySettingsCommand:
        ShowSettingsWindow();
        break;
    case kTrayRestartCommand:
        RequestRestart();
        break;
    case kTrayRestartExplorerCommand:
        if (!RestartWindowsExplorer())
            MessageBoxW(controlHwnd_ ? controlHwnd_ : hwnd_,
                _LW("app.interact.restart_explorer_fail"),
                L"SnowDesktop", MB_OK | MB_ICONWARNING);
        break;
    case kTrayExitCommand:
        if (settingsWindow_)
            settingsWindow_->ShowExitConfirm();
        else
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
