# 项目脚本

本目录统一保存开发、测试和发布入口及其实现：

- `build.bat`：默认无进程副作用的 Release 编译；
- `build_debug.bat`：默认无进程副作用的 Debug 编译；
- `test.bat`：构建并运行完整 CTest 测试集（包含国际化静态与运行时契约）；
- `widget-dev.bat`：同步并监听本地 Lua 组件，保存后热重载，无需重复编译；
- `steam-dev.bat`：以正式 App ID 创建临时本地 Steam 上下文，运行主程序、创作者管理器或 Bridge，退出后安全清理；
- `release.bat`：无参数打开发布 TUI，带参数作为 Agent/自动化 CLI。

- `release_manager.ps1`：统一发布状态、打包、仓库同步、合并及发布流程；
- `package_release.ps1`：生成携带版、MSIX、符号包和商店上传包；
- `package_steam.ps1`：生成 Steam 专属载荷，只允许携带 `steam_api64.dll`，拒绝 SDK 头文件、导入库、工具和 `steam_appid.txt`；
- `squash_release_to_main.bat`：只执行本地 squash、提交和标签；
- `widget_dev.ps1`：组件校验、开发目录同步与监听实现。

常用命令：

```bat
scripts\build.bat
scripts\build_debug.bat
scripts\test.bat
scripts\widget-dev.bat widgets\reminders
scripts\widget-dev.bat widgets\reminders -Once
scripts\steam-dev.bat manager
scripts\steam-dev.bat bridge status
scripts\release.bat
scripts\release.bat status -Json
scripts\release.bat package
```

构建入口默认不会关闭 SnowDesktop 或重启 Explorer。若任务栏 Hook DLL 仍被占用，
先正常退出 SnowDesktop；需要自动解除占用时传入 `--reload-shell`，脚本会明确提示并短暂
重启 Shell。本地脚本、CI 和 IDE 共用 `CMakePresets.json` 中的配置。

`scripts\widget-dev.bat` 需要先构建一次宿主。首次创建开发候选时会重启一次
SnowDesktop 以发现组件；开发候选默认不覆盖已安装版本，需要在“我的组件”卡片中
显式激活。之后修改 `main.lua`、清单、本地化、模块或资源文件，只需保存即可同步；
候选处于激活状态时会触发事务式热重载。

`scripts\steam-dev.bat` 需要 SDK-enabled Release 构建及正在运行、已登录且拥有
SnowDesktop 开发许可的 Steam 客户端。脚本从 `packaging\steam-identity.json`
读取正式 App ID，只在 `.build\Release\` 临时创建 `steam_appid.txt`；若该文件原本
存在则校验但不删除。正式 Steam 包始终拒绝携带此开发文件。

组件创建流程位于 SnowDesktop 主程序的“组件开发工具”页。该页可将开放 Agent
Skill 一键同步到共享目录及 Codex、Claude Code、Cursor、GitHub Copilot、Gemini
CLI 的兼容目录；每份 Skill 自带 `bin\snowwidget.exe`，并提供 `capabilities`、
`api-contract`、`validate` 与 `pack` 命令。创意工坊管理器只处理 Steam 发布，并且默认只发现
`.build\Release\data\widgets\dev` 中的开发组件，不读取内置组件。

发布流程的完整说明见 `packaging\README.md`。
