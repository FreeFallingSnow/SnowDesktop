# 任务栏隐藏与 Windows 10 外观适配

状态：开发候选，桌面交互与 Windows 10 Explorer 实测待完成。

## 用户行为

- Dock 页新增“始终隐藏系统任务栏”，默认关闭。只有 Dock 启用且宿主运行时才请求隐藏有 Dock 的显示器上的任务栏；无 Dock 的屏幕不受强制隐藏。更改首屏/末屏固定位置、Dock 显示范围或显示器布局后，下一次守护定时器更新旧、新屏幕的接管状态。
- 开始菜单、搜索、通知中心等已有检测器识别到的系统面板打开时，临时放行所在屏幕的任务栏；任务视图打开时放行所有屏幕。关闭面板后恢复隐藏。这与外观规则是否启用无关，不改变保存的隐藏开关或 Windows 按钮偏好。
- 开启后，Dock 的 Windows 按钮强制可见，沿用已有开始菜单操作；设置页对应开关不可关闭。原 `showWindowsButton` 偏好不被写成 `true`，退出此模式后恢复原偏好。
- 设置关闭、Dock 关闭或宿主退出时撤销隐藏。Explorer 内的窗口状态持有宿主进程句柄，以窗口线程定时器检测异常退出并恢复；Explorer 重启后由宿主重新连接。
- 为释放非自动隐藏任务栏的工作区占位，隐藏模式运行期间临时启用 Windows 自动隐藏；这是系统全局设置，无 Dock 的屏幕也会暂时采用自动隐藏，但允许正常唤起。关闭隐藏模式、Dock 或宿主退出后恢复原值。主任务栏在 Explorer 中持有恢复责任，宿主异常退出仍可恢复；原值保存在共享映射中，Explorer 重启后继续使用。只改变自动隐藏位，保留其他系统状态位；面板临时显示不重新占用工作区。若 Shell 拒绝恢复，保留定时器重试；宿主和 Explorer 同时崩溃导致共享映射丢失时，无法保证恢复原值。
- Dock 设置中“项目与行为”位于“位置与布局”前。手动开启 Dock 后，仅“项目与行为”和“悬浮 Dock 与边缘手势”短暂高亮；快照同步不重播，关闭系统动画时静态提示，离开页面或关闭 Dock 时清除。
- 新配置默认开启“全屏应用时禁用手势唤起”；已保存的显式关闭值保持不变。

## 实现与兼容边界

隐藏采用 Explorer 进程内的 `DWMWA_CLOAK`，作用于主、副任务栏窗口。针对已接管窗口拦截解除 cloak 的请求，保存系统最后请求的原状态，撤销时恢复。其他窗口、开始菜单和全局 Windows 键不在拦截范围。该机制使任务栏窗口不可见，并不取消 Explorer 的全部内部展开或焦点处理。

这条路径不依赖当前被动展开保护的 `Taskbar.dll` RVA/PDB 身份；现有被动保护仍限于已适配的 Win11 模块。DWM cloak 在 Win10 和 Win11 均有接口基础，不能据此将所有系统交互视为通过。

Win10 经典任务栏采用独立背景后端：系统 Accent 提供透明、模糊或亚克力材质，DirectComposition 背景层绘制纯色、多色渐变和 1 DIP 边框。背景层位于任务栏子窗口下方，不创建输入窗口，系统继续绘制按钮和托盘。沿用宿主已有的默认、可见窗口、最大化窗口和 Shell 界面规则。

Win10 的前景文字/图标配色跟随系统，模糊强度由系统决定；隐藏这两个无法独立控制的编辑项，保留已保存值。亚克力调用失败时尝试系统模糊。高对比度下释放自定义背景。经典任务栏对子窗口背景的处理、材质的实际效果和动态规则触发仍需 Win10 实测。

外观后端失败时单独恢复原生背景，保留仍被请求的任务栏隐藏；每 10 秒重试，外观配置变化时立即重试。设置页分别报告外观连接状态和实际主副任务栏隐藏状态。

私有设置 IPC 版本为 22；任务栏共享内存版本为 11，分别保存各任务栏的隐藏目标、面板状态及自动隐藏恢复状态，宿主、设置进程和 Hook 必须同时更新。未出现在目标列表里的任务栏不会被全局标记强制隐藏。任务栏屏幕匹配使用最近显示器，以免自动隐藏把窗口移到屏幕外后丢失面板和 Dock 所属屏幕。没有组件 Lua API、capability 或 `apiVersion` 变化。`suppressSystemTaskbar` 属于独立 Dock 偏好，不进入布局备份；旧版本忽略该字段。

