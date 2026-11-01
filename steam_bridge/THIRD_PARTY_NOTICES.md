# Steam Bridge 第三方声明 / Steam Bridge Third-Party Notices

`steam_bridge/` 是独立项目，源码采用本目录 [LICENSE](LICENSE) 中的 MIT 许可证。以下条目说明该项目使用的第三方材料。

`steam_bridge/` is an independent project whose source is licensed under MIT in this directory's [LICENSE](LICENSE). The entries below identify third-party material used by this project.

## Steamworks SDK

配置 `SNOWDESKTOP_STEAMWORKS_SDK_ROOT` 后，桥接程序在构建时包含 SDK 头文件、链接导入库，并将 `steam_api64.dll` 复制到可执行文件旁。

When `SNOWDESKTOP_STEAMWORKS_SDK_ROOT` is set, the bridge includes SDK headers at build time, links the import library, and copies `steam_api64.dll` beside the executable.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [Steamworks API](https://partner.steamgames.com/doc/sdk/api) |
| 版本或提交 / Version or revision | 单独获取的外部 SDK / Separately obtained external SDK |
| 版权 / Copyright | Valve Corporation |
| 许可证 / License | [Steamworks SDK Access Agreement](https://partner.steamgames.com/documentation/sdk_access_agreement/) |

**分发 / Distribution**

SDK、头文件、库、工具和可再分发二进制不包含在本仓库中，不适用桥接源码的 MIT 或 SnowDesktop 核心的 GPL。仅可随应用分发 Valve 指定的可再分发文件；不得将 SDK、源码、头文件、工具或 `steam_appid.txt` 发布到公共仓库或发行包。

The SDK, headers, libraries, tools, and redistributable binaries are not included in this repository and are not covered by the bridge-source MIT or SnowDesktop core GPL license. Only files Valve designates as redistributable may accompany the application; do not publish the SDK, its source, headers, tools, or `steam_appid.txt` in public repositories or release packages.

## Dear ImGui

`SnowDesktopWorkshopManager.exe` 使用 Dear ImGui 及其官方 Win32、DirectX 11 后端。

`SnowDesktopWorkshopManager.exe` uses Dear ImGui and its official Win32 and DirectX 11 backends.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [ocornut/imgui](https://github.com/ocornut/imgui) |
| 版本或提交 / Version or revision | 1.92.5 WIP；以 `IMGUI_VERSION` 为准，未记录上游提交 / Defined by `IMGUI_VERSION`; no upstream revision recorded |
| 版权 / Copyright | Copyright (c) 2014-2025 Omar Cornut and contributors |
| 许可证 / License | [MIT](../third_party/imgui/LICENSE.txt) |

**分发 / Distribution**

源码位于 `third_party/imgui/`；完整许可原文保留在该目录及本文中。

The source is in `third_party/imgui/`; the complete original license is retained there and reproduced below.

## 许可原文 / Original license text

The MIT License (MIT)

Copyright (c) 2014-2025 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
