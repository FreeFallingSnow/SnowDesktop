# 耦合运行性能调试工具

该工具在桌面、Dock、设置和 Lua 实例同时运行时采集诊断数据，默认关闭。
仅 `scripts/profile.bat start` 或 `capture` 显式开启一次采集；退出或到期后关闭，
不保存启用设置，不随重启恢复。关闭时埋点只检查空 hook，不读取性能时钟、分配
采集缓冲、启动采样线程/定时器或写性能文件。没有网络监听或遥测上传。

## 自动化入口

先用 `scripts/build.bat` 生成当前 Release，再正常启动应用。脚本只连接已有
控制窗口，不启动应用、切换桌面来源、修改布局或操作桌面交互。

```powershell
# 探测能力及当前状态，不开启采集
scripts/profile.bat status

# 在现有耦合场景中采集 60 秒，到期生成报告和摘要
scripts/profile.bat capture -Seconds 60

# 非阻塞启动，JSON 返回 session、processId、report 等字段
scripts/profile.bat start -Seconds 120 -TargetProcessId 1234

# 只允许停止指定 session，不会停止其他采集；完成文件稍后发布
scripts/profile.bat stop -Session <start返回的session>

# 对已完成的采集生成摘要和 CSV；不需要宿主正在运行
scripts/profile.bat report -ReportPath <capture.json绝对路径>

# 对比匹配的模块、阶段和实例，按采集秒数归一化
scripts/profile.bat compare -BaselinePath <旧capture.json> -ReportPath <新capture.json>
```

采集时长为 1～600 秒。可用 `-OutputDirectory` 指定输出目录；默认使用
`artifacts/v<version.json版本>/performance/<时间-session>/`。以报告中的
`hostVersion` 和 `session.json` 中的实际可执行文件 SHA-256 为被测构建依据；
仓库版本号只决定默认归档位置。输出已有采集时拒绝覆盖。脚本成功返回 0，失败
返回非零，错误写标准错误。旧构建返回“不支持协议”，不会被脚本重启或替换。

`start` 适合 Agent 开始采集后继续其他工作，再读取结果；`capture` 适合自动化
作业等待采集完成。脚本取消时会尽力停止自己的 session；即使调用方退出，宿主
工作线程仍负责限时停止。若主线程阻塞，组件内存采样会延迟，但后台进程/GPU
采样及到期关闭仍继续。采样 API 本身阻塞时到期处理可能延迟，不能视为硬实时期限。

## 输出及指标口径

- `session.json`：启动时间、目标 PID、可执行文件路径/哈希、操作系统及输出路径。
- `capture.json`：schema 1、宿主版本、时长、聚合统计、带线程/父级/关联 ID 的事件。
- `summary.json`：CPU 比例、按自身 CPU 时间排序的阶段及全部资源指标。
- `scopes.csv` / `gauges.csv`：可供脚本、表格或分析程序读取的汇总。

宿主先独占创建 `capture.json.partial`，完整写入并关闭后才发布 `capture.json`。
崩溃、写入失败或目标文件冲突时保留 partial；它不是成功报告。脚本的 `capture`
模式在成功发布后自动生成派生文件；`start` 模式需另运行 `report`。

| 指标/分类 | 含义 |
|---|---|
| `wallMs` / `selfWallMs` | 经过时间 / 扣除已埋点同步子调用后的经过时间；包含等待和被抢占时间 |
| `cpuMs` / `selfCpuMs` | `GetThreadTimes` 线程用户态+内核态时间差；短调用可能量化为零，需同时检查 `cpuSamples` |
| `p95WallUpperMs` | 以 1 微秒起始、2 倍增长的直方图得到的 P95 上界，不是精确分位数 |
| `processCpuPercent` | 进程 CPU 增量 / 实际采样区间 / 逻辑处理器数；保留实际采样区间长度 |
| `lua_bytes` | 实例 Lua 分配器当前值的定时采样；不含宿主堆、共享缓存、纹理，不是完整组件内存 |
| `private_commit_bytes` / `working_set_bytes` | 进程私有提交 / 物理驻留工作集，二者不能替代 |
| `surface_bgra_bytes_estimate` | 组件合成表面的宽×高×4估算；不是实际显存驻留量 |
| `gpu_engine_percent` | 当前 PID 的单个 PDH GPU 引擎实例，owner 保留 LUID/引擎身份；不得跨引擎相加成总 GPU 百分比 |
| `gpu_dedicated_bytes` / `gpu_shared_bytes` | 当前 PID 对应 GPU 内存计数器实例；共享资源不能跨进程简单求和 |
| `shared.system` / `shared.audio` / `composition.shared` | 共享数据采样、音频交付、合成；不强行分配给某个组件 |
| `profiler` | 采集器初始化及部分采样成本；进程总指标仍包含埋点、锁竞争等观测开销 |

