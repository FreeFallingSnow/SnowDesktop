# 项目脚本

本目录统一保存开发、测试和发布入口及其实现：

- `build.bat`：默认无进程副作用的 Release 编译；
- `build_debug.bat`：默认无进程副作用的 Debug 编译；
- `test.bat`：按完整、快速、核心、标签或名称构建并运行 CTest 测试；
- `test_manager.ps1`：从 CTest 清单动态选择测试及其构建目标，避免在脚本中重复维护目标列表；
- `widget-dev.bat`：同步并监听本地 Lua 组件，保存后热重载，无需重复编译；
- `steam-dev.bat`：以正式 App ID 创建临时本地 Steam 上下文，运行主程序、创作者管理器或 Bridge，退出后安全清理；
- `steam_local_deploy.ps1`：默认只读预检，将构建载荷显式部署为 Steam 安装根内隔离的 `steam-local-dev` runtime；
- `release.bat`：无参数打开发布 TUI，带参数作为 Agent/自动化 CLI。

- `release_manager.ps1`：统一发布状态、打包、仓库同步、合并及发布流程；
- `package_release.ps1`：生成携带版、MSIX、符号包和商店上传包；
- `package_steam.ps1`：生成 Steam 专属载荷，只允许在 `SnowDesktop.Runtime` 中携带 `steam_api64.dll`，拒绝 SDK 头文件、导入库、工具和 `steam_appid.txt`；
- `steam_pipe.ps1`：为 Steam 专属载荷生成 SteamPipe VDF，支持不上传的 Preview、固定私有开发分支上传和独立确认的公开分支上传；
- `write_deployment_manifest.ps1`：由 MSBuild 调用，生成确定性的 WinAppSDK 自包含部署清单；
- `deployment_payload.psm1`：供携带版、MSIX 与 Steam 打包共用的清单校验、第三方运行时隔离、复制和 AppX fragment 合并模块；
- `squash_release_to_main.bat`：只执行本地 squash、提交和标签；
- `widget_dev.ps1`：组件校验、开发目录同步与监听实现。

常用命令：

```bat
scripts\build.bat
scripts\build_debug.bat
scripts\test.bat
scripts\test.bat fast
scripts\test.bat core
scripts\test.bat label rules
scripts\test.bat name quick_navigation
scripts\test.bat list
scripts\widget-dev.bat widgets\reminders
scripts\widget-dev.bat widgets\reminders -Once
scripts\steam-dev.bat manager
scripts\steam-dev.bat bridge status
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\steam_local_deploy.ps1 -PayloadDirectory <已解压的干净携带版目录> -BuildId local-test
scripts\release.bat
scripts\release.bat status -Json
scripts\release.bat package
scripts\release.bat package -ReloadShell
scripts\release.bat package-steam
scripts\release.bat steam-preview
scripts\release.bat steam-upload-dev -Yes -ConfirmVersion 1.0.5.0 -ConfirmPrivateBranch internal-dev
scripts\release.bat steam-upload-public -Yes -ConfirmVersion 1.0.5.0 -ConfirmPublicBranch public
```

构建入口默认不会关闭 SnowDesktop 或重启 Explorer。任务栏与 Wallpaper Engine Hook 均从
进程专属临时副本注入，所以正常退出后残留在目标进程中的临时模块不会阻止下一次构建；只有旧版本仍直接加载构建目录 DLL
等确实占用构建输出的情况才需要 `--reload-shell`。脚本会明确提示并短暂重启 Shell。发布 CLI
对应使用 `-ReloadShell`；发布 TUI 检测到实际构建输出占用时会在执行前请求一次明确确认。
本地脚本、CI 和 IDE 共用 `CMakePresets.json` 中的配置。

测试按使用场景分层：日常改动优先用 `name <regex>` 或 `label <regex>` 运行最小充分集合；
`core` 只运行核心测试；`fast` 运行除 `integration` 外的测试；无参数或 `full` 运行全部测试。
筛选参数是 CTest 正则表达式。完整测试用于任务最终交付、Pull Request 和发布验证，不要求每个
中间 Commit 重复执行。

`scripts\widget-dev.bat` 需要先构建一次宿主。首次创建开发候选时会重启一次
SnowDesktop 以发现组件；开发候选默认不覆盖已安装版本，需要在“我的组件”卡片中
显式激活。之后修改 `main.lua`、清单、本地化、模块或资源文件，只需保存即可同步；
候选处于激活状态时会触发事务式热重载。

`scripts\steam-dev.bat` 需要 SDK-enabled Release 构建及正在运行、已登录且拥有
SnowDesktop 开发许可的 Steam 客户端。脚本从 `packaging\steam-identity.json`
读取正式 App ID，只在 `.build\Release\` 临时创建 `steam_appid.txt`；若该文件原本
存在则校验但不删除。正式 Steam 包始终拒绝携带此开发文件。

`scripts\steam_local_deploy.ps1` 不修改 Steam 的 appmanifest、depot 或正式
`distribution`。默认仅显示将要写入的位置；显式增加 `-Apply` 后才通过 staging 和
SHA-256 校验写入 `<Steam安装根>\.snowdesktop\dev\<build-id>`。开发数据使用同一
Steam 安装根内的 `.snowdesktop\dev-data\<profile-id>`，但部署动作本身不会创建数据，
也不会接管生产自启动。该入口用于调试部署身份，不能冒充 Steam 客户端安装或更新。
输入必须是解压后的干净发行载荷；不要直接传 `.build\Release`，因为构建目录可能包含
当前用户的 `data`，部署器会拒绝把用户数据或部署状态复制进开发 runtime。缺少
Steamworks SDK 时，可先用 `package_release.ps1 -SkipBuild -Development` 生成携带版
ZIP，解压到独立临时目录后再作为 `-PayloadDirectory`。

SteamPipe 操作需要将 `SNOWDESKTOP_STEAMCMD_PATH` 指向 `steamcmd.exe`，并在
`SNOWDESKTOP_STEAM_BUILD_ACCOUNT` 中提供仅具备所需应用权限的构建账号名。脚本不接受
密码、Steam Guard 代码或分支密码；先人工运行一次 SteamCMD 完成登录与 Steam Guard，
之后自动化只使用 SteamCMD 自身缓存的登录状态，并关闭密码提示。`steam-preview` 使用
SteamPipe 的 Preview 模式，不上传 depot 内容，也不改变任何分支；`steam-upload-dev`
只能上传并 `SetLive` 到 `packaging\steam-pipe.json` 中的私有开发分支；
`steam-upload-public` 只能上传并 `SetLive` 到配置中精确命名为 `public` 的公开分支。
两种上传都必须分别确认当前版本和对应分支名，开发上传不能借此指向公开分支。

组件创建流程位于 SnowDesktop 主程序的“组件开发工具”页。该页可将开放 Agent
Skill 一键同步到共享目录及 Codex、Claude Code、Cursor、GitHub Copilot、Gemini
CLI 的兼容目录；每份 Skill 自带 `bin\snowwidget.exe`，并提供 `capabilities`、
`api-contract`、`validate` 与 `pack` 命令。创意工坊管理器只处理 Steam 发布，并且默认只发现
`.build\Release\data\widgets\dev` 中的开发组件，不读取内置组件；因此仅进入 Steam 载荷，
携带版和 MSIX 均不分发该管理器。

发布流程的完整说明见 `packaging\README.md`。