## 验证范围

`dock_and_window_rules` 中的独立窗口检查使用测试进程自己的窗口和真实 DWM/生产 Hook，核对强制 Windows 按钮且保留基础偏好、开始接管、外部解除 cloak 请求、ShowWindow/SetWindowPos 请求、正常释放、原有 cloak 保留以及宿主进程死亡后的恢复。子进程以挂起状态创建并由测试销毁，不进入宿主代码、不访问用户数据。

同一测试还调用经典背景的真实 DirectComposition 创建、渐变/边框绘制、横竖方向尺寸更新和释放入口。成功的 HRESULT 只能证明调用完成，不能证明 Win10 Explorer 中的最终像素和层次正确。

通过另一 DirectComposition 设备占用同一窗口的背景目标，实际复现外观后端失败，检查隐藏不会被解除、外观失败状态仍可见，以及释放冲突后修改外观可恢复绘制。其他窗口的 DWM 调用必须保持原行为。

设置 IPC 测试检查隐藏设置与原 Windows 按钮偏好同时跨进程保留。本地化检查覆盖全部语言键。

面板回归在经典及 Win11 两条原生控制路径下覆盖单屏放行、多屏隔离、任务视图全屏放行、关闭面板立即重新隐藏以及保持同一接管实例。自动隐藏检查仅替换 `SHAppBarMessage` 的系统设置边界，不修改测试机的真实任务栏设置；覆盖原开/关偏好、退出恢复、进程死亡恢复、Explorer 重连和系统拒绝应用/恢复。真实工作区尺寸和真实面板识别仍需实机确认。

### 实机清单

| 环境 | 必须观察的场景 |
| --- | --- |
| Win11 | Dock 开关、隐藏开关、Windows 按钮点击、Win/Win+T/Win+B、触边、通知及窗口最小化；开始菜单关闭后继续操作 Dock |
| Win10 22H2 | 上述隐藏场景；纯色、透明、模糊、亚克力、多色渐变和边框；检查任务栏按钮/托盘背景是否遮挡自定义背景 |
| 多屏 | 主副任务栏、主屏切换、显示器增删、不同 DPI；Dock 只在一个屏幕时仅该屏任务栏被强制隐藏；交换首屏/末屏后旧侧恢复、新侧接管，无须重启 |
| 恢复 | 关闭选项、关闭 Dock、正常退出、终止宿主、Explorer 重启；原自动隐藏偏好和原 cloak 不被破坏 |
| 主题 | 系统浅/深色、高对比度、主题切换、休眠恢复、显卡/DWM 重建设备 |
| 工作区 | 原自动隐藏开/关两种状态；开启隐藏后最大化应用与桌面布局不留任务栏空位，临时面板显示不挤压布局；关闭和退出后恢复原值 |
| Dock 设置 | 卡片顺序；只在手动启用时高亮两张卡片；浅/深色、高对比度和关闭系统动画；全屏手势默认开启且已有关闭值保留 |
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

## 2026-09-24 面板、占位与设置调整验证记录

本轮代码输入为 `e85a683eada565c71a7838813f2adeacea9eded1`，环境与前一记录相同。用户已有的 `docs/testing_audit_product_issues.md` 修改和 `.codex-remote-attachments/` 未修改或暂存。下面的结果仅属于本候选，不能与前一轮拼接成全量通过。

| 检查 | 实际结果 |
| --- | --- |
| `scripts/build.bat --reload-shell` | 16:32:15–16:43:22，退出码 0；标准 Release x64 宿主与 Hook 生成，无编译/链接警告。构建前已告知停止 SnowDesktop 并重载 Explorer |
| 独立窗口 `probe.bat` | 退出码 0；实际 DWM/生产 Hook 覆盖面板单屏放行、任务视图多屏放行、面板关闭后重新隐藏、原自动隐藏偏好恢复及失败重试；系统自动隐藏 API 使用替身，不更改测试机真实设置 |
| 面板负向对照 | `negative.bat` 在隔离副本中移除面板放行，退出码 6，命中两条原生路径各 3 个预期失败；未覆盖生产源码 |
| 工作区负向对照 | `negative-work-area.bat` 在隔离副本中跳过临时自动隐藏，退出码 8，命中 8 个预期生命周期失败；未覆盖生产源码 |
| `scripts/test.bat` | 16:44:14–16:47:24，脚本退出码 1、CTest 退出码 8；119/120 通过，`component_preview` 失败；默认排除 `manual` 条目 |
| 受影响检查 | `dock_and_window_rules`、`settings_controller`（含 IPC）、Dock 设置页契约、本地化及现有动态规则检查通过；显式关闭的全屏手势限制在 IPC 中保留 |
| 桌面验收 | 真实面板检测、任务栏临时显示、工作区释放与恢复、两张卡片高亮、多屏及 Win10 Explorer 外观仍待用户实机验证 |

