# Lua

Lua 脚本引擎，用于 SnowDesktop 的 Lua 组件运行时和 `snowwidget` 开发工具。

## 来源

- 上游：[Lua](https://www.lua.org/)。
- 本地版本：5.4.7，以 [lua.h](lua.h) 的版本宏为准。
- 版权：Copyright (C) 1994-2024 Lua.org, PUC-Rio。
- 许可证：MIT，完整声明见 [LICENSE.txt](LICENSE.txt)。

## 本地文件与使用方式

目录保存 Lua 核心和标准库源码。根目录 [CMakeLists.txt](../../CMakeLists.txt) 将选定源码编译为 `SnowDesktopLua`；`lua.c`、`luac.c` 和上游 `Makefile` 保留在目录中，不属于该库目标的源码清单。

发行包随附 `licenses/Lua-LICENSE.txt`。

## 本地回移补丁

- `lcode.c` 的 `luaK_exp2val` 回移 [Lua 5.4.7 官方缺陷列表第 2 项](https://www.lua.org/bugs.html#5.4.7-2) 的比较表达式索引修正，在表达式为 `VJMP` 时先生成寄存器结果。其余版本宏保持 5.4.7；回归由 `lua_runtime` 的实际编译执行检查覆盖。
