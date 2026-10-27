# Perfetto 性能采集

运行时默认关闭。第一次显式开始采集时初始化固定的 Perfetto v58.2
in-process SDK。宿主使用 SDK 原生线程轨道、slice、counter 和 flow；业务
Scope 只传递实例与操作上下文，不保存事件、计时栈、CPU 时钟、直方图或聚合结果。
日志、崩溃转储和实际动画调度器保留原有职责。

## CLI 与产物

```powershell
scripts/profile.bat setup
scripts/profile.bat status
scripts/profile.bat capture -Preset idle -Seconds 120
scripts/profile.bat capture -Preset interaction -Seconds 60 -ScopeCpu
scripts/profile.bat capture -Preset interaction -Modules "lua,render,input" -InputSampleEvery 4 -Seconds 30
scripts/profile.bat start -Preset widget-detail -WidgetIds <instance-id> -Session <id>
scripts/profile.bat status -Session <id>
scripts/profile.bat stop -Session <id> -Generation <generation>
scripts/profile.bat wait -Session <id>
scripts/profile.bat analyze -ReportPath <session.json> -Offline
scripts/profile.bat compare -BaselinePath <summary.json> -ReportPath <summary.json>
```

`setup` 从官方 HTTPS 地址取得固定 Windows x64 Trace Processor，并校验长度及
SHA-256。其缓存位于用户 LocalAppData/SnowDesktop/tools/perfetto/v58.2。
也可通过 `SNOWDESKTOP_TRACE_PROCESSOR` 指向同一哈希的离线程序。构建不联网；
仓库工具不依赖用户安装 Skill。离线分析不上传 trace。

`-Modules` 接受逗号分隔的固定模块组：resource、lua、task、frame、input、dock、
popup、layout、render、icon、cache。筛选只能缩小预设；请求预设未启用的组会拒绝，
资源与会话边界标记始终保留。CLI 先确认宿主能力，旧宿主不能静默忽略新参数。
`-InputSampleEvery 1..1000` 按会话加盐的操作哈希以名义 1/N 比例选择输入细节，
同一生产线程同步操作组中的嵌套范围与指标采用相同决定。默认 1 保留全部。
manifest 和报告记录请求筛选、实际类别、观察/选择操作组数及类别/实例过滤探针次数；
这是采集启用期间的诊断操作组观测，不是操作系统输入事件数或 SQL slice 窗口。
选择计数不代表跨边界或丢失事件已成功序列化，须结合质量字段判断；不外推完整计数或延迟分布。
配置指纹包含筛选与采样率，不同配置不能作为同条件比较。

新采集只有原生格式。旧 `summary/trace` 参数分别映射到 idle/interaction 预设，
不再选择产品后端。`report` 保留 schema 1 JSON 的只读分析；原生会话 manifest
和分析 summary 使用 schema 2。旧 WM_COPYDATA 写入明确拒绝。

`Start` 接受请求后先返回 starting；SDK 确认后才进入 recording。随后依次进入
stopping、finalizing、completed 或 failed。导出和分析均不在 UI 线程执行。
`requestId` 对相同配置幂等，冲突配置拒绝；停止绑定 sessionId 与 generation，
旧会话的重试不能停止新会话。最终状态保留最近 20 个会话。

CLI 通过控制窗口发现 PID 并惰性打开 v3 本地管道。双向检查真实进程身份；
服务端核对 SID、会话和完整性级别，拒绝远程客户端。长度帧、请求大小、读写和
确认均有上限。客户端不能指定服务端写入任意路径；`OutputDirectory` 是完成后
由客户端复制证据的目标目录。

宿主默认写入当前用户 LocalAppData/SnowDesktop/diagnostics/performance/<session>/，
不依赖便携软件安装目录是否可写。`.pftrace.partial`
和 `session.json.partial` 独占创建；flush、close 后原子发布，不覆盖同名文件。
失败保留 partial，且不会被报告为完成。总空间为 1 GiB、最多 20 个完成会话；
失败证据也计入空间，不自动删除。达到上限后需用户选择清理证据再采集。

manifest 包含 SDK/应用版本、进程角色、请求与配置身份、原生时间边界、可执行文件
及 trace 哈希、覆盖范围和探针丢失计数。未知源码提交保持 null，不从附近 Git
检出推断运行二进制来源。实例、包和 HWND/指针关联使用每次会话的匿名标识。