全量配置耗时 2.41 秒、编译及输出整理 40.62 秒、CTest 145.35 秒，完整日志未发现编译或链接警告。`component_preview` 的具体失败是 `tests/component_preview_tests.cpp` 中测试夹具的 `SetCursorPos(0, 0)` 返回失败，消息为 `the preview fixture can move outside the pending preview`；尚未进入后续兄弟预览切换的行为断言。该输入操作失败的具体环境原因未定位，没有修改该模块或放宽断言，也没有靠重复执行获取绿色结果。之前失败过的 `steam_runtime_update` 和 `shell_launch_worker` 本轮通过，但不据此声明它们的历史不稳定问题已解决。

完整测试构建后产物 SHA-256：

- `.build/Release/SnowDesktop.exe`：`E8A71619FF71C17A18F1E2B2C6B921E2C478480930C4E80260D0BA89EE8A70D0`
- `.build/Release/SnowDesktop.Runtime/SnowDesktopTaskbarHook.dll`：`B45C62EEB06ED2CA90E755D3A210FD2D596E41C09002FBEC14DEA56666F16AD3`

本机证据位于忽略目录 `.codex-probes/taskbar-panels/`：`build-1.log`、`build-1-result.json`、`full-tests.log`、`full-tests-result.json`、`full-tests.xml`、`probe-1.log`、`negative-panels.log` 和 `negative-work-area.log`。`final-inputs.json` 绑定候选提交、变更文件及构建入口哈希，`final-test-selection.json` 保存实际自动测试集合，`final-artifacts.json` 保存产物与日志哈希。本节后续文档提交不改变运行输入。

## 2026-09-24 Dock 范围与屏幕外匹配验证记录

本轮代码输入为 `68a6f9a7c77473a303afdee14f04d1112a806e2a`，环境与前一记录相同。按用户确认，将强制隐藏范围改为有 Dock 的屏幕。暂停的全屏手势尝试已另存于本机忽略目录 `.codex-probes/fullscreen-swipe-shelved/`，没有混入本候选；用户已有文档和附件也未修改、暂存。

| 检查 | 实际结果 |
| --- | --- |
| `scripts/build.bat --reload-shell` | 17:42:12–17:51:44，退出码 0；Release x64 宿主和 Hook 生成，无编译/链接警告。构建前检查到正在运行的宿主及 Explorer 中的多份旧 Hook，已告知并重载 Shell |
| 独立窗口 `probe.bat` | 退出码 0；真实 DWM/生产 Hook 验证 Dock 目标双向切换、旧目标释放、未匹配任务栏不被隐藏，以及屏幕外任务栏仍按面板所属屏幕放行。自动隐藏系统 API 使用替身 |
| 范围负向对照 | `negative-scope.bat` 在隔离副本恢复全局隐藏，退出码 6；两条原生路径分别命中换屏两个方向及移除目标的预期失败 |
| 屏幕匹配负向对照 | `negative-monitor.bat` 在隔离副本恢复 `MONITOR_DEFAULTTONULL`，退出码 4；两条原生路径分别命中面板提前放行和面板期间放行的预期失败 |
| `scripts/test.bat` | 17:52:48–17:55:33，脚本退出码 1、CTest 退出码 8；119/120 通过，默认排除 `manual` 条目。`component_preview` 再次在测试夹具移动鼠标时失败 |
| 受影响检查 | `dock_and_window_rules`、`settings_controller`、Dock 设置页契约和本地化通过；`shell_launch_worker`、`steam_runtime_update` 本轮也通过 |
| 实机验收 | 真实首屏/末屏交换、Dock 显示范围切换、原自动隐藏关闭时的开始菜单等面板个性化、Win10 外观及多屏实际表现仍待用户验证 |

