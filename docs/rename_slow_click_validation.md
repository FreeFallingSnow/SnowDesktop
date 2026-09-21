# 文件名称慢速单击重命名验证

## 改动范围

在桌面普通图标、集合文件、文件夹映射和 Dock 文件夹弹窗中，单独选中一个文件后，再以普通单击点击名称区域，松开后等待一个系统双击时间进入现有重命名编辑框。是否为双击由 Windows 的消息分类决定，不额外按两次按下的间隔拒绝普通单击。

选中状态可以来自鼠标或键盘，不要求先前点击的位置与本次名称点击接近。两次点击之间、以及名称单击完成后的等待期间，单纯移动鼠标不会取消重命名；本次按住鼠标期间超过拖动阈值仍按拖动取消。

第二次点击图标图像、多选、Ctrl/Shift/Alt、拖动、快速双击、键盘命令、右键、滚动、取消捕获、窗口失焦、关闭弹窗和重建项目会阻止或取消触发。计时器再次核对稳定文件身份、所属界面、名称位置及单选状态，避免对刷新后同一索引上的其他文件启动编辑。

列表名称命中和编辑框使用列表图标尺寸、缩放及名称列宽度。文件夹条目按实际槽中的对象查找位置，不把数据索引当作排序/过滤后的显示索引。

范围不包括组件标题、快捷导航窗口、Dock 启动图标和使用特效内嵌标题的大图标卡片；这些入口继续使用原有交互和重命名命令。未新增组件公共 API。

## 自动化边界

`tests/rename_click_cases.h` 加入现有 `slot_runtime_contract` 条目，直接调用生产 `RenameClickController`。替身仅提供时间、命中目标、名称矩形和选中状态。测试保护触发时序、名称区域、拖动取消、身份变化、选择失效及过期计时器；不据此宣称宿主桌面交互已通过。

隔离副本使用同一组用例进行负向对照，未修改工作区生产实现：

- 禁用 `Release` 的触发：应出现非零退出及触发断言失败。
- 跳过计时器对目标、几何和资格的复核：应出现非零退出及过期目标断言失败。

本轮日志保存在 `.codex-probes/slow-rename-20260921/`，属于不提交的本地验证证据。

## 验证记录

2026-09-21，Release / MSVC 18 / Windows SDK 10.0.26100.0：

- `scripts/build.bat`：退出码 0，37 秒；标准 Release 产物已生成，无编译/链接警告。首次 `--reload-shell` 构建因三处 `GetSlots` 的 const 限定不匹配失败，调整调用后通过。
- `scripts/test.bat name "^(slot_runtime_contract|dock_and_window_rules|widget_interaction_rules)$"`：退出码 0，3/3 通过；含构建共 33.05 秒，CTest 执行 2.11 秒。
- 隔离状态机探针：生产版本退出码 0；禁用触发版本退出码 1（20 个失败断言）；跳过目标复核版本退出码 1（12 个失败断言）。探针以 `/W4 /WX` 编译通过。
- 首次 `scripts/test.bat full`：构建阶段在链接 `SnowDesktop.exe` 时遭遇文件占用（LNK1104），退出码 1，未运行 CTest，不能计为通过。
- 实机验收：未运行，待用户反馈。

首个候选 `9569db52` 的 EXE SHA256：`1376816836595A964FD136DBA585DF6CC429B7044198FE2B21D29E9FB885C4EA`。

排除大图标卡片图案误命中后的最终检查点：

- `scripts/build.bat`：退出码 0，398.84 秒；标准 Release 构建通过，无编译/链接警告。日志 `build-card-guard.log`。
- `scripts/test.bat full`：退出码 0，120/120 通过；含配置/构建共 112.79 秒，CTest 执行 87.54 秒。无编译/链接警告；默认排除 manual 诊断。日志 `full-final.log`，CTest 报告 `.build/Testing/test-run-ba0a52923c854596841b1fad61f0f4ae.xml`。
- 最终 EXE SHA256：`8F965671394FA77DCEE125A94664528630D823CEBB43EEDD22A01F1CDDBC226C`。
- 验证覆盖当时完整工作区输入，包含其他任务尚未提交的剪贴板/拖放及 Dock 修改；本任务仅提交重命名差异。1,070 个源码、测试、资源、脚本和构建输入的 SHA256 清单位于 `final-input-sha256.txt`，清单 SHA256 为 `ED975A284CF166CE538D5B4D75310D04D7CF6663A845686F332D483AEB40D04B`。测试前后内容核对一致；后续输入变化须重新判断证据有效性。
- 桌面实机验收仍待用户反馈；全量自动测试和下述原生控件探针不算 SnowDesktop 桌面点击验收。

