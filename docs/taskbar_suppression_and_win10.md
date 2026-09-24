# 任务栏隐藏与 Windows 10 外观适配

状态：开发候选，桌面交互与 Windows 10 Explorer 实测待完成。

## 用户行为

- Dock 页新增“始终隐藏系统任务栏”，默认关闭。只有 Dock 启用且宿主运行时才请求隐藏所有显示器的任务栏。
- 开启后，Dock 的 Windows 按钮强制可见，沿用已有开始菜单操作；设置页对应开关不可关闭。原 `showWindowsButton` 偏好不被写成 `true`，退出此模式后恢复原偏好。
- 设置关闭、Dock 关闭或宿主退出时撤销隐藏。Explorer 内的窗口状态持有宿主进程句柄，以窗口线程定时器检测异常退出并恢复；Explorer 重启后由宿主重新连接。
- 隐藏不改 Windows 的自动隐藏设置、注册表或工作区。因此原本非自动隐藏的任务栏仍可能保留系统工作区占位；这不等于释放任务栏空间。

## 实现与兼容边界

隐藏采用 Explorer 进程内的 `DWMWA_CLOAK`，作用于主、副任务栏窗口。针对已接管窗口拦截解除 cloak 的请求，保存系统最后请求的原状态，撤销时恢复。其他窗口、开始菜单和全局 Windows 键不在拦截范围。该机制使任务栏窗口不可见，并不取消 Explorer 的全部内部展开或焦点处理。

这条路径不依赖当前被动展开保护的 `Taskbar.dll` RVA/PDB 身份；现有被动保护仍限于已适配的 Win11 模块。DWM cloak 在 Win10 和 Win11 均有接口基础，不能据此将所有系统交互视为通过。

Win10 经典任务栏采用独立背景后端：系统 Accent 提供透明、模糊或亚克力材质，DirectComposition 背景层绘制纯色、多色渐变和 1 DIP 边框。背景层位于任务栏子窗口下方，不创建输入窗口，系统继续绘制按钮和托盘。沿用宿主已有的默认、可见窗口、最大化窗口和 Shell 界面规则。

Win10 的前景文字/图标配色跟随系统，模糊强度由系统决定；隐藏这两个无法独立控制的编辑项，保留已保存值。亚克力调用失败时尝试系统模糊。高对比度下释放自定义背景。经典任务栏对子窗口背景的处理、材质的实际效果和动态规则触发仍需 Win10 实测。

外观后端失败时单独恢复原生背景，保留仍被请求的任务栏隐藏；每 10 秒重试，外观配置变化时立即重试。设置页分别报告外观连接状态和实际主副任务栏隐藏状态。

私有设置 IPC 版本为 22；任务栏共享内存版本为 9，宿主、设置进程和 Hook 必须同时更新。没有组件 Lua API、capability 或 `apiVersion` 变化。`suppressSystemTaskbar` 属于独立 Dock 偏好，不进入布局备份；旧版本忽略该字段。

## 验证范围

`dock_and_window_rules` 中的独立窗口检查使用测试进程自己的窗口和真实 DWM/生产 Hook，核对强制 Windows 按钮且保留基础偏好、开始接管、外部解除 cloak 请求、ShowWindow/SetWindowPos 请求、正常释放、原有 cloak 保留以及宿主进程死亡后的恢复。子进程以挂起状态创建并由测试销毁，不进入宿主代码、不访问用户数据。

同一测试还调用经典背景的真实 DirectComposition 创建、渐变/边框绘制、横竖方向尺寸更新和释放入口。成功的 HRESULT 只能证明调用完成，不能证明 Win10 Explorer 中的最终像素和层次正确。

通过另一 DirectComposition 设备占用同一窗口的背景目标，实际复现外观后端失败，检查隐藏不会被解除、外观失败状态仍可见，以及释放冲突后修改外观可恢复绘制。其他窗口的 DWM 调用必须保持原行为。

设置 IPC 测试检查隐藏设置与原 Windows 按钮偏好同时跨进程保留。本地化检查覆盖全部语言键。

### 实机清单

| 环境 | 必须观察的场景 |
| --- | --- |
| Win11 | Dock 开关、隐藏开关、Windows 按钮点击、Win/Win+T/Win+B、触边、通知及窗口最小化；开始菜单关闭后继续操作 Dock |
| Win10 22H2 | 上述隐藏场景；纯色、透明、模糊、亚克力、多色渐变和边框；检查任务栏按钮/托盘背景是否遮挡自定义背景 |
| 多屏 | 主副任务栏、主屏切换、显示器增删、不同 DPI；Dock 只在一个屏幕时所有任务栏仍按选项隐藏 |
| 恢复 | 关闭选项、关闭 Dock、正常退出、终止宿主、Explorer 重启；原自动隐藏偏好和原 cloak 不被破坏 |
| 主题 | 系统浅/深色、高对比度、主题切换、休眠恢复、显卡/DWM 重建设备 |
| 动态规则 | 可见窗口、最大化、开始菜单/搜索/任务视图分别切换；各显示器配置互不串用 |

