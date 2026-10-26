# 整体资源占用统计

复用 `profile.bat`、已有 Perfetto 会话、采样工作线程与 UI 资源回调。
默认关闭。没有新增业务定时器、资源所有权账本、缓存容量或淘汰策略。
示例：`scripts/profile.bat capture -Preset idle -Seconds 60 -ScopeCpu`。
固定分析器的 `counters.csv` 与 summary counters 自动保留新增指标。

## 进程与辅助进程

- `process` 约每秒记录原有 CPU、private_commit_bytes、working_set_bytes、
  handle_count、page_fault_count，并增加 private_working_set_bytes、
  shared_commit_bytes、GDI/USER 数量及 available 标记。
- 扩展内存结构保守限定 Windows build >=22631；不满足或调用失败时保留 EX
  基础数据，不把缺失的 private working set 写成 0。较旧系统仍可使用低频驻留页分类。
- `process.family.memory` 每 10 秒枚举进程，纳入当前主进程、同一 EXE 完整路径的
  活后代，以及同目录的 SnowDesktopSteamBridge、SnowDesktopWorkshopManager、
  SnowDesktopLauncher 和 snowwidget 后代。父子创建时间链须有效，句柄对应进程须已在枚举开始前创建，拒绝枚举后重用的PID；
  最多 64 个成员。不同路径的同名程序、外部启动的
  应用和 Explorer 不纳入。设置/守护/Shell helper 使用相同 EXE 且属该链时可纳入。
  其他目录/未列出的 EXE、经未纳入的中间进程产生的后代、独立启动的实例暂未覆盖。
- `observed_*_sum_bytes` 是成功查询的当前成员观测和。只有 `sample_complete=1`
  才表示本轮发现与查询完整；失败候选数和观察成员数同时记录。总工作集相加包含
  共享页重叠，不能称为整组唯一物理内存。private commit/working set 不与 GPU 字节相加。
- `process.aux.memory` 按 PID+创建时间构造会话匿名 owner，输出各辅助进程 OS 指标、
  thread_count 和 present。完整枚举确认成员消失后发出 present=0；旧字节样本不能解释为仍存活。
  program_kind 的 1/2/3/4/5 分别表示同 EXE、Steam Bridge、Workshop Manager、Launcher、snowwidget；
  只由核对后的文件路径分类，不推断同 EXE 子进程的具体启动模式。
  主进程 thread_count 同样每 10 秒更新。进程持续退出/重启时存在采样间隔未覆盖的部分。
- 不在子进程启动 SDK、读取命令行或注入探针；manifest 的
  processResourceStatistics 明确 OS 计数覆盖和 childExecutionCaptured=false。
  原 participants/hostOnly 继续表示执行轨迹的进程范围。

## 主进程地址空间

`process.address_space.memory` 每 10 秒在采集工作线程执行 VirtualQueryEx 与
QueryWorkingSetEx。PRIVATE、IMAGE、MAPPED 分别记录 committed_region_bytes、
reserved_address_bytes、resident_bytes、private_resident_bytes、shareable_resident_bytes。

reserved 是地址预留，committed_region 是映射区域分类；二者都不等同物理 RAM 或
进程 PrivateUsage。resident 才是本轮有效驻留页，private/shareable 是其分区。
可共享不表示已经由多个进程共同使用；写时复制页仍可能属于 IMAGE/MAPPED 类型。
查询不访问页内容、不强制调入、不锁页、不裁剪工作集，也不做 HeapWalk。

IMAGE 进一步按有限文件名组分为 main、graphics、text、shell_ui、runtime、other。
分组仅解释映像页，不表示对应 DLL 在堆或驱动中分配的全部内存。未识别名称归 other，
名称不可用数量单列，完整路径不进入 trace。每组只输出已提交映像区域及驻留指标。

单轮最多 65,536 个区域、2,097,152 个查询页、约 200 ms 检查预算；每批最多 4,096 页。
超时、达到预算、进程查询失败时 sample_available=0 并输出错误与已扫描规模，不发布
不完整的字节汇总。available=0 时此前的字节样本也不能当作本轮有效结果。
查询不是原子快照，页状态可以在 API 调用间变化。

sampler_workspace_bytes_estimate 是本轮查询数组的尺寸；采样会产生少量临时自身开销。
`profiler/sample.address_space`、`sample.process_family` 记录实际采样墙钟及可用线程 CPU。
扫描预算是在 API 批次边界检查，不能保证单个 Windows API 调用有硬超时。

## 宿主模型载荷

`host.model.memory` 在现有 UI 资源回调中只读统计：桌面项目、组件及 Dock popup、
文件夹条目、页面、Dock 引用、快捷导航应用/搜索结果、布局记录与桌面 key 索引。

- entries：当前条目数；widgets_and_dock_popup_entries 只计 widgets_，popup 的内联对象
  已包含在 DesktopApp 中，其字符串和嵌套容器计入同组。
- vector_storage_bytes：vector capacity×sizeof(value_type)；嵌套 vector 单列。
- selected_text_payload_bytes/fields：明确选中的字符串内容字节/字段数，不是堆占用。
  包括名称、路径、页面/组件标识、包来源、集合成员及分类顺序；不序列化内容。
  首批未覆盖 PIDL、large-icon 配置、shortcut extension、JSON/COM 对象及全部业务缓存。
- map_value_storage_lower_bound_bytes 与 bucket_storage_bytes_estimate 分列，不包含
  节点链接、分配器开销；desktop_app_inline_bytes 只计 sizeof(DesktopApp)。

字符串内容可能位于已计入对象的 SSO 内联区，因此不得把上述项相加冒充准确堆总量。
像素、位图和文字布局继续沿用 native.*.memory、widget.*.memory 指标，不重复实现。
`profiler/sample.host_models` 单列 UI 遍历成本。

本轮可以把进程总量、地址类别与部分业务载荷并列观察；匿名私有区内的具体堆分配、
逐线程栈、字体/COM/图形驱动内部所有者及内核侧资源仍未归因。没有把未归因余量标为代码或浪费。