全量配置耗时 2.42 秒、编译及输出整理 36.50 秒、CTest 125.40 秒，完整日志未发现编译或链接警告。唯一失败仍为 `the preview fixture can move outside the pending preview`，对应 `SetCursorPos(0, 0)` 返回失败；没有修改该组件预览路径或放宽断言，也没有重复执行来取得绿色结果。因此本候选不能记录为全量通过，历史不稳定问题也未宣称解决。

本候选保留原有工作区释放机制：临时开启的是 Windows 全局自动隐藏，无 Dock 屏幕允许正常唤起但也暂时采用自动隐藏；关闭隐藏功能后恢复原设置。私有协议为 v11，宿主与 Hook 应作为同一构建使用。

完整测试构建后产物 SHA-256：

- `.build/Release/SnowDesktop.exe`：`8FE75100FC421A994CF80FA59EE5D0B6DF0B2F7D504A6F3DD1A4D1623561A79E`
- `.build/Release/SnowDesktop.Runtime/SnowDesktopTaskbarHook.dll`：`4A9E7DC0DD32BA7D4C4CDDBAE1E3B5E604C6A1063ECCA1F8CAAC9E133D5BC1FC`

本机证据保存在 `.codex-probes/taskbar-scope/`：`build-1.log`、`build-1-result.json`、`full-tests.log`、`full-tests-result.json`、`full-tests.xml`、`probe-1.log`、`negative-scope.log` 和 `negative-monitor.log`。`final-inputs.json`、`final-test-selection.json`、`final-artifacts.json` 分别记录源码输入、实际自动测试集合和产物/日志哈希；本节后续文档提交不改变运行输入。

## 2026-09-24 个性化连接异常反馈与会话清理

用户在 `68a6f9a7` 候选上反馈任务栏始终跟随系统主题，修改个性化没有效果，并提供持续显示“正在连接 Explorer 任务栏”的设置截图。此反馈表示外观实机验收未通过，不能将前述独立窗口回归当成真实 Explorer 外观已经正常。

清理前只读核对得到：Explorer PID 为 `76116`，其中只有一个 SnowDesktop Taskbar Hook 副本，来源为此前宿主进程 `45960` 的 `data/ShellHook/` 目录；其 SHA-256 为 `4A9E7DC0DD32BA7D4C4CDDBAE1E3B5E604C6A1063ECCA1F8CAAC9E133D5BC1FC`，与当时的发行 Hook 相同。驻留副本确实存在，但没有证据表明它是旧版 DLL 或协议不匹配。

只读共享状态为 v11，`enabled=1`、`appearanceEnabled=1`、`suppressTaskbar=0`，宿主 PID 为 `31952`，包含两个任务栏目标；`status=2`（Connected）、`lastError=0`、`diagnosticStage=240`（XAML 订阅完成）。这说明连接已建立但外观未报告 Applied；设置页将此状态统一显示为“正在连接”。尚未定位是视觉树登记、任务栏重建还是会话交接导致，不能仅凭该状态宣称旧 Hook 已确认为根因。

告知用户副作用后执行 `scripts/build.bat --reload-shell`，18:08:47–18:09:25，退出码 0，无编译或链接警告。新 Explorer PID 为 `68592`，检查时未加载 SnowDesktop Taskbar Hook。此次没有修改生产代码或测试，也没有删除布局和设置；未重复运行全量，前次全量失败仍保留。用户重新启动软件后的外观及连接状态待反馈，没有使用桌面自动化启动或操作宿主。

本次标准构建后的 SHA-256：宿主 `A8A14C95AB9B6E27F5F54A645FA606ED54EC5758560294DDEC6F972BFA4D33F2`，Hook `DA8879ABE76344761EC2F417F5FB133164A652BF54ABB0B9B45513D6B93A2C76`。原始共享状态和构建记录位于忽略目录 `.codex-probes/taskbar-reconnect/` 的 `before.json`、`build.log`、`build-result.json`。

## 2026-09-24 个性化恢复反馈与隐藏状态候选

用户确认上一节清理 Explorer 会话后，个性化已恢复；这验证了该次会话清理的恢复效果，不代表驻留 Hook 的异常根因已经定位或软件内重连缺陷已全部消除。随后用户反馈“始终隐藏”实际生效但设置仍显示正在连接，截图后已关闭该选项；之后读取到 `suppressTaskbar=0` 与用户后续操作一致，不能用于否定截图中的问题。