## 指标解释与有效性

宿主在现有资源采样回调中补充 `native.icon.memory`、
`native.bitmap.memory` 和 `native.text.memory` 计数；只在显式采集期间
遍历当前缓存，不增加定时器、Shell查询、解码、美化或GPU像素读回。
`profiler/sample.icon_text_resources` 记录这段采样自身的成本。

- `*_source_references/unique_count/unknown_size_count` 区分模型引用、
  本次采样中的不同HBITMAP和没有尺寸元数据的图源；BGRA字节按已有尺寸估算。
  菜单图片和部分控制图源没有尺寸元数据时只计数量，不把未知占用当零字节。
- D2D逐表记录references、unique_count和bgra_bytes_estimate。
  `observed_unique_*` 仅对本次列出的宿主D2D缓存/固定槽去重，
  不覆盖命令列表独占引用、驱动分配、合成表面或WidgetEngine的独立缓存。
  指针集合在每次UI线程采样结束时销毁，不作为跨采样的资源身份。
- `prepared_payload_bytes` 是UI准备队列的pixel size，capacity另列；
  不包含worker、ready mailbox或分配器保留页。mask使用非等待锁；
  `mask_sample_available=0` 时不输出该次mask容量，不能解释成缓存已清空。
- 布局input_bytes只计宿主拥有的键字符串及网格自然宽度缓存的文本，
  不含IDWriteTextLayout、字体集合、容器节点和分配器开销。
  文字阴影位图在native.bitmap.memory中单列。
- 图标库统计当前解码源和pending数，不初始化尚未使用的图标库。
  large_runtime只覆盖模型当前持有的资产/预览，不覆盖大图标服务的cache-only资产。
  各图源组可能引用同一个源，不将分组字节直接相加为全宿主唯一占用。

这些值是缓存存量观测；没有改变缓存容量、LRU、准入、释放、图像尺寸或显示行为。
逻辑像素载荷与进程私有提交、工作集及GPU实际用量分别比较，不能相互替代。

固定 SQL 位于 tools/performance/sql，直接交给官方 Trace Processor。分析目录保存
最终 SQL、CSV、summary.json、查询哈希、诊断日志及 perfetto_analysis_report.md。
查询包 v2 只接受 probeSchemaVersion 2 的原生会话；旧 JSON 继续通过 report 只读分析，
旧原生会话仍可直接用 Perfetto 查看。查询输出每流最多 4 MiB、错误日志最多 256 KiB、
摘要最多 16 MiB；每次托管分析预留 64 MiB，总空间继续受 1 GiB 限额约束。

- 窗口使用原生 capture.start/capture.stop；只分析完全包含的闭合 slice。
  未闭合、跨边界、数据丢失和探针丢弃单独报告，不能以完整汇总掩盖。
- wallMs 是包含等待和子工作的墙钟时间；selfWallMs 只扣除**已记录**层级的子工作。
  过滤掉的工作仍可能包含在该值中，不能等同独占线程 CPU。
- thread CPU 由 SDK 提供，只对实际具有 thread_dur 的 slice 汇总。没有 CPU 样本时
  保持 null；无法证明子调用完整覆盖时 selfCpuMs 保持 null。
  模块和实例的 rootInclusiveCpuMs 只取本组最外层同步 Scope，避免同组嵌套重复；
  仍包含 native API 和共享子工作，各组会重叠，不得相加或称为实例独占/纯 Lua VM CPU。
- 进程 CPU 使用 GetProcessTimes 的累计值差，分母是该 counter 实际首尾采样窗口。
  同时输出 CPU ms/s、单核百分比及按逻辑处理器数归一化的百分比。
- 内存等 gauge 不累加；明确的累计量才计算差值/速率，且至少两个样本、正窗口和
  无计数器回退。缺样本、重置或无效证据保持 null。
- GPU 数据只能表达进程级引擎/内存观测，不按 Lua 实例分摊 GPU 时间。
  进程数据复用现有系统数据提供器的 GPU 查询。采集只增删 performance-capture
  consumer，不停止组件或系统面板的订阅。
  回调、应用更新和提交不代表操作系统唤醒、DWM 呈现或显示器实际帧率。
- 跨会话匿名实例标识不同。比较工具只比较相同查询/预设配置的有效模块观测；
  已知 SDK、系统、处理器、显示、DPI、刷新率和电源差异会拒绝比较；无法证明的
  机器身份、组件内容、布局和负载保留 unknown，不能据数值变化直接判断因果。

