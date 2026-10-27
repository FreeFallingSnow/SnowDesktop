# 第三方声明 / Third-Party Notices

SnowDesktop 使用或随附下列第三方软件和资源。各条目仅适用于所列组件；核心采用 GNU GPL v3.0。

SnowDesktop uses or redistributes the third-party software and assets listed below. Each notice applies to its named component. The core is licensed under GNU GPL v3.0.

## 对应源码 / Corresponding source

对应源码（包括第三方修改与构建脚本）可从 [官方仓库](https://github.com/FreeFallingSnow/SnowDesktop) 免费获取。构建说明和固定依赖版本见 `README.md`、`README.en.md`、`CMakeLists.txt`、`CMakePresets.json` 和 `scripts/README.md`。条目中的上游链接标明来源，项目改编内容包含在对应源码中。

The corresponding source, including third-party modifications and build scripts, is available free of charge from the [official repository](https://github.com/FreeFallingSnow/SnowDesktop). Build instructions and pinned dependencies are documented in `README.md`, `README.en.md`, `CMakeLists.txt`, `CMakePresets.json`, and `scripts/README.md`. Upstream links identify origins; project adaptations are included in the corresponding source.

<a id="windows-app-sdk"></a>

## Microsoft Windows App SDK

用于 WinUI 设置窗口及其运行时。

Provides WinUI settings windows and their runtime.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [microsoft/WindowsAppSDK](https://github.com/microsoft/WindowsAppSDK) |
| 版本或提交 / Version or revision | 2.4.0 NuGet |
| 版权 / Copyright | Copyright (c) Microsoft Corporation |
| 许可证 / License | Microsoft Software License Terms |

**分发 / Distribution**

运行时按固定 NuGet 包中的许可条款与第三方 NOTICE 自包含分发；文件记录于 `SnowDesktop.deployment.json`，发行包复制到 `licenses/`。

The runtime is redistributed self-contained under the license terms and third-party NOTICE from the pinned NuGet package. The files are recorded in `SnowDesktop.deployment.json` and copied to `licenses/` in release packages.

<a id="windows-ml"></a>

## Microsoft Windows ML Runtime

随应用分发的 Windows ML 运行时。

The Windows ML runtime redistributed with the application.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [Windows ML](https://learn.microsoft.com/windows/ai/new-windows-ml/) |
| 版本或提交 / Version or revision | 2.1.74 NuGet |
| 版权 / Copyright | Copyright (c) Microsoft Corporation |
| 许可证 / License | Microsoft Software License Terms |

**分发 / Distribution**

固定 NuGet 包的许可与第三方声明随发行包复制到 `licenses/`。

The license and third-party notices from the pinned NuGet package are copied to `licenses/` in release packages.

<a id="webview2"></a>

## Microsoft Edge WebView2 SDK

随应用使用的 WebView2 SDK。

The WebView2 SDK used by the application.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [Microsoft Edge WebView2](https://developer.microsoft.com/microsoft-edge/webview2/) |
| 版本或提交 / Version or revision | 1.0.3719.77 NuGet |
| 版权 / Copyright | Copyright (c) Microsoft Corporation |
| 许可证 / License | BSD 3-Clause |

**分发 / Distribution**

固定 NuGet 包的许可与第三方声明随发行包复制到 `licenses/`。

The license and third-party notices from the pinned NuGet package are copied to `licenses/` in release packages.

<a id="cppwinrt"></a>

## Microsoft.Windows.CppWinRT

应用使用的 C++/WinRT 支持包。

The C++/WinRT support package used by the application.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [microsoft/cppwinrt](https://github.com/microsoft/cppwinrt) |
| 版本或提交 / Version or revision | 3.0.260818.1 NuGet |
| 版权 / Copyright | Copyright (c) Microsoft Corporation |
| 许可证 / License | MIT |

**分发 / Distribution**

固定 NuGet 包的 MIT 许可证随发行包复制到 `licenses/`。

The MIT license from the pinned NuGet package is copied to `licenses/` in release packages.

<a id="everything"></a>

## Everything SDK

用于快捷导航查询本机 Everything 文件索引。

Queries the local Everything file index for quick navigation.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [Everything SDK](https://www.voidtools.com/support/everything/sdk/) |
| 版本或提交 / Version or revision | 随仓库保存的源码，未记录独立版本或提交 / Bundled source; no separate version or revision recorded |
| 版权 / Copyright | Copyright (C) 2016, 2022 David Carpenter |
| 许可证 / License | [MIT](third_party/everything/LICENSE.txt) |

| 本地内容 / Local content | 用途（中文） | Purpose (English) |
| --- | --- | --- |
| `third_party/everything/include/Everything.h`、`third_party/everything/src/Everything.c`、`third_party/everything/ipc/Everything_IPC.h` | 客户端接口和 IPC 协议 | Client interfaces and IPC protocol |
| [everything_search.cpp](src/navigation/everything_search.cpp) | 查询入口 | Search integration |

**分发 / Distribution**

Everything 主程序由用户另行安装并运行。发行包随附 `licenses/Everything-SDK-LICENSE.txt`。

Users install and run the Everything application separately. Release packages include `licenses/Everything-SDK-LICENSE.txt`.

<a id="imgui"></a>

## Dear ImGui

用于创意工坊管理器的即时模式界面。

Provides the immediate-mode interface for Workshop Manager.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [ocornut/imgui](https://github.com/ocornut/imgui) |
| 版本或提交 / Version or revision | 1.92.5 WIP；以 `IMGUI_VERSION` 为准，未记录上游提交 / Defined by `IMGUI_VERSION`; no upstream revision recorded |
| 版权 / Copyright | Copyright (c) 2014-2025 Omar Cornut and contributors |
| 许可证 / License | [MIT](third_party/imgui/LICENSE.txt) |

| 本地内容 / Local content | 用途（中文） | Purpose (English) |
| --- | --- | --- |
| [third_party/imgui/](third_party/imgui/) | 核心、文本和字体辅助头文件，Win32 与 DirectX 11 后端；`imconfig.h` 为配置入口 | Core, text/font helper headers, and Win32/DirectX 11 backends; configured through `imconfig.h` |
| [CMakeLists.txt](CMakeLists.txt) | 构建 `SnowDesktopWorkshopImgui` | Builds `SnowDesktopWorkshopImgui` |

**分发 / Distribution**

发行包随附 `licenses/DearImGui-LICENSE.txt`。

Release packages include `licenses/DearImGui-LICENSE.txt`.

<a id="lua"></a>

## Lua

用于 Lua 组件运行时和 `snowwidget` 开发工具。

Provides the script engine for Lua widgets and the `snowwidget` authoring tools.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [Lua](https://www.lua.org/) |
| 版本或提交 / Version or revision | 5.4.7；以 `lua.h` 的版本宏为准 / Defined by the version macros in `lua.h` |
| 版权 / Copyright | Copyright (C) 1994-2024 Lua.org, PUC-Rio |
| 许可证 / License | [MIT](third_party/lua/LICENSE.txt) |

| 本地内容 / Local content | 用途（中文） | Purpose (English) |
| --- | --- | --- |
| [third_party/lua/](third_party/lua/) | 核心与标准库；选定源码构建为 `SnowDesktopLua`，不将 `lua.c`、`luac.c` 或上游 `Makefile` 加入该库目标 | Core and standard libraries; selected sources build `SnowDesktopLua`, excluding `lua.c`, `luac.c`, and the upstream `Makefile` from that target |

`lcode.c` 的 `luaK_exp2val` 回移 [Lua 5.4.7 官方缺陷列表第 2 项](https://www.lua.org/bugs.html#5.4.7-2)：表达式为 `VJMP` 时先生成寄存器结果。版本宏保持 5.4.7，回归入口为 `lua_runtime`。

`luaK_exp2val` in `lcode.c` backports [official Lua 5.4.7 bug fix 2](https://www.lua.org/bugs.html#5.4.7-2), producing a register result for `VJMP` expressions. Version macros remain 5.4.7; the regression entry is `lua_runtime`.

**分发 / Distribution**

发行包随附 `licenses/Lua-LICENSE.txt`。

Release packages include `licenses/Lua-LICENSE.txt`.

<a id="libwebp"></a>

## libwebp

用于静态链接的 WebP 解码；原生图片回归使用编码器生成独立的有损和无损样本。

Provides the statically linked WebP decoder; native image regressions use the encoder to generate independent lossy and lossless fixtures.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [libwebp](https://developers.google.com/speed/webp)；[原始归档 / Source archive](https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz) |
| 版本或提交 / Version or revision | 1.6.0 |
| 版权 / Copyright | Copyright (c) 2010 Google Inc. |
| 许可证 / License | [BSD 3-Clause](third_party/libwebp/COPYING)；[专利授权 / Patent grant](third_party/libwebp/PATENTS) |

| 本地内容 / Local content | 用途（中文） | Purpose (English) |
| --- | --- | --- |
| [third_party/libwebp/](third_party/libwebp/) | 官方源码归档；应用仅链接解码器，禁用上游命令行工具和测试 | Official source archive; the application links only the decoder, with upstream command-line tools and tests disabled |

原始归档 SHA-256 / Source archive SHA-256：`e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564`。

**分发 / Distribution**

发行包随附 `licenses/libwebp-LICENSE.txt` 和 `licenses/libwebp-PATENTS.txt`。

Release packages include `licenses/libwebp-LICENSE.txt` and `licenses/libwebp-PATENTS.txt`.

<a id="puff"></a>

## puff

用于有界 ZIP deflate 解码。

Provides bounded ZIP deflate decoding.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [zlib contrib/puff](https://github.com/madler/zlib/tree/v1.3.1/contrib/puff) |
| 版本或提交 / Version or revision | zlib v1.3.1；puff 2.3 |
| 版权 / Copyright | Copyright (C) 2002-2013 Mark Adler, all rights reserved |
| 许可证 / License | [zlib license](third_party/puff/puff.h) |

| 本地内容 / Local content | 用途（中文） | Purpose (English) |
| --- | --- | --- |
| [puff.c](third_party/puff/puff.c)、[puff.h](third_party/puff/puff.h) | 保持上游原文件 | Unmodified upstream files |

| 文件 / File | SHA-256 |
| --- | --- |
| `puff.c` | `5b9d75aeb5baf3575415bc6ade3f2a02e50b6b971b3f8b4fda2b03543bc6e52f` |
| `puff.h` | `969b7be2a930db0cdcb19b0e5b29ae6741f5a8f663b6dba6d647e12ec60cfa8e` |

**分发 / Distribution**

许可保留在 `puff.h`，随发行包复制为 `licenses/puff-LICENSE.txt`。

The license is retained in `puff.h` and copied to release packages as `licenses/puff-LICENSE.txt`.

<a id="minhook"></a>

## MinHook

用于任务栏与 Wallpaper Engine 捕获 Hook。

Provides Windows API hooks for the taskbar and Wallpaper Engine capture.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook) |
| 版本或提交 / Version or revision | 随仓库保存的源码，未记录版本或提交 / Bundled source; no version or revision recorded |
| 版权 / Copyright | Copyright (C) 2009-2017 Tsuda Kageyu |
| 许可证 / License | [BSD 2-Clause 及附带代码声明 / BSD 2-Clause and bundled-code notices](third_party/minhook/LICENSE.txt) |

| 本地内容 / Local content | 用途（中文） | Purpose (English) |
| --- | --- | --- |
| [third_party/minhook/](third_party/minhook/) | 接口、Hook、跳板、缓冲及 x86/x64 指令解码；构建按架构选择源码 | Interfaces, hooks, trampolines, buffers, and x86/x64 instruction decoding; sources are selected by build architecture |
| [CMakeLists.txt](CMakeLists.txt)、[wallpaper_hook/CMakeLists.txt](src/wallpaper_hook/CMakeLists.txt) | 宿主和独立 Hook 构建入口 | Host and standalone-hook build entries |

**分发 / Distribution**

发行包随附 `licenses/MinHook-LICENSE.txt`。

Release packages include `licenses/MinHook-LICENSE.txt`.

<a id="pinyin-data"></a>

## pinyin-data

用于汉字名称的全拼与首字母搜索。

Provides full-pinyin and initial-letter matching for Chinese names.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [mozillazg/pinyin-data](https://github.com/mozillazg/pinyin-data) |
| 版本或提交 / Version or revision | 随仓库保存的数据，未记录版本或提交 / Bundled data; no version or revision recorded |
| 版权 / Copyright | Copyright (c) 2016 mozillazg |
| 许可证 / License | [MIT](third_party/pinyin-data/LICENSE) |

| 本地内容 / Local content | 用途（中文） | Purpose (English) |
| --- | --- | --- |
| [kMandarin.txt](third_party/pinyin-data/kMandarin.txt) | 上游来源数据 | Upstream source data |
| [pinyin_full_table.cpp](third_party/pinyin-data/pinyin_full_table.cpp)、[pinyin_full_table.h](third_party/pinyin-data/pinyin_full_table.h) | 由数据生成的紧凑 C++ 查询表 | Compact C++ lookup tables generated from the data |
| [name_pinyin.h](src/navigation/name_pinyin.h)、[search_match.cpp](src/navigation/search_match.cpp) | 名称查询中的使用入口 | Name-search integration |

**分发 / Distribution**

发行包随附 `licenses/PinyinData-LICENSE.txt`。

Release packages include `licenses/PinyinData-LICENSE.txt`.

<a id="font-awesome"></a>

## Font Awesome Free Solid

用于组件兼容图标。

Provides backward-compatible widget icons.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [Font Awesome](https://fontawesome.com/)；[Free License](https://fontawesome.com/license/free) |
| 版本或提交 / Version or revision | 6.5.2 |
| 版权 / Copyright | Copyright (c) Font Awesome |
| 许可证 / License | [SIL Open Font License 1.1](third_party/font-awesome/LICENSE.txt) |

| 本地内容 / Local content | 用途（中文） | Purpose (English) |
| --- | --- | --- |
| [fa-solid-900.ttf](assets/fonts/fa-solid-900.ttf) | 通过 `src/resources/resource.rc` 嵌入应用 | Embedded through `src/resources/resource.rc` |

**分发 / Distribution**

发行包随附 `licenses/FontAwesome-LICENSE.txt`，保留字体名称为 `Font Awesome`。

Release packages include `licenses/FontAwesome-LICENSE.txt`; the reserved font name is `Font Awesome`.

<a id="fluent-system-icons"></a>

## Fluent System Icons

用于菜单、状态栏和控制中心图标。

Provides icons for menus, the status bar, and Control Center.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [microsoft/fluentui-system-icons](https://github.com/microsoft/fluentui-system-icons) |
| 版本或提交 / Version or revision | `21d5d02f724be2aaf586564775fff73a18a76eb6` |
| 版权 / Copyright | Copyright (c) 2020 Microsoft Corporation |
| 许可证 / License | [MIT](third_party/fluentui-system-icons/LICENSE) |

| 本地内容 / Local content | 用途（中文） | Purpose (English) |
| --- | --- | --- |
| [FluentSystemIcons-Regular.ttf](assets/fonts/FluentSystemIcons-Regular.ttf) | 嵌入应用的 Regular 图标字体 | Embedded Regular icon font |
| [status_bar_battery.h](src/system/status_bar/status_bar_battery.h) | Battery Charge 20 Filled 的上游 SVG 转换为等价 Direct2D 路径，用于充电指示 | The upstream Battery Charge 20 Filled SVG converted to equivalent Direct2D geometry for charging indication |
| [status_bar_view.cpp](src/system/status_bar/status_bar_view.cpp) | Alert 24 Filled 通知轮廓，几何失败时回退 Regular 字形 | Alert 24 Filled notification outline, with a Regular-glyph fallback if geometry creation fails |

Regular 字体 SHA-256 / Regular font SHA-256：`53204be5637d3872892e64aa7012f79f1e7480f03b534ae1d29a66e62a96d7fc`。

**分发 / Distribution**

发行包包含 Regular 字体和 `licenses/FluentSystemIcons-LICENSE.txt`。

Release packages include the Regular font and `licenses/FluentSystemIcons-LICENSE.txt`.

<a id="misans"></a>

## MiSans Regular

可选界面字体，本地文件来自用户提供的官方字体包。

An optional interface font obtained from a user-provided official font package.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [官方下载页 / Official download page](https://hyperos.mi.com/font/zh/) |
| 版本或提交 / Version or revision | 4.009 |
| 版权 / Copyright | Copyright © 2020-2025 Beijing Xiaomi Mobile Software Co., Ltd. |
| 许可证 / License | [MiSans 字体知识产权许可协议 / MiSans font intellectual property license](third_party/misans/LICENSE.pdf) |

| 本地内容 / Local content | 用途（中文） | Purpose (English) |
| --- | --- | --- |
| [MiSans-Regular.otf](assets/fonts/MiSans/MiSans-Regular.otf) | 保持原始文件，未裁剪字形、转换格式或修改字体 | Original file, without subsetting, format conversion, or font modification |

**分发 / Distribution**

字体随应用分发，发行包随附 `licenses/MiSans-LICENSE.pdf`。文件大小、SHA-256 和字重说明见 [字体资源说明](assets/fonts/README.md)。

The font is bundled with the application, with `licenses/MiSans-LICENSE.pdf` included in release packages. See the [font asset documentation](assets/fonts/README.md) for file size, SHA-256, and font-weight behavior.

<a id="harmonyos-sans"></a>

## HarmonyOS Sans SC

可选界面字体，本地文件来自用户提供的官方字体包。

An optional interface font obtained from a user-provided official font package.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [官方下载页 / Official download page](https://developer.huawei.com/consumer/cn/design/resource/) |
| 版本或提交 / Version or revision | 1.0 |
| 版权 / Copyright | Copyright 2021 Huawei Device Co., Ltd. |
| 许可证 / License | [HarmonyOS Sans Fonts License Agreement](third_party/harmonyos-sans/LICENSE.txt) |

| 本地内容 / Local content | 用途（中文） | Purpose (English) |
| --- | --- | --- |
| [HarmonyOS_Sans_SC_Regular.ttf](assets/fonts/HarmonyOS-Sans/HarmonyOS_Sans_SC_Regular.ttf) | 保持原始文件，未裁剪字形、转换格式或修改字体 | Original file, without subsetting, format conversion, or font modification |

**分发 / Distribution**

字体随应用分发，发行包随附 `licenses/HarmonyOS-Sans-LICENSE.txt`。文件大小、SHA-256 和字重说明见 [字体资源说明](assets/fonts/README.md)。

The font is bundled with the application, with `licenses/HarmonyOS-Sans-LICENSE.txt` included in release packages. See the [font asset documentation](assets/fonts/README.md) for file size, SHA-256, and font-weight behavior.

<a id="winui-acrylic-noise"></a>

## WinUI 亚克力噪点 / WinUI acrylic noise

用于宿主和组件预览的玻璃材质噪点纹理。

Provides the glass-material noise texture used by the host and widget previews.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [microsoft/microsoft-ui-xaml](https://github.com/microsoft/microsoft-ui-xaml) |
| 版本或提交 / Version or revision | `7b68d3e0b771a57d80098799406234efee479517` |
| 版权 / Copyright | Copyright (c) Microsoft Corporation. All rights reserved. |
| 许可证 / License | [MIT](third_party/microsoft-ui-xaml/LICENSE) |

| 本地内容 / Local content | 用途（中文） | Purpose (English) |
| --- | --- | --- |
| [NoiseAsset_256X256_PNG.png](third_party/microsoft-ui-xaml/NoiseAsset_256X256_PNG.png) | 上游 `controls/dev/Materials/Acrylic/Assets/NoiseAsset_256X256_PNG.png` 原图 | Original upstream `controls/dev/Materials/Acrylic/Assets/NoiseAsset_256X256_PNG.png` |
| [generate_acrylic_noise_asset.py](scripts/generate_acrylic_noise_asset.py) | 生成 `src/ui/render/acrylic_noise_asset.h`；根目录运行 `python scripts/generate_acrylic_noise_asset.py --check` 核对 | Generates `src/ui/render/acrylic_noise_asset.h`; check from the repository root with `python scripts/generate_acrylic_noise_asset.py --check` |

原图 SHA-256 / Original PNG SHA-256：`4f2aa94a2e345a32dae689176b86c644bef87081ff9daef8c77417731862031b`。

渲染使用 256×256 纹理，以物理像素尺寸平铺；2% 透明度采用上游 `AcrylicBrush::sc_noiseOpacity` 的取值。

Rendering tiles the 256×256 texture at physical-pixel size, using the upstream `AcrylicBrush::sc_noiseOpacity` value of 2%.

**分发 / Distribution**

发行包随附 `licenses/WinUI-AcrylicNoise-LICENSE.txt`；本文附有许可原文。

Release packages include `licenses/WinUI-AcrylicNoise-LICENSE.txt`; the original license text is reproduced below.

<a id="perfetto"></a>

## Perfetto SDK

用于原生性能采集与分析。仓库保存官方 C++ SDK 合并文件。

Provides native performance capture and analysis. The repository vendors the official C++ SDK amalgamation.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [google/perfetto](https://github.com/google/perfetto)；[v58.2](https://github.com/google/perfetto/releases/tag/v58.2)；[原始归档 / Source archive](https://github.com/google/perfetto/releases/download/v58.2/perfetto-cpp-sdk-src.zip) |
| 版本或提交 / Version or revision | v58.2；`add693d8b338ba9599dbcbc3e300b1ab8c000897` |
| 版权 / Copyright | The Android Open Source Project |
| 许可证 / License | [Apache-2.0](third_party/perfetto/LICENSE) |

| 本地内容 / Local content | 用途（中文） | Purpose (English) |
| --- | --- | --- |
| [perfetto.h](third_party/perfetto/perfetto.h)、[perfetto.cc](third_party/perfetto/perfetto.cc) | 保持官方原文件和版权声明 | Original upstream files and copyright notices |
| [SnowDesktop.Perfetto.cmake](cmake/SnowDesktop.Perfetto.cmake) | 核对原始文件并生成编译副本 | Verifies original files and generates the compiler copy |

| 文件 / File | SHA-256 |
| --- | --- |
| SDK ZIP | `e56126ff81f0f914d35fe03c72419d147acf2890f36fbda7d4558fd5b84c4b09` |
| `perfetto.h` | `df97880d56c765a4c51ffe7fd2c71ce4fbb3164677d81185e8488c1516e87470` |
| `perfetto.cc` | `cfd4d552272f37da310d43c96c418788567d12174017e2fce061797297391e4c` |

编译副本有两处 MSVC C4267 类型调整：chunk 索引用 `size_t` 与生产和消费接口匹配，最多五字节的 uint32 varint 长度显式表示为 `uint32_t`。调整不改变 trace 格式，未关闭编译器警告，编译副本仅在内容变化时重写。

The compiler copy has two MSVC C4267 type adaptations: a chunk index uses `size_t` to match producer and consumer APIs, and the at-most-five-byte uint32 varint length is represented as `uint32_t`. These do not change the trace format or disable compiler warnings; the compiler copy is rewritten only when its contents change.

**分发 / Distribution**

发行包随附 `licenses/Perfetto-LICENSE.txt`。采集、分析和回归入口见 [PERFORMANCE.md](src/diagnostics/PERFORMANCE.md)。

Release packages include `licenses/Perfetto-LICENSE.txt`. Capture, analysis, and regression entries are documented in [PERFORMANCE.md](src/diagnostics/PERFORMANCE.md).

<a id="yasb"></a>

## YASB

参考并改编托盘协议和系统控制相关实现。

Provides references and adapted portions for tray protocols and system controls.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [amnweb/yasb](https://github.com/amnweb/yasb/tree/d6d1e6d553b0aac34fd5fb34928d3ca82b8d055f) |
| 版本或提交 / Version or revision | `d6d1e6d553b0aac34fd5fb34928d3ca82b8d055f` |
| 版权 / Copyright | Copyright (c) 2024 amnweb；Copyright (c) 2021 denBot |
| 许可证 / License | [MIT](third_party/yasb/LICENSE) |

下表的上游路径均相对于上述固定提交。 / Upstream paths below are relative to the pinned revision.

| 使用内容 / Incorporated material | YASB 上游路径 / Upstream paths | SnowDesktop 位置 / Local integration |
| --- | --- | --- |
| 托盘通信结构 / Tray wire structures | `src/core/widgets/services/systray/hook/trayhook.cpp`、`src/core/utils/win32/structs.py` | [tray_protocol.h](src/taskbar_hook/tray_protocol.h) |
| 托盘鼠标回调顺序 / Tray mouse callback order | `src/core/widgets/services/systray/systray_widget.py` | [tray_model.cpp](src/system/tray/tray_model.cpp) |
| 固定、折叠图标组织和系统图标标识 / Pinned and collapsed icon organization and system icon identifiers | `src/core/widgets/services/systray/systray_popup.py`、`systray_widget.py`、`src/core/widgets/yasb/systray.py` | [system_panel_model.cpp](src/system/panel/system_panel_model.cpp)、[tray_presentation.h](src/system/tray/tray_presentation.h) |
| 蓝牙音频 KS 连接、断开与电池属性 / Bluetooth audio KS connection, disconnection, and battery properties | `src/core/widgets/services/bluetooth/bluetooth_audio.py`、`bluetooth_api.py` | [system_control_bluetooth.cpp](src/system/controls/system_control_bluetooth.cpp) |
| 电源模式 GUID 与动态 API 支持检查 / Power-mode GUIDs and dynamic API availability checks | `src/core/widgets/services/power_mode/power_mode_api.py` | [system_control_power.cpp](src/system/controls/system_control_power.cpp) |
| 勿扰 WNF 状态标识与通知计数载荷 / DND WNF identifiers and notification-count payloads | `src/core/widgets/services/dnd/dnd_api.py`、`src/core/widgets/services/notifications/windows_notification.py` | [status_bar_notification.cpp](src/system/status_bar/status_bar_notification.cpp) |

**分发 / Distribution**

发行包随附 `licenses/YASB-LICENSE.txt`。

Release packages include `licenses/YASB-LICENSE.txt`.

<a id="graphics-effect"></a>

## OpenHarmony GraphicsEffect

改编边缘光与阴影计算，用于玻璃材质的 CPU 透明度遮罩。

Adapts edge-light and shadow calculations for CPU opacity masks in glass materials.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [openharmony/graphic_graphics_effect](https://github.com/openharmony/graphic_graphics_effect) |
| 版本或提交 / Version or revision | `da8e11652a705ea2141c35de1a1fff501148740e` |
| 版权 / Copyright | Copyright (c) 2025-2026 Huawei Device Co., Ltd. |
| 许可证 / License | [Apache-2.0](third_party/graphics-effect/LICENSE) |

| 本地内容 / Local content | 用途（中文） | Purpose (English) |
| --- | --- | --- |
| [flat_glass_rim.h](src/theme/flat_glass_rim.h) | 图标底板、原生面板、Dock 和组件预览共用 | Shared by icon plates, native panels, Dock, and widget previews |

| 上游文件 / Upstream file | 改编内容（中文） | Adapted material (English) |
| --- | --- | --- |
| `src/effect/shader/ge_sdf_edge_light_shader.cpp` | 边缘宽度上下限、smoothstep 响应、内外光晕边界、阈值和距离衰减 | Edge-width bounds, smoothstep response, inner/outer halo boundaries, thresholds, and distance attenuation |
| `src/effect/shader/ge_frosted_glass_effect.cpp` | 对向扇区遮罩、smoothstep 羽化和指数内阴影衰减 | Opposing fan masks, smoothstep feathering, and exponential inner-shadow attenuation |

SnowDesktop 于 2026 年将这些计算移植到现有遮罩；光照方向、角度、亮度、宽度和光晕参数由本项目调校。

SnowDesktop adapted these calculations to its opacity masks in 2026, with project-specific light direction, angle, brightness, width, and halo parameters.

**分发 / Distribution**

完整 Apache-2.0 许可原文保留在本文件中，随发行包交付。

The complete original Apache-2.0 license is retained in this file and distributed with release packages.

<a id="attraction"></a>

## OpenHarmony 吸引效果计算 / OpenHarmony attraction calculations

改编 `RSAttractionEffectFilter` 的控制点轨迹和时间参数。

Adapts the control-point trajectories and timing parameters of `RSAttractionEffectFilter`.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [openharmony/graphic_graphic_2d](https://github.com/openharmony/graphic_graphic_2d) |
| 版本或提交 / Version or revision | `e72069008cd4762ddd150d164861c43f8183e439` |
| 版权 / Copyright | Copyright (c) 2024 Huawei Device Co., Ltd. |
| 许可证 / License | Apache-2.0 |

| 本地内容 / Local content | 用途（中文） | Purpose (English) |
| --- | --- | --- |
| [dock_attraction_rules.h](src/dock/dock_attraction_rules.h) | 保留版权与 Apache-2.0 声明的改编实现 | Adapted implementation retaining copyright and Apache-2.0 notices |

**分发 / Distribution**

改编内容包含在对应源码中；本文附有完整 Apache-2.0 许可原文。

The adaptations are included in the corresponding source; the complete original Apache-2.0 license is reproduced below.

<a id="translucenttb"></a>

## TranslucentTB

改编部分代码，用于任务栏集成和桌面背景合成。

Includes adapted portions for taskbar integration and desktop backdrop composition.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [TranslucentTB/TranslucentTB](https://github.com/TranslucentTB/TranslucentTB/tree/322e2b7395a51975150126276308b415970e080b) |
| 版本或提交 / Version or revision | `322e2b7395a51975150126276308b415970e080b` |
| 版权 / Copyright | TranslucentTB contributors |
| 许可证 / License | [GPL-3.0-only](LICENSE) |

| 本地内容 / Local content | 用途（中文） | Purpose (English) |
| --- | --- | --- |
| [ShellViewCoordinator.idl](src/taskbar_dynamic/ShellViewCoordinator.idl) | 改编文件 | Adapted file |
| [taskview_visibility.h](src/taskbar_hook/taskview_visibility.h) | 改编文件 | Adapted file |
| [taskbar_hook.cpp](src/taskbar_hook/taskbar_hook.cpp)、[desktop_backdrop_compositor.cpp](src/app/render/desktop_backdrop_compositor.cpp) | 部分改编代码 | Adapted portions |

**分发 / Distribution**

SnowDesktop 于 2026 年修改相关代码，沿用与核心相同的 GPL-3.0-only。完整许可在根目录 `LICENSE`，上游源码和历史见固定提交。

SnowDesktop modified these portions in 2026 under GPL-3.0-only, the same license as the core. The full license is in root `LICENSE`; upstream source and history are available at the pinned revision.

<a id="deskmakeover"></a>

## DeskMakeover

改编 `dm-icon-core` 的标准化形状控制点与连续圆角几何。

Adapts normalized shape control points and continuous corner geometry from `dm-icon-core`.

| 项目 / Field | 内容 / Value |
| --- | --- |
| 上游 / Upstream | [nicepkg/deskmakeover](https://github.com/nicepkg/deskmakeover)；[视觉语言说明 / Visual-language reference](https://github.com/nicepkg/deskmakeover/blob/main/docs/specs/02-visual-language.md) |
| 版本或提交 / Version or revision | 2026 年引用的上游 `main` / Upstream `main` as referenced in 2026 |
| 版权 / Copyright | Copyright (c) 2026 Jinming Yang |
| 许可证 / License | MIT |

| 本地内容 / Local content | 用途（中文） | Purpose (English) |
| --- | --- | --- |
| [icon_beautify.cpp](src/icons/icon_beautify.cpp) | 图标形状几何 | Icon shape geometry |

**分发 / Distribution**

本文附有完整 MIT 许可原文。

The complete original MIT license is reproduced below.

<a id="steam-bridge"></a>

## 独立 Steam Bridge 项目 / Independent Steam Bridge project

`steam_bridge/` 使用自己的 [MIT 许可证](steam_bridge/LICENSE) 和 [第三方声明](steam_bridge/THIRD_PARTY_NOTICES.md)。Steamworks SDK 的使用与分发条件见该独立项目的声明。

`steam_bridge/` has its own [MIT license](steam_bridge/LICENSE) and [third-party notices](steam_bridge/THIRD_PARTY_NOTICES.md). Its notice document describes Steamworks SDK use and distribution conditions.

<a id="optional-interface-fonts"></a>

## 可选界面字体的分发条件 / Optional interface font distribution

MiSans 与 HarmonyOS Sans 分别适用其字体协议，不适用 SnowDesktop 源码的 GPL。应用随附原始 OTF/TTF 文件，不进行字形裁剪、转换或字体再开发，并保留完整协议。禁止作为独立字体产品再次分发。

MiSans and HarmonyOS Sans are separately licensed font assets, not GPL-licensed SnowDesktop code. The application bundles the original OTF/TTF files unchanged, without glyph subsetting, conversion, or font redevelopment, and retains their full agreements. They may not be redistributed as standalone font products.

## 许可原文 / Original license texts

### DeskMakeover — MIT

Copyright (c) 2026 Jinming Yang

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

### OpenHarmony — Apache License 2.0

```text

                                 Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/

   TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION

   1. Definitions.

      "License" shall mean the terms and conditions for use, reproduction,
      and distribution as defined by Sections 1 through 9 of this document.

      "Licensor" shall mean the copyright owner or entity authorized by
      the copyright owner that is granting the License.

      "Legal Entity" shall mean the union of the acting entity and all
      other entities that control, are controlled by, or are under common
      control with that entity. For the purposes of this definition,
      "control" means (i) the power, direct or indirect, to cause the
      direction or management of such entity, whether by contract or
      otherwise, or (ii) ownership of fifty percent (50%) or more of the
      outstanding shares, or (iii) beneficial ownership of such entity.

      "You" (or "Your") shall mean an individual or Legal Entity
      exercising permissions granted by this License.

      "Source" form shall mean the preferred form for making modifications,
      including but not limited to software source code, documentation
      source, and configuration files.

      "Object" form shall mean any form resulting from mechanical
      transformation or translation of a Source form, including but
      not limited to compiled object code, generated documentation,
      and conversions to other media types.

      "Work" shall mean the work of authorship, whether in Source or
      Object form, made available under the License, as indicated by a
      copyright notice that is included in or attached to the work
      (an example is provided in the Appendix below).

      "Derivative Works" shall mean any work, whether in Source or Object
      form, that is based on (or derived from) the Work and for which the
      editorial revisions, annotations, elaborations, or other modifications
      represent, as a whole, an original work of authorship. For the purposes
      of this License, Derivative Works shall not include works that remain
      separable from, or merely link (or bind by name) to the interfaces of,
      the Work and Derivative Works thereof.

      "Contribution" shall mean any work of authorship, including
      the original version of the Work and any modifications or additions
      to that Work or Derivative Works thereof, that is intentionally
      submitted to Licensor for inclusion in the Work by the copyright owner
      or by an individual or Legal Entity authorized to submit on behalf of
      the copyright owner. For the purposes of this definition, "submitted"
      means any form of electronic, verbal, or written communication sent
      to the Licensor or its representatives, including but not limited to
      communication on electronic mailing lists, source code control systems,
      and issue tracking systems that are managed by, or on behalf of, the
      Licensor for the purpose of discussing and improving the Work, but
      excluding communication that is conspicuously marked or otherwise
      designated in writing by the copyright owner as "Not a Contribution."

      "Contributor" shall mean Licensor and any individual or Legal Entity
      on behalf of whom a Contribution has been received by Licensor and
      subsequently incorporated within the Work.

   2. Grant of Copyright License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      copyright license to reproduce, prepare Derivative Works of,
      publicly display, publicly perform, sublicense, and distribute the
      Work and such Derivative Works in Source or Object form.

   3. Grant of Patent License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      (except as stated in this section) patent license to make, have made,
      use, offer to sell, sell, import, and otherwise transfer the Work,
      where such license applies only to those patent claims licensable
      by such Contributor that are necessarily infringed by their
      Contribution(s) alone or by combination of their Contribution(s)
      with the Work to which such Contribution(s) was submitted. If You
      institute patent litigation against any entity (including a
      cross-claim or counterclaim in a lawsuit) alleging that the Work
      or a Contribution incorporated within the Work constitutes direct
      or contributory patent infringement, then any patent licenses
      granted to You under this License for that Work shall terminate
      as of the date such litigation is filed.

   4. Redistribution. You may reproduce and distribute copies of the
      Work or Derivative Works thereof in any medium, with or without
      modifications, and in Source or Object form, provided that You
      meet the following conditions:

      (a) You must give any other recipients of the Work or
          Derivative Works a copy of this License; and

      (b) You must cause any modified files to carry prominent notices
          stating that You changed the files; and

      (c) You must retain, in the Source form of any Derivative Works
          that You distribute, all copyright, patent, trademark, and
          attribution notices from the Source form of the Work,
          excluding those notices that do not pertain to any part of
          the Derivative Works; and

      (d) If the Work includes a "NOTICE" text file as part of its
          distribution, then any Derivative Works that You distribute must
          include a readable copy of the attribution notices contained
          within such NOTICE file, excluding those notices that do not
          pertain to any part of the Derivative Works, in at least one
          of the following places: within a NOTICE text file distributed
          as part of the Derivative Works; within the Source form or
          documentation, if provided along with the Derivative Works; or,
          within a display generated by the Derivative Works, if and
          wherever such third-party notices normally appear. The contents
          of the NOTICE file are for informational purposes only and
          do not modify the License. You may add Your own attribution
          notices within Derivative Works that You distribute, alongside
          or as an addendum to the NOTICE text from the Work, provided
          that such additional attribution notices cannot be construed
          as modifying the License.

      You may add Your own copyright statement to Your modifications and
      may provide additional or different license terms and conditions
      for use, reproduction, or distribution of Your modifications, or
      for any such Derivative Works as a whole, provided Your use,
      reproduction, and distribution of the Work otherwise complies with
      the conditions stated in this License.

   5. Submission of Contributions. Unless You explicitly state otherwise,
      any Contribution intentionally submitted for inclusion in the Work
      by You to the Licensor shall be under the terms and conditions of
      this License, without any additional terms or conditions.
      Notwithstanding the above, nothing herein shall supersede or modify
      the terms of any separate license agreement you may have executed
      with Licensor regarding such Contributions.

   6. Trademarks. This License does not grant permission to use the trade
      names, trademarks, service marks, or product names of the Licensor,
      except as required for reasonable and customary use in describing the
      origin of the Work and reproducing the content of the NOTICE file.

   7. Disclaimer of Warranty. Unless required by applicable law or
      agreed to in writing, Licensor provides the Work (and each
      Contributor provides its Contributions) on an "AS IS" BASIS,
      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
      implied, including, without limitation, any warranties or conditions
      of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
      PARTICULAR PURPOSE. You are solely responsible for determining the
      appropriateness of using or redistributing the Work and assume any
      risks associated with Your exercise of permissions under this License.

   8. Limitation of Liability. In no event and under no legal theory,
      whether in tort (including negligence), contract, or otherwise,
      unless required by applicable law (such as deliberate and grossly
      negligent acts) or agreed to in writing, shall any Contributor be
      liable to You for damages, including any direct, indirect, special,
      incidental, or consequential damages of any character arising as a
      result of this License or out of the use or inability to use the
      Work (including but not limited to damages for loss of goodwill,
      work stoppage, computer failure or malfunction, or any and all
      other commercial damages or losses), even if such Contributor
      has been advised of the possibility of such damages.

   9. Accepting Warranty or Additional Liability. While redistributing
      the Work or Derivative Works thereof, You may choose to offer,
      and charge a fee for, acceptance of support, warranty, indemnity,
      or other liability obligations and/or rights consistent with this
      License. However, in accepting such obligations, You may act only
      on Your own behalf and on Your sole responsibility, not on behalf
      of any other Contributor, and only if You agree to indemnify,
      defend, and hold each Contributor harmless for any liability
      incurred by, or claims asserted against, such Contributor by reason
      of your accepting any such warranty or additional liability.

   END OF TERMS AND CONDITIONS
```

### WinUI acrylic noise — MIT

    MIT License

    Copyright (c) Microsoft Corporation. All rights reserved.

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
    SOFTWARE
