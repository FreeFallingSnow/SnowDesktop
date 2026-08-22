#pragma once

#include <cstddef>
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

enum class DockWindowIconSource
{
    None,
    AppUserModel,
    Executable,
    Window,
    GenericExecutable,
};

enum class DockWindowActivationObservationAction
{
    Stop,
    WaitForRestore,
    Activate,
};

struct DockRunningIndicatorColor
{
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 1.0f;
};

/**
 * @brief 根据文字明暗和窗口状态选择运行指示器颜色。
 *
 * 浅色文字通常位于深色或毛玻璃背景上，灰白指示器容易与文字和高光
 * 混在一起，因此统一使用高饱和蓝色。深色文字分支继续使用深灰色。
 */
constexpr DockRunningIndicatorColor ResolveDockRunningIndicatorColor(
    bool darkText,
    bool foreground,
    bool minimized) noexcept
{
    if (darkText)
    {
        return foreground
            ? DockRunningIndicatorColor{
                0.14f, 0.16f, 0.22f, 1.0f }
            : DockRunningIndicatorColor{
                0.24f, 0.26f, 0.32f,
                minimized ? 0.82f : 0.90f };
    }
    return foreground
        ? DockRunningIndicatorColor{
            0.12f, 0.56f, 1.0f, 1.0f }
        : DockRunningIndicatorColor{
            0.28f, 0.64f, 1.0f,
            minimized ? 0.82f : 0.90f };
}

/**
 * @brief 选择运行区应用图标来源。
 *
 * AUMID 和带专用图标的可执行文件提供稳定的高清应用标识，应优先
 * 使用。传统 Win32 程序的宿主 EXE 可能只返回系统通用图标；此时
 * 窗口通过 WM_GETICON 暴露的图标比通用占位图更准确。
 */
constexpr DockWindowIconSource ResolveDockWindowIconSource(
    bool appUserModelIconAvailable,
    bool executableIconAvailable,
    bool executableIconIsGeneric,
    bool windowIconAvailable) noexcept
{
    if (appUserModelIconAvailable)
        return DockWindowIconSource::AppUserModel;
    if (executableIconAvailable &&
        !executableIconIsGeneric)
        return DockWindowIconSource::Executable;
    if (windowIconAvailable)
        return DockWindowIconSource::Window;
    if (executableIconAvailable)
        return DockWindowIconSource::GenericExecutable;
    return DockWindowIconSource::None;
}

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
 * @brief 将预览缩略图点击转换为窗口级点击动作。
 *
 * 与 Dock 图标的应用级判定不同，缩略图点击针对具体窗口：Dock 图标在
 * 应用任一窗口处于前台时判定为前台（点击即最小化整个应用），而缩略图
 * 必须只看被点窗口自身。同一应用多窗口场景下，点击非前台窗口的卡片
 * 应激活该窗口，而不是把整组窗口误判为前台而最小化。
 */
constexpr DockClickAction ResolveDockWindowPreviewClickAction(
    bool minimized, bool windowForeground) noexcept
{
    if (minimized)
        return DockClickAction::Restore;
    if (windowForeground)
        return DockClickAction::Minimize;
    return DockClickAction::Activate;
}

/**
 * @brief 关闭请求尚未完成时，禁止 Dock 再向同一窗口或应用分发命令。
 *
 * WM_CLOSE 是异步消息；目标窗口在真正销毁前仍会通过 IsWindow 检查。
 * 关闭动作必须在这段竞态窗口内优先于激活、恢复和再次启动。
 */
constexpr bool ShouldSuppressDockWindowCommand(
    bool closePending) noexcept
{
    return closePending;
}

/**
 * @brief 仅吞掉一个真实、明确匹配的固定 Dock 条目释放事件。
 *
 * 运行区和常用区没有固定条目索引，两者都使用 size_t(-1)。不能把
 * 两个“无索引”哨兵误判为同一条目，否则窗口最小化、恢复和激活
 * 都会在命令分发前被提前返回。
 */
constexpr bool ShouldSuppressDockClickRelease(
    std::size_t pressedEntryIndex,
    std::size_t suppressedEntryIndex) noexcept
{
    constexpr std::size_t noEntry =
        static_cast<std::size_t>(-1);
    return pressedEntryIndex != noEntry &&
        pressedEntryIndex == suppressedEntryIndex;
}

/**
 * @brief 判断两次释放是否属于同一个 Dock 项的双击。
 *
 * 固定 Dock 与悬浮 Dock 使用不同 HWND。两次点击跨越这两个表面时，Windows
 * 不一定生成 WM_LBUTTONDBLCLK，因此释放阶段还需要按稳定条目身份和系统双击
 * 时间补做判定。哨兵值不能彼此匹配，否则两个“无条目”会被误判为双击。
 */
