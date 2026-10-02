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

- `release_manager.ps1`：统一发布状态、打包、本地合并及官方源码仓库发布流程；
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

MSBuild 同时构建的项目数由预设的 `jobs` 控制；每个项目内 MSVC 同时编译的源码数由
`SNOWDESKTOP_COMPILE_JOBS` 控制。后者默认使用本机逻辑处理器数的一半，最少 1、最多 8，
避免直接占满开发机。可按内存和其他任务负载覆盖，设置保存在该构建目录的 CMake 缓存中：

```bat
cmake --preset release -DSNOWDESKTOP_COMPILE_JOBS=4
scripts\build.bat
```

Debug 构建对应使用 `--preset debug` 和 `scripts\build_debug.bat`。恢复当前机器的自动默认值时，
运行 `cmake --preset release -U SNOWDESKTOP_COMPILE_JOBS`。项目数与项目内编译数可能相乘，
因此不要同时大幅提高两者；比较构建耗时时应固定源码输入并记录实际配置。

测试按使用场景分层：日常改动优先用 `name <regex>` 或 `label <regex>` 运行最小充分集合；
`core` 只运行核心测试；`fast` 运行除 `integration` 外的测试；无参数或 `full` 运行完整自动测试集合。
这些自动模式与对应 CTest 预设都排除 `manual` 手动诊断。Shell 文件操作集成测试已独立为手动诊断，
平时不执行，遇到相关文件操作或 Shell 阻塞问题时运行 `scripts\test.bat name "^shell_file_operation_worker$"`；
其预检回归仍自动运行。`name` / `label` 的显式筛选可以包含手动条目，`list` 会显示全部条目及标签。
直接使用 CTest 时，`tests` 是自动全量，`all-tests` 包含手动诊断；自动全量通过不代表手动条目已执行。
筛选参数是 CTest 正则表达式。完整测试用于任务最终交付、Pull Request 和发布验证，不要求每个
中间 Commit 重复执行。

`scripts\widget-dev.bat` 需要先构建一次宿主。新增开发候选后，打开组件设置或右键
“添加组件”菜单即可重新发现组件，无需重启；`-RestartHost` 仍可显式重启宿主。
新发现且验证通过的开发 UUID 默认激活，已保存的停用选择保持不变；敏感权限仍需授权。
之后修改 `main.lua`、清单、本地化、模块或资源文件，只需保存即可同步；
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

## 同目录协作构建

只有实际修改源码、构建配置或相关构建输入的任务登记。只读代码审计、性能评估、阅读日志、
写外置报告和无需编译的定向补测不调用 `begin`/`finish`，不触发构建，也不阻塞开发参与者；`status` 只查询状态。
性能测量另需避开正在运行的构建/测试，登记协议不提供测量资源预约。

两个开发对话各自先登记，保存 JSON 返回的 `batchId`，再开始修改：

```bat
scripts\build.bat begin task-a
scripts\build.bat begin task-b
```

仍在编辑阶段的登记进入同一批次。每个参与者用同一个任务 ID 和返回的批次 ID 完成：

```bat
scripts\build.bat finish task-a -Batch <batchId>
scripts\build.bat finish task-b -Batch <batchId>
scripts\build.bat status
scripts\build.bat status -Batch <batchId>
```

先完成的调用等待；最后完成者在同一个状态事务内冻结成员并取得构建权，调用原有
`build.bat` 和 `test.bat`，构建失败时不启动测试。所有参与者得到相同的 JSON 结果、批次 ID、
参与者列表、退出码、错误和日志路径。重复 `begin` 复用尚在编辑的登记；重复 `finish` 等待或读取
指定批次的历史结果，不重新构建。`finish` 必须传批次 ID，避免任务 ID 被下一批复用后串批。
同一任务 ID 已重新登记时，`finish` 拒绝返回旧批成功；没有新登记但输入已变时也拒绝重放
旧成功结果。未重新打开登记的等待者取得同一份持久化结果。`status -Batch` 用于读取历史，
它不验证当前改动；早于输入校验功能的成功记录也只能作为历史查询。

