# Lua

Lua 脚本引擎，用于 SnowDesktop 的 Lua 组件运行时和 `snowwidget` 开发工具。

## 来源

- 上游：[Lua](https://www.lua.org/)。
- 本地版本：5.4.7，以 [lua.h](lua.h) 的版本宏为准。
- 版权：Copyright (C) 1994-2024 Lua.org, PUC-Rio。
- 许可证：MIT，完整声明见 [LICENSE.txt](LICENSE.txt)。

## 本地文件与使用方式

目录保存 Lua 核心和标准库源码。根目录 [CMakeLists.txt](../../CMakeLists.txt) 将选定源码编译为 `SnowDesktopLua`；`lua.c`、`luac.c` 和上游 `Makefile` 保留在目录中，不属于该库目标的源码清单。

SnowDesktop 的宿主 API、权限和组件生命周期由 [src/widget/](../../src/widget/) 实现。发行包随附 `licenses/Lua-LICENSE.txt`。
