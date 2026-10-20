# 任务栏最小化保护验证记录

2026 年 10 月 19 日的首轮候选 `c054b369fa46a4d862a63f67224beffaeec3e32a` 通过自动检查后实测失败。第二轮 `029b043b32465b0209bbcfb313e190ec8428bb54` 改用宿主独立输入采集，标准构建与定向回归已通过，仍待实机验收。

## 行为约定

| 操作 | 自动隐藏模式下的临时保护 |
| --- | --- |
| 应用自身或标题栏发起最小化 | 临时隐藏所有屏幕的任务栏，避免被动展开 |
| SnowDesktop Dock 发起最小化 | 同样保护所有屏幕 |
| 点击原生 Windows 任务栏使窗口最小化 | 豁免临时隐藏，保留用户对任务栏的操作 |
| 仅把鼠标停在任务栏上 | 不构成输入来源豁免 |
| 同一次任务栏操作的延迟最小化通知 | 沿用该窗口的限时豁免，避免重新隐藏 |
| 下一次从应用或 Dock 发起最小化 | 重新判断来源，不永久沿用此前的豁免 |

首轮原生 Hook 在任务栏线程取出输入消息时记录接收窗口和事件时间，但该入口未覆盖此次实机操作。第二轮由宿主独立线程记录离散点击与键盘操作，在最小化前判断来源；此保护路径继续使用已有 DWM 遮蔽能力，不依赖 Taskbar PDB。

## 已执行检查

- 命令：`scripts/build.bat ready-and-wait taskbar-origin-minimize-20261019 -Batch 26bbfec037274a048f7d8a46ffc587ff -Revision 0 -ReloadShell`。
- 标准 `scripts/build.bat` Release 阶段通过，构建起止输入摘要一致，没有编译或链接警告。
- CTest 仅选择 `dock_and_window_rules`，1/1 通过，没有重试；测试用时 20.42 秒，CTest 总用时 20.46 秒。
- **并行度记录更正：** `c054b369` 提交正文的“并行度配置 4”应为 CTest preset 的 `execution.jobs=24`。`4` 是构建 preset 的并行数。本次只有一个测试条目，因此实际最多同时执行一个测试。该更正不改变测试选择或结果。
- 新用例经过实际安装的消息队列 Hook，并在两个隔离窗口上检查输入归属、跨屏 DWM 取消和清理；纯规则用例覆盖延迟通知、后续操作及时间戳回绕。没有操作用户桌面窗口。
- 主程序的 Windows 版本资源完整，文件版本与产品版本均为 `1.0.11.0`。
- 日志：`.build/collaboration/26bbfec037274a048f7d8a46ffc587ff.log`。
- 报告：`.build/Testing/test-run-c9c8187b421e43c9bce6dfc5b835a30b.xml`。

## 被测文件身份

| 文件 | SHA256 |
| --- | --- |
| `.build/Release/SnowDesktop.exe` | `96758FE9FC7339A4733F43A05E98BACC8844409C7856BDEB9145686C6EF50E9B` |
| `.build/Release/SnowDesktop.Runtime/SnowDesktopTaskbarHook.dll` | `C9DA9BECEAEC817C5CD2C1905E71174B4AFB160AC1989D8220CCEAB97807BD7A` |
| `.build/Release/SnowDesktopTaskbarHook.dll` | `353B9FEAA653AF0A2CB35EFA62C77C04CA8187F2024CCEDBEC5C817AA5F72D32` |
| `.build/Release/tests/SnowDesktopDockAndWindowRulesTests.exe` | `F1BCB3C9BF4B7DAFD714C927CF0CC6A2395D583EE584801E5A206F22B2F95867` |

标准构建整理后的运行目录 Hook 与测试阶段重链接到根输出目录的 Hook 分别记录，不将两者声称为同一个二进制。后续共享构建可能更新输出，实机结果应绑定当时实际加载的文件身份。

## 首轮实机失败

