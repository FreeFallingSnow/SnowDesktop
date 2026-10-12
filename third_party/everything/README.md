# Everything SDK

Everything 的客户端 SDK，用于 SnowDesktop 快捷导航中的本机文件搜索。

## 来源

- 上游：[Everything SDK](https://www.voidtools.com/support/everything/sdk/)。
- 本地记录：随仓库保存的 SDK 源码；当前未记录独立版本号或上游提交。
- 版权：David Carpenter，完整声明中的年份分别为 2016、2022。
- 许可证：MIT，完整声明见 [LICENSE.txt](LICENSE.txt)。

## 本地文件与使用方式

`include/Everything.h`、`src/Everything.c` 和 `ipc/Everything_IPC.h` 提供客户端接口和 IPC 协议。[everything_search.cpp](../../src/navigation/everything_search.cpp) 调用 SDK 查询 Everything 索引。

仓库包含客户端 SDK，Everything 主程序由用户另行安装并运行。发行包随附 `licenses/Everything-SDK-LICENSE.txt`。
