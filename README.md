# 飘雪桌面（SnowDesktop）

[简体中文](./README.md) | [English](./README.en.md)

让 Windows 桌面整齐，也合你的心意。自动分类桌面文件，将常用应用和文件夹集中收纳，用 Dock 快速启动和切换窗口。搭配实用组件、玻璃主题和多屏独立布局，按自己的习惯安排桌面。

## 📦 安装

<p>
  <a href="https://store.steampowered.com/app/5080330/SnowDesktop/"><img src="https://img.shields.io/badge/Steam-171A21?style=for-the-badge&amp;logo=steam&amp;logoColor=white" alt="Steam 商店" width="137" height="44"></a>
  &nbsp;
  <a href="https://apps.microsoft.com/detail/9PLLGJVL4LC3"><img src="https://get.microsoft.com/images/zh-cn%20dark.svg" alt="从 Microsoft 获取" height="44"></a>
</p>

[官网](https://snowdesktop.com/) | [GitHub](https://github.com/FreeFallingSnow/SnowDesktop)

## 📊 版本对比

两个版本都包含 SnowDesktop 的完整核心功能。

| 功能 | Microsoft Store | Steam |
| --- | --- | --- |
| 桌面整理 | ✓ 包含 | ✓ 包含 |
| Dock 与快捷导航 | ✓ 包含 | ✓ 包含 |
| 内置组件 | ✓ 包含 | ✓ 包含 |
| Steam 创意工坊 | — 不包含 | ✓ 包含 |
| 高级功能 | — 不包含 | ✓ 包含 |
| 更新与修复 | 随版本号迭代发布 | 更新更及时，可选测试渠道 |

Steam 版支持通过 Steam 创意工坊发现并安装社区组件。当前支持的高级功能：**软件大图标**。

详见[官网版本对比](https://snowdesktop.com/zh-cn/compare/)。

## ✨ 功能亮点

- 🖥️ **为不同屏幕安排独立布局**：为不同显示器安排不同内容，分别调整各页面的网格行列数，再按需要设置图标间距和组件位置。桌面放不下时，还可以新增页面，翻页查看更多内容。
- 🗂️ **把文件和快捷方式放到真正合适的位置**：
  - **集合**：将常用应用和快捷方式按用途分区收纳，让桌面上的常用入口一目了然。
  - **集合组**：用标签页切换多组集合，在同一块桌面区域安排工作、学习或娱乐所需的工具。
  - **桌面文件分类**：自动按类型分组显示桌面文件，让文档、图片等内容更容易找到。
  - **文件夹映射**：将常用文件夹的内容直接呈现在桌面，随时查看和管理，无需反复打开文件夹窗口。
  - **文件组**：把桌面文件和多个文件夹的内容集中到一处，跨来源搜索所需文件。
- 🚀 **更集中地访问应用和窗口**：Dock 可以停靠在屏幕四边，也可以使用需要时临时唤起的悬浮 Dock。你可以向其中添加应用、文件夹栈和集合，查看正在运行的程序，并通过窗口预览快速激活、最小化或关闭窗口。
- 🔎 **快捷导航**：快捷导航可以按名称查找桌面项目和已安装的应用。本机安装并运行 Everything 后，还能在这里搜索它已索引的文件。
- 🧩 **在桌面上使用实用组件**：
  - **内置组件**：SnowDesktop 内置模拟与数字时钟、月历、日程、提醒、系统监控、媒体控制、便签、番茄钟、RSS 阅读器和快速启动等组件。
  - **组件管理与 Lua 扩展**：可以从本地组件包安装新组件，管理组件的启用、停用和更新，也可切回已保留的旧版本。有开发需求时，还可以用 Lua 编写自己的组件。
- 🎨 **外观个性化**：
  - **主题与样式**：选择深浅色主题，调整组件和 Dock 的颜色、透明度、圆角与背景效果，搭配毛玻璃或亚克力材质。
  - **Windows 11 任务栏**：Windows 11 的系统任务栏也可以设置为透明、深色、浅色或毛玻璃等外观，并调整图标和文字的明暗。你可以设置自动切换规则，例如平时保持透明，有应用窗口或窗口最大化时使用另一种外观；打开开始菜单、搜索或任务视图时，也可切换到指定样式。应用窗口规则按各显示器分别判断。
- 💾 **备份与迁移**：备份和恢复 SnowDesktop 的布局、设置、组件包及组件数据。

## 🛠️ 构建

依赖：CMake 3.24+、Visual Studio 2022（“使用 C++ 的桌面开发”工作负载）、
Windows 10/11 SDK 10.0.19041.0 或更高版本。NuGet 会按项目固定版本还原
Microsoft Windows App SDK 2.4.0 和 Microsoft.Windows.CppWinRT 3.0.260818.1，
无需在开发机或目标机器预装 Windows App SDK Runtime。

```bat
.\scripts\build.bat
```

默认构建不会终止 SnowDesktop 或重启 Explorer。如果任务栏 Hook DLL 正被占用，
预检会在编译前停止并提示。请先正常退出 SnowDesktop；需要自动解除占用时可使用
`.\scripts\build.bat --reload-shell`，该参数会终止 SnowDesktop 并短暂重启 Explorer。

## 🧱 技术栈

- C++20 / MSVC
- WinUI 3 / Windows App SDK 2.4.0（设置中心，Win32 XAML Island）
- Direct2D + Direct3D 11 + DirectComposition
- Dear ImGui（仅创意工坊管理器）
- Lua 5.4（脚本引擎）
- Fluent System Icons Regular（现代右键菜单与组件菜单图标）
- Font Awesome 6 Free（组件兼容图标）
- WinHTTP（Lua HTTP 运行时）

## 🤝 参与贡献

**项目目前处于高频开发期，暂不接受外部 Pull Request 的代码合并。**
欢迎通过 [Issues](https://github.com/FreeFallingSnow/SnowDesktop/issues)、
[QQ 用户群 976422547](https://qm.qq.com/q/HyazkCIRig) 或参与
[Steam 测试版](./CONTRIBUTING.md#加入-steam-测试版)交流与反馈。
参与方式、开发规范及许可说明详见[中文贡献指南](./CONTRIBUTING.md)或 [English](./CONTRIBUTING.en.md)。

## 📄 许可证

SnowDesktop 核心代码采用 GNU General Public License v3.0，详见 [LICENSE](./LICENSE)。
仓库中包含的第三方组件及其版权和许可信息，统一记录在
[THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md)。
发行包会在 `licenses/` 目录中随附所分发第三方组件的完整许可证与声明文件。

独立的 `steam_bridge/` 创意工坊桥接程序采用 MIT 许可证，详见
[steam_bridge/LICENSE](./steam_bridge/LICENSE)。Steamworks SDK 本身不包含在本仓库内，
也不适用上述 GPL 或 MIT 许可证。