constexpr bool IsMatchingPendingDockDoubleClickRelease(
    std::size_t pressedEntryIndex,
    std::size_t pressedFrequentItemIndex,
    std::size_t pendingEntryIndex,
    std::size_t pendingFrequentItemIndex,
    unsigned long elapsed,
    unsigned long doubleClickTime) noexcept
{
    constexpr std::size_t noItem =
        static_cast<std::size_t>(-1);
    const bool sameEntry =
        pressedEntryIndex != noItem &&
        pressedEntryIndex == pendingEntryIndex;
    const bool sameFrequentItem =
        pressedFrequentItemIndex != noItem &&
        pressedFrequentItemIndex ==
            pendingFrequentItemIndex;
    return elapsed <= doubleClickTime &&
        (sameEntry || sameFrequentItem);
}

/**
 * @brief 未执行专用双击动作时，把 WM_LBUTTONDBLCLK 重放为第二次按下。
 *
 * Windows 会用 WM_LBUTTONDBLCLK 替代第二个 WM_LBUTTONDOWN。若正在运行
 * 的应用在这里直接返回，后续 WM_LBUTTONUP 就没有对应的按下状态，动画
 * 期间的第二次点击也会被吞掉。
 */
constexpr bool ShouldDispatchDockDoubleClickPress(
    bool specialDoubleClickHandled) noexcept
{
    return !specialDoubleClickHandled;
}

/**
 * @brief 普通异步最小化被系统拒绝时，改由默认窗口过程执行系统命令。
 *
 * 管理员权限窗口会通过 UIPI 拒绝来自普通完整性进程的
 * ShowWindowAsync，但 DefWindowProc(SC_MINIMIZE) 仍可执行与系统
 * 任务栏一致的默认最小化行为。
 */
constexpr bool NeedsDockMinimizeSystemCommandFallback(
    bool showWindowAccepted) noexcept
{
    return !showWindowAccepted;
}

/**
 * @brief 普通关闭消息被 UIPI 等系统策略拒绝时使用系统关闭命令回退。
 */
constexpr bool NeedsDockCloseSystemCommandFallback(
    bool closeMessageAccepted) noexcept
{
    return !closeMessageAccepted;
}

/**
 * @brief 判断异步恢复请求失败后是否需要系统命令回退。
 *
 * 高完整性窗口会通过 UIPI 拒绝普通进程的 ShowWindowAsync。
 * 调用方应通过默认窗口过程执行一次 SC_RESTORE，真实窗口
 * 退出最小化后再执行置前，避免重复恢复改变最大化状态。
 */
constexpr bool NeedsDockRestoreRequestFallback(
    bool wasMinimized,
    bool showWindowAccepted) noexcept
{
    return wasMinimized && !showWindowAccepted;
}

/**
 * @brief 执行一次可验证的高完整性窗口恢复回退。
 *
 * 先通过默认窗口过程执行 SC_RESTORE；只有窗口仍处于最小化
 * 状态时才调用窗口切换器，避免重复恢复改变原有最大化状态。
 * 可调用对象由平台层注入，使调用顺序和系统命令可以直接回归测试。
 */
template <typename ExecuteSystemCommand,
    typename IsWindowStillIconic,
    typename ExecuteWindowSwitch>
void ApplyDockRestoreRequestFallback(
    bool fallbackRequired,
    ExecuteSystemCommand&& executeSystemCommand,
    IsWindowStillIconic&& isWindowStillIconic,
    ExecuteWindowSwitch&& executeWindowSwitch)
{
    if (!fallbackRequired)
        return;
    executeSystemCommand(static_cast<WPARAM>(SC_RESTORE));
    if (isWindowStillIconic())
        executeWindowSwitch();
}

/**
 * @brief 判断显示请求后是否可以安全地将目标窗口切到前台。
 *
 * ShowWindowAsync 返回成功只代表请求已投递。最小化窗口必须等到
 * IsIconic 清除后再切换，确保所有应用（包括恢复到最大化的窗口）
 * 都在最终显示状态下置前。
 */
constexpr bool ShouldSwitchDockWindowAfterShow(
    bool wasMinimized,
    bool restoreCompleted) noexcept
{
    return !wasMinimized || restoreCompleted;
}

/**
 * @brief 判断最后活动弹窗能否作为 Dock 的实际激活目标。
 */
constexpr bool IsDockWindowActivationPopupEligible(
    bool valid,
    bool visible,
    bool iconic,
    bool noActivate) noexcept
{
    return valid && visible && !iconic && !noActivate;
}

/**
 * @brief 判断普通前台请求失败后是否可以共享输入队列重试。
 */
constexpr bool ShouldRetryDockWindowForegroundActivation(
    bool foregroundMatched,
    bool synchronousActivationSafe) noexcept
{
    return !foregroundMatched && synchronousActivationSafe;
}

/**
 * @brief 以前台请求优先、附加输入队列重试为后备激活一个 Dock 窗口。
 *
 * 两个请求都必须只操作最终激活窗口。普通路径不得先调用任务切换器或
 * BringWindowToTop；否则根窗口、最后活动弹窗和目标窗口会连续进入前台，
 * 改写无关应用之间的 Z-order。重试回调由平台层负责临时附加输入队列。
 */
