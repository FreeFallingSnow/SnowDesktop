# MinHook

Windows API Hook 库，用于 SnowDesktop 的任务栏与 Wallpaper Engine 捕获 Hook。

## 来源

- 上游：[TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook)。
- 本地记录：随仓库保存的源码；当前未记录版本号或上游提交。
- 版权：Copyright (C) 2009-2017 Tsuda Kageyu。
- 许可证：BSD 2-Clause，完整声明及附带代码的版权说明见 [LICENSE.txt](LICENSE.txt)。

## 本地文件与使用方式

`include/MinHook.h` 提供接口；`src/` 保存 Hook、跳板、内存缓冲及 x86/x64 指令解码实现。构建目标按架构选择所需源码，入口见根目录 [CMakeLists.txt](../../CMakeLists.txt) 和 [wallpaper_hook/CMakeLists.txt](../../src/wallpaper_hook/CMakeLists.txt)。

发行包随附 `licenses/MinHook-LICENSE.txt`。
