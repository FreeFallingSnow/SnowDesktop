# 任务栏最小化保护验证记录

2026 年 10 月 19 日的本地候选已通过标准 Release 构建及任务栏定向回归，用户桌面的双屏最小化表现仍待验证。候选源码提交为 `c054b369fa46a4d862a63f67224beffaeec3e32a`，基于 `1fd39d321ccddbcbdda62bfc1f9d9ae2b13a587a` 和 `a2a4e62c1df6f3452e02fdf6254205ce71fa9100`。

## 行为约定

| 操作 | 自动隐藏模式下的临时保护 |
| --- | --- |
| 应用自身或标题栏发起最小化 | 临时隐藏所有屏幕的任务栏，避免被动展开 |
| SnowDesktop Dock 发起最小化 | 同样保护所有屏幕 |
| 点击原生 Windows 任务栏使窗口最小化 | 豁免临时隐藏，保留用户对任务栏的操作 |
| 仅把鼠标停在任务栏上 | 不构成输入来源豁免 |
| 同一次任务栏操作的延迟最小化通知 | 沿用该窗口的限时豁免，避免重新隐藏 |
| 下一次从应用或 Dock 发起最小化 | 重新判断来源，不永久沿用此前的豁免 |

原生 Hook 在任务栏线程取出输入消息时记录接收窗口和事件时间，宿主以近期匹配输入推断来源。协议更新到 v15，使已有居民 Hook 升级到新监听逻辑。此保护路径使用已有 DWM 遮蔽能力，不依赖 Taskbar PDB；输入时序与真实 Shell 路径的兼容性需要实机确认。

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

## 实机验收与问题跟踪

在自动隐藏模式下，分别从主屏和副屏的原生任务栏点击活动应用使其最小化，确认任务栏保持正常响应；随即移动鼠标，确认后到的最小化通知不会把任务栏藏起来。再分别通过应用标题栏和 Dock 最小化，确认所有屏幕的任务栏不被动闪现。

连续切换这三条路径，并检查屏幕边缘、键盘、开始菜单、任务视图及托盘菜单的主动访问。上述场景尚未完成用户实机验收，隔离窗口测试通过不能作为已解决原始闪现问题的依据。本次没有运行应用全量测试。

初始运行日志和可补录的 Issue 草稿保存在 `.codex-probes/taskbar-minimize-20261019/`。尚无远端 Issue 编号，没有推送或发布；修复类 Issue 的关闭还需要实际渠道交付证据。
