# 飘雪桌面（SnowDesktop）

一款 Windows 桌面整理美化工具：用 Direct2D 渲染的自定义桌面，替代 Explorer 原生图标，支持多显示器网格布局、可嵌入组件和 Lua 脚本扩展。
## 安装

[发行版仓库](https://github.com/FreeFallingSnow/SnowDesktop_Release)

## 功能

- **网格布局**：网格布局，容纳桌面内容和小组件，可自由调整行列。
- **多屏支持**：多屏分页独立存储，插拔显示器不会错乱，副屏离线时支持翻页查看对应内容。
- **桌面组件**：
  - **集合**：参考移动端系统的大文件夹，用于存储软件快捷方式，可自定义大小。
  - **桌面文件分类**：用于分类收纳桌面文件，支持列表显示和图标显示
  - **文件夹映射**：将非桌面文件夹映射到桌面显示，支持列表显示和图标显示
  - **Lua 脚本组件**：可扩展的组件，用于美化桌面

- **快捷导航**：快捷键呼出，按集合组件分类显示，用于在非桌面场景快速启动软件
- **个性化**：支持自定义组件颜色

## 构建

依赖：CMake 3.24+、Visual Studio 2022、Windows 10 SDK（0x0A00）

```bat
.\build.bat
```

构建脚本会自动终止已在运行的 SnowDesktop.exe。

## 技术栈

- C++20 / MSVC
- Direct2D + Direct3D 11 + DirectComposition
- Dear ImGui（设置窗口）
- Lua 5.4（脚本引擎）
- spdlog（日志）
- Font Awesome 6 Free（图标）
- WinHTTP（Lua HTTP 运行时）

## 协议

GNU General Public License v3.0 — 详见 [LICENSE](./LICENSE)