新代码输入为 `ff5acd6248ba5505243ac9d629fd95a0c2111d91`，环境与前文相同。宿主为已经连接的 XAML 外观补挂原生隐藏时，不再重置外观状态或重复等待首次订阅的 Ready 事件；主副任务栏均先在各自线程挂接，包括宿主缓存的临时重挂父窗口的目标。隐藏运行状态仅检查快照中有 Dock 的目标，面板临时放行仍属正常接管，不要求无 Dock 屏幕也完成原生隐藏挂接。私有 v11 协议布局及公共组件 API 未改变。

| 检查 | 实际结果 |
| --- | --- |
| `scripts/build.bat --reload-shell` | 19:31:29–19:32:11，退出码 0，Release x64 宿主及 Hook 生成；无编译或链接警告，执行前已说明停止软件和重启 Explorer |
| 独立窗口 `probe.bat` | 退出码 0；真实线程 Hook、生产连接流程、原生接管及 DWM 覆盖首次连接、外观已连接后的隐藏接管、取消、无 Dock 屏幕、面板放行、未 cloaked 目标及空目标 |
| 重复等待负向对照 | `negative-ready.bat` 在隔离头文件中恢复每次等待首次 Ready，退出码 1，命中开启隐藏后无法完成连接的预期失败 |
| 范围负向对照 | `negative-scope.bat` 在隔离头文件中恢复检查无 Dock 屏幕，退出码 2，命中普通隐藏及面板临时放行的两个状态失败 |
| `scripts/test.bat` | 19:33:42–19:36:27，退出码 0；自动集合 120/120 通过，CTest 134.26 秒；默认排除 `manual` 条目 |
| 实机验收 | 个性化清理后的恢复已获用户确认；新候选的隐藏开关往返、连接提示消失、多屏及真实面板操作仍待用户确认；Win10 Explorer 外观仍未实测 |

新增回归使用测试进程的独立窗口和未命名事件，仅替换 XAML 首次通知及系统自动隐藏设置边界，不操纵实际 Explorer 或宿主窗口，也不修改真实自动隐藏偏好。负向对照没有改写生产源码。前文 `component_preview` 等历史不稳定失败的原因并未解决；本次单次通过如实保留，不靠重试获得绿色结果，也不据此宣称历史问题已经消失。

最终产物 SHA-256：宿主 `7ADA20C4068AE732CE951C536C38998DC3AC813211230DC4588EDAFAA11421DF`，Hook `2F5F3FB680DC99E4393C469F2EDC738EE7C129C14241763C446FEDDC0DD37539`。本机证据在 `.codex-probes/taskbar-status/`：构建、完整测试、独立窗口及两项负向对照的日志和结果 JSON；`full-tests.xml`、`final-test-selection.json`、`final-inputs.json` 和 `final-artifacts.json` 绑定自动测试集合、源码/工具链与产物哈希。用户已有审计文档修改和附件目录均未修改或暂存。

## 2026-09-24 隐藏提示验收与 Win10 测试分支交付

用户对 `ff5acd62` 候选反馈“没问题了”，确认此前已隐藏但持续显示连接中的问题不再出现，并要求精简设置提示后上传测试分支用于 Win10 验证。该反馈仅覆盖用户本次操作，不扩展为所有面板、多屏与 Win10 场景已经验收。

文案候选为 `a58bee83b946a2f9549ad104fc9ab5a35adecda2`：移除任务栏主题与 Windows 设置入口中重复标题的说明，压缩始终隐藏、全屏手势、规则顺序及 Windows 10 限制提示；全部十种语言同步，保留隐藏范围、面板临时显示、Windows 按钮及错误提示，未改变行为。

| 检查 | 实际结果 |
| --- | --- |
| `scripts/build.bat --reload-shell` | 19:43:51–19:44:48，退出码 0，标准 Release 构建通过；无编译或链接警告 |
| `scripts/test.bat` | 19:45:13–19:47:51，退出码 0，120/120 通过，含本地化及设置页检查；配置 2.41 秒、编译整理 22.73 秒、CTest 131.07 秒，默认排除 `manual` |
| `scripts/package_steam.ps1 -SkipBuild` | 19:48:16–19:48:31，退出码 0；406 个文件逐项大小、哈希核对通过，宿主和 Hook 与已测产物一致；没有用户数据、开发 App ID 文件或官方社区组件源码 |
| SteamPipe 上传 | `scripts/steam_pipe.ps1 -Mode UploadDev -SkipPackage -Yes -ConfirmVersion 1.0.7.0 -ConfirmPrivateBranch internal-dev`，退出码 0，19:49:50 返回 BuildID **25504408**；App/Depot 为 `5080330/5080331` |
| 分支与客户端更新 | 桌面 Steam 的安装清单确认 `BetaKey=internal-dev`、`buildid=25504408`；19:50:36 的内容日志确认下载提交完成，Depot manifest 为 `2074353354913472653`；已安装的 406 个发行文件全部匹配上传包哈希 |
| Steam 客户端恢复 | 上传与查询结束后正常关闭并以 `-silent` 重启原有客户端；19:50:15 的新连接日志确认登录 `OK`，未强杀游戏或进程树 |
| 待验证 | 精简文案的实机排版、Win10 任务栏材质/渐变/边框、面板放行、工作区恢复及多屏交互由用户继续验证 |

