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

确认当前构建输出被 SnowDesktop 或 Hook 占用时，可显式使用
`scripts\build.bat finish task-a -Batch <batchId> -ReloadShell`。此请求保存在本批，
由实际取得构建权的参与者传入标准 `build.bat --reload-shell`；等待其他编辑者时不执行清理，
无此参数时维持默认不终止应用的行为。执行前告知关闭 SnowDesktop、短暂重启 Explorer 的副作用，
并在整个构建和测试结束前避免重新打开被链接的应用。新批次不会继承此参数。

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
删除与重命名；HEAD 仅作为元数据记录，摘要不包含提交身份；另包含`CMakeUserPresets.json`（即使被 Git 忽略）和相关工具链/SDK
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

## 协作协议 v2：非阻塞就绪、检查与选择计划

`begin` 返回 `batchId` 与 `editRevision`。开始写文件前登记；各对话使用独立 ID。
`claim` 用同一状态锁检查路径及父目录冲突，不会解决同文件的修改合并。已有脏文件必须先审阅，再显式 `-AdoptExistingChanges`；没有声明的老参与者会显示为未确定所有权。

```bat
scripts\build.bat begin task-a
scripts\build.bat claim task-a -Batch <batchId> -Revision <editRevision> -Files src/my_module.cpp,tests/my_module_tests.cpp
scripts\build.bat plan task-a -Batch <batchId> -Revision <editRevision> -Scope module -Suites selected -Tests my_module -Inputs src/my_module.cpp,tests/my_module_tests.cpp -Reason "reviewed independent module; dependency mapping to my_module"
scripts\build.bat ready task-a -Batch <batchId> -Revision <editRevision>
scripts\build.bat status -Batch <batchId>
scripts\build.bat wait task-a -Batch <batchId> -Revision <editRevision>
```

示例测试名必须替换为 `scripts\test.bat list` 的实际名称。`ready` 原子标记就绪、记录输入并排队内置轻量检查，返回后由隐藏的独立等待进程接管，无需占住调用终端。重复调用不会重复开启同修订的存活等待者；所有参与者共用冻结批次结果。`wait` 只观察，不会替正在编辑的任务完成登记。兼容的阻塞 `finish` 在新批次也必须带修订号；推荐 `ready`。

等待阶段可只读审查接口、签名、夹具、测试名称、静态语法及已有可复用证据。内置 `check` 只做差异空白、变化的 PowerShell 和指定构建 JSON 语法及输出占用观察，不声称完成原生编译、接口语义审查或宿主 UI 验收。

```bat
scripts\build.bat check task-a -Batch <batchId> -Revision <editRevision>
scripts\build.bat begin task-a
rem begin 成功返回后才能修复文件；使用新 editRevision 重报 plan，再 ready
```

检查输入、编辑修订及 operationId 一起持久化。失败、中断、待执行或失效的检查挡住冻结；异常退出不会当作通过。`check` 可显式重试已退出检查的只读评估；需要写入时先 `begin` 原子重开。旧进程或旧修订命令无权标记新编辑完成；重开后旧计划与证据失效。检查结束与冻结都核对输入，范围中的内容变化需要重开/更新计划；不自动把编辑者的异常退出变成完成。

测试计划接受 `full/core/fast/selected/none` 与字面 CTest 名称，拒绝 shell/regex。未知影响、公共接口、基础设施和声明的构建/公共路径按规则升级为全量自动测试；全量排除 manual，显式声明的 manual 仍执行。独立模块允许选定测试；文档、独立组件或工具可按现有规则 `-Suites none`，必须有输入范围与审查/豁免理由；这会报告 `skipped/not-required`，不声称宿主通过。空集合、未知名称、失败、跳过或未运行不算通过。源码依赖关系仍需要人/Agent 审阅，脚本不自动推导 C++ 依赖。

冻结时合并、去重并持久化 `<batch>.plan.json`，之后不可改写；`coverage.json` 记录每个任务请求、实际覆盖、失败和执行前测试二进制 SHA-256。覆盖关联不是缺陷责任认定。源码摘要 v2 按文件内容计算，HEAD 作为元数据，不因只改变提交身份而失效。结果另记实际构建阶段及可观测输出二进制身份。端点身份检查不能排除构建途中改动后恢复、外部 SDK 或未登记写入者。

