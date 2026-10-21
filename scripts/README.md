# 项目脚本

本目录统一保存开发、测试和发布入口及其实现：

普通 Release、Debug、测试及 IDE 包装入口先核对共享输出是否空闲，再提交持久 Run 并观察结果；退出观察方不会取消已提交执行。新协作批次冻结后同样提交一个 Run，所有等待方读取共同结果；活动旧协议批次保留原执行方式。独立启动、观察和显式取消仍可用 `build.bat run`：

```bat
scripts\build.bat task-open my-task -TaskName "当前调查任务"
scripts\build.bat run start -Task my-task -Action tests -Mode name -Filter build_run
scripts\build.bat run status -RunId <runId>
scripts\build.bat run wait -RunId <runId> -Seconds 30
scripts\build.bat run cancel -RunId <runId> -Task my-task
```

`start` 只提交一次执行，返回后工作进程继续持有自己的执行租约。请求先在同目录唯一临时文件中完整写入并刷新，关闭后再以不可覆盖的改名发布为 `.build/collaboration/runs/<id>.request.json`，后续不改写；发布前终止创建者不会留下可被最终文件名发现的半成品请求。发布完成仍须通过 Run 状态判断执行是否已开始。记录、日志、资源采样和带代际标记的取消请求分别保存。`wait` 最多等待 60 秒，未结束返回 10，紧急未确认消息返回 6；两者均保留运行。退出观察者不取消执行，只有显式 `cancel` 才请求终止本次私有 Job 内的进程树。取消请求和已完成取消分开报告。

启动必须获父 Job 允许脱离；拒绝时保留 `start-failed` 和原始请求，不退化成依附聊天的后台进程。执行者随后为编译和测试创建带 `KILL_ON_JOB_CLOSE` 的私有 Job，未继承执行租约或 Job 句柄；执行者异常退出会清理子进程。观察接口将已确认退出的执行者投影为 `interrupted`，保留最后记录和请求，不按心跳超时重新接管。继续执行需显式新建 Run。普通入口与协作入口均禁用 MSBuild 节点复用，避免旧工作节点逃出本次归属。

