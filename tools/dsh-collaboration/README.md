# SnowDesktop DSH 协作插件

此目录是独立 Cordis bundle。它不修改 DSH 宿主源码或安装文件，也不手写 profile 的 package.json/cordis.patch.yml。运行接口按本机 DSH `0.2.1-rc.2` 随包的插件、工具、文件写入与 Typert API 编写；其他版本须重新核对接口。

插件在配置的 SnowDesktop 仓库内登记调查任务；DSH 的 `fs/write-intent` / `fs/edit-intent` 在受支持文件写入前校验当前批次、编辑轮次和路径归属。`snow_collaboration` 工具提供 open/status/progress/begin/claim/plan/messages/wait-messages/acknowledge。任务登记、事件及编辑授权仍保存在现有协调器中。

悬浮窗注册到 DSH 的 `shell.overlay`，使用宿主 React、语言和主题。拖动标题栏可自由移动，标题栏聚焦后用方向键微调（Shift 加大步幅）；“位置”直接选择四角。收起、拖动位置在当前客户端保存，窗口缩小时保持标题栏可见，兼容旧版四角偏好。只保存查看偏好；不加载 iframe，不在浏览器访问其他 localhost 服务，不提供执行或确认按钮。Host 读取前核对本仓库服务回执中的实例 nonce，拒绝模拟服务、错误监听者和重定向。

信息以当前批次和参与者为先，明确等待哪项编辑或检查，区分数据读取时间与任务自行报告的进展时间。其余最近五分钟没有活动更新的登记默认折叠并显示最后报告时间，不据此撤回登记或推断任务已结束。尚未冻结测试计划时显示原因，不把零计数当作通过；有实际选择后区分执行数量、失败、未运行及尚待报告的结果。紧急消息区分适配器收到和智能体确认；断线保留上次快照并标记旧数据。

## 安装

先确认 cordis.patch.yml 的 repositoryRoot 与 dashboardPort 是本仓库及其已核对面板端口。当前文件已配置本机的 `D:/Code/Other/SnowDesktop` 与 `8765`。

在目标 DSH profile 内通过它自己的 `plugin_manager` 调用：

```json
{"action":"install_bundle","target":"D:\\Code\\Other\\SnowDesktop\\tools\\dsh-collaboration"}
```

核对返回的 application 和 warnings，再用 cordis_inspect_query 检查服务 snowCollaboration.snapshot、工具 snow_collaboration、fs/write-intent 监听以及 Client shell.overlay 注册。只有 applied 表示本轮已生效；restart-required、failed 或 overridden 需要按实际返回处理。更换已安装包后可能需要重启，不依据相同 slot ID 声称加载了新版。不得通过手工编辑 profile、安装目录或拼接包管理器命令代替安装接口。

当前 Codex 连接没有 DSH 的 plugin_manager/cordis_inspect 接口；仓库内语法和原生桥接回归不能替代 DSH 安装、真实 UI 和宿主事件验收。

## 任务和消息

Client 先通过 `ctx.remote.$mount` 注册自定义 snapshot namespace，再在显式注入该 namespace 的子作用域注册悬浮窗；仅依赖 `api-remotes` 不会自动挂载第三方贡献。snapshot 读取 Gateway 的 `RemoteResult` 成功或错误分支。卸载撤销挂载和进行中的调用；断线保留上次快照及时间，不把旧数据写成当前成功状态。

`dsh_installed_sdk` 是显式手动工具测试，要求本机安装 DSH，使用其实际 Cordis、Typert registry 和 Gateway Client 验证动态注入、返回分支与卸载；没有用户 profile 或模型请求。它不属于默认 `tools` 集合，也不替代实际界面验收。

所有登记通知和协调事件消息必须携带插件自有的 `source.kind`，并使用 `form: notice` 和有界摘要。DSH 的 `createUserMessage` 不会补全来源；缺少来源既会中断下一步模型请求，也会使持久日志无法重新加载。两种通知均通过同一构造函数创建，回归检查同时覆盖注入与序列化后的消费。

新建和已有存活 Agent 在进入本仓库时补接任务；调查登记不占编辑权。工具读取之前也核对是否已登记。首次写入前必须通过 begin 和 claim；未登记、未 claim、已就绪、已冻结及已结束的轮次都不能通过写入校验。插件保存自己实际取得的批次和修订，外部重开不能使旧许可自动变成新许可；插件重载后也必须显式重新 begin，不能从状态文件静默接收编辑权。任务 ID 从 Agent 身份生成，避免直接以任意用户文本作路径或 shell 参数。结构化参数通过 PowerShell argv 传递，不经过 shell 拼接。

`fs/write-intent` / `fs/edit-intent` 覆盖使用 DSH 文件系统接口的新建、覆盖、编辑及字符串替换写入。普通外部编辑器、直接文件系统调用和任意 shell 脚本没有接入这些事件，插件不能据此声称拥有操作系统级写保护。现有 AGENTS.md 与构建入口仍约束这些动作。点时校验不替代路径授权、当前修订与实际执行边界。

有意义的进展由 Agent 调用 progress 汇报；60 秒心跳只根据实际工具完成事件补充最近活动，不把心跳报成模型已向用户说明。事件只读显示与 adapter 确认分别记录；只有 Agent 显式调用 acknowledge 才写 agent 确认。注入的事件是项目数据，不增加用户授权。插件不提供给其他聊天发消息、撤销他人登记、恢复他人批次或发布的工具。

`wakeResidentAgents` 默认 false：紧急事件注入当前存活 Agent 的持久 inbox，等下一步消费；不会自动启动已空闲模型。用户明确授权项目内自动唤醒后，可在 DSH 的正常插件配置入口开启。开启后仅对本插件观察到且仍存活的空闲 Agent 调用 followup；不能唤醒已卸载、已结束或其他宿主中的会话。普通进度只更新视图，不唤醒模型。

插件当前只观察执行结果，不通过短超时 execFile 启动构建，避免关闭等待工具时误杀编译所有者。执行仍走标准 ready-and-wait；持久 Run 接入完成后再扩展启动面。等待消息默认 30 秒，不重复创建消费者。

## 验证边界

`dsh_collaboration` 为 C++ 原生回归，调用生产 core.js 与真实 PowerShell 协调器，在隔离仓库中验证调查可见、未授权写入拒绝、claim 后实际写入、就绪拒绝、重开和批次结束后的权限失效。Node 是插件桥接的运行依赖；缺少时明确失败，不省略用例。它不以 Node 夹具代替 DSH Host/Client 的实际安装与渲染验收。

DSH 实际验收须覆盖：新 Agent 调查立即可见；未授权的 read/write/edit 行为边界；新文件与现有文件、已就绪及重开；紧急事件保留和显式确认；默认不开空闲唤醒；插件卸载后监听、定时器和悬浮窗移除；亮/暗主题、中英文、窄窗口、长任务名、面板断线、键盘收起与四角定位。面板不可用时显示连接失败并保留上次快照的时间，不把旧状态当作当前验证通过。

图标为本目录原创 SVG。包没有安装脚本；安装范围和实际激活结果由 DSH 插件管理器确认。