SteamCMD 的 `app_info_print` 只返回公开分支，并未提供私有分支 BuildID；浏览器没有已登录的 Steamworks 会话。初次查询结果如实保留为未确认，后续通过桌面 Steam 的私有分支下载、安装清单和全部文件哈希完成核对，没有再次登录 SteamCMD 打断已恢复的客户端。上传成功与查询缺少私有分支信息是两项不同结果。

运行包标识为 `1.0.7.0-2249ba309928f45d`。宿主 SHA-256 为 `C8902808C8A2E6872AE0BA4E7CD56CA54EF7AD9A2C684CCD2A00DC0FD59C63D0`，Hook 为 `8A0CBD5605B92BAC7076DF953B04D47CC8443C28F3BA14EC6202B0EF251DDF83`，Steam ZIP 为 `6EA41C494738C87B9BE9E7BD5C86154B7F6DB33E0B780072B48B51A981027D9D`。

本次证据统一保存于 `artifacts/v1.0.7.0/taskbar-win10-test-20260924/`：`inputs.json`、构建及测试日志/结果、`full-tests.xml`、打包及哈希检查、`upload.log`、`publish-result.json`、`client-verification.json`、`client-update.log` 和 `steam-restoration.json`。发布只涉及 Steam 测试分支；未推送 Git 分支、修改 `main`、创建版本标签或发布 GitHub Release。用户已有审计文档修改和附件目录保持原状。

## 参考