采集由 CLI 与 Agent 显式控制，设置页不提供性能采集控件。产物和状态使用统一控制器。

### 宿主维护、任务栏与状态栏计量

以下探针复用现有 Scope 和有限事件目录，默认关闭，不增加业务定时器、轮询或日志。
`idle` 预设可记录 `timer.callback`、`shared.maintenance`、`shared.taskbar` 和
`shared.status_bar`；不要用只选 Lua 的过滤会话判断这些工作不存在。
需要旧 `status_bar.snapshot/content.paint/hover.paint` 细节时使用 `interaction`。

- `timer.callback` 的 `host.*`、`status_bar.*` phase 是已接收的 WM_TIMER 分发；
  现有 `scheduler` phase 是截止时间队列回调。三者分别统计，不是系统唤醒数。
- `shared.maintenance` 记录 runtime tick、隐藏表面回收、Steam订阅协调、显示拓扑轮询、
  回收站实际异步查询、Start菜单、Show Desktop和任务栏诊断子工作。
  `taskbar.refresh.not_due`只表示本轮外观刷新未到期，不代表其他守护子工作均为空。
  `dock.discovery.dirty/fallback`记录发现请求原因，实际发现仍看已有
  `dock.running.discovery` Scope；拖放等延期不算已完成枚举。
- `shared.taskbar` 的 `refresh/window_state.scan/controller.apply`分别计量协调、
  窗口扫描和控制器应用。`publish`是完整共享状态发布，`apply.post.*`和
  `restore.send.*`是每个目标的消息发送结果。成功发送不证明Explorer已应用或绘制；
  `refresh.disabled/window_state_unchanged`记录已有分支的返回原因。
- `shared.status_bar.paint.request`覆盖普通Paint请求及准备成本；
  `paint.deferred/preparing_scroll/unavailable/unchanged/empty_client/empty_damage`
  是不同跳过原因。旧内容绘制Scope位于部分检查之后，不能直接当请求总数。
  `input_method.sample`是单一后台读取的工作时间，不包含200 ms等待；
  `input_method.changed/unchanged`是各栏窗口轮询的比较结果。
- `draw.full.*`、`draw.partial.*`分别表示一次完整内容绘制操作和一次局部更新操作结果；
  一次局部操作可能包含多个损伤区。`surface.begin.*`、`surface.end.*`分别统计实际
  BeginDraw/EndDraw调用结果，包含背景、内容和逐损伤区绘制；不能将操作数再加到API次数。
  `commit.success/failure`统计状态栏四条直接Commit路径，`shared.status_bar.commit`
  Scope计量Commit调用。成功不等于DWM呈现一帧。

上述事件均在固定报告的 `events.csv` 按module/metric/匿名owner输出；`events`是事件条数，
`amount`是事件值之和，不取最后一个样本作为累计次数。栏owner使用会话匿名化的窗口实例序号，
重建后的实例不复用旧身份；底层逐损伤区渲染器的surface事件owner为0，汇总API次数时包含它，
不把owner 0理解成另一块屏幕。`volume.completions`是实际取出的任务完成数，
不等同空队列检查次数。`scopes.csv`提供次数、墙钟和可用线程CPU；包含子工作的CPU不跨行累加。

计时器创建成功、到期消息迟到、Explorer内材质应用、核心idle退出及真实系统唤醒尚不由这些
应用探针覆盖。真实线程唤醒需另用ETW的ReadyThread/CSwitch和等待原因分析，缺证据保持未知。
本轮计量不改变周期、订阅、显隐、失败重试或恢复期限。

任务栏扫描进一步提供以下诊断项：

- `guard.control_due/foreground_changed/window_dirty_due/foreground_settling`记录一次
  guard刷新时成立的条件；同次可以多项成立，不相加为刷新次数。用父子切片关联对应的refresh。
- `scan.request.forced/dirty`记录refresh进入扫描分支时采用强制请求还是脏标记请求；
  status bar等直接调用扫描的路径可能没有这两种事件，仍按实际调用栈区分。
- `scan.com_setup/windows/shell_ui`分别覆盖虚拟桌面COM对象准备、实际窗口枚举和Shell UI查询。
  三者是`window_state.scan`的子范围，不再与父范围CPU相加。
