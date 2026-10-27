# Dear ImGui

即时模式界面库，用于 SnowDesktop 创意工坊管理器。

## 来源

- 上游：[ocornut/imgui](https://github.com/ocornut/imgui)。
- 本地版本：1.92.5 WIP，以 [imgui.h](imgui.h) 的 `IMGUI_VERSION` 为准；当前未记录上游提交。
- 版权：Copyright (c) 2014-2025 Omar Cornut。
- 许可证：MIT，完整声明见 [LICENSE.txt](LICENSE.txt)。

## 本地文件与使用方式

目录保留 ImGui 核心、内置文本和字体辅助头文件，以及 `backends/` 中的 Win32、DirectX 11 后端；[imconfig.h](imconfig.h) 为库配置入口。

根目录 [CMakeLists.txt](../../CMakeLists.txt) 将这些源码编译为 `SnowDesktopWorkshopImgui`。发行包随附 `licenses/DearImGui-LICENSE.txt`。
