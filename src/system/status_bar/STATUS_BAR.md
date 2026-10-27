# 状态栏模块

状态栏、控制中心、托盘、日历和资源面板使用原生 Direct2D／DirectComposition 呈现，设置窗口使用 WinUI。设备读取与控制复用共享系统服务，界面消费快照和异步任务结果。

## 代码职责

| 位置 | 职责 |
| --- | --- |
| [status_bar.cpp](status_bar.cpp)、[status_bar_view.cpp](status_bar_view.cpp) | 栏体窗口、布局、绘制、输入和状态更新 |
| [status_bar_interaction.h](status_bar_interaction.h) | 按稳定身份处理悬停、按下、键盘焦点和激活 |
| [status_bar_appbar.h](status_bar_appbar.h)、[status_bar_fullscreen.h](status_bar_fullscreen.h) | 工作区占位和按屏全屏显隐 |
| [status_bar_notification.cpp](status_bar_notification.cpp) | 只读勿扰状态和通知数量 |
| [../tray/](../tray/) | 托盘采集、身份、顺序、回调、菜单定位和图标几何 |
| [../panel/](../panel/) | 自有弹窗模型、布局、切换和原生呈现 |
| [../controls/](../controls/)、[../calendar/](../calendar/) | 设备任务与日历日程数据 |
| [app_status_bar.cpp](../../app/features/app_status_bar.cpp) | DesktopApp 事件接线与跨模块协调 |

## 栏体与 Dock

独立状态栏支持屏幕顶部和底部，通过 AppBar 预留工作区，正常显示时置顶且不抢焦点；全屏隐藏栏体及附属窗口并保留占位。

Dock 靠边且与状态栏同侧时，状态栏控件并入 Dock。整栏服从 Dock 的显隐、仅唤起时显示和开合动画；融合外观跟随 Dock，沿用图标放大、悬浮名称及原有效果设置。未启用仅唤起模式时预留位置，背景、侧区与内容准备完成后一起显示。

菜单和搜索复用应用现有入口。点击栏体空白关闭弹窗，同一入口再次点击收起，切换入口时先关后开。融合栏普通右键复用 Dock 菜单，应用图标保留对象菜单。

## 弹窗与交互

- 自有弹窗由统一宿主管理关闭、打开中、已打开和关闭中四种状态；切换只保留最新目标。
- 控制中心与媒体条在同一宿主中呈现，卡片间透明区域不接收输入；提示使用独立、不激活的宿主。
- 输入、布局与绘制使用同一份已提交矩形和稳定身份。数据更新不能把按下目标或键盘焦点转给替换后的图标。
- 内容、主题、布局和提示分别失效；同一提示随数据原位更新，未变化内容复用缓存。
- 关闭时释放订阅、捕获、定时器、UIA 连接和图标几何，迟到结果按代次丢弃。DPI、显示器和图形设备变化接入宿主恢复流程。
- 设备扫描与控制在共享任务服务执行。具体权限、确认、取消和回读语义见 [系统控制说明](../controls/SYSTEM_CONTROLS.md)。

## 托盘与通知

托盘以图标身份及进程所有者关联快照、操作和几何；界面消费采集结果，Explorer 私有协议由托盘服务与 Hook 处理。第三方菜单的保留和位置调整只针对本次激活目标。Dock 的托盘恢复见 [DOCK_TRAY.md](../../dock/DOCK_TRAY.md)。

勿扰指示读取当前生效配置，包括自动规则。Windows 10 的未读数量与 Windows 11 的通知中心总数分别处理，读取失败显示未知；点击打开系统通知中心，不采集通知内容。

Explorer 私有托盘结构、WNF 状态和部分设备 API 的兼容性依赖 Windows 版本。第三方来源及许可见 [YASB 来源说明](../../../THIRD_PARTY_NOTICES.md#yasb)。

## 诊断与测试

运行日志和性能采集入口见 [LOGGING.md](../../diagnostics/LOGGING.md) 与 [PERFORMANCE.md](../../diagnostics/PERFORMANCE.md)。

托盘回调、焦点、图标几何和采集链测试位于 [tests/system/](../../../tests/system/)、[tray_model_tests.cpp](../../../tests/tray_model_tests.cpp)、[tray_focus_tests.cpp](../../../tests/tray_focus_tests.cpp) 和 [tray_live_tests.cpp](../../../tests/tray_live_tests.cpp)。生产面板预览使用实际模型与渲染器，可检查布局、像素和无设备副作用路径；桌面交互、第三方菜单、真实硬件和多屏显示按 [AGENTS.md](../../../AGENTS.md) 完成对应场景验证。