`finish` 后需要继续修改时，先查询 `status`，再用同一任务 ID 调用 `begin`。
批次仍为 `editing` 时，`begin` 在原批次重新打开已完成的登记、清除完成时间，不重复添加参与者；
只有收到成功登记结果后才可编辑。旧的 `finish` 等待进程会以退出码 2 结束并提示登记已重新打开，
不能再次将新一轮编辑标记为完成。全部修改完成后重新调用 `finish`。重复 `begin` 不增加编辑轮次。
批次已为 `building` 时，`begin` 继续等待原批次结果，随后进入新批次；已撤回的登记不能重新打开。

构建和测试期间的新 `begin` 等待，上一批结果持久化后才登记到下一批。登记成功前、`finish`
调用后至结果返回或同批重新登记成功前不得修改共享源码、构建配置或其他构建输入；执行下述无需编译的定向补测并
写入独立的忽略日志目录不在此限制内。`-WaitSeconds 60` 可限制登记/屏障等待时间（默认 24 小时）；
等待超时返回 2 并保留已有登记，稍后重复同一个命令。该参数不会中止已启动的构建。
成功返回 0；标准构建/测试失败返回原始退出码；用法/状态错误返回 2；显式恢复出的中断或取消
结果返回 4；输入起止校验失败返回 5（`invalidated`），即使构建/测试命令自身返回 0 也不算通过。
`status` 和成功执行的 `recover` 返回 0，结果内的 `exitCode` 保留原始结论。