GPU 指标不可用或没有匹配 PID 时以实例数量 0/缺失值表示，不伪造“GPU 占用 0%”。
DWM 和驱动公共工作没有逐组件归属；本工具也不报告逐组件 GPU 时间。
设置页面记录打开、导航、快照应用等宿主阶段，XAML 后续布局/呈现不等于这些同步
方法的返回时间，需要进一步的 WPR/WPA XAML 跟踪。

## 耦合关系

`events.id` 为会话内事件 ID，`parent` 为同步父作用域，`thread` 为线程 ID。
Lua `protectedCall` 从宿主组件执行上下文继承实例 owner。
`task.start`、`task.dispatch`、`task.completion` 的 `correlation` 是同一任务 ID；
这表示生命周期关联，不把异步等待记为 CPU，也不声称已覆盖所有后台执行器的 CPU。

组件失效时保存来源作用域，实际绘制消费来源后生成 `widget.draw.source` 事件：
`parent` 是绘制作用域 ID，`value` 是来源作用域 ID，`phase` 是表面名称。
因此延后绘制和一次绘制合并多个失效来源仍可追溯。来源不用于平均分摊 GPU 或
公共合成时间。未经过已埋点失效入口的绘制、采集前已经排队的更新、被截断的事件
可能没有可追溯来源。同步父子耗时会重叠，不能累加所有 inclusive 行。

时间线默认最多保留前 32768 条事件，聚合最多 4096 组；每个表面/实例最多保留
16 个未消费来源，共最多 1024 个来源组。报告明确给出 `droppedEvents`、
`droppedGroups` 和 `droppedLinks`。时间线满后既有组继续汇总，因此 CPU/资源
摘要不依赖被截断的事件。跨采集边界尚未结束的作用域不计入该次报告。

## 控制协议 v1

此接口用于本机自动化，不是 Lua 公共 API，不要求更改组件 `apiVersion`、
`minHostVersion` 或 capability。既有组件不需要修改；旧宿主不支持时客户端拒绝执行。

- 目标为现有 `SnowDesktopControlWindow` / `SnowDesktopControl`，可按 PID 选择。
- 注册消息 `FreeFallingSnow.SnowDesktop.Performance.v1` 返回状态：
  `0x53445010` 空闲、`0x53445011` 采集中、`0x53445012` 输出中、`0x53445013` 失败。
- `WM_COPYDATA.dwData = 0x53445031`。载荷为 NUL 结尾 UTF-16，最多 16384 字节，
  正好五行：`1`、`start|stop`、session、秒数、输出 JSON 绝对路径。
  stop 的最后两行为空；session 为 1～64 个 ASCII 字母/数字/连字符。
- 命令返回 1 表示接受，0 表示拒绝；start 不接受覆盖已有报告或 partial。
- 标准 Windows 窗口消息权限边界适用，客户端不会修改 UIPI 或申请管理员权限。

## 验证与优化

运行 `scripts/test.bat name widget_runtime_diagnostics` 验证默认关闭、
Lua 结果保持、嵌套归属、跨线程隔离、失效来源、事件上限、重复采集、超时、
旧会话隔离和非法协议载荷；`scripts/test.bat name lua_runtime` 运行原有 Lua 测试。
这些用例加入已有诊断测试目标，不新增测试程序。
`SnowDesktopWidgetRuntimeDiagnosticsTests.exe --performance-control-fixture` 提供
仅用于协议验证的不可见控制窗口，按 PID 定向连接；它不验证真实桌面交互。

先在相同机器、显示器配置和工作负载下记录多次基线，再一次修改一个热点。
资源下降须同时检查交互、动画、提醒及更新行为。实际桌面视觉、框选、拖放、Dock
悬停等依照仓库规则由用户实机验证，不使用桌面自动化捕捉宿主窗口。
