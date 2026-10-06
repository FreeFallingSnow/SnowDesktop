# 通知关闭后输入积压现场分析（2026-09-27）

用户在候选 68 修改期间再次报告“唤起通知菜单导致全应用卡顿”，要求先分析当前现场。该次候选 68 现场诊断未关闭 SnowDesktop、未重启 Explorer，未操作或枚举桌面宿主窗口；采用非侵入进程转储、线程与对象状态读取、CPU 采样及现有日志分析。

## 实际采集

- 15:56:04 保存主进程 PID 33624 的完整内存转储，进程当时运行约 13 分 34 秒；PID 2820 为 `--watch-process-handle` 监护进程，并非第二份桌面宿主。
- 主进程约 330 MiB 私有内存、139 个线程。该单点数值不能证明内存泄漏，也不能证明此前约 400 MiB 问题消失。
- 调试器成功加载当前宿主的私有 PDB。模块时间戳为 15:35:30，属于已编译的候选 67；尚未编译的 68 修改不在此次现场中。
- 另一次 3.205 秒采样，进程消耗 0.25 CPU 秒；主 UI 线程 28952 消耗 0.15625 CPU 秒。不能用这个短样本排除此前短暂停顿。

## 已知事实

| 检查 | 现场状态 | 能支持的结论 |
| --- | --- | --- |
| 主线程堆栈 | `DesktopApp::Run → MsgWaitForMultipleObjectsEx`，上一条消息为 `WM_TIMER` | 采样瞬间没有阻塞在通知发键、菜单同步循环或设备调用中 |
| 通知激活 | token 为 0，activation monitor 为空 | 未见通知轮询或排队持有未结束 |
| 菜单／搜索 | menu owner／monitor 为空，Shell 菜单深度为 0；QuickNav 已关闭 | 未见这些会话占用输入的残留 |
| 系统面板 | showing／closing／modal 均为 false，pending 和 afterClose 为空 | 未见面板停在关闭后打开阶段 |
| 动画调度 | frameEntries 为 0，timerEntries 为 3，dispatching 为 false | 未见仍在动画回调或动画完成队列中重入 |
| Dock 设置 | 采样时 `edgeAttached=false`，底部岛式，非仅唤起模式 | 不能将此现场归因于融合栏动画门禁 |
| 后台 WLAN | 工作线程在 `WlanGetAvailableNetworkList` 的 RPC 等待 | 没有主线程等待该线程的证据，不作为界面卡顿根因 |
| 日志 | 15:53:48.170–.177 桌面显示／输入窗口隐藏，15:53:48.571–.579 恢复，约 400 ms | 属于可追查的相关现象；缺少成功通知动作时刻，尚不能证明因果关系 |

当前**尚未定位原卡顿根因，也未验证解决**。没有通过增加延时、无条件重试、结束后台 WLAN 线程或修改系统设置猜测修复。

## 用户补充与诊断方向修正

用户随后明确：通知打开后 SnowDesktop 左键、右键均不响应，点击桌面空白不能关闭系统通知；通过快捷键唤起面板，再用键盘打开一个应用后，积压的点击集中执行，随后左右键恢复正常。卡住期间全局快捷键及面板键盘操作仍可执行，鼠标输入积压应与主线程整体停摆分开诊断，优先检查输入投递和前景／焦点交接。先前主线程在消息等待中的快照不能证明用户当时可正常操作，也不能据此关闭问题。

源码核对：