- [微软 DWM 窗口属性](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute)
- [微软 ABM_SETSTATE](https://learn.microsoft.com/en-us/windows/win32/shell/abm-setstate)（返回值不能证明状态已生效，须读取实际状态）
- [TranslucentTB 经典任务栏材质与恢复路径](https://github.com/TranslucentTB/TranslucentTB/blob/release/TranslucentTB/taskbar/taskbarattributeworker.cpp)

具体构建、测试、负向对照和产物证据随候选提交记录；未实际运行的项目不计为通过。
## 2026-09-24：Windows 10 颜色实测失败

用户在 Windows 10 虚拟机反馈：毛玻璃、亚克力材质可以切换，但所有背景颜色均无效果。
用户提供的 `current-runtime.txt`、`confirmed-runtime.txt`、`launch-history.txt` 均记录
`1.0.7.0-2249ba309928f45d`，与测试分支 BuildID `25504408` 的运行包一致；
`previous-runtime.txt` 记录 `1.0.7.0-3118c0e99fcd38d8` 是前一版本记录。
这证明新版运行包已经选择并确认启动，但不能单独证明 Explorer 中实际加载的 Hook 身份。

结论：包含初始 Win10 后端 `06434d9d` 的候选未通过 Win10 背景颜色验收。
现有隔离测试只验证 DirectComposition 调用成功及控制器生命周期，没有验证最终可见像素，
不能代替 Win10 Explorer 的视觉验收。当前怀疑原生任务栏与附加合成图层的兼容性，根因待查。

## 2026-09-24：原生着色、独立背景与共享主题候选

用户进一步确认纯色和渐变都无效，要求参照 TranslucentTB，并把 Win10 浅深色控制恢复到原提示位置，
与底部“开始菜单与系统面板”的设置双向同步。

### 实现与参考

- `a3b921ff`：参照 [TranslucentTB 的经典任务栏实现](https://github.com/TranslucentTB/TranslucentTB/blob/322e2b7395a51975150126276308b415970e080b/TranslucentTB/taskbar/taskbarattributeworker.cpp)，
  将纯色的非预乘 ABGR 和透明度传给 `ACCENT_POLICY`，亚克力失败回退毛玻璃时保留颜色。
  Win11 的 XAML 后端与这条 Win10 路径独立。
- `5585ffda`：额外绘制改为任务栏下方的独立窗口，原生任务栏保持透明、图标仍位于其上方。
  参考了 [RainbowTaskbar 的背景窗口放置方式](https://github.com/ad2017gd/RainbowTaskbar/blob/5ee0f34e91e401f4f0c806d5fc4994b5feb9887a/RainbowTaskbar/Helpers/Taskbar.cs)，
  未复制其实现。背景窗口不激活、穿透鼠标，随任务栏移动、隐藏、抑制和销毁；绘制范围裁切到显示器。
  这样不再占用 Explorer 的 DirectComposition 目标，也不把颜色画到任务栏图标之上。
- `ca1469eb` 保留失败证据：独立窗口的渐变像素正确，但原生纯色着色没有显示指定红色。
  `fd1cdbb6` 随后在独立画布中统一绘制颜色、渐变和边框，原生材质只负责模糊；
  无渐变和边框的纯色样式仍可直接使用任务栏原生着色，不重复叠加透明度。
- 设置页现状审查确认：底部选项已通过同一个控制器写入 `SystemUsesLightTheme`，原来的 Win10
  图标与文字选项却被隐藏。因此复用原行显示浅色/深色并复用该控制器；两处选择立即同步，周期刷新
  读取同一份请求/系统状态。该行说明它与系统面板共用主题，十种语言同步更新。场景规则不单独切换
  Windows 全局主题，Win11 的独立图标配色逻辑保持原有路径。

### 实际验证边界

本轮标准构建均生成 Release 宿主，未出现编译或链接警告。最后一次运行时输入的构建是
`scripts/build.bat`（`solid-canvas-build.log`）；此前为更新占用的 Hook，按规则运行过
`scripts/build.bat --reload-shell`。一次普通构建被应用运行预检阻止，退出码 3，未计为编译失败或通过。

隔离 Win11 测试使用自建的透明窗口及绿色前景标记，直接调用生产 `ClassicSurface`：

- 最终纯色像素为左右均 `COLORREF 0x0000ff`（红色）；渐变左右为 `0x0c00f3`、`0xf3000c`。
- 两种情况下前景均为 `0x00ff00`（绿色），隐藏与销毁检查通过。
- 在生产源码隔离副本恢复旧的透明颜色策略，原生颜色测试出现 6 个预期失败；把绘制重新绑定到
  任务栏自身目标，独立背景测试出现 3 个预期失败。负向对照没有覆盖用户工作区源码。

两次全量曾各有 119/120 通过，唯一失败是组件预览夹具调用 `SetCursorPos`，尚未到产品行为断言。
`8a49ddff` 将测试窗口移到虚拟桌面之外，保留真实“指针在预览外”的条件，取消移动用户鼠标的依赖，
未修改组件预览生产代码。定向 `component_preview` 1/1 通过；在隔离源码删除待替换预览的关闭保护后，
同一个测试准确失败于 `a stale close timer keeps the old frame while a sibling is pending`。
因此不是忽略失败、无条件重试或放宽原问题断言。后续全量结果单独登记。

这些证据验证了颜色输出、前后层级、生命周期及测试失效信号，**仍不能替代 Win10 Explorer 验收**。
需要用户测试：纯色及透明度、渐变、边框、毛玻璃/亚克力叠加、图标可见与可点击、任务栏自动隐藏、
Dock 抑制与系统面板临时显示、多屏移动以及上下两处主题选项同步。更新 Hook 后应重启一次 Explorer，
避免驻留的旧 DLL 继续参与本轮测试。

证据目录：`artifacts/v1.0.7.0/taskbar-win10-color-20260924/`。`inputs.json` 绑定源码、测试、资源、
工具链及候选提交；构建、测试、像素探针、负向对照、打包和上传各保留独立日志。
当前运行包身份 `1.0.7.0-0632740838f492d0`；404 个运行载荷文件已逐项核对大小和 SHA-256，
加启动器及清单共 406 个文件。宿主和 Hook 与实际构建输出哈希一致。

### 最终交付记录

- `scripts/build.bat`：20:51:54–20:52:32，退出 0；随后仅调整预览测试源码和文档，宿主输入未变，
  复用该有效标准构建。宿主 SHA-256 `ACC6AFC8BC58B822CA9F261E2CD08C756E13BC9499E0E27E096478B3FDA2C895`，
  Hook SHA-256 `DFE7D2DE1617433EDC94B673D937E838034DEFAA18EBECFD31CE75AA3DD3B02A`。
- `scripts/test.bat full`：21:00:19–21:02:48，本次 **120/120 通过**，脚本退出 0，CTest 132.34 秒；
  无编译或链接警告。测试报告 `test-run-da03243b100449018bd525fdeaa4add3.xml`，绑定 `8a49ddff`。
- `scripts/package_steam.ps1 -SkipBuild`：退出 0；便携 Steam ZIP SHA-256
  `A7E339D6D91ED567AE83AF8884CDE1B533544218ECBC8111F22FE5BFF75E6044`。
- `scripts/steam_pipe.ps1 -Mode UploadDev -SkipPackage -Yes -ConfirmVersion 1.0.7.0 -ConfirmPrivateBranch internal-dev`：
  21:03:55 上传成功，BuildID **25505879**；只更新私有测试分支，没有推送 Git 分支、main 或版本标签。
- 本机 Steam 于 21:03:58 重启，21:04:25 新连接日志确认重新在线；21:04:52 安装完成。
  客户端清单为 `internal-dev` / `25505879`，Depot manifest `679889778882297972`，406 个安装文件
  与上传包逐项哈希一致。上传脚本中远程查询字段保留未验证状态，实际分支验收以
  `client-verification.json` 和本次客户端下载日志为准；未在客户端恢复后再次登录 SteamCMD。

交付仍是 Win10 待实机验证候选，不把隔离像素、全量测试和成功上传写成 Win10 Explorer 缺陷已经解决。

## 2026-09-24：Win10 默认自动主题候选

用户要求 Win10 的主题和颜色提供默认“自动”，并确认自动应匹配 SnowDesktop 当前任务栏外观。
在现有主题行和底部系统面板选项中增加自动，不增加卡片。两处共用持久化的
`classicTaskbarSystemTheme`（-1 自动、0 浅色、1 深色），旧配置缺少字段时采用自动；
设置进程通过私有 IPC 传递此选择。刷新显示的是偏好，不将自动覆盖成当前生效的浅色/深色。
Win11 仍使用原有独立图标颜色和系统面板设置。

运行时先选择各屏幕的当前任务栏场景，再用主任务栏的文字配色决定 Windows 全局浅深色：
浅色文字使用深色系统主题，深色文字使用浅色系统主题。这样开始菜单场景、最大化场景与默认场景
沿用相同的外观解析；多屏不会争写同一个 Windows 主题。手动浅色/深色覆盖自动，切换任务栏预设
不清除手动选择。主任务栏使用 Windows 原生外观时，自动不主动修改 Windows 主题。
此功能沿用已有系统主题写入行为，选择会影响开始菜单等系统面板，退出软件不回滚 Windows 主题。

测试覆盖自动的浅深色映射、原生外观不写系统主题、手动选择优先及三态私有 IPC 传输。
隔离头文件副本反转自动映射后，同一组映射断言确定失败；没有改动真实 Windows 主题。
构建、全量回归和上传证据分别保存在 `artifacts/v1.0.7.0/taskbar-win10-auto-20260924/`。
Win10 上控件双向同步、重启保留选择、实际图标配色与动态场景切换仍需用户实机验收。

用户随后要求托盘右键菜单也豁免任务栏抑制。原生控制器增加任务栏菜单循环、任务栏线程收到的
右键输入和托盘溢出面板检测；右键到应用菜单的交接有 1.5 秒开启宽限，识别到真实菜单线程后
保持到菜单循环结束，不因宽限到期而隐藏长时间打开的菜单。直接菜单在 `WM_ENTERMENULOOP`
阶段先释放宿主隐藏，DWM 抑制拦截也采用相同豁免。普通应用菜单没有托盘来源时不触发豁免。
菜单状态通过窗口属性供宿主复用已有系统面板场景，关闭或卸载时清除属性并恢复抑制。
参考 Microsoft 的 [菜单循环通知](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-entermenuloop)
及 [GUI 线程菜单状态](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-guithreadinfo)。
第三方自行绘制且不进入 Win32 菜单循环的托盘菜单尚无实机证据，需随 Win10 菜单场景一起验收。

首次标准构建在最终链接时遇到 `LNK1104`：构建期间本机 SnowDesktop 于 21:23 再次启动，
占用了 `.build/Release/SnowDesktop.exe`。该次退出 1，不计为构建通过；后续结果单独登记。