全部就绪后只读核对输出占用；没有明确授权则保留编辑批次、等待占用释放。当前执行者获得冻结和构建权后才使用显式 `ready/finish -ReloadShell` 授权，优先关闭已核对路径的应用，必要时重载确实占用输出的 Explorer。无法核对身份的进程不得终止。新批次不继承授权；普通 `begin/status/check` 不会停止应用。

Git 提交使用协作事务租约与明确认领的路径，不把其他任务的暂存文件混进提交，也不 reset/stash：

```bat
scripts\build.bat commit task-a -Batch <batchId> -Revision <editRevision> -Files src/my_module.cpp,tests/my_module_tests.cpp -MessageFile C:\Temp\task-a-commit.txt
scripts\build.bat issue task-a -Batch <batchId> -Reason "Alpha failed; reproduction and logs reviewed" -Assignee task-b
scripts\build.bat issue task-b -Batch <batchId> -IssueId <issueId> -IssueState deferred -Assignee task-b -Reason "repair in next batch; old result retained"
```

消息文件使用 UTF-8，并遵守双语提交规范。Git 租约只约束合作调用者；手动 Git、Git hook 自己的修改及同文件混合修改仍需协调。事务保存 parent/commit/实际路径和其他 index 条目稳定性；异常时保留证据，无回滚或覆盖用户修改。问题交接单独持久化，失败责任初始未分配；不得按测试请求者自动归责或重写冻结结果。

热升级不强制迁移活动 v1 批次。其 `begin/finish/status/recover` 合同继续保留，`ready/check/plan` 拒绝并要求下一批启用；已在运行的旧 PowerShell 使用其启动时加载的逻辑，无法被新文件追溯升级。旧结果只能按其证据查询，不充当新协议验证。不要删除活动登记强行换协议。

看板由协作入口自动启动，地址和手动停启详见 [本地看板](../tools/build-dashboard/README.md)。


### 本地等待、有限重试与失败续修

完成自身编辑后先 `ready`，后台按声明计划中的 `lightChecks: ["builtin-basic"]` 执行一次只读检查并等待屏障。
相同修订的活跃工作进程复用；没有变化时不重复检查、构建或调用模型。只读走查发现需修改时仍先 `begin` 原子重开。

需要等冻结批次结束、文件归属释放或其他编辑者关闭时，可登记一个持久化观察票据（安装好的 Python 3.8+）：

```bat
scripts\build.bat watch start task-A --condition window --timeout 1800
scripts\build.bat watch start task-A --condition files --files src/example.cpp --timeout 1800
scripts\build.bat watch start task-A --condition peers --batch BATCH --revision REVISION --timeout 1800
scripts\build.bat watch start task-A --condition result --batch BATCH --revision REVISION --timeout 1800
scripts\build.bat watch status --ticket TICKET
scripts\build.bat watch resume --ticket TICKET --timeout 1800 --reason "已诊断退出的只读等待进程"
scripts\build.bat watch cancel --ticket TICKET
```

`window/files` 可在新任务 `begin` 前调用；`peers/result` 必须使用已有登记的批次和修订。
票据观察每 2 秒在本地进行，不执行模型/API、检查、构建或编辑授权，默认半小时，最多一天。
`start` 的相同条件复用票据；仅在旧票据终止后显式 `--new` 才创建新票据。超时/退出不清活动登记。
结果先写不可变 `.build/collaboration/<ticket>.wait-attempt<N>.json`，再更新 `.wait.json`；进程身份和内核租约防止重复工作进程。
明确退出的等待进程需显式 `resume`；陈旧编辑者和检查永远不自动完成。未知占用、检查失败、构建退出或自身登记阻止归属释放时返回 `attention`。