Win11 上的独立窗口测试不能代替 Win10 验收。可用 Win10 22H2 虚拟机检查经典任务栏；若虚拟显卡限制 DWM 材质，需真实设备补充。SnowDesktop 桌面宿主的观察与交互由用户完成，不使用桌面自动化工具绕过仓库规则。

## 2026-09-24 候选验证记录

代码输入为 `94f26dc24a6539f1d153f669da14c6b8091de76f`，包含此前的 `06434d9d` 和 `7b9ee917` 两次尝试。代码与测试已提交；当时工作区另有用户自己的 `docs/testing_audit_product_issues.md` 修改和 `.codex-remote-attachments/`，未修改、暂存或作为本轮构建输入。后续本节文档更新不改变运行输入。

环境：Windows 11，系统构建 `26300.9539`；Visual Studio 18 2026、MSVC `19.50.35730.0`、Windows SDK `10.0.26100.0`，Release x64。未在 Win10、其他 Windows 构建、虚拟显卡或真实多屏任务栏上验证。

| 检查 | 实际结果 |
| --- | --- |
| `scripts/build.bat` | 16:08:37–16:09:31，退出码 0，生成 Release 宿主和 Hook；完整日志无编译/链接警告 |
| 生产 Hook 对象的独立窗口检查 | `probe.bat` 退出码 0；包含隐藏、解除隐藏拦截、原状态恢复、宿主进程死亡、经典外观调用、合成目标冲突和恢复 |
| 负向对照 | 只在隔离副本中放行解除 cloak，`negative.bat` 退出码 2，命中两条预期显示失败；未覆盖生产源码 |
| 外观失败回归 | 前一版本在真实合成目标冲突下解除隐藏，复现退出码 1；相同冲突在本候选中保留隐藏，解除冲突并修改外观后恢复绘制 |
| 最终 `scripts/test.bat` | 16:11:55–16:15:01，脚本退出码 1、CTest 退出码 8；119/120 通过，`shell_launch_worker` 失败；默认排除 `manual` 条目 |
| 本次受影响条目 | `dock_and_window_rules`（含真实 DWM/经典后端检查）、设置/IPC、动态任务栏规则、设置页面契约和本地化条目通过 |
| 桌面验收 | Win10 Explorer 外观、Win11 Dock/开始菜单交互、设置页面状态提示、多屏及设备恢复均待实机 |

最终全量的配置阶段 3.25 秒、编译及输出整理阶段 29.99 秒、CTest 阶段 151.84 秒。不能将这一轮写成“全量通过”。失败信息为 `reopening an existing folder must keep Explorer visible`，诊断值 `kind=4 attempt=1 completed=1 result=0 visible=0`；属于未修改的 Shell 集成路径，原因未解决。本轮并未向 Explorer 加载新任务栏 Hook，也未启动桌面宿主做视觉验证。

此前 `7b9ee917` 上的一轮全量同样为 119/120，其中 `steam_runtime_update` 失败。隔离诊断复现恢复用测试宿主返回 `0xC0000142`（`STATUS_DLL_INIT_FAILED`），版本回退指针和日志已正确写入，具体 DLL 初始化失败来源未定位。该条目在最终一轮通过，但不能用此结果抹去之前的不稳定失败；不将两轮结果拼成一次有效全量基线。未为这两条检查增加重试或放宽断言。

最终可执行文件 SHA-256：

- `.build/Release/SnowDesktop.exe`：`4D262B9A826C18AA34BFD6C43B348F3A3379FE3F01E019BA8559E5C7FEB906E2`
- `.build/Release/SnowDesktop.Runtime/SnowDesktopTaskbarHook.dll`：`B9969CE893617E4F65CC64FB0ACF4B957E7BA3B6C5922FCB6951C95B175F8849`

本机原始日志保存在忽略目录 `.codex-probes/taskbar-native/`：`build-3.log`、`full-tests-2.log`、`probe-3.log`、`negative-3.log`、`recovery-before.log`、`steam-diagnostic-1.log`。`final-inputs.json` 记录各改动文件及构建入口的 SHA-256，`final-test-selection.json` 记录实际 CTest 选择集合，`final-artifacts.json` 记录产物和日志指纹。两轮 CTest XML 也已留存；这些是本机证据，不是构建依赖或发行内容。

## 参考

- [微软 DWM 窗口属性](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute)
- [TranslucentTB 经典任务栏材质与恢复路径](https://github.com/TranslucentTB/TranslucentTB/blob/release/TranslucentTB/taskbar/taskbarattributeworker.cpp)

具体构建、测试、负向对照和产物证据随候选提交记录；未实际运行的项目不计为通过。
