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
避免直接占满开发机。可按内存和其他任务负载，在实际使用的 `CMakePresets.json` preset 的
`cacheVariables` 中声明 `SNOWDESKTOP_COMPILE_JOBS`，再走受保护的标准入口；修改配置输入仍须
登记/claim。共享输出不直接运行未持租约的 `cmake --preset`；诊断参数实验使用独立输出目录。

Debug 构建使用 debug preset 和 `scripts\build_debug.bat`。项目数与项目内编译数可能相乘，
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

## 本地执行权与通用终端

普通 Release、Debug、测试入口（包括会先配置的 `list`）和
`powershell.exe -NoProfile -File scripts/build_entry.ps1 -Action ide [-Configuration Debug] [-Targets target]`
共用本地 `state.lock/build.lock`。先取得执行权，再检查输出占用、配置、编译或整理；活动 editing、
冻结批次或独占产物租约存在时返回 2，带批次/阶段/参与者摘要，不停止应用。
协调器冻结后的子进程使用绑定存活 owner、启动时间、进程祖先、批次身份及独占租约的凭据。
任意设置环境变量、旧凭据或另一进程的凭据均不能绕过；普通入口期间新 begin 也不能登记编辑。
凭据不是对同一用户恶意改写状态的安全边界。子进程只在本入口的私有 Windows Job 内运行，
入口退出会收尾其自建进程，不能连带用户应用。

CMake 配置和重新生成的原生 IDE 目标对 `.build/.build_debug` 检查上述凭据；共享目录的 IDE 外部
构建命令用 `build_entry.ps1 -Action ide`。此前已生成且未重新配置的项目不含新检查，不能被热升级
追溯保护。直接 CMake 的独立输出目录仍可用于诊断，不能冒充标准 `scripts/build.bat` 发布验证，
也不能自行写入共享 runtime/data。已经运行的旧脚本同样不会追溯升级；接入前核对旧构建已停止。
CI 使用相同入口，独立 checkout 不依赖 Codex。

流程不需要 Codex 会话 ID、消息工具或唤醒 API。CMD、PowerShell 5.1、pwsh、IDE、其他 Agent 和 CI
读取同一份本地 JSON、日志、覆盖和退出码。`status` 是无创建目录/锁/服务的原子只读观察，
缺少状态输出无活动批次，损坏/不可读状态明确失败；执行记录可能是历史，不能当作活跃证明。
看板为本机独立只读服务；写/执行协作入口沿用自动确保策略，`status` 不启动它。
自动确保也先探测 Python，探测最多五秒、看板启动助手最多十二秒；不可用时基础协调继续。
watch/resource 不可用时返回 2 并给出基础 PowerShell 路径，不启动不可用别名或假报增强已执行。

当前状态统一读协调器。无需发消息询问谁开了批次、复述已有计划/哈希或确认不变状态；仅具体
文件冲突、依赖改变、失败责任交接或需要决策时通信，注明 batch/revision/issue 和新证据。
这个规则减少可省往返，不禁止 Agent 自由发言；已有样本不足以证明消息风暴。无通信工具时
只持久化 `issue` 供人工读取，不声称已通知，也不等待无法发生的 API 唤醒。

异常恢复先检查状态、日志和差异，确认指定编辑者/执行者停止后用
`recover ID -Batch B -ConfirmStopped -Reason ...`；冻结批次恢复省略 ID。
活/未知 owner 或仍持租约均拒绝恢复。超时/陈旧年龄不自动完成、清登记、抢锁或重跑。
不可变 `<batch>.json`、计划、输入、覆盖、日志保留在 `.build/collaboration`。
结果先持久化再退役；中断收尾用显式 recover，不覆盖结果。

v1 活动批次继续使用原 `begin/finish/status/recover` 合同；v2 必须带最新修订号，见下文。
不删除活动登记迁移。`status -Batch` 仅是历史观察；`finish/wait` 复用通过还核对内容摘要和已记录
二进制。源码起止摘要只发现端点差异，不能证明中途没有改后恢复或外部 SDK/缓存没有变化。
无需编译的定向补测可以直接运行匹配的既有程序，先确认输入/产物对应并隔离日志与外部资源；
不能用默认先配置/构建的 test 入口代替纯执行补测。