票据 `eligible` 只表明曾观察到可操作窗口，恢复时必须重新 `begin/claim` 取得实际权限。
就绪任务的文件归属会保留到批次结束；不要让自己的 `editing` 登记一边阻止本批结束，一边等待其他就绪者释放文件。
同目录同文件仍需明确协调编辑顺序；未声明归属的活动任务无法自动证明文件可用。
传统 `begin/finish/wait -WaitSeconds N` 仍可作为一次长阻塞工具调用，进程内轮询不等于模型轮询。

对话无实质工作时可在登记 `ready/watch` 后结束当前回合，并交接任务、票据、下一步和未完成事项。
后续回合只读一次票据及结果再恢复操作；不要用“status → 短睡眠 → status”消耗模型调用。
本地完成、看板变化和持久结果不会自动唤醒 Work 对话；当前没有可信的本地事件回调入口。
Work 的回合结束通知只能提醒当前回合结束，不代表后续构建完成或任务已全部完成。

计划执行由 `build_batch_tests.ps1` 调用 `build_test_retry.py`：生产构建和首次测试运行只执行一次。
自动重试只白名单 `build_dashboard/build_dashboard_browser` 且要求 CTest `retry-isolated-resource` 标签，
并且该隔离用例在断言前已释放全部自建资源，写出绑定本次运行 token 的结构化 `isolated-port-race/exitCode 75` 信号。
目前只覆盖临时监听端口竞争；普通断言、编译错误、任意 timeout、77 跳过、缺报告、用户进程占用均不自动重试。
不会按日志关键词猜故障类别，也不关闭用户进程或叠加 CTest 的无限/全套重复。

每项最多 3 次（首次 + 2 次），退避 1/3 秒，总重试预算 300 秒。仅重跑合格失败项，保留原计划集合和分母。
源码/相关环境、测试二进制和工具指纹改变会中止，不能混用不同输入的通过记录。
首次报告、每次单项 JUnit/日志/不可变尝试记录以及汇总 `<batch>.retry.json` 都保留。
重试通过标 `passed-after-retry / flaky`；耗尽、未执行、环境阻断和输入失效分别显示，首次失败不会被抹掉。
中途崩溃后旧执行 ID 禁止自动重放（即使已完成尝试的汇总尚未落盘）；保留记录、诊断并进入新的续修尝试。
仅已审查的用例可以扩展白名单与结构化信号；标签或退出码自身不能授权任意测试重试。

失败结束一次执行尝试，不删除已保存成员、需求、冻结计划、输入、日志和责任交接：

```bat
scripts\build.bat status -Batch FAILED_BATCH
scripts\build.bat repair task-A -Batch FAILED_BATCH -Revision FAILED_REVISION -Reason "依据编译诊断续修"
rem 保存返回的新 batchId/editRevision，再 claim 文件、修改和 ready。
scripts\build.bat claim task-A -Batch CHILD_BATCH -Revision CHILD_REVISION -Files src/example.cpp
scripts\build.bat ready task-A -Batch CHILD_BATCH -Revision CHILD_REVISION
scripts\build.bat repair-abandon -Batch FAILED_BATCH -Reason "本次续修明确放弃；保留失败证据"
```

续修使用新的执行批次 ID，以 `logicalBatchId/repairOf/attempt` 关联同一逻辑轮次；并发 `repair` 原子复用同一子尝试。
原需求保留在 `originalRequirement`，其他成员无需重登原需求。旧修订、检查和测试通过不沿用：续修保守全量，
继承成员自动重新只读检查，修复者必须重新取得文件归属（旧归属不自动授权），冻结后统一重测当前输入。
重复 `repair` 不重开已就绪编辑者；进一步修复必须引用最新失败子尝试，不能覆盖祖先结果。
映射落盘中断时从当前 `repairOf` 对账；未知遗失状态拒绝创建重复尝试。无关活动批次不会被替换或清空，先登记本地窗口等待。
`repair-abandon` 只关闭没有活动续修登记的关联，不代替别人撤销活动登记；活动任务只可显式退出自己停止的编辑登记。
旧活动协议不强制迁移。失败归属仍通过 `issue` 保存证据，受影响覆盖不等于已确认缺陷责任。
