# SnowDesktop DSH 协作插件

此目录是独立 Cordis bundle。它不修改 DSH 宿主源码或安装文件，也不手写 profile 的 package.json/cordis.patch.yml。插件 `0.2.23` 的三个 DSH peer 依赖精确对齐本机 DSH `0.2.1-rc.5`，插件、工具、文件写入与 Typert API 通过该版本随包的真实 SDK 集成测试核对；其他版本须重新核对接口。

插件在配置的 SnowDesktop 仓库内登记调查任务；DSH 的 `tools/execute` 在标准 `write` / `edit` 执行前校验当前批次、编辑轮次和路径归属，再交由宿主执行观察版本、沙箱和文件修改检查。`snow_collaboration` 工具提供 open/status/progress/begin/claim/plan/messages/wait-messages/acknowledge。任务登记、事件及编辑授权仍保存在现有协调器中。

协作页注册到 DSH 的 `conversation.view`，使用宿主 React、语言和主题，顺序位于 chat（0）与 trajectory（10）之后（20）。旧版位置、尺寸和收起偏好不再生效，活动、轮次、筛选和展开偏好继续保留。导航用蓝色，执行、等待和结果使用宿主语义色；强调色按宿主实际 `color-scheme` 选择，手动主题切换不依赖系统外观偏好。不加载 iframe，不在浏览器访问其他 localhost 服务，不提供执行或确认按钮。Host 读取前核对本仓库服务回执中的实例 nonce，拒绝模拟服务、错误监听者和重定向。

信息以当前批次和参与者为先，明确等待哪项编辑或检查，区分数据读取时间与任务自行报告的进展时间。其余最近五分钟没有活动更新的登记默认折叠并显示最后报告时间，不据此撤回登记或推断任务已结束。尚未冻结测试计划时显示原因，不把零计数当作通过；有实际选择后区分执行数量、失败、未运行及尚待报告的结果。紧急消息区分适配器收到和智能体确认；断线保留上次快照并标记旧数据。

- 总览：批次、阶段、输入、待处理事项、最新进展、等待票据及恢复入口。
- 任务：按文字与状态筛选本批参与者和调查登记，展开文件声明、验证计划、轻量检查、修订和宿主能力。
- 验证：执行时间、所有者、测试结果和耗时、请求覆盖、重试与每次重试日志、输入/产物证据、问题交接和续修关系。
- 日志：文字筛选、错误/警告筛选、暂停/恢复跟随，以及尾部截断与字节范围说明。
- 消息：全部协作事件、搜索、待确认紧急消息筛选、适配器和智能体确认计数。
- 历史：批次搜索、结果筛选、打开历史记录查看其他标签页的完整明细，再返回当前批次。

原有无参 `snowCollaboration.snapshot` 不变。新增 `snowCollaboration.read(route)` 仅允许 `/api/status`、固定 32 位小写十六进制批次详情，以及网页已有的两种看板测试第 2/3 次重试日志。Host 在 HTTP 请求前拒绝任意 URL、查询串、路径跳转和其他文件；每次读取仍检查本仓库服务身份，不新增写权限。界面展示范围沿用网页服务的有界记录和日志尾部，不声称加载了服务未提供的历史或完整日志。DSH 兼容版本为本机 `0.2.1-rc.5`；需要正常更新插件以加载新增 Host Remote。

## 安装

先确认 cordis.patch.yml 的 repositoryRoot 指向目标仓库。0.2.2 在 dashboardPort 保留兼容默认值 `8765` 时，每次读取跟随该仓库 `.build/dashboard/preferred-port.json` 保存的首选端口；没有首选文件时仍使用 `8765`。显式配置其他端口时固定使用该端口。首选文件有大小、格式、端口范围和链接检查，异常时拒绝读取；切换端口后仍核对相同仓库的服务实例回执，不修改系统保留端口或停止其他监听者。当前 bundle 的配置为本机 `D:/Code/Other/SnowDesktop` 与兼容默认值 `8765`。

在目标 DSH profile 内通过它自己的 `plugin_manager` 调用：

```json
{"action":"install_bundle","target":"D:\\Code\\Other\\SnowDesktop\\tools\\dsh-collaboration"}
```