## 耦合运行性能调试

`profile.bat` 是默认关闭的性能采集入口。`status` 查询当前宿主能力，
`capture -Seconds 60` 采集并生成 JSON/CSV，`start` / `stop -Session ...`
支持异步自动化控制，`report` / `compare` 支持离线分析。

## 协作协议 v2：登记、计划与阻塞等待

`begin` 返回 `batchId` 与 `editRevision`。开始写文件前登记；各对话使用独立 ID。
`claim` 用同一状态锁检查路径及父目录冲突，不会解决同文件的修改合并。已有脏文件必须先审阅，再显式 `-AdoptExistingChanges`；没有声明的老参与者会显示为未确定所有权。

```bat
scripts\build.bat begin task-a
scripts\build.bat claim task-a -Batch <batchId> -Revision <editRevision> -Files src/my_module.cpp,tests/my_module_tests.cpp
scripts\build.bat plan task-a -Batch <batchId> -Revision <editRevision> -Scope module -Suites selected -Tests my_module -Inputs src/my_module.cpp,tests/my_module_tests.cpp -Reason "reviewed independent module; dependency mapping to my_module"
scripts\build.bat ready-and-wait task-a -Batch <batchId> -Revision <editRevision>
scripts\build.bat status -Batch <batchId>
scripts\build.bat wait task-a -Batch <batchId> -Revision <editRevision>
```

When only closing SnowDesktop is authorized, use `scripts\build.bat --close-application`
or append `-CloseApplication` to `ready-and-wait`. The execution owner closes only
identified application processes. Existing or newly observed Explorer Hook owners,
and unknown owners, stop the build; this option never terminates or restarts Explorer.
It is mutually exclusive with `--reload-shell` / `-ReloadShell`.

示例测试名必须替换为 `scripts\test.bat list` 的实际名称。`ready-and-wait` 默认在原工具进程中标记就绪、执行计划中的轻量检查、等待屏障和共同结果；不依赖 Python 或隐藏后台启动。工具 yield 后续等同一 session。`wait` 仅观察；兼容 `finish -AutoCheck` 使用同一执行路径。`ready` 仅为明确选择的后台模式：同修订存活 worker 复用，启动失败保留 pending 登记、报告 unavailable 和前台接管命令，不假报已运行。Windows PowerShell 路径按系统目录解析，pwsh 调用可用。

等待阶段可只读审查接口、签名、夹具、测试名称、静态语法及已有可复用证据。内置 `check` 只做差异空白、变化的 PowerShell 和指定构建 JSON 语法及输出占用观察，不声称完成原生编译、接口语义审查或宿主 UI 验收。

```bat
scripts\build.bat check task-a -Batch <batchId> -Revision <editRevision>
scripts\build.bat begin task-a
rem begin 成功返回后才能修复文件；使用新 editRevision 重报 plan，再 ready
```

检查输入、编辑修订及 operationId 一起持久化。失败、中断、待执行或失效的检查挡住冻结；异常退出不会当作通过。`check` 可显式重试已退出检查的只读评估；需要写入时先 `begin` 原子重开。旧进程或旧修订命令无权标记新编辑完成；重开后旧计划与证据失效。检查结束与冻结都核对输入，范围中的内容变化需要重开/更新计划；不自动把编辑者的异常退出变成完成。

测试计划接受 `full/core/fast/selected/none` 与字面 CTest 名称，拒绝 shell/regex。未知影响、公共接口和基础设施 scope 按规则升级为全量自动测试；文件夹名称本身不是依赖分析；全量排除 manual，显式声明的 manual 仍执行。独立模块允许选定测试；文档、独立组件或工具可按现有规则 `-Suites none`，必须有输入范围与审查/豁免理由；这会报告 `skipped/not-required`，不声称宿主通过。空集合、未知名称、失败、跳过或未运行不算通过。源码依赖关系仍需要人/Agent 审阅，脚本不自动推导 C++ 依赖。

