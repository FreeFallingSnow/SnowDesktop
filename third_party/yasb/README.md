# YASB

[YASB](https://github.com/amnweb/yasb) 是 Windows 状态栏项目。SnowDesktop 参考并改编了其中的托盘协议和系统控制实现。

## 来源

- 固定提交：[`d6d1e6d553b0aac34fd5fb34928d3ca82b8d055f`](https://github.com/amnweb/yasb/tree/d6d1e6d553b0aac34fd5fb34928d3ca82b8d055f)。下表的上游路径均相对于该提交。
- 版权：Copyright (c) 2024 amnweb；原始项目 Copyright (c) 2021 denBot。
- 许可证：MIT，完整声明见 [LICENSE](LICENSE)，发行包随附为 `licenses/YASB-LICENSE.txt`。

## 在 SnowDesktop 中的用途

| 用途 | YASB 来源 | SnowDesktop 实现 |
| --- | --- | --- |
| 托盘通信结构 | `src/core/widgets/services/systray/hook/trayhook.cpp`、`src/core/utils/win32/structs.py` | [tray_protocol.h](../../src/taskbar_hook/tray_protocol.h) 改编 Explorer 私有托盘消息结构 |
| 托盘鼠标回调 | `src/core/widgets/services/systray/systray_widget.py` | [tray_model.cpp](../../src/system/tray/tray_model.cpp) 参考右键按下、抬起及上下文菜单回调顺序 |
| 托盘折叠面板与系统图标 | `src/core/widgets/services/systray/systray_popup.py`、`systray_widget.py`、`src/core/widgets/yasb/systray.py` | [system_panel_model.cpp](../../src/system/panel/system_panel_model.cpp) 参考固定与折叠图标的组织方式；[tray_presentation.h](../../src/system/tray/tray_presentation.h) 使用系统音量、网络和电池图标标识 |
| 蓝牙音频连接与电量 | `src/core/widgets/services/bluetooth/bluetooth_audio.py`、`bluetooth_api.py` | [system_control_bluetooth.cpp](../../src/system/controls/system_control_bluetooth.cpp) 改编 KS 连接／断开方法及电池属性识别 |
| 电源模式 | `src/core/widgets/services/power_mode/power_mode_api.py` | [system_control_power.cpp](../../src/system/controls/system_control_power.cpp) 参考模式 GUID 和动态 API 支持检查 |
| 勿扰状态与通知数量 | `src/core/widgets/services/dnd/dnd_api.py`、`src/core/widgets/services/notifications/windows_notification.py` | [status_bar_notification.cpp](../../src/system/status_bar/status_bar_notification.cpp) 参考 WNF 状态标识与计数载荷 |
