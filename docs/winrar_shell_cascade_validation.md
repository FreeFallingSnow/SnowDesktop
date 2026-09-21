# WinRAR Shell 二级菜单排查记录

日期：2026-09-21。版本：1.0.7.0。状态：编译与自动检查通过，桌面原场景待实机验证。

## 现象与根因

用户反馈：右键点击“展开更多选项”后可以看到 WinRAR，但悬停或点击无法展开二级菜单。

现有入口在 `src/app/app_item_menu.cpp` 和 `src/app/app_shell_menu.cpp` 中创建 Shell
`IContextMenu`，并由 `HandleShellContextMenuMessage` 转发初始化和绘制消息。消息转发已经存在。
原 `TrackShellPopupMenuWithDesktopPump` 将 `TrackPopupMenuEx` 放到另一个 STA 线程，
再同步转回创建扩展的主线程处理消息。独立对照确认，这种拆分会使本机 WinRAR 的延迟子菜单
删除占位项后变成空菜单，虽然 `HandleMenuMsg2(WM_INITMENUPOPUP)` 仍返回 `S_OK`。

使用本机 WinRAR／RarExt 7.30.1、同一隔离样本和相同查询标志
`CMF_NORMAL | CMF_CANRENAME | CMF_SYNCCASCADEMENU`，观察结果如下：

| 样本 | 原跨线程跟踪 | 当前生产跟踪器 |
| --- | --- | --- |
| TXT | 子菜单从 1 个占位项变为 0 项；无可见菜单矩形 | 4 条压缩命令；菜单项矩形非空 |
| 有效 ZIP | 子菜单从 1 个占位项变为 0 项；无可见菜单矩形 | 4 条命令及 1 条分隔线；包含打开 WinRAR、解压文件、解压到目录和就地解压 |

探测仅操作独立诊断程序创建的菜单，自动取消并确认返回命令为 0；没有调用压缩、解压或其他
第三方命令，没有启动、捕获或操作 SnowDesktop 桌面宿主。以上是对当前安装版本的等价证据，
不代表所有 WinRAR 版本和其他 Shell 扩展均已完成兼容性验收。

## 调整与覆盖

`src/shell_popup_menu_tracker.h` 保留现有临时前台窗口和消息转发，但在菜单 owner 的线程上
创建并跟踪菜单，拒绝从其他线程调用。`app_shell_menu.cpp` 复用已有
`UiAnimationScheduler::MessagePumpScope`，让原生菜单循环继续推进动画与组件定时任务。
保留取消、置顶策略、选中命令返回和关闭后 owner 清理；没有新增界面入口或公共组件 API。

影响调用链包括桌面文件、路径文件、文件夹背景、桌面背景和新建菜单。定向范围覆盖 Shell
调用、右键规则、Shell 集成和动画调度；消息循环调整还完成一次完整自动测试。

新增回归位于既有 `shell_context_menu_invoke` 集成测试中：真实调用生产跟踪器、原生窗口及
菜单循环，仅用受控延迟填充回调替代第三方扩展，检查线程归属、命令保留、非空菜单矩形、
取消和提前取消。隔离副本恢复旧跨线程跟踪后，同一用例以退出码 1 报告
`deferred cascade initializes on the menu-tracking STA and keeps its commands`；当前实现退出码 0。
失败来自确定断言，不以编译失败或超时充当复现。

## 本次执行

| 命令／检查 | 结果 |
| --- | --- |
| `scripts/test.bat name "^(shell_context_menu_invoke|ui_animation_scheduler|right_click_contract|shell_integration_contract)$"` | 4/4 通过，退出码 0，CTest 17.94 秒 |
| `scripts/build.bat --reload-shell` | 标准 Release 构建通过，退出码 0，总计 32.17 秒，宿主 EXE 已生成 |
| `scripts/test.bat` | 119/119 通过，退出码 0，CTest 94.46 秒，隔离输出检查通过 |
| 独立真实 WinRAR TXT／ZIP 对照及回归负向对照 | 结果见上表及回归说明 |

第一次全量请求被运行占用预检中断，未执行测试。提醒用户后重载 Shell、完成标准构建，再运行
上述有效全量。构建的既有 `timeout` 命令输出输入重定向提示，但不影响退出码；本次标准构建
和测试没有观察到新增编译警告。默认排除的手动诊断未运行。

证据目录为 `.codex-probes/winrar-cascade/`：`targeted.log`、`build.log`、`full.log`、
`forward.log`、`production.log`、`zip-forward.log`、`zip-production.log`、`negative.log`、
`positive.log`。全量 CTest 报告为
`.build/Testing/test-run-07c91013e7544242aa1b828eaa758fc0.xml`。

验证使用当时的完整工作区输入，包括用户既有的文件夹刷新等未提交改动；这些改动不属于本次
提交。基点 `8296b4bec200e48521f3af5e4ec392a37b73bd5c`，差异保存在 `workspace.diff`，
1,058 个源码、测试、资源、脚本和依赖文件的哈希保存在 `inputs.json`，全量结束后核对未变化。
工具链为 MSBuild 18.5.4、Windows SDK 10.0.26100.0，Release 配置。

标准构建后及全量结束后的 `.build/Release/SnowDesktop.exe` SHA256 均为
`54FBA4AF9C4E6DCDD158CABF036CA85EDE38E342C39F5FDA7AE426EE67ABE561`。

## 待实机验证

重新启动上述 Release 宿主，在原文件上打开“展开更多选项”，检查鼠标悬停、点击及键盘进入
WinRAR 二级菜单。再检查菜单取消、打开菜单时的组件／Dock 动画、新建菜单，以及其他已安装
Shell 扩展。真实命令执行和桌面交互尚未验收；依据仓库规则以 `try` 保存，用户反馈后再记录
对应 `verify`。

## 接口参考

- [Microsoft：TrackPopupMenuEx 的 owner 与消息约定](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-trackpopupmenuex)
- [Microsoft：IContextMenu3::HandleMenuMsg2](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-icontextmenu3-handlemenumsg2)

跨线程触发 WinRAR 空子菜单的结论来自本次对照实验，并非上述接口文档对 WinRAR 的专门说明。