## 原生控件时序对照

微软文档说明，原生 ListView 点击已聚焦项的名称时会设置计时器，以避免与双击冲突；文档没有规定具体延时值：[Default List-View Message Processing](https://learn.microsoft.com/en-us/windows/win32/controls/listview-message-processing)。

本机独立 Win32 ListView 探针（Common Controls v6，测试项 `sample.txt`，未操作真实文件或 SnowDesktop 桌面宿主）记录 `NM_CLICK` 和 `LVN_BEGINLABELEDITW`。保持本机原有 `GetDoubleClickTime() = 500 ms`，未改系统设置：

| 输入 | 从按下到 NM_CLICK | 从按下到开始编辑 | 单击完成后等待 |
| --- | --- | --- | --- |
| 立即排队松开 | 16 ms | 516 ms | 500 ms |
| 按住约 120 ms 再松开 | 141 ms | 641 ms | 500 ms |

采用“第二次松开后等待系统双击时间”规则的依据仅为上述 ListView 测量；它没有验证旧版额外的按下时间门槛，也不能证明资源管理器或 SnowDesktop 的完整交互一致。最初未聚焦探针未触发编辑，属于无效测量；上表仅使用显式设置控件焦点后收到编辑通知的两次有效结果。原始日志为 `native-zero-hold-3.log` 和 `native-held-click-3.log`。

## 等待偏长反馈后的调整

用户反馈当前重命名需要的延时明显偏长，质疑未被识别成双击的点击为什么还不能重命名；`ed2096a0` 记录对前两个候选的交互验收未通过。未采集用户当时的 EXE 哈希和精确点击时序，不把以下代码问题直接认定为该现场的完整根因。

- 去掉 `Press` 中对相同目标、两次按下时间间隔的额外拦截。Windows 使用时间及位置识别双击，`WM_LBUTTONDBLCLK` 替代第二个按下消息；主窗口、浮动 Dock 和弹窗均启用了 `CS_DBLCLKS`，双击仍通过现有路径取消等待并打开文件。依据：[About Mouse Input — Double-Click Messages](https://learn.microsoft.com/en-us/windows/win32/inputdev/about-mouse-input#double-click-messages)。
- 保留松开后等待系统双击时间，以兼容已选中文件上的快速双击。计时器若提前或以旧排队消息到达，先停止原定时器，再按剩余时间重设，避免追加一个完整周期。该条件风险来自代码审查，尚无用户现场的计时日志。
- 回归覆盖系统交付的普通按下不被二次判定拒绝、真实双击取消后最终松开不启动编辑，以及距离期限仅剩 16 ms 时只请求剩余时间。旧实现使用新的普通单击断言，在隔离探针中退出码 1，两个业务断言失败；日志 `duplicate-gate-negative.log`。
- 调整后的生产状态机探针退出码 0；将剩余时间错误替换成完整 500 ms 的隔离版本退出码 1，提前计时器断言失败。均以 `/W4 /WX` 编译通过；日志 `system-click-probe-build.log`、`system-click-probe-positive.log` 和 `system-click-probe-full-interval.log`。探针直接执行生产状态机，但不执行宿主 `WM_TIMER` 分发。
- 本轮变更仅涉及重命名状态机、其宿主接线和现有测试用例；没有变更命中区域、选择、拖放、窗口消息路由或重命名提交逻辑。受影响自动化入口为 `slot_runtime_contract`，其他交互规则沿用此前全量检查点；本轮按普通交互调校运行定向回归，不能称为再次全量通过。

本次执行（2026-09-21，Release / MSVC 18 / Windows SDK 10.0.26100.0）：

- `scripts/build.bat --reload-shell`：退出码 0，413.61 秒；标准 Release 产物已生成，无编译/链接警告，日志 `build-system-click.log`。重载阶段原有 `timeout /t 2` 在非交互输入下报告不支持输入重定向，属于编译前的等待命令诊断；后续构建完成，Explorer 进程确认于本次重载时重新启动。
- `scripts/test.bat name "^slot_runtime_contract$"`：退出码 0，1/1 通过；含配置/构建 11.04 秒，CTest 执行 1.46 秒，无编译/链接警告。日志 `test-system-click.log`，报告 `.build/Testing/test-run-471ae65b0eab4f89938f0450a2ed2598.xml`。
- 1,070 个输入与上一全量清单比较，仅 `app_rename_target.cpp`、`rename_click_controller.h`、`rename_click_cases.h` 改变；本次构建及测试前后文件集合和内容保持一致。本次清单 `system-click-input-sha256.txt` 的 SHA256 为 `C26335AA3EB919CC32BCF41F8080D13E3BE4A2E8E8E82188B2A50AA2524170BA`。
- EXE SHA256：`E53406796F5AB7F85F607813BC07360C5926ADC3B58D564E5E446E705F91A2E4`。
- 本候选的实际点击手感、宿主计时器分发及编辑框验收待用户实机验证；未把原生 ListView 的独立测量写成 Explorer 完整行为一致。

## 移动鼠标限制的调整

用户进一步明确：已选中项目后，普通单击名称即可重命名，不应要求两次点击的位置接近。`718eac2d` 记录了旧生产状态机仍会在松开后的等待期间因指针移出名称而取消候选；隔离探针使用“已选中名称按下、松开、移出名称、期限到达”的输入，退出码 1、一个业务断言失败，日志 `hover-cancel-negative.log`。该证据不等于用户桌面现场复现。

本次仅移除 `Move` 对已完成单击候选的取消。按住期间的拖动阈值、双击、键盘命令、选择或目标变化、失焦及其他既有取消仍保留。回归增加两次点击间移到远处再返回的输入，将松开后移出名称的预期改为仍触发编辑；保留拖动后返回的取消断言，删除一个与前句重复的期限为零断言。受影响入口仍为 `slot_runtime_contract`，不改变宿主消息路由、命中、计时器或重命名提交逻辑。

调整后的生产状态机探针以 `/W4 /WX` 编译并通过，退出码 0，日志 `free-motion-probe-build.log` 和 `free-motion-probe-positive.log`。调用链复核确认 `OnMouseMoveAt` 统一进入该状态机，`OnMouseLeave` 的被动悬停清理不取消等待，计时器使用原点击点重新核对目标，不要求当前指针仍停留在名称上；这是代码审查范围，不替代桌面实机验收。

本次执行（2026-09-21，Release / MSVC 18 / Windows SDK 10.0.26100.0）：

- `scripts/build.bat --reload-shell`：退出码 0，439.01 秒，标准 Release 产物已生成，无编译/链接警告。使用终端输入运行，Shell 重载的等待命令未再出现输入重定向诊断。日志 `build-free-motion.log`。
- `scripts/test.bat name "^slot_runtime_contract$"`：退出码 0，1/1 通过；含配置/构建 11.82 秒，CTest 执行 1.42 秒，无编译/链接警告。日志 `test-free-motion.log`，报告 `.build/Testing/test-run-4ead7846e5d1427da0f7def01b016e78.xml`。
- 相对上一候选的 1,070 个输入，仅 `rename_click_controller.h` 和 `rename_click_cases.h` 改变；本次构建和测试输入内容、文件集合核对一致。清单 `free-motion-input-sha256.txt` 的 SHA256 为 `9AE5F3323042F10B4CBBFCEFE37815D8D89D3517664A46AB8F9CF8F782A3EBCA`。本轮执行定向回归，未重复全量。
- EXE SHA256：`8A2D29EDFEF63A6D3E1398D0C02291F0EB89801E00430C0D94B119B2CE13830E`。
- 实际桌面中移动指针后的点击效果与编辑框验收待用户验证。

## 实机验收清单（待执行）

使用本轮标准构建的 `.build/Release/SnowDesktop.exe`，在每种文件界面确认：

1. 首次单击只选中；移动鼠标到远处再返回单击名称，或通过键盘选中后直接单击名称，均延迟出现编辑框。松开后移出名称仍触发，文件名选区和扩展名处理与 F2 一致。
2. 快速双击仍打开文件；已选中文件上的快速双击也不弹出编辑框。
3. 第二次单击图像区域不重命名；Ctrl/Shift 多选及多个文件已选中时不重命名。
4. 从名称拖动后移回、按 Esc、右键、滚动、切换前台窗口和关闭弹窗取消等待。
5. 列表、详情列、排序/搜索过滤及缩放后，命中和编辑框仍位于同一文件名称；点击修改时间、类型、大小列不重命名。
6. 等待期间文件删除、改名、刷新或改变布局，不对其他文件进入编辑。
7. 输入新名称后 Enter 提交，Esc 放弃；F2 和右键重命名仍可用。核对文件实际名称与刷新后的显示。

仓库规则禁止通过桌面自动化验证桌面宿主；以上实机结果不能由构建或状态机测试替代。
