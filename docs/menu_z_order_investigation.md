# 集合弹窗右键菜单层级排查（2026-09-05）

本次检查以 `release/v1.0.5.0` 的 `8f37396b` 为基线。用户确认 Steam 测试分支 BuildID `25138237` 更新后仍失败。当前能够复现的是消息循环的恢复缺口；该设备在最新构建中首次发生层级反转的具体调用，仍需要新日志确认。

## 实测证据

| 材料 | 观察 |
| --- | --- |
| `SnowDesktop (1).log` | 35 次菜单会话；`apply-layer-policy` 造成 84 次 `beforeAbove=1 → afterAbove=0` |
| `SnowDesktop (2).log` | 14 次菜单会话；39 次同类反转；26 次能按菜单 HWND 配对到恢复成功记录 |
| 最新构建反馈 | 用户明确确认 `8f37396b` / BuildID `25138237` 仍失败；上述两份日志均早于该版本上传 |

第二份日志的 26 个配对间隔为 8–219 ms，平均 38.73 ms。计算起点为 `FloatingPopupZOrder` 的反转时间，终点为同一菜单 HWND 下一条 `restore-result rootAboveOwner=1`。该统计只覆盖能配对的记录，不代表所有会话的完整持续时间。

普通消息后的错误层级涉及 `WM_PAINT`、`WM_MOUSEMOVE`、`WM_MOUSELEAVE`、`WM_RBUTTONUP` 和应用消息。日志记录证明了对应函数调用前后的变化，尚不能区分其内部具体哪一次 Win32 调用或同步重入造成了变化。

## 已确认的实现缺口

1. **恢复依赖动画事件。** `modern_menu.cpp` 的原消息循环只在 `dispatchScheduledWork` 后调用 `RestoreOwnedPopupZOrder()`。普通消息处理完即提交画面，并继续处理后续输入。动画事件未就绪时，错误层级可以一直保留到菜单结束。
2. **保护仅覆盖部分入口。** `ApplyFloatingPopupLayerPolicy()` 传递了活动菜单，但 `UpdateFloatingPopupWindowBounds()` 的尺寸更新与首次显示直接调用 `SetWindowPos`；浮动 Dock 的策略也没有传递该保护参数。单独修补窗口对函数无法约束全部宿主位置变更。
3. **保护不能纠正已有反转。** 窗口对保护要求菜单当前已在宿主上方；该前提一旦失效，后续刷新仍可继续重排。因此需要同时防止宿主越过菜单并及时恢复错误位置。
4. **原恢复只检查根菜单。** 根菜单高于宿主时直接返回，忽略子菜单低于父菜单的情况；原逻辑也未明确排除已被新会话替换的旧菜单。

## 对前几轮结论的修正

- 旧日志没有 `shellPopupMenuLayerDepth_` 和请求的 `hWndInsertAfter`，无法据此认定当时请求 `HWND_NOTOPMOST`。普通项目菜单的 `ShowModernMenu()` 调用本身不创建 `ShellPopupMenuLayerGuard`；该 guard 用于原生 Shell 菜单路径。
- 第二份日志从 `LoadItems start` 开始，缺少进程启动记录。没有背景窗初始化行，不能单独证明该会话没有背景窗。
- “置顶菜单必然使其 owner 置顶”的解释不符合公开 API 约定。Microsoft 文档说明：提升窗口会提升它拥有的窗口，但不会改变它的 owner；非置顶窗口可以拥有置顶窗口。参见 [SetWindowPos](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos)。
- 现有回归测试只在动画回调中提升 owner，且未断言操作确实造成反转。测试通过不足以证明覆盖了用户日志中的失败状态。
- 尚无跨设备的系统版本、显卡驱动、刷新率与窗口配置对照，不能将设备差异归因到某一项。

## 本轮调整与验证方法

宿主在 `WM_WINDOWPOSCHANGING` 中检查当前可见的自有菜单。菜单位于宿主上方时，为本次位置变更添加 `SWP_NOZORDER`，保留几何调整；隐藏窗口以及已隐藏的旧菜单不受此保护。集合弹窗和浮动 Dock 共用该规则。该处理依据 Windows 允许在此消息中修改 `WINDOWPOS` 的约定，参见 [WM_WINDOWPOSCHANGING](https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-windowposchanging)。

菜单循环在普通消息处理后、画面提交前恢复菜单栈，并在提交后再次检查；恢复不再依赖动画事件。根菜单及级联子菜单均参与检查，退出中、隐藏或被替换的会话不执行恢复。

新增测试直接构造已观察到的反转状态，并断言状态确实发生，避免依赖某个 Windows 版本是否会在提升 owner 时自动重现问题。该测试不提供动画调度句柄，要求恢复发生在画面提交和下一条排队输入之前。测试在旧代码上失败，在本轮代码上通过。另有真实 Win32 窗口检查覆盖直接重排、几何调整、子菜单恢复以及旧菜单隐藏后的宿主层级变更。

`ModernMenuHostContext` 记录宿主、原生菜单深度、请求的 popup TOPMOST、背景窗可用性和玻璃设置；`ModernMenuHostGuard` 记录实际收到的插入目标及保护前后的 flags。原始消息处理后的状态与画面提交后的状态分别记录。

验证结果：

- `scripts/test.bat name modern_menu_interaction`：1/1 通过；新普通消息回归已在修改前明确失败。
- `scripts/test.bat name dock_and_window_rules`：1/1 通过。
- `scripts/build.bat --reload-shell`：通过，已生成 `.build/Release/SnowDesktop.exe`。
- `scripts/test.bat full`：115/115 通过，测试运行耗时 32.66 s。
- `git diff --check`：通过。

构建中存在 WinUI 生成头文件的既有 `C4002 / GetCurrentTime` 宏警告，未发现本次改动新增的警告。首轮沙箱内测试构建被 MSBuild FileTracker 的访问限制中断，改在标准权限环境完成验证；一次带管道符的批处理筛选调用因命令转义失败未执行测试，随后分别运行上述两个定向入口。

## 原设备验收

更新待验证构建后，在原集合弹窗中打开同一项目菜单，检查初次显示、连续右键替换、鼠标移动和级联子菜单。若仍失败，需要同次复现的 `ModernMenuHostContext`、`ModernMenuHostGuard`、`FloatingPopupZOrder` 和 `ModernMenuZOrder` 记录，以区分保护未触发、另一路径重排或恢复调用失败。桌面宿主的视觉验收由用户实机完成；独立 Win32 测试不能代替该验收。