PowerShell 7 启动器通过原生进程 API 启动 Windows PowerShell 5.1 时，仅在子进程环境中移除继承的 `PSModulePath`，由 5.1 按自身规则重建默认模块路径，避免误载 7 版 Utility 等同名模块。父进程路径、普通 7 子进程及其他环境值保持原样；仅附加到当前 Core 进程的临时模块路径不会传给 5.1。此行为沿用 [Microsoft 对中间进程启动 Windows PowerShell 的说明](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.core/about/about_psmodulepath#starting-windows-powershell-from-powershell-7)。启动就绪握手、六秒初始化预算和业务开始后禁止重放的规则不变。

原生工具回归驱动直接启动 `powershell.exe` 时，也使用 5.1 自身的默认模块环境；夹具明确提供的 `PSModulePath` 覆盖仍保留，便于验证污染环境等失败路径。此隔离仅属于测试驱动，不修改用户或机器环境。启动超时与模块解析错误分别记录，不以模块路径检查通过代替所有初始化场景通过。

普通入口保持 busy 时立即拒绝、前台日志与原退出码；已提交后由工作进程重新取得执行权。没有显式 Task 的普通调用会创建 `entry-<id>` 调查登记，结果结束后关闭；显式 `run start` 仍须传入已登记的 Task。`build_entry.ps1 -Task ID` 可把标准入口绑定既有 Task；IDE 配置与目标、测试筛选均保存到不可变请求。观察期遇到未确认紧急事件返回 6，保留 Run。请求和日志路径会显示在终端。

`run status/wait` 的 `stageEvents` 读取同一 Run 下按进程分开的追加记录，保留单调时钟耗时、父阶段、真实退出码与日志路径。当前覆盖标准预检、入口管线，以及测试中的 configure/test-target-build/arrange/test-execute；编译与链接尚未独立采集，不按日志文字推算。阶段读取失败单独返回 `stageError`，不伪装成零耗时。CPU/内存/I/O Job 采样仍单独保存。

新 `begin` 创建协议 v3，v3 失败续修保留该协议；已有 v1/v2 批次不迁移。全部就绪后的冻结事务只保留一次 Run 预约，冻结计划以哈希绑定。独立执行器重新取得产物租约并接手 owner，验证预约、进程代际、计划和修订后执行；等待者退出不会回收该租约。启动被父 Job 拒绝时保留请求并公布共同失败，不回退前台执行。内部执行/发布接口只接受同一进程的真实 Run 所有者，环境变量或另一个调用方无法取得执行权。

批次执行先保存 `pending-result`，释放产物租约后再在状态事务中公布共同结果和退役；`publishedUtc` 与执行完成时间分别记录。共同结果带 `runId`，日志、资源和阶段沿用该 Run。执行者崩溃保留冻结状态与 Run 的 interrupted 观察，需核对真实进程后显式 recover；不因心跳陈旧或观察超时接管。取消仍须使用 Run 所属 Task，紧急消息让观察方返回 6，不取消实际执行。

工具、文档和组件 scope 的测试计划保留实际测试选择，不再因此调用完整宿主 build.bat；测试目标仍由 CTest 清单及真实依赖动态构建，是否触及宿主运行资源仍由对应检查决定。其他 scope 保留标准宿主构建需求。scope 与依赖映射必须如实声明，不能用 tool 规避实际宿主改动的验证。

尚未完成 codegen/compile/link、资源锁和编译单元计量、面板接入。资源采样缺失用不可用或 null 表示，不作为零占用或性能改善。此入口不等同于 B 批全部完成。

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
scripts\test.bat name "^widget_settings_service$"
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
`core` 只运行核心测试；`fast` 运行除 `integration` 外的测试；显式 `full` 运行完整自动测试集合。
无参数只显示用法，不配置、构建或执行测试；直接调用 PowerShell 测试入口而未指定模式时只列清单。
这些自动模式与对应 CTest 预设都排除 `manual` 手动诊断。Shell 文件操作集成测试已独立为手动诊断，
平时不执行，遇到相关文件操作或 Shell 阻塞问题时运行 `scripts\test.bat name "^shell_file_operation_worker$"`；
其预检回归仍自动运行。`name` / `label` 的显式筛选可以包含手动条目，`list` 会显示全部条目及标签。
直接使用 CTest 时，`tests` 是自动全量，`all-tests` 包含手动诊断；自动全量通过不代表手动条目已执行。
筛选参数是 CTest 正则表达式。跨模块、公共 API、异步和基础设施改动也按调用链与依赖选择相关项目；
普通交付、Pull Request 合入和缺少旧全量基线不自动要求全量。全量只用于最终发布候选、用户明确
要求，或有具体证据且经依赖分析仍无法界定充分定向集合的整体覆盖需求；启动前说明必要性。

`build_collaboration` 保留同一测试身份和全部断言，将 PowerShell 7 任务/事件、Windows PowerShell
任务/事件及协调器生命周期分为三个独立临时仓库并发执行。CTest 为它保留 9 个槽（每组 3 个），
不增加全局 24 槽预算，也不放宽原 180 秒超时；其他未声明预算的工具测试仍使用 3 槽。
各组输出单调时钟 `CASE` 计时与结果，并在任一组失败时等待其他组清理后返回失败。
已有匹配测试二进制可显式加 `--sequential` 作同输入串行诊断比较；该模式不改变标准 CTest 清单，
不计作新的通过或提速证据，性能结论仍需绑定实际输入、资源和多次可比测量。

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

## 阶段与资源观测

自动看板确保先核对本检出的实例回执和有界 loopback 健康响应；已匹配时不再探测或启动 Python。
没有看板管理入口的隔离夹具跳过这项可选能力，不影响基本协调。服务正常启动后记录本检出的
首选端口，后续写/执行入口沿用；主工作区和隔离检出可分别选择端口，status 保持只读。
身份不同、服务停止或偏好无效时不视作健康，不关闭未知监听者；管理和寿命边界见
`tools/build-dashboard/README.md`。

标准 Release、Debug、IDE 与测试入口复用 `build_command.ps1` / `build_telemetry.ps1`。
配置、目标构建、整理和测试执行分别记录单调时钟区间；Run 的排队与管线也有独立区间。
首次观察到 shared-output busy 至取得执行权或原因变化的区间保存在 `waitIntervals`，包含重试和退避，
不冒充操作系统锁内部的阻塞时间。观察者关闭仍不取消 Run。
普通 Run 持有输出租约期间，`begin` 与失败续修也不能新建或重开编辑授权；调查登记继续可用。
被拒请求没有修改源文件或撤销任务，运行释放租约后重新调用原请求。此检查与普通入口的租约获取共用
短时元数据事务，保持“有编辑者时不开始执行”和“执行中不授予编辑权”两个方向一致。

Visual Studio 生成器从该输出的 `CMakeCache.txt` 确定实际 VS 实例，使用其 Framework 程序集和本机
Framework C# 编译器生成 `build_msbuild_logger.cs`。缓存绑定源码、引用、编译器及 DLL 哈希；不下载工具。
每个 MSBuild logger 初始化都有独立的 `<run>.msbuild.*.jsonl`，避免 CMake 多目标重用参数造成覆盖。
事件只保留任务身份、项目名、区间和计数，不持久化完整命令行、环境或任意任务参数。

CL/Link/Lib、XAML 及自定义构建分别显示真实任务区间。`elapsedSeconds` 使用中央 logger 的单调事件接收计时，
`sourceUtc/sourceElapsedSeconds` 另保留源事件时间；并行区间和各层管线不能直接相加当作墙钟总时长。
编译数为实际 CL 命令中已发起的源文件单元，支持有界响应文件展开；不以项目声明的候选 Sources 数代替。
失败编译仍属于已发起单元，未知投影、未结束任务、未采集或达到预算分别保留，不能显示成成功或零工作。

Job CPU、峰值 Job 内存、进程与 I/O 来自该 Run 私有 Job 的采样。网页和 DSH 悬窗通过同一只读快照显示
普通 Run 与批次关联 Run、等待、阶段、资源和日志。记录先验证请求/结果的根目录、ID、nonce；不公开执行凭据。

### 测试证据与人工验收

批次结果的 `evidence.tests` 逐条保存测试结果、原始尝试/报告哈希、请求任务、候选输入、测试二进制、
CTest 命令/属性摘要和执行环境摘要。采集在目标生成后、测试执行前完成；缺少数据保持 incomplete。
输入修正后的补验属于新的尝试，原失败记录不覆盖。`build.bat evidence-list ID -Batch B` 只读查询。
当前环境摘要绑定已解析工具、缓存/预设及明确环境字段，但外部 SDK 和语义依赖尚未闭合，
`reuseAllowed=false`，不能据此自动跨候选省略测试；普通独立 Run 的逐条索引和去重仍待接入。

人工验收独立于构建与测试。先用 `task-list` 读取当前任务修订，然后登记：
两个写命令均须使用最新 `TaskRevision`；未结束的验收须先完成或取消，再关闭 Task。

```bat
scripts\build.bat acceptance-start ID -Batch B -Revision R -TaskRevision T -AcceptanceCriterion "原始样本出现预期结果" -Files .build/Release/SnowDesktop.exe
scripts\build.bat acceptance-list ID -Batch B
scripts\build.bat acceptance-complete ID -TaskRevision T -AcceptanceId A -AcceptanceOutcome passed -Reporter user -Reference "实际反馈或证据位置" -Notes "原始样本、观察步骤和结果"
```

开始时要求候选已通过且输入稳定、任务需求修订一致，运行文件须属于该候选的输入、已记录构建输出或测试输出。
旧格式缺少需求关联、未验证的现有输出以及路径链接均不推断为可验收候选。文件关联表示可核对的文件身份，
实际进程加载与可见行为仍由观察和引用证明，不能仅凭文件哈希宣称实机通过。
完成时重新核对输入、产物与需求修订；变化后不能登记 pass，可如实记录 failed/cancelled，再准备新候选。
反馈来源为 user 或 agent，须记录真实观察；已完成记录不可改写，结果不改动原构建/测试结论，也不表示发布。
区间使用同一系统启动周期内的 QPC 单调计数；UTC 只用于显示，跨重启或无法取得时钟身份时明确未记录耗时。
参见 [Windows 高精度计时说明](https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps)。
不补造既往验收的开始时间。只读看板与悬浮窗分别展示逐条测试证据和人工验收，不把两者相加为测试通过数。
观测文件每 Run 的读取预算为 4 MiB / 100 文件，超限保留原始记录并报告不可用；界面任务区间只显示最近80项，标注省略数。
观测能力本身不构成提速证明，性能比较仍需相同输入、工具链、缓存和外部负载下的独立基线。

## 耦合运行性能调试

原生 MSVC 构建默认复用 WebP SIMD 编译探测。缓存绑定编译/链接工具、配置及选项、环境中的工具链身份和
探测头文件及构建规则内容；默认与附加标志分别记录。头文件或编译参数变化会重新编译探测程序。自定义工具链、
检查模块、标准包含目录、导入库或额外链接配置保留上游检查。`-DSNOWDESKTOP_CACHE_WEBP_PROBES=OFF` 可用于同输入的无缓存对照。
这只省去重复的能力探测，不跳过 CMake 配置、共享执行权检查或测试选择。

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

测试计划接受 `full/core/fast/tools/selected/none` 与字面 CTest 名称，拒绝 shell/regex。所有 scope 均可按实际依赖选择相关项目，`unknown/public/infrastructure` 不自动升级全量；文件夹名称本身不是依赖分析。每份计划必须有 `Inputs` 和 `Reason`，说明依赖映射或豁免依据；全量须显式声明 `-Suites full` 并在理由中说明符合 AGENTS 的整体覆盖需求。未声明计划的 ready/finish 会保留 editing 并报错，遗漏 Suites 默认 selected 且必须提供 Tests，不启动全量。`full/core/fast` 排除 tools 和 manual；工具回归用显式测试名或确有必要的 `-Suites tools` 请求，显式声明的 manual 仍执行。文档、独立组件或工具可按现有规则 `-Suites none`；这会报告 `skipped/not-required`，不声称宿主通过。空集合、未知名称、失败、跳过或未运行不算通过。源码依赖关系与全量必要性仍需要人/Agent 审阅，脚本不自动推导 C++ 依赖或判断自然语言理由。

冻结时合并、去重并持久化 `<batch>.plan.json`，之后不可改写；`coverage.json` 记录每个任务请求、实际覆盖、失败和执行前测试二进制 SHA-256。覆盖关联不是缺陷责任认定。源码摘要 v2 按文件内容计算，HEAD 作为元数据，不因只改变提交身份而失效。结果另记实际构建阶段及可观测输出二进制身份。端点身份检查不能排除构建途中改动后恢复、外部 SDK 或未登记写入者。

全部就绪后只读核对输出占用；未请求 ReloadShell 则保留编辑批次等待释放。按 AGENTS 允许的既有授权执行前告知副作用并声明 ReloadShell，无需重复确认。当前执行者获得冻结和构建权后才使用显式 `ready/finish -ReloadShell` 授权，优先关闭已核对路径的应用，必要时重载确实占用输出的 Explorer。无法核对身份的进程不得终止。新批次不继承授权；普通 `begin/status/check` 不会停止应用。

Git 提交使用协作事务租约与明确认领的路径，不把其他任务的暂存文件混进提交，也不 reset/stash：

```bat
scripts\build.bat commit task-a -Batch <batchId> -Revision <editRevision> -Files src/my_module.cpp,tests/my_module_tests.cpp -MessageFile C:\Temp\task-a-commit.txt
scripts\build.bat issue task-a -Batch <batchId> -Reason "Alpha failed; reproduction and logs reviewed" -Assignee task-b
scripts\build.bat issue task-b -Batch <batchId> -IssueId <issueId> -IssueState deferred -Assignee task-b -Reason "repair in next batch; old result retained"
```

消息文件使用 UTF-8，并遵守双语提交规范。Git 租约只约束合作调用者；手动 Git、Git hook 自己的修改及同文件混合修改仍需协调。事务保存 parent/commit/实际路径和其他 index 条目稳定性；异常时保留证据，无回滚或覆盖用户修改。问题交接单独持久化，失败责任初始未分配；不得按测试请求者自动归责或重写冻结结果。

旧 v1 批次仍可只读查询和显式恢复，`ready/check/plan` 拒绝并要求下一批启用。未冻结且缺少计划的旧登记也不会默认全量；确认所属操作已停止后，用既有 recover 流程保留证据并退出登记，再在新 v2 批次声明计划。已在运行的旧 PowerShell 使用启动时加载的逻辑，无法被新文件追溯升级。旧结果只能按其证据查询，不充当新协议验证；不要删除活动登记强行换协议。

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
原需求保留在 `originalRequirement`，其他成员无需重登原需求；每个成员保留 scope/suites/tests/inputs/reason（包括 none 和显式 manual），合并去重。repairOf 和未知/公共/基础设施 scope 均不强制 full；仅保留符合明确触发条件的显式 full 需求，历史 scope 推导的 requiredFull 标记不再升级选择范围。修复者按新输入重报实际依赖与失败/受影响测试，不能凭关联身份伪称旧覆盖通过。旧修订、检查和测试通过不沿用：
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

### 调查登记与协作事件

调查与编辑权使用两个独立修订；调查不会加入构建屏障，也不会由超时自动结束。

```bat
scripts\build.bat task-open task-unique -TaskName "调查构建等待" -Adapter dsh
scripts\build.bat task-stage task-unique -TaskRevision 0 -TaskPhase investigating -Progress "已找到等待入口" -RequirementsRevision 1
scripts\build.bat task-list
scripts\build.bat begin task-unique
scripts\build.bat claim task-unique -Batch BATCH -Revision EDIT_REVISION -Files scripts/example.ps1
scripts\build.bat edit-verify task-unique -Batch BATCH -Revision EDIT_REVISION -Files scripts/example.ps1
```

`task-open` 对未关闭 ID 幂等；每个任务用独立 ID。`task-stage` 支持 investigating/editing/waiting/awaiting_input/blocked/validating，进展为任务自身报告，不能作为编辑授权或验证证据。阶段变化和活动期间不超过 60 秒报告有意义进展。Task 与事件保存在现有 state.json 的 taskLedger 中，沿用短时原子事务，不维护另一套调度状态。`begin` 对旧调用补录，来源为 begin-compatibility，不伪造调查起点。批次结束后显式 `task-close ID -TaskRevision N -TaskPhase completed/cancelled`；关闭不能清除活动编辑登记。

```bat
scripts\build.bat event-post task-unique -Target target-task -EventKey requirement-unique -EventKind requirements -Importance urgent -Text "需求已更新，请复核" -RequirementsRevision 2
scripts\build.bat event-read target-task -AfterSequence 0
scripts\build.bat event-wait target-task -AfterSequence 0 -WaitSeconds 30
scripts\build.bat event-ack target-task -Sequence 1 -AckLevel adapter
scripts\build.bat event-ack target-task -Sequence 1 -AckLevel agent
```

消息以 EventKey 幂等，重用键但改内容会拒绝；Target 为 `*` 时固定投递给当时已登记且未关闭的任务，后来加入者不追收旧广播。事件写入、适配器收到、智能体确认分别记录。read/wait 不代替确认；单次返回至多 100 条，续读以最后实际返回的事件 sequence 为游标，不用顶层高水位跳过后续页。event-wait 默认最多等待 30 秒，超时返回空事件和 timedOut，不取消任务或构建。finish/wait 在等待阶段收到未确认紧急事件时返回 6，保留编辑登记和批次；执行中的编译不会因观察者退出而在此接口里取消。只在安全等待边界检查事件，持久 Run 的独立执行在后续批次实现。

任务上限 2000、事件上限 4096，达到上限明确拒绝新增，保留全部未消费消息；不静默淘汰。需要通过审阅后的迁移归档。当前 CLI 能力声明不含 DSH 写入拦截或 idleWake；DSH 插件会通过宿主能力接入，悬浮窗读取同一接口，网页保持只读。普通编辑器和任意 shell 写入不能被单独的 edit-verify 校验强制拦截。

### 运行时依赖

持久 Run 仅将 Windows 共享/锁冲突（32/33，包括 PowerShell 包装的异常）识别为短暂占用。
批次请求预留状态事务最多等待 30 秒；执行器入场时保留队列与取消能力。其他 I/O 或权限错误直接报错，
不会因异常包装误判为排队，也不会因一次短暂状态读取而使整批失败。

基础 plan/claim/ready-and-wait/status/repair 只需 PowerShell，不需要安装 Python。
所有批处理入口、CMake 和构建子进程优先使用 PowerShell 7，未安装时回退系统 Windows PowerShell 5.1。
`SNOWDESKTOP_ENTRY_POWERSHELL` 保存该次执行所选的绝对路径，子进程沿用它；可显式设为系统 5.1 路径做兼容检查。
进程启动前传入 `PSExecutionPolicyPreference=Bypass`，替代重复的 `-ExecutionPolicy Bypass` 启动参数，
保持原有进程级策略，不修改用户或计算机的持久化策略，也不改变组策略优先级。
协调器与看板共用一个 PowerShell 进程；后台等待和工具回归的启动握手在目标脚本执行前确认就绪。
后台等待只继承自己的输入与日志文件句柄，不继承父命令的输出管道或协作锁句柄；父命令返回后等待进程仍可继续。
六秒内未就绪或启动失败时最多再尝试一次 5.1，保留首次输出、PID、运行时和 JSON 证据；
未发放执行许可的子进程不能运行目标脚本。已开始的目标超时或非零退出不会重放；
Python 入口的总超时包含两次启动和目标执行，不通过扩大原有测试预算掩盖失败。
可选重试增强对 Python 做最多五秒的可执行/版本探测（3.8+）；不存在、别名损坏或版本不足时，
在尚未开始执行前仅回退一次原测试计划。已经开始的执行失败不自动重放，旧失败/尝试与 flaky 标记保留。
票据和语言逐键事务需要 Python；缺少时使用前台 finish/ready-and-wait 和串行整文件 claim，
不能冒充增强功能可用或绕过同键/冻结保护。无自动安装。

测试驱动、流程和验收断言统一由 C++ 原生测试程序执行，清单由 CMake 的 `SnowDesktopTests` 聚合目标提供。
`tests/` 不包含 Python 测试脚本，CMake 配置也不探测 Python 或因为解释器缺失省略测试。
被测的看板、等待、重试和共享资源工具保留原实现语言；这些集成场景仍需要对应的生产工具运行时，
缺少依赖会明确失败，不计为通过。PowerShell 协议夹具只传递参数和观测数据，断言留在 C++ 中。
`manual` 浏览器诊断仅在显式选择时成为要求。

`scripts/test.bat` 的 full、core、fast、tools 和 name/label 选择使用共同 CTest 预设，
并行度统一配置在 `CMakePresets.json` 的 `all-tests.execution.jobs` 中，不在说明中维护另一份固定值。
各测试使用独立临时目录、日志和端口；共享桌面、Explorer 或宿主运行资源的条目通过
`RESOURCE_LOCK snowdesktop_desktop` 互斥。CTest 会在锁占用期间调度其他独立测试，无需串行运行整套测试。

无参数只显示用法；显式 `full` 只运行应用产品测试，`core` 和 `fast` 也排除 `tools` 标签。
协作构建、计划、筛选、PowerShell 启动、等待、重试、共享资源和看板通过 `scripts/test.bat tools`
单独回归，默认排除工具的 manual 诊断。`name`/`label` 可显式选择任何组和手动条目。
`SnowDesktopTests` 仍保存统一原生目标清单，应用、工具、核心和快速聚合从声明标签派生：
常规测试使用 `SnowDesktopAppTests`，工具回归使用 `SnowDesktopBuildToolTests`；
协作计划合并应用与显式工具需求时使用完整聚合，不让常规 full 隐式加入工具用例。

`ready-and-wait` 两种依赖环境下直接返回共同 batch.exitCode：通过/明确豁免 0，失败原码，
用法/状态/检查 attention/等待超时 2，恢复出的取消/中断 4，输入失效 5。
观察 `watch wait` 的 eligible/completed=0 仅表示观察条件满足，其他观察结论=2；result 内仍保留
原始 batch.exitCode，它不是验证入口的通过结论。WaitSeconds 仅限制等待，不取消存活构建；
pipeline 没有新增执行预算。未知存活身份需显式诊断，不抢占或清登记。
