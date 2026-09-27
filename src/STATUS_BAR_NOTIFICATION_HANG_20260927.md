# 通知唤起卡顿现场分析（2026-09-27）

用户在候选 68 修改期间再次报告“唤起通知菜单导致全应用卡顿”，要求先分析当前现场。本轮未关闭 SnowDesktop、未重启 Explorer，未操作或枚举桌面宿主窗口；采用非侵入进程转储、线程与对象状态读取、CPU 采样及现有日志分析。

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
