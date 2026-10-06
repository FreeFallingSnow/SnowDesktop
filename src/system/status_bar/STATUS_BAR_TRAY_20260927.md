# 托盘菜单专项复核（2026-09-27，候选 87）

源码候选：`a7e3db55b4bceb71445567d0ff121cafbcb948da`。标准 Release 构建、两项定向回归和真实 Shell 自有图标测试通过。**WPS、抖音、微信菜单在原桌面上的最终显示仍待用户验收，不能写成三款应用已经全部修复。** 没有按应用名称、安装路径或品牌窗口类做定向适配。

## 本次证据与修改

| 问题 | 已确认的证据 | 候选中的处理及边界 |
| --- | --- | --- |
| WPS 菜单有时在顶部、有时跑到底部 | 用户两次截图与只读 SHOW 记录吻合：同一菜单在正确位置显示，也会直接在远离点击点的底部显示；坏样本确为 callback 同线程、实际 foreground/active 的无 owner 工具弹窗 | 原规则要求靠近点击点，漏掉这种远处首帧。加入右键手势、真实活动窗口、线程及工具窗口的联合识别；不凭同进程移动普通窗口 |
| 托盘面板再次打开时丢失锚点 | `HideNow` 清除图标几何；原位置重开时布局、窗口尺寸和位置可全不变，`Arrange` 不一定重新发布几何；现场存在 `geometry=0` 回调 | 打开成功及实际点击／键盘／双击激活前重新发布当前图标矩形。原桌面反复开合尚未验收 |
| 微信固定图标与展开区均不响应右键，重启后恢复 | 用户重启后恢复；只读记录确认新进程执行 ADD→SETVERSION(4)。Qt 官方实现会在重复 ADD 失败后跳过 SETVERSION | collector 先开启 5 秒、同身份一次的重注册会话；只有原生查询证实同 HWND/ID 图标已存在、owner 仍有效且已采集，才确认重复 ADD，随后由应用自己声明版本 |
| 抖音顶部溢出及自绘菜单二次调整 | 用户截图存在问题；本轮只读记录未取得可归属的抖音菜单 SHOW，不能把 WPS 的透明阴影负坐标误认作抖音菜单实测 | 通用策略处理重复 SHOW、最终尺寸、越界与已绑定根菜单跳位；子菜单保持相对位置。同目标异步回声不消耗重复修正次数。抖音原问题仍待验收 |

重注册不读取 ADD/MODIFY 的 `uTimeout/uVersion` 联合字段来猜版本，不替第三方发送 SETVERSION 或 DELETE。GUID 图标的原生查询不能证明 HWND 所有权，未在这条恢复路径中放宽。原生查询必须为 S_OK 且矩形非空，并绕开 SnowDesktop 的坐标覆盖；未满足条件时保留原生失败结果。会话不因同 epoch 重复请求而无限延长。

定位观察仅针对本次操作的目标进程，在短会话内处理新显示菜单；仍排除普通应用窗口、透明阴影、工具提示和无法证明关联的远处窗口。它不保证可移动辅助进程、不同 UI 线程或拒绝异步定位的所有第三方弹窗。

技术依据：[Qt 5.15.14 官方托盘实现](https://raw.githubusercontent.com/qt/qtbase/v5.15.14-lts-lgpl/src/plugins/platforms/windows/qwindowssystemtrayicon.cpp)、[Microsoft Shell_NotifyIcon](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shell_notifyiconw)、[Microsoft NOTIFYICONIDENTIFIER 身份规则](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-notifyiconidentifier)。

## 实际执行的验证

- `scripts/build.bat --reload-shell`：退出 0，生成 `.build/Release/SnowDesktop.exe`，无编译／链接警告。预检发现宿主和 Explorer Hook 占用，已告知后关闭宿主并重载 Explorer；未自动启动桌面宿主。Shell 重载阶段的输入重定向诊断单独保留，不算编译警告或完整构建失败。
- `scripts/test.bat name "^(dock_and_window_rules|modern_menu_interaction)$"`：2/2，通过；CTest 12.15 秒。覆盖托盘协议／定位规则、相关窗口及菜单交互边界。
- `scripts/test.bat name "^tray_live_integration$"`：1/1，通过，未跳过；CTest 0.62 秒。真实 Explorer、自有隐藏进程先注册 v4，再启动生产采集器；先证明原重复 ADD 失败且应用不发 SETVERSION，再确认新握手使应用自行恢复 v4，同一会话第二次重复 ADD 仍拒绝。原 GUID 图标、坐标查询、v4/legacy 回调、修改、隐藏、删除与重连检查同时保留。
- 测试构建重新链接了 CMake 根输出的 Hook，其哈希与标准构建已安排到 Runtime 的 Hook 不同。因此使用同一测试程序直接对 `.build/Release/SnowDesktop.Runtime/SnowDesktopTaskbarHook.dll` 再做一次真实 Shell 检查，退出 0、约 0.38 秒。没有为此重编宿主或重跑全量，交付 Runtime 的原文件保持不变。
- 此次真实 Shell 记录中，**原来正在运行的微信 PID 61004、HWND 0x581260** 也自行重新声明了 v4；没有再次退出／重开微信。该证据确认真实应用的协议重报路径，不等于自动点击或菜单视觉验收。
- 隔离负向对照编译真实生产源和正式模型回归：当前实现退出 0；只禁用新增活动菜单识别的副本准确触发对应坏坐标断言，退出 1。两者编译均通过且开启 `/W4 /WX`；没有复制定位算法或自动操作桌面。首次辅助脚本因旧 Python 无 `removeprefix` 在编译前退出，修正辅助脚本后取得有效证据，不把该脚本失败当成缺陷复现。
- 标准构建前至测试结束的 1433 个已跟踪／新增输入哈希一致。此次没有全量运行；77–86 中未受影响的设置、日历、设备、Lua 等结果仍按原证据保留，不包装为本轮完整测试通过。

## 交付与未关闭事项

新候选位于 `.build/Release/SnowDesktop.exe`，SHA256：`bface240881e5e3bd54fd05b31d000328c2196c05c929db5c0023d53a49a829f`。

Runtime Hook SHA256：`f8695299b558a55e4322ae001b38372f9c82f91535d04ef79389843dc68d7596`。源码提交、测试 Hook 和自有图标程序的独立哈希见 `.codex-probes/statusbar-implementation/87-final-evidence.json`。

待原场景确认：启动新候选后，微信不重启时固定／展开区的右键；WPS 展开区反复关闭再打开后的定位；抖音菜单顶部完整可见及子菜单操作。此前通知关闭卡顿“暂未复现”、设置退出事件、全屏／多屏及融合栏视觉仍维持总报告中的状态，本次未据托盘回归替它们关闭问题。

原始证据：`.codex-probes/statusbar-implementation/87-menu-events.jsonl`、`87-menu-events-full.jsonl`、`tray-registration-readonly-20260927/evidence-20260927-230813-724856-watch.jsonl`、`87-build.log`、`87-tests.log`、`87-tray-live-detail.log`、`87-runtime-live.log`、`87-placement-negative/run-20260927T152816604962Z/evidence.json`。观察器已经结束；未推送或发布，未编辑用户数据。
