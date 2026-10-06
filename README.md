# 飘雪桌面（SnowDesktop）

[简体中文](./README.md) | [English](./README.en.md)

**让桌面，好看又好用。**

SnowDesktop 是一款以组件为核心的 Windows 桌面整理与个性化软件。通过不同组件收纳应用、整理文件、查看日程，再搭配 Dock、快捷导航和外观设置，让桌面上的东西更好找，常用操作更方便，整体风格也更合自己的喜好。

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

### 软件再多，也能井井有条

使用集合组件，把办公、设计、游戏等软件按用途收纳，不用在满屏图标里逐个寻找。分组多了，还可以使用集合组组件，将多个集合整合在一起，通过标签页切换，在同一块桌面区域访问不同的软件分组。

使用桌面文件组件，让文档、图片等文件按类型归类显示。经常使用的文件夹，则可以通过文件夹映射组件，把其中的内容直接显示在桌面。查看资料、打开文档时，不用再反复进入文件夹，文件仍保留在原来的位置。

### 常用入口，就在手边

把常用应用和文件夹放进 Dock，停靠在屏幕边缘。需要时，通过快捷键或边缘触发将 Dock 唤到前台，在其他窗口上方访问常用内容。

Dock 配有流畅灵动的动画，让应用启动和窗口最小化、还原更有动感。

### 想找什么，直接输入

使用快捷导航，输入名称即可查找应用、桌面项目和已整理的文件，不必逐个翻找图标。接入 Everything 后，还能搜索它索引的本机文件。

除了找应用和文件，也可以从快捷导航发起网页搜索、查找设置或进行简单计算，减少查找入口和切换工具的步骤。

### 常看的信息，不必反复打开

使用日程、ToDo 清单和便签组件，把今天的安排、需要完成的事情和临时记录放在桌面上，回到桌面就能看到。

还可以添加时钟、月历、媒体控制和系统状态等组件，查看时间、控制音乐或关注系统状态。按需要组合和摆放，只保留自己用得上的内容。

### 实用，也要合眼缘

通过图标美化统一应用图标的风格，搭配底色、玻璃质感与颜色滤镜，让不同软件的图标与壁纸更协调。喜欢更醒目的布局，还可以把常用应用设为软件大图标，选择形状或换上自己的图片。

调整集合、Dock、状态栏和桌面组件的颜色、透明度与毛玻璃效果，搭配整套桌面外观。喜欢的样式可以保存为主题，随时切换，也支持导入和导出，不必每次重新调整。

### 更多灵感，来自创意工坊

通过 Steam 创意工坊订阅社区制作的组件和主题，为桌面添加新的工具和装饰，或尝试不同的外观搭配。

有自己的想法，也可以借助 AI 做成组件。SnowDesktop 提供组件制作 Skill，可配合支持 Skill 的 AI 编程工具使用。从描述需求、生成组件，到预览调整、检查打包，再到确认后发布到创意工坊，AI 都能协助完成。

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
- [YASB](https://github.com/amnweb/yasb)（MIT；状态栏托盘和系统控制参考，固定版本与复用范围见 [说明](third_party/yasb/README.md)）
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