- `window_event`覆盖原生WinEvent回调。`dirty.*`按事件类型记录实际增加任务栏修订的次数；
  `dirty.dock_filtered_self/window`记录已增加修订后、被现有Dock通知过滤的事件。
  过滤计数是类型计数的子集，不相加，也不能仅凭同一窗口内出现就归因到某次扫描。
  两项沿用已有查询结果，不新增窗口枚举、窗口身份查询或窗口标题等采集字段。

这些项使用有限事件名、默认关闭，保持既有过滤位置及判断。未记录标记事件来源到扫描的持久队列，
不把同一轮合并的多个事件强行分配为唯一根因；事件计数和刷新条件需结合实际时间序列解释。

Lua 配额分配器只在采集代次有效时累计成功的逻辑分配、重分配和释放，低频输出
字节流量、次数、基线和存量峰值。不逐次发送分配事件；失败分配不计入流量。
增长是窗口内观测，不是泄漏结论。任务队列到实际执行线程使用 SDK flow，CPU
只来自同步执行 Scope；协程只记录同步阶段，外部服务等待没有伪造 CPU。

动画生命周期使用 SDK 异步轨道，应用更新和实际回调分开记录；合并呈现及真实
Commit 另记事件。每个会话最多分配 1024 个动画轨道；轨道 ID 与对象地址及回调键
分离，复用对象仍产生独立生命周期，超过上限写入 droppedTracks 并使质量判据失败。
animations.csv 的 updateFps 是应用更新频率，presentedFps 和
droppedFrames 在无法归因时保持 null。交互预设在 Windows 11 可选读取 Compositor
Clock 显示合成统计，每次最多 10 ms、256 帧及 16 个目标，不等待或提升系统合成时钟。
compositor.csv 仅表达显示合成，不能代表每个动画，也不能完整覆盖 independent flip。

## 验证与实施边界

`widget_runtime_diagnostics` 使用真实 SDK、真实 Lua 和官方分析器验证 native trace、
CPU 可用/不可用、层级、flow、预设过滤、重启、幂等、会话隔离及失败证据。其隐藏
控制专用 HWND 仅用于实际 CLI/管道集成，不启动桌面宿主。渲染回归通过 SDK
Interceptor 观察已序列化事件，不保留另一套产品 profiler。

当前链路覆盖宿主或显式组件评估进程，角色记录于 manifest。独立设置进程明确排除。Lua 分配流量、
后台任务归因、调度事件和共享 GPU 数据源使用上述实际产品入口。跨进程分析、
显示呈现到具体视觉对象的归因及目标机器上的完整负载开销仍需独立验证。
标准构建、可控工作负载和独立设置预览不能替代桌面实际显示与交互验收。

`SnowDesktopWidgetRuntimeDiagnosticsTests --performance-overhead <全新目录>` 执行
真实 Lua 配额分配/回调与固定探针负载，轮换五种采集模式各五次，记录进程 CPU、
墙钟、常驻内存和停止后的安静窗口。冷关闭与 SDK 初始化后的关闭分开记录；
这项测量只适用于明确的夹具工作负载，不代表整个桌面应用的开销验收。

## 组件开发中的性能评估

`snowwidget capabilities` 的 `performanceProfile.schemaVersion=1` 表示 CLI 支持组件评估。
`snowwidget profile <包目录> <全新输出目录> --seconds 5 --workload redraw --scope-cpu`
在独立预览进程复用真实引擎、命名定时器、Lua 分配器和同一 Perfetto 控制器。
`--workload idle` 只随组件实际失效重绘；`redraw` 按 `--interval-ms` 请求重绘。
组件命令同样支持 `--modules lua,render` 与 `--input-sample-every 4`；先核对
`performanceProfile.moduleFiltering/inputSampling`，实际宿主结果必须确认请求配置。
静置模式不建立无用的重绘周期，资源采样仍使用现有 1 Hz 机制。
预热和初次加载在测量窗口之前；PNG 回读、编码与离线分析在窗口之后。

输出包含 `evaluation.json`、`preview.png`、原生 trace、会话 manifest 和固定 SQL 报告。
角色为 `widget-preview`，实时系统服务和实际桌面呈现不在覆盖范围；GPU 不按实例分摊。
预览数据与时钟仍使用预览语义，真实计时窗口由 SDK 和系统时钟给出。
指标解释与可执行工作流见分发 Skill 的 `references/performance-evaluation.md`。