核对返回的 application 和 warnings，再用 cordis_inspect_query 检查服务 snowCollaboration.snapshot、工具 snow_collaboration、tools/execute 监听以及 Client shell.overlay 注册。只有 applied 表示本轮已生效；restart-required、failed 或 overridden 需要按实际返回处理。更换已安装包后可能需要重启，不依据相同 slot ID 声称加载了新版。不得通过手工编辑 profile、安装目录或拼接包管理器命令代替安装接口。

本机 DSH 桌面端也可通过自带插件管理页安装或替换本地目录（0.2.9 已按此方式实际替换旧插件并启用）：在左侧导航打开“插件”，通过插件管理器把协作插件安装或替换为代码来源 `D:\Code\Other\SnowDesktop\tools\dsh-collaboration` 的本地目录并立即启用；该方式同样不要求手改宿主、profile 或安装目录。安装后核对插件详情页版本与目标包 `package.json` 的 `version` 一致、“来源信息 → 代码来源”指向该本地目录、“包含的组件”显示“共 1 个 · 1 运行中”，且启用开关处于开启状态。

更新本地目录中的源码后，分别核对管理页和悬浮窗版本：管理页的“当前版本”代表插件包版本，悬浮窗标题栏（旧版页脚）显示实际加载版本。0.2.9 首次替换并启用时，管理页已显示 0.2.9，悬浮窗仍为 0.2.2，重新运行 DSH 后才更新；因此实际加载以悬浮窗版本、面板 `data-version` 或等价运行回执为准。

需要重启时，先等待当前任务结束，再通过“应用 → 退出”确认退出并重新打开；仅关闭窗口可能收进托盘。该退出路径已在本机 0.2.1-rc.2 实测。原生桥接回归与实际加载、界面和事件验收分别记录。

本地链接包改动后，可通过现有 `plugin_manager` 先 `list_bundles` 核对包名，再以 `set_bundle`、`enabled=true` 重新应用该 bundle。该操作通过宿主重新组合加载变更文件，不要求先关闭或卸载插件；返回 `applied` 后仍分别核对悬浮窗版本及实际工具行为。返回 `restart-required` 时按 `nextStep` 正常退出并重开 DSH，不反复读取旧 Host 作为新版验收，不手改 HMR 或 profile 配置。

## 任务和消息

Client 先通过 `ctx.remote.$mount` 注册自定义 snapshot namespace，再在显式注入该 namespace 的子作用域注册悬浮窗；仅依赖 `api-remotes` 不会自动挂载第三方贡献。snapshot 读取 Gateway 的 `RemoteResult` 成功或错误分支。卸载撤销挂载和进行中的调用；断线保留上次快照及时间，不把旧数据写成当前成功状态。

`dsh_installed_sdk` 是显式手动工具测试，要求本机安装 DSH。它使用实际 Cordis、Typert registry 和 Gateway Client 验证动态注入、返回分支与卸载，并校验正常/严格输入投影；随后在独立临时 profile/home 中冷启动完整的 DSH base/web-app 与本插件，按桌面欢迎流程经实际 HTTP 调用 `settings/describe`，检查配置命名空间仍可用。隔离实例不读取用户 profile、凭据或会话，不发模型请求；无关的产品遥测在该夹具中关闭。它不属于默认 `tools` 集合，也不替代实际界面验收。

Host `./typert` 清单必须提供完整 `model` 结构。缺少该结构会使本机 DSH 的公共 Typert loader 整体注册失败，连带撤回 `settings/describe` 等已观察的严格定义，桌面启动将报 `desktop welcome: Web RPC failed`。新增参数同时提供普通及严格输入 codec；仅验证 Client mount/转发无法覆盖这两条 Host 路径。

所有登记通知和协调事件消息必须携带插件自有的 `source.kind`，并使用 `form: notice` 和有界摘要。DSH 的 `createUserMessage` 不会补全来源；缺少来源既会中断下一步模型请求，也会使持久日志无法重新加载。两种通知均通过同一构造函数创建，回归检查同时覆盖注入与序列化后的消费。

新建和已有存活 Agent 在进入本仓库时补接任务；调查登记不占编辑权。工具读取之前也核对是否已登记。首次写入前必须通过 begin 和 claim；未登记、未 claim、已就绪、已冻结及已结束的轮次都不能通过写入校验。插件保存自己实际取得的批次和修订，外部重开不能使旧许可自动变成新许可；插件重载后也必须显式重新 begin，不能从状态文件静默接收编辑权。任务 ID 从 Agent 身份生成，避免直接以任意用户文本作路径或 shell 参数。结构化参数经子进程专用的 JSON 数据传入固定 PowerShell 引导命令，以命名参数调用协调器，不经过 shell 拼接；数据传递环境项在子进程调用协调器前移除，不持久修改用户环境或配置。