template <typename IsForeground,
    typename RequestForeground,
    typename RetryForeground>
bool ApplyDockWindowForegroundActivation(
    bool synchronousActivationSafe,
    IsForeground&& isForeground,
    RequestForeground&& requestForeground,
    RetryForeground&& retryForeground)
{
    if (isForeground())
        return true;
    requestForeground();
    if (isForeground())
        return true;
    if (!ShouldRetryDockWindowForegroundActivation(
            false, synchronousActivationSafe))
        return false;
    retryForeground();
    return isForeground();
}

/**
 * @brief 根窗口与实际弹窗都可响应时，才允许执行同步置前操作。
 */
constexpr bool IsDockWindowSynchronousActivationSafe(
    bool rootWindowSafe,
    bool activationWindowSafe) noexcept
{
    return rootWindowSafe && activationWindowSafe;
}

/**
 * @brief 解析异步恢复/激活观察器的下一步动作。
 *
 * 恢复请求在窗口退出最小化前没有短超时；真正显示后则只在有限窗口内
 * 重试置前，避免一个已经被用户后续操作取代的旧请求长期抢占前台。
 */
constexpr DockWindowActivationObservationAction
ResolveDockWindowActivationObservationAction(
    bool windowValid,
    bool closePending,
    bool rootWindowSafe,
    bool awaitingRestore,
    bool iconic,
    bool foregroundMatched,
    bool activationRetryExpired) noexcept
{
    if (!windowValid || closePending || !rootWindowSafe ||
        foregroundMatched)
    {
        return DockWindowActivationObservationAction::Stop;
    }
    if (iconic)
    {
        return awaitingRestore
            ? DockWindowActivationObservationAction::WaitForRestore
            : DockWindowActivationObservationAction::Stop;
    }
    return activationRetryExpired
        ? DockWindowActivationObservationAction::Stop
        : DockWindowActivationObservationAction::Activate;
}

/**
 * @brief 判断最小化动画是否必须与顶层浮动 Dock 隔离。
 *
 * 最小化会立即捕获目标窗口所在的屏幕区域。如果浮动 Dock 仍在顶层，
 * 它会作为覆盖层被写入屏幕快照。调用方应先尝试只包含目标 HWND 的
 * DWM 缩略图；不可用时再关闭并同步 Dock 后执行屏幕抓取。恢复复用
 * 已有窗口快照或 DWM 缩略图，不需要隔离。
 */
constexpr bool RequiresFloatingDockMinimizeCaptureIsolation(
    bool floatingDockVisible,
    DockClickAction action) noexcept
{
    return floatingDockVisible &&
        action == DockClickAction::Minimize;
}

/**
 * @brief 判断最小化窗口是否应恢复到最大化状态。
 */
constexpr bool ShouldRestoreDockWindowMaximized(
    UINT placementFlags, UINT placementShowCommand) noexcept
{
    return (placementFlags & WPF_RESTORETOMAXIMIZED) != 0 ||
        placementShowCommand == SW_SHOWMAXIMIZED;
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
    return ShouldRestoreDockWindowMaximized(
            placementFlags, placementShowCommand)
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

/**
 * @brief 判断顶层窗口当前是否仍应显示在任务栏/Dock。
 *
 * 普通最小化窗口仍保留 WS_VISIBLE，因此可以继续显示。部分托盘应用会在
 * 处理 WM_CLOSE 时先最小化再隐藏窗口，此时 IsIconic 仍可能为真；隐藏状态
 * 必须优先，不能因为窗口仍处于最小化态就把它重新加入 Dock。
 */
constexpr bool IsTaskWindowPresentationEligible(
    bool visible, bool /*iconic*/) noexcept
{
    return visible;
}

/**
 * @brief 判断恢复窗口时是否播放图标到窗口的过渡动画。
 *
 * Dock 图标点击与预览缩略图点击共用同一条窗口命令路径：窗口最小化
 * （或反向取消最小化动画）且过渡层与锚点都可用时播放恢复动画，否则
 * 只能回退到无动画的直接恢复。
 */
constexpr bool ShouldAnimateDockWindowRestore(
    bool minimized,
    bool transitionAvailable,
    bool anchorAvailable) noexcept
{
    return minimized && transitionAvailable && anchorAvailable;
}

/**
 * @brief 判断可见预览是否应跟随悬停锚点漂移而不重建。
 *
 * Dock 放大让图标视觉矩形随指针逐像素变化；预览可见且身份仍匹配时，
 * 只移动预览窗口即可，重新弹窗会反复重注册缩略图。仅当身份变化或
 * 预览不可见时才回到重新挂起定时器的路径。
 */
constexpr bool ShouldFollowDockPreviewAnchor(
    bool previewVisible,
    bool identityMatched,
    bool anchorChanged) noexcept
{
    return previewVisible && identityMatched && anchorChanged;
}

} // namespace snowdesktop::dock_window_rules
