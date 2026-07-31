# 项目脚本

本目录统一保存开发、测试和发布入口及其实现：

- `build.bat`：默认无进程副作用的 Release 编译；
- `build_debug.bat`：默认无进程副作用的 Debug 编译；
- `test.bat`：构建并运行完整 CTest 测试集（包含国际化静态与运行时契约）；
- `widget-dev.bat`：同步并监听本地 Lua 组件，保存后热重载，无需重复编译；
- `release.bat`：无参数打开发布 TUI，带参数作为 Agent/自动化 CLI。

- `release_manager.ps1`：统一发布状态、打包、仓库同步、合并及发布流程；
- `package_release.ps1`：生成携带版、MSIX、符号包和商店上传包；
- `squash_release_to_main.bat`：只执行本地 squash、提交和标签；
- `widget_dev.ps1`：组件校验、开发目录同步与监听实现。

常用命令：

```bat
scripts\build.bat
scripts\build_debug.bat
scripts\test.bat
scripts\widget-dev.bat widgets\reminders
scripts\widget-dev.bat widgets\reminders -Once
scripts\release.bat
scripts\release.bat status -Json
scripts\release.bat package
```

构建入口默认不会关闭 SnowDesktop 或重启 Explorer。若任务栏 Hook DLL 仍被占用，
先正常退出 SnowDesktop；只有在明确接受 Shell 短暂重启时才传入
`--reload-shell`。本地脚本、CI 和 IDE 共用 `CMakePresets.json` 中的配置。

`scripts\widget-dev.bat` 需要先构建一次宿主。首次创建开发覆盖时会重启一次
SnowDesktop 以发现组件；之后修改 `main.lua`、清单、本地化、模块或资源文件，
只需保存即可同步并触发事务式热重载。

发布流程的完整说明见 `packaging\README.md`。
