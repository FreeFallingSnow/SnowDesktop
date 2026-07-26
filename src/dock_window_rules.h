#pragma once

#include <windows.h>

namespace snowdesktop::dock_window_rules
{

enum class DockClickAction
{
    None,
    Launch,
    Activate,
    Minimize,
    Restore,
};

/**
 * @brief 将按下瞬间的 Dock 指示状态转换为唯一的点击动作。
 *
 * 该结果会随鼠标手势保存到释放阶段，释放时不得再读取前台窗口并改写
 * 动作。这样长线始终表示最小化、短线始终表示切到前台、圆点始终表示
 * 恢复，与用户按下时看到的状态完全一致。
 */
constexpr DockClickAction ResolveDockClickAction(
    bool running, bool minimized, bool foreground) noexcept
{
    if (!running)
        return DockClickAction::Launch;
    if (minimized)
        return DockClickAction::Restore;
    if (foreground)
        return DockClickAction::Minimize;
    return DockClickAction::Activate;
}

/**
 * @brief Selects the one show command used to restore a minimized window.
 *
 * Windows marks a window minimized from a maximized state with
 * WPF_RESTORETOMAXIMIZED. Issuing multiple generic restore commands can first
 * honor that flag and then immediately restore the maximized window to its
 * normal rectangle, so callers must send only the returned command once.
 */
constexpr int ResolveDockRestoreShowCommand(
    UINT placementFlags, UINT placementShowCommand) noexcept
{
    return (placementFlags & WPF_RESTORETOMAXIMIZED) != 0 ||
            placementShowCommand == SW_SHOWMAXIMIZED
        ? SW_SHOWMAXIMIZED
        : SW_RESTORE;
}

/**
 * @brief 判断顶层窗口的扩展样式与 Owner 关系是否允许显示在任务栏/Dock。
 *
 * Windows 默认不在任务栏显示工具窗口、有 Owner 的窗口和
 * WS_EX_NOACTIVATE 窗口；WS_EX_APPWINDOW 会显式覆盖这些默认规则。
 */
constexpr bool IsTaskWindowStyleEligible(
    LONG_PTR extendedStyle, bool hasOwner) noexcept
{
    if ((extendedStyle & WS_EX_APPWINDOW) != 0)
        return true;
    return (extendedStyle & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) == 0 &&
        !hasOwner;
}

} // namespace snowdesktop::dock_window_rules