- 主循环使用 `QS_ALLINPUT`、`MWMO_INPUTAVAILABLE` 和无窗口／消息范围过滤的 `PeekMessage`，没有发现只处理绘制或定时器、漏取鼠标消息的过滤。
- 设置为独立进程；宿主的设置消息预处理返回 false，没有发现设置页过滤掉桌面鼠标消息的路径。
- 桌面渲染窗口由 `AttachWindowToDesktopHost` 挂到 Explorer 的 WorkerW／Progman。Microsoft 说明跨线程 parent/owner 会隐式关联输入队列，关联线程须协作取出输入；这使“主线程仍能处理定时器，但输入排队”等现象成为应检查的方向。详见 [跨线程窗口与输入队列](https://devblogs.microsoft.com/oldnewthing/20130412-00/?p=4683) 和 [共享输入队列的顺序](https://devblogs.microsoft.com/oldnewthing/20130605-00/?p=4163)。这是代码与官方机制支持的候选原因，尚无卡顿时 Explorer 同步现场证明。
- 独立的 1×1 顶层键盘代理仍创建在上述主 UI 线程，不能凭其没有 parent 就认定输入队列已隔离。已找到的显式 `AttachThreadInput` 成功路径均有对应解绑，未发现确定的漏解绑分支。

需捕获通知动作前后的 active/focus/capture 状态，以及恢复时真实鼠标消息的时间戳延迟；仅记录一次回调执行耗时不足以诊断排队输入。此次没有为验证猜测而改动 Explorer 父子关系或强抢系统通知焦点。

## 后续定位

候选 68 只补充通知入队、等待恢复、快捷键发出与结束耗时，以及超过阈值的界面消息／调度／提交耗时记录。诊断不改变通知动作、输入排队或设备服务语义。后续需把实际卡顿时刻与这些记录对应；持续卡顿和通知打开瞬间的短暂停顿分别判断。

原始证据保存在本机忽略目录 `.codex-probes/statusbar-implementation/notification-hang-68/`：`20260927-155602-33624.dmp`、`20260927-155602-33624-capture.log`、`inspect-main.log`、`inspect-state.log`、`inspect-hoststate.log`、`inspect-finalstate.log`、`cpu-samples.json`、同时间日志副本与匹配的 EXE／PDB。转储不进入发行包或 Git。

本轮现场诊断不等于 UI 实机验收；其他反馈见 [反馈账本](STATUS_BAR_FEEDBACK_20260927.md) 和 [复核报告](STATUS_BAR_AUDIT_20260927.md)。

## 75–76：用户明确关闭方式与隔离复核

用户随后补充：卡住出现在关闭通知之后，关闭方式是**点击通知面板外的空白处**。当前定位以这一最新复现路径为准；快捷键打开 QuickNav、用键盘启动应用后，积压鼠标点击集中执行并恢复。此前“唤起后”的笼统描述不作为卡住阶段的精确证据。

只读核对 `app_status_bar.cpp`：通知链在发出 Win+N 后结束，没有 Shell 通知窗口的关闭回调。因此 send-input／finished 记录不能证明外部点击关闭时的输入状态。75 的自有面板隐藏发生在发键之前，不能推断 Shell 随后关闭时不会重新激活或聚焦旧窗口。`system_panel.cpp` 对已经关闭模型的 WM_ACTIVATE 交给默认处理；缺少现场证据，不据此添加拦截激活或强抢前台的修复。

本轮实际调用改动前的 `DesktopBackdropCompositor::HidePopupWindowPair`，在从不切换到输入桌面的私有桌面复现：隐藏 content／真实 EDIT 后仍保留自有键盘焦点；子框销毁后焦点落回 content。先 SW_HIDE 或只改变 SWP_NOACTIVATE 未完整消除；最后仅对仍属于已隐藏内容窗的焦点执行 SetFocus(nullptr)，可释放且不改变另一个自有测试窗口的焦点。SetActiveWindow(nullptr) 在该环境没有清除 active，未加入生产改动。此处 GetForegroundWindow 始终为空，不能视为实际外部进程前台交接。

75 `96a0cd64` 已加入上述最后一步焦点清理；76 本次全量 119/120，其中真实 compositor 的 content、EDIT、已销毁子框、其他窗口和重复隐藏检查通过，失败项是测试选择器超时。本项只证明隐藏后的键盘焦点后置条件，**没有证明通知原卡顿已解决**。菜单非法 owner 回归中发现的超时另行修订，与通知现场不混为同一原因。

原始诊断：`.codex-probes/statusbar-implementation/notification-hide-75/README.md`、`commands-results.log`、`null-active-results.log`。其中 `notification-hide-73/` 是同轮新建的早期目录名，非历史候选 73 的验证；原文件保留。读取的 `.build/Release/data/SnowDesktop.log.1` 副本 SHA256 `7e2442b675559f2b895dd65cce5a00d2d25e83193f9ea9b711e2947c743cd714`。17:40:42 日志显示发送输入接受，但之后仍有鼠标消息处理，未出现足以对应本次故障的 InputMessageDelayed，不能把这个片段当作确定卡住现场。

下一次原现场最关键的证据是：**点击外部空白关闭之后、使用 QuickNav 恢复之前**，读取 UI／前台／Explorer 线程的 active、focus、capture 和 GUI flags，并对齐第一条鼠标消息及 focus-request 的时间。空白点击若进入自有桌面处理，会经过 `app_pointer_down.cpp` 的 RestoreInteractionInputFocus；记录是否进入可区分“尚未投递”与“处理后仍异常”。恢复后的 InputMessageDelayed 才能确认积压时长。当前没有为取得证据而自动操作、枚举或重新启动桌面宿主。

## 77–78 后续边界

本批继续只读复核通知发键、输入捕获、低级鼠标钩子、AttachThreadInput 成功后的解绑，以及桌面鼠标入口，未找到可直接证明此次积压症状的漏清理。通知关闭时没有本应用的等待循环；跨线程 Explorer 父窗口带来的输入队列关联仍只是需要故障瞬间证据的方向，未贸然拆分父窗口或强制抢焦点。

77 修订独立状态栏全屏显隐，78 补日历检查，与 Shell 通知关闭后的输入交接是不同问题。本次未重新运行全量；原选择器失败已定向补验通过，不改变本通知缺陷“根因未确认、未验证修复”的状态。

## 79–80 范围说明

这两批只调整日历右栏／按钮比例和电池供电标记。生产离屏检查通过不改变本报告结论：通知外部点击关闭后的鼠标积压仍无已确认根因或修复，未重跑全量、未自动启动或操作桌面宿主。