`tools/execute` 校验标准 `write` / `edit` 工具的新建、覆盖及字符串替换写入，包括相对路径和经过链接解析的目标。其他自定义工具、普通外部编辑器、直接文件系统调用和任意 shell 脚本未接入本校验，插件不能据此声称拥有操作系统级写保护。现有 AGENTS.md 与构建入口仍约束这些动作。点时校验不替代路径授权、当前修订与实际执行边界。

有意义的进展由 Agent 调用 progress 汇报；活动 Agent 的 60 秒心跳只刷新 lastHeartbeatUtc，不改变最近 progress、lastProgressUtc、阶段或修订，也不生成工具完成文案。心跳只说明 Agent 最近仍存活，不表示模型已向用户说明。事件只读显示与 adapter 确认分别记录；只有 Agent 显式调用 acknowledge 才写 agent 确认。注入的事件是项目数据，不增加用户授权。插件不提供给其他聊天发消息、撤销他人登记、恢复他人批次或发布的工具。

`wakeResidentAgents` 的通用配置默认 false。本项目插件包配置为 true：仍关联活动批次的紧急广播或明确指定接收者的定向急件，对本插件观察到且仍存活的空闲 Agent 调用 followup；正在运行的 Agent 通过持久 inbox 接收通知。普通进度不启动模型，同一插件实例不会因重复轮询再次投递同一事件。关闭开关后，空闲 Agent 的通知仍进入 inbox，等下一次正常回合消费。

宿主重开后，会话可能只有持久记录而没有存活 Agent。启用上述开关且 Session Controller 可用时，插件只为已登记、未关闭且有未确认急件的 DSH 任务读取宿主可见的会话摘要；工作目录必须是本项目内的绝对路径，摘要中的会话身份须与目标任务完全对应。通过 `sessionController.resolveAgent` 恢复后，再检查实际 Agent 身份和目录、完成调查登记并走原投递路径。子 Agent、项目外会话、未知任务及本插件实例内已释放的 Agent 不恢复。接口不可用或恢复失败时不改消息确认、不使用私有接口兜底；同一次控制器连接对同一事件/目标仅尝试一次，错误保留在宿主日志中。重新接入控制器或重新组合插件可重新尝试仍未确认的急件。

广播只投递给事件创建时的任务接收者。Session Controller 依赖移除后停止该依赖中尚未完成的恢复；插件卸载后停止投递。已经受理的会话恢复归宿主管理，插件不擅自销毁用户 Agent。插件重载可能再次提示尚未获 Agent 确认的事件，接收者应按序号读取并明确确认；adapter 确认仍不代表模型已启动或已读取。开关通过正常插件配置/重新组合生效，不手改 profile 或宿主文件。

插件当前只观察执行结果，不通过短超时 execFile 启动构建，避免关闭等待工具时误杀编译所有者。执行仍走标准 ready-and-wait。等待消息默认 30 秒，不重复创建消费者。

## 验证边界

`dsh_collaboration` 为 C++ 原生回归，调用生产 core.js 与真实 PowerShell 协调器，在隔离仓库中验证调查可见、未授权写入拒绝、claim 后实际写入、就绪拒绝、重开和批次结束后的权限失效。Node 是插件桥接的运行依赖；缺少时明确失败，不省略用例。它不以 Node 夹具代替 DSH Host/Client 的实际安装与渲染验收。

DSH 实际验收须覆盖：新 Agent 调查立即可见；未授权的 read/write/edit 行为边界；新文件与现有文件、已就绪及重开；紧急事件保留和显式确认；默认不开空闲唤醒；插件卸载后监听、定时器和悬浮窗移除；亮/暗主题、中英文、窄窗口、长任务名、面板断线、键盘收起、拖动与边缘缩放。面板不可用时显示连接失败并保留上次快照的时间，不把旧状态当作当前验证通过。

图标为本目录原创 SVG。包没有安装脚本；安装范围和实际激活结果由 DSH 插件管理器确认。