冻结时合并、去重并持久化 `<batch>.plan.json`，之后不可改写；`coverage.json` 记录每个任务请求、实际覆盖、失败和执行前测试二进制 SHA-256。覆盖关联不是缺陷责任认定。源码摘要 v2 按文件内容计算，HEAD 作为元数据，不因只改变提交身份而失效。结果另记实际构建阶段及可观测输出二进制身份。端点身份检查不能排除构建途中改动后恢复、外部 SDK 或未登记写入者。

全部就绪后只读核对输出占用；未请求 ReloadShell 则保留编辑批次等待释放。按 AGENTS 允许的既有授权执行前告知副作用并声明 ReloadShell，无需重复确认。当前执行者获得冻结和构建权后才使用显式 `ready/finish -ReloadShell` 授权，优先关闭已核对路径的应用，必要时重载确实占用输出的 Explorer。无法核对身份的进程不得终止。新批次不继承授权；普通 `begin/status/check` 不会停止应用。

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

完成自身编辑后默认 `ready-and-wait`，前台按声明计划中的 `lightChecks: ["builtin-basic"]` 执行一次只读检查并等待屏障。
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

默认保持对话活动，执行阻塞 `ready-and-wait/watch wait`；脚本等待不重复向模型返回。工具因时限 yield 时继续同一 session，不重新启动 status 命令。后台结束回合仅是明确选择无人值守时的选项。
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

完整计划还必须完成输出隔离检查点。coverage 的可选 `postChecks.outputIsolation` 记录
`not-run/passed/failed`、开始/完成时间、实际退出码和错误；初次 configure 前即写 `not-run`。
首次 CTest 失败时未执行的检查点，在合格失败项补测通过后单独执行一次，不重新配置、编译、
整理输出或运行 CTest。已失败的检查点不会自动重跑，也不会被测试重试通过覆盖；旧完整 coverage
缺少此字段时按未执行处理。检查点前后仍核对源码、工具和测试产物指纹，失效时不能声称全量通过。
任务的可选 `requiresOutputIsolation` 区分完整请求和同批的定向请求；现有字段和 CLI 保持兼容。

依赖宿主运行目录的 CTest 条目以 `host-runtime` 标签声明；定向入口从实际选择清单推导占用预检
和构建后整理，不维护另一份目标名列表。旧清单仍可通过 command/REQUIRED_FILES 中的宿主绝对路径识别。
显式 Shell 重载由持有外层执行租约的 owner 在创建私有构建 Job 前处理，等待方不执行进程操作。
恢复的 Explorer 不继承私有执行凭据；构建子进程仍受原子的 Job 分配和取消/崩溃时 kill-on-close 约束。
预检失败的关联日志和独立 `.preflight.json` 保留原始错误及退出码，成功后的凭据恢复不依赖 Job 日志前缀。

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
原需求保留在 `originalRequirement`，其他成员无需重登原需求；每个成员保留 scope/suites/tests/inputs/reason（包括 none 和显式 manual），合并去重。repairOf 本身不强制 full；未知/公共/基础设施或原有 full 要求仍全量。修复者按新输入重报实际依赖与失败/受影响测试，不能凭关联身份伪称旧覆盖通过。旧修订、检查和测试通过不沿用：
继承成员重新只读检查；后台不可用时前台完成全部 pending 成员的检查，修复者必须重新取得文件归属（旧归属不自动授权），冻结后统一重测当前输入。
重复 `repair` 不重开已就绪编辑者；进一步修复必须引用最新失败子尝试，不能覆盖祖先结果。
映射落盘中断时从当前 `repairOf` 对账；未知遗失状态拒绝创建重复尝试。无关活动批次不会被替换或清空，先登记本地窗口等待。
`repair-abandon` 只关闭没有活动续修登记的关联，不代替别人撤销活动登记；活动任务只可显式退出自己停止的编辑登记。
旧活动协议不强制迁移。失败归属仍通过 `issue` 保存证据，受影响覆盖不等于已确认缺陷责任。


