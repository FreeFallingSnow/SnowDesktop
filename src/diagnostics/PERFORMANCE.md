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

宿主默认写入数据目录 diagnostics/performance/<session>/。`.pftrace.partial`
和 `session.json.partial` 独占创建；flush、close 后原子发布，不覆盖同名文件。
失败保留 partial，且不会被报告为完成。总空间为 1 GiB、最多 20 个完成会话；
失败证据也计入空间，不自动删除。达到上限后需用户选择清理证据再采集。

manifest 包含 SDK/应用版本、进程角色、请求与配置身份、原生时间边界、可执行文件
及 trace 哈希、覆盖范围和探针丢失计数。未知源码提交保持 null，不从附近 Git
检出推断运行二进制来源。实例、包和 HWND/指针关联使用每次会话的匿名标识。

## 指标解释与有效性

固定 SQL 位于 tools/performance/sql，直接交给官方 Trace Processor。分析目录保存
最终 SQL、CSV、summary.json、查询哈希、诊断日志及 perfetto_analysis_report.md。

- 窗口使用原生 capture.start/capture.stop；只分析完全包含的闭合 slice。
  未闭合、跨边界、数据丢失和探针丢弃单独报告，不能以完整汇总掩盖。
- wallMs 是包含等待和子工作的墙钟时间；selfWallMs 只扣除**已记录**层级的子工作。
  过滤掉的工作仍可能包含在该值中，不能等同独占线程 CPU。
- thread CPU 由 SDK 提供，只对实际具有 thread_dur 的 slice 汇总。没有 CPU 样本时
  保持 null；无法证明子调用完整覆盖时 selfCpuMs 保持 null。
- 进程 CPU 使用 GetProcessTimes 的累计值差，分母是该 counter 实际首尾采样窗口。
  同时输出 CPU ms/s、单核百分比及按逻辑处理器数归一化的百分比。
- 内存等 gauge 不累加；明确的累计量才计算差值/速率，且至少两个样本、正窗口和
  无计数器回退。缺样本、重置或无效证据保持 null。
- GPU 数据只能表达进程级引擎/内存观测，不按 Lua 实例分摊 GPU 时间。
  回调、应用更新和提交不代表操作系统唤醒、DWM 呈现或显示器实际帧率。
- 跨会话匿名实例标识不同。比较工具只比较相同查询/预设配置的有效模块观测；
  仍须人工核对机器、显示、组件、布局和负载，不能据数值变化直接判断因果。

## 验证与实施边界

`widget_runtime_diagnostics` 使用真实 SDK、真实 Lua 和官方分析器验证 native trace、
CPU 可用/不可用、层级、flow、预设过滤、重启、幂等、会话隔离及失败证据。其隐藏
控制专用 HWND 仅用于实际 CLI/管道集成，不启动桌面宿主。渲染回归通过 SDK
Interceptor 观察已序列化事件，不保留另一套产品 profiler。

当前链路覆盖宿主。独立设置进程在 manifest 明确排除；跨进程覆盖、设置页控制、
Lua 分配 churn/后台归因、调度与原生动画诊断、共享 GPU 数据源及完整负载开销验收
仍属于本方案后续实施项。标准构建或合成夹具不替代桌面实际显示与交互验收。