- 用户在当前构建上点击原生任务栏最小化，任务栏仍被临时隐藏。
- 14:31:37.309 与 14:31:37.310 的日志中，鼠标在副屏任务栏内 `[3560,1572]`，主屏与副屏任务栏都出现 `phase=protection flags=1 request=6`；同一批操作没有 `taskbar-origin minimize exempted` 记录。
- 当时宿主 PID 为 `52912`。Explorer 重启后的 PID 为 `47232`，只读模块检查确认加载的是首轮测试阶段 DLL，SHA256 为 `353B9FEAA653AF0A2CB35EFA62C77C04CA8187F2024CCEDBEC5C817AA5F72D32`。
- 14:32:52 的只读检查显示，两条任务栏的 `SnowDesktop.Taskbar.MinimizeInputTick.v1` 都为 `0`。根窗口及其 InputSite 子窗口属于同一个 Explorer UI 线程 `40748`，不能归因为监听装在了另一个子窗口线程。
- 以上证据确认当前监听没有为这批任务栏操作发布输入标记；尚未证明具体是哪一层 Shell 分发绕过了监听。原有私有窗口的入队测试未覆盖这个实际输入入口。
- 原始日志快照、窗口线程与输入标记记录保存在 `.codex-probes/taskbar-minimize-20261019/origin-failed/`。没有通过桌面自动化复现或操作宿主。

失败记录提交为 `68b8042db28c783e1125985013792f4f9078818f`，不改写首轮 `try` 的历史验证状态。

## 第二轮候选

源码提交 `029b043b32465b0209bbcfb313e190ec8428bb54` 将输入采集放在宿主独立线程。低级鼠标与键盘 Hook 始终继续原 Hook 链，仅保存离散点击的位置和时间、键盘操作的时间与前台窗口句柄。鼠标移动不会覆盖点击；新的点击或键盘操作替换旧来源，近期输入期限为 1500 毫秒。

最小化前使用保存的点击位置查询命中窗口，并核对它与各屏任务栏的祖先关系。键盘来源使用事件时记录的前台窗口。任务栏来源或其延迟通知命中豁免时，同步取消并应用各屏当前的遮蔽请求。该来源判断仍依赖输入与操作时序的相关性，实机结果需要核对决策日志。

- 标准命令：`scripts/build.bat ready-and-wait taskbar-origin-input-repair-20261019 -Batch 6abd767e0b0e42c19a31001a74e414d8 -Revision 0 -ReloadShell`。
- Release 构建通过，输入摘要稳定，无编译或链接警告。
- `dock_and_window_rules` 通过，用时 20.11 秒；同批另一个任务选择的 `winui_widget_settings_presenter` 也通过，共 2/2，没有重试，CTest 总用时 20.16 秒。CTest preset 并行度为 24，实际按共享资源约束先后执行。
- 新测试替代 OS 输入投递及命中窗口查询边界，经过生产回调、专用线程生命周期、原子快照和真实子窗口祖先判断。覆盖 Explorer 输入属性为空的情况、鼠标移动、后续点击与键盘操作、时间戳回绕、部分注册失败及恢复；没有合成物理桌面输入。
- 报告：`.build/Testing/test-run-4d90f6e509e54ef79c12e9c9816b6be7.xml`；日志：`.build/collaboration/6abd767e0b0e42c19a31001a74e414d8.log`。
- 构建后主程序 SHA256：`98F0C712B66C0D40B98B002DCC94C061CD535119DC4F86D600BA6D96BEC5CF7B`；任务栏测试程序 SHA256：`2280A0E2CE0AC22C3D72BBC4109B43721AC4CA011B40E369FC3936E514294F8C`。
- 启动诊断为 `[TaskbarMinimizeInput] dedicated observer ready=... error=...`；每次请求记录 `[TaskbarMinimizeProtection] decision=exempt/protect` 及来源时间、类型、命中窗口和点击位置，用于区分采集失败与来源判断失败。

## 实机验收与问题跟踪

在自动隐藏模式下，分别从主屏和副屏的原生任务栏点击活动应用使其最小化，确认任务栏保持正常响应；随即移动鼠标，确认后到的最小化通知不会把任务栏藏起来。再分别通过应用标题栏和 Dock 最小化，确认所有屏幕的任务栏不被动闪现。

连续切换这三条路径，并检查屏幕边缘、键盘、开始菜单、任务视图及托盘菜单的主动访问。首轮任务栏来源豁免已实测失败，后续候选仍需重新验收，隔离窗口测试通过不能作为已解决原始闪现问题的依据。本次没有运行应用全量测试。

初始运行日志和可补录的 Issue 草稿保存在 `.codex-probes/taskbar-minimize-20261019/`。尚无远端 Issue 编号，没有推送或发布；修复类 Issue 的关闭还需要实际渠道交付证据。