### 默认阻塞等待与共享语言条目

```bat
scripts\build.bat ready-and-wait task-A -Batch BATCH -Revision REVISION -WaitSeconds 1800
scripts\build.bat watch wait task-A --condition window --timeout 1800
scripts\build.bat watch wait task-A --condition files --files src/example.cpp --timeout 1800
scripts\build.bat watch wait --ticket TICKET
```

默认保持对话活动，执行阻塞 `ready-and-wait/watch wait`；脚本等待不重复向模型返回。工具因时限 yield 时继续同一 session，不重新启动 status 命令。后台结束回合仅是明确选择无人值守时的选项。

完成者的文件声明保留为来源追溯；新任务可在活动编辑窗口 claim 接手，仍在编辑的代码文件需排队。接手只读检查在全体编辑结束后自动重验，原测试要求保留，不需要原会话修改文件。原会话旧轮次或 finished 状态不能再次写入。

明确豁免清单只有 `scripts/shared_resources.json` 中列出的十个 `lang/*.json`；语言条目使用以下安全路径，无需原对话同意：

```bat
scripts\build.bat resource prepare task-A --batch BATCH --revision REVISION --file lang/zh-CN.json --keys font.title,font.family
rem 编辑返回的 patchFile，仅填写这些键的新字符串；预期旧值保存在独立登记中。
scripts\build.bat resource apply task-A --batch BATCH --revision REVISION --request REQUEST_ID
scripts\build.bat resource status --request REQUEST_ID
```

prepare 默认有效十五分钟（`--timeout` 至多一小时）。apply 持有短时 state/Git 事务锁，重读最新文件，仅比较已声明键的旧值；不同键合并保留，相同键旧值改变拒绝，忙时重复同一 apply 请求排队。保留原键顺序、无关内容、BOM 和换行，支持新增字符串键；每个受支持语言仍需真实翻译。成功请求可重复读取同一结果；中途崩溃留写入意图，不重放不确定写入。token 过期、旧轮次、finished 编辑者和冻结批次均拒绝。准备/写入不授予任意代码文件豁免，也不撤销活跃代码声明。

共享文件禁止从陈旧副本整文件写回；所有协作写入须走上述事务。整文件重排、格式转换或结构调整用 `claim -Files lang` 串行编辑，并先等待其他活动语言编辑结束。脚本不能拦截绕过规则的普通编辑器，未经协作接口的同时保存仍须人工解决。

### 缺少依赖与退出码

基础 plan/claim/ready-and-wait/status/repair 只需 Windows PowerShell，不需要安装 Python。
可选重试增强对 Python 做最多五秒的可执行/版本探测（3.8+）；不存在、别名损坏或版本不足时，
在尚未开始执行前仅回退一次原测试计划。已经开始的执行失败不自动重放，旧失败/尝试与 flaky 标记保留。
票据和语言逐键事务需要 Python；缺少时使用前台 finish/ready-and-wait 和串行整文件 claim，
不能冒充增强功能可用或绕过同键/冻结保护。无自动安装。

CMake 始终声明六项自动工具回归和一项 manual 浏览器回归。缺少或不可用 Python 时仍声明条目，
标记 environment-blocked，实际执行返回 78；不从 inventory 静默消失。test_manager 冻结计划覆盖
记录所需条目、阻断和未执行，在编译前阻断不完整的选择；独立原生选择仍可运行。
恢复解释器后需新执行证据，旧 blocked 不能充当通过。显式 manual 才成为要求。

`ready-and-wait` 两种依赖环境下直接返回共同 batch.exitCode：通过/明确豁免 0，失败原码，
用法/状态/检查 attention/等待超时 2，恢复出的取消/中断 4，输入失效 5。
观察 `watch wait` 的 eligible/completed=0 仅表示观察条件满足，其他观察结论=2；result 内仍保留
原始 batch.exitCode，它不是验证入口的通过结论。WaitSeconds 仅限制等待，不取消存活构建；
pipeline 没有新增执行预算。未知存活身份需显式诊断，不抢占或清登记。