协调状态、每批不可覆盖的结果 `<batchId>.json` 和日志 `<batchId>.log` 保存在忽略的
`.build\collaboration\`，不要删除或提交。短事务使用 Windows 文件共享锁，构建另持独占租约；
进程退出自动释放句柄。结果以临时文件、落盘和原子替换发布，发布后才清理该批登记；不会清空
其他活动批次。JSON 损坏或版本/路径不匹配时停止，保留证据供诊断，不自动重置。

冻结成员后、启动构建前记录 `inputStart`；构建与测试结束后记录 `inputEnd`。摘要使用
SHA-256，包含 Git 的已跟踪和未忽略的新增文件的相对路径与实际内容，因而覆盖未提交内容、
删除与重命名；同时包含 HEAD、`CMakeUserPresets.json`（即使被 Git 忽略）和相关工具链/SDK
环境路径。所有参与者的结果绑定同一个批次、输入摘要及原始 `pipelineExitCode`。
生成输出、协调状态、忽略的探测/报告目录和根目录说明文档不参与摘要，避免构建自触发；
运行时分发的 notices 和组件 Skill 等资源仍属于输入。外置报告放在仓库之外或 `.codex-probes/`。

这保证守约的 `begin` 调用者在构建期间等待，并检测绕过登记后在两个采样端点之间仍有差异的
输入变动；不锁住源文件，也不是完整的文件系统快照。中途修改后又恢复、采样过程中的竞态、
仓库之外的 SDK/工具内容及生成缓存的外部改写不由此摘要保证。起止一致只能报告“端点摘要一致”，
不能声称未发生任何中途改动。检测出变动或末尾无法读取输入时结果失效，保留日志并释放租约，
不自动重试；先确认所有修改者停止，再重新登记新批次。

异常恢复必须先查看 `status`、日志和共享目录差异，确认对应编辑者或构建已停止，检查残留改动
及同文件冲突。编辑登记不绑定短暂的 `begin` 进程；登记年龄只用于诊断，不自动判为完成。
确认放弃一个编辑者后可显式撤回它：

```bat
scripts\build.bat recover task-b -Batch <batchId> -ConfirmStopped -Reason "editor stopped; remaining changes reviewed"
```

撤回记为 `withdrawn` 并保留到批次结果持久化，其他编辑和已完成登记都保留；不表示该编辑者
成功完成。其他参与者完成后仍构建共享目录中的实际改动。全部撤回时持久化 `cancelled` 结果，
不构建。如果编辑者可以继续，直接用原任务 ID 继续修改后调用 `finish`，无需撤回。

每次构建的子进程树在专用 Windows Job 中运行，协调器崩溃会停止它启动的构建/测试子进程，
不会停止其他任务或用户应用。协作子进程临时设置 `MSBUILDDISABLENODEREUSE=1`，避免复用
本批之外的旧 MSBuild worker；不修改系统环境。批次继续保持冻结，不自动重跑或放行编辑。
确认 owner 已退出后：

```bat
scripts\build.bat recover -Batch <batchId> -ConfirmStopped -Reason "build owner exited; log reviewed"
```

脚本拒绝恢复仍活着、无法检查或仍持构建租约的 owner。恢复持久化 `interrupted` 结果并放行下一批；
若结果已落盘而清理状态前进程退出，则复用原结果完成清理，不重复构建。恢复从不自动启动构建。
失败或中断后需要修改构建输入或重新构建的重试属于新批次，各参与者重新 `begin`。
无需编译的定向补测不重新登记，也不因原批次失败而重新执行完整构建测试。

此协议只约束调用者。原有无参数/`--reload-shell` 构建、Debug、默认先编译的 `test.bat` 和 IDE 入口
会修改共享产物，协作期间不要另行调用。执行已生成测试程序不属于构建；对应输入未变且没有二进制、
日志、数据或外部资源冲突时，可在其他任务编辑期间直接定向补测，不必等待其 `finish`。
启用协议前等待已有编译退出，要求所有修改者遵守登记屏障。
协作构建不提供自动停止应用或重启 Explorer 的选项；占用时记录原有预检失败，正常退出占用
应用后在新批次重试。共享目录中的同文件冲突和未登记修改无法由脚本自动解决。

补测不是完整测试。已编译且输入匹配的条目可直接通过现有 CTest 清单执行，例如：

```bat
ctest --test-dir .build -C Release -R "^widget_author_tools$" --no-tests=error --output-on-failure
```

执行前核对所选清单、产物来源和运行依赖；保存命令、二进制指纹和独立结果，不附加配置或编译。
若共享 `Testing/` 日志或测试资源可能冲突，使用隔离的运行目录，或按 CTest 清单直接执行程序并
保留参数、环境、工作目录和超时。资源冲突只等待或隔离冲突资源，不升级为全量。仅相关输入变化、
产物缺失或匹配关系无法确认时才需要重新编译；此时继续遵守协作构建规则。

独立补测开始前查询 `.build/verification/` 及相关已有批次/测试报告，复用适用于当前输入的通过
结果；相同条目正在运行时只等待该项。每次以唯一 `<runId>.json` 写入 `running` 记录，结束后改为
`passed`、`failed` 或 `interrupted`，包含任务、所选测试、命令、配置、产物/运行资源指纹、输入
匹配依据、时间、退出码和独立日志路径。这是对话执行约定，现有运行器不会自动登记或去重。
没有有效记录时只补缺项；陈旧 `running` 先核对进程和日志，不永久阻挡，也不触发完整构建测试。

轻量并发回归入口为 `scripts\test.bat name "^build_collaboration$"`；测试使用临时目录与可控假
构建，不操作真实构建产物或用户数据。实现位于 `build_manager.ps1`，`build_job.cs` 只负责
受控进程树生命周期，`build_inputs.ps1` 负责输入内容身份。

## 耦合运行性能调试

`profile.bat` 是默认关闭的性能采集入口。`status` 查询当前宿主能力，
`capture -Seconds 60` 采集并生成 JSON/CSV，`start` / `stop -Session ...`
支持异步自动化控制，`report` / `compare` 支持离线分析。
