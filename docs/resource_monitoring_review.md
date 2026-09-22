# Windows 资源监控方案评估

日期：2026-09-22。对象：SnowDesktop 的系统资源监控与 RX 6800、RX 6750 占用偏高反馈。

建议保留 Windows 原生接口作为基础采集层，先修正聚合和差分口径。GPU 默认显示最忙引擎占用；温度、功耗、频率及厂商活跃率通过可选厂商后端扩展。现阶段没有证据支持为了 GPU 百分比而整体引入另一套硬件监控运行时。

## 1. GPU 偏高的证据

原实现对 `GPU Engine(*)\Utilization Percentage` 按 adapter LUID 直接求和，再截到 100%。这些计数器按进程和引擎分开；同一引擎的进程占用可以相加，不同引擎的占用不能直接相加为整卡占用。Windows 任务管理器以最忙引擎代表 GPU 整体利用率。[微软的指标说明](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/)

例如两个进程在同一 3D 引擎分别占 20%、30%，Copy 占 40%，Video Decode 占 45%，原算法得到 135%，显示为 100%；最忙引擎为 50%。只取单个进程计数器最大值也不正确，这组输入会误得 45%。

本机于 20:27:08–20:27:10 对同一批 Windows 计数器做三次对照，活跃 adapter 的“全部引擎求和 / 最忙引擎”分别为 10.697% / 9.977%、14.173% / 12.817%、16.878% / 15.955%。这是 NVIDIA/Intel 环境下的采样证据，说明累加会抬高读数；RX 6800、RX 6750 尚缺同窗采样，不能据此认定原反馈只有这一项原因。

候选改动 `cb3f4652` 按完整 LUID、物理 GPU 编号、引擎编号聚合，保留同引擎各进程求和，再取 adapter 内最大值。相同 `engtype` 的不同引擎也独立计数。现有 Lua 组件已经取各 adapter 的最大值，无需再改 GPU 百分比的显示汇总。

## 2. 开源项目源码对照

以下版本固定到此次读取的提交；结论来自列出的采集路径，未声称运行过这些项目的全部功能。

| 项目与提交 | 实际采集方式 | 可借鉴内容及限制 |
| --- | --- | --- |
| [System Informer `2a0045bf`](https://github.com/winsiderss/systeminformer/blob/2a0045bfc572d1c9ea6d8843dc6cf6cae1e20fd7/plugins/ExtendedTools/counters.c) | GPU 性能计数器通过 `PerfOpenQueryHandle` / `PerfQueryCounterData` 读取；总占用选择引擎最大值。[另有 D3DKMT 路径](https://github.com/winsiderss/systeminformer/blob/2a0045bfc572d1c9ea6d8843dc6cf6cae1e20fd7/plugins/ExtendedTools/gpumon.c) | 适合参考引擎统计、资源生命周期与采样架构。不能简单描述成“只使用 D3DKMT” |
| [LibreHardwareMonitor `dc51e75b`](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/blob/dc51e75bd97b15ce17ded0885e67bad47be0765b/LibreHardwareMonitorLib/Hardware/Gpu/AmdGpu.cs) | AMD 的 `GPU Core` 走 ADL Activity / PMLog；D3D 引擎和显存另外读取。[NVIDIA 使用 NVAPI 并结合 NVML](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/blob/dc51e75bd97b15ce17ded0885e67bad47be0765b/LibreHardwareMonitorLib/Hardware/Gpu/NvidiaGpu.cs)，[Intel 独显使用 IGCL telemetry](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/blob/dc51e75bd97b15ce17ded0885e67bad47be0765b/LibreHardwareMonitorLib/Hardware/Gpu/IntelDiscreteGpu.cs) | 厂商传感器适配参考价值高；不同传感器有不同语义。整体接入需要 .NET 边界，部分传感器需要管理员权限 |
| [TrafficMonitor `188b8773`](https://github.com/zhongyang219/TrafficMonitor/blob/188b8773b959733bfc0e4f506c762d9a255883d4/OpenHardwareMonitorApi/OpenHardwareMonitorImp.cpp) | 通过硬件监控封装读传感器，GPU 优先选名为 `GPU Core` 的 Load；缺失时取 Load 最大值 | 能解释其 AMD 数值为何与 Windows 最忙引擎不同。按名称及 Load 类型选取不是所有硬件通用的语义契约 |
| [Rainmeter `762b74c1`](https://github.com/rainmeter/rainmeter/blob/762b74c16f19b9ed3169b0d4f86fdfb351134d7c/Library/MeasureUsageMonitor.cpp) | UsageMonitor 使用 PDH，支持实例筛选、按进程名归并和实例汇总 | 可参考共享后台采集和过滤。GPU 实例按进程归并后相加，不能直接把这种总和当作任务管理器式整卡占用；[文档也将 Index=0 定义为实例总和](https://github.com/rainmeter/rainmeter-docs/blob/master/source/manual/plugins/usagemonitor.html) |
| [psutil `76805f58`](https://github.com/giampaolo/psutil/blob/76805f580474b5f5766e44ae3dd5aaf2d97cea7f/psutil/arch/windows/cpu.c) | Windows CPU 使用系统时间接口；[内存](https://github.com/giampaolo/psutil/blob/76805f580474b5f5766e44ae3dd5aaf2d97cea7f/psutil/arch/windows/mem.c)、[逐网卡统计](https://github.com/giampaolo/psutil/blob/76805f580474b5f5766e44ae3dd5aaf2d97cea7f/psutil/arch/windows/net.c)、[磁盘](https://github.com/giampaolo/psutil/blob/76805f580474b5f5766e44ae3dd5aaf2d97cea7f/psutil/arch/windows/disk.c)也封装系统接口 | 适合参考指标边界及网卡独立计数。为原生 C++ 宿主引入 Python 封装没有必要，也不能靠它统一解决 GPU 传感器 |
| [PresentMon `e5024e21`](https://github.com/GameTechDev/PresentMon/blob/e5024e2152013f0ab9cb4aa4751af15ae7957be3/README.md) | ETW 帧事件分析；服务层另行汇集厂商硬件遥测 | 适合帧率、帧时间、渲染延迟诊断。ETW 图形事件与厂商 GPU 传感器是两类数据；完整服务不是桌面百分比卡片的必要依赖 |

许可证记录：System Informer 和 PresentMon 为 MIT，LibreHardwareMonitor 主体为 MPL-2.0，psutil 为 BSD-3-Clause，Rainmeter 为 GPL-2.0，TrafficMonitor 使用 Anti-996 文本。具体文件和第三方依赖仍以各仓库许可证为准；本次只参考接口及算法，没有复制这些项目的实现。

## 3. GPU 可选采集路径

“最忙引擎时间”“厂商 GPU Core 活跃率”“显存容量占比”“显存控制器忙碌率”是不同指标。后端切换不能静默把它们写进同一个百分比字段。

| 路径 | 覆盖与指标 | 权限、依赖与维护成本 | 对 SnowDesktop 的判断 |
| --- | --- | --- | --- |
| Windows PDH GPU Engine + GPU Adapter Memory | 跨 AMD / NVIDIA / Intel，取决于 WDDM 驱动和计数器可用性；引擎负载、整卡专用/共享显存 | 一般可在普通用户下读取，无额外硬件监控驱动；需处理预热、实例变化、错误状态和聚合 | **当前默认方案**。先校正口径，并为失效数据提供明确状态 |
| PerfLib V2 原始计数器 | 与性能计数器体系相连，System Informer 用其收集 GPU 信息 | 自行处理 counter set、二进制布局、时间基准和格式化；代码量较大 | 只有实测证明 PDH 开销或稳定性构成瓶颈，再评估替换；尚未做两者性能对比 |
| D3DKMT 节点时间与显存统计 | 跨厂商 WDDM 节点，可直接对 running time 差分 | 系统/驱动结构兼容维护较重；[微软仍将 QueryStatistics 标注为系统保留接口](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtquerystatistics) | 适合诊断对照或经过版本验证的后备路径，不应仅因开源项目采用就优先替换公开计数器接口 |
| AMD ADLX | GPU usage、温度、功耗、频率、VRAM 等，按设备逐项探测 | C/C++ 接口，依赖兼容的 AMD 驱动；SDK 有自己的许可文件 | **AMD 扩展首选候选**。[GetSupportedGPUMetrics / IsSupportedGPUUsage](https://gpuopen.com/manuals/adlx/programming-with-adlx/adlx-samples/cplus-samples/performance-monitoring/perfgpumetrics/) 成功后再启用；RX 6800 / RX 6750 不能仅凭型号假定所有指标可用 |
| AMD ADL / Overdrive / PMLog | 老一代及现有 AMD 监控实现常用，LHM 仍有多条回退路径 | 多代接口、支持标志、采样窗口与空值处理较复杂 | 作为旧驱动补充。新增实现先评估 ADLX，再按实际缺口决定是否加入 ADL |
| NVIDIA NVAPI / NVML | 厂商 GPU 活跃率、温度、频率、功耗等 | NVIDIA 驱动依赖，消费卡与 WDDM 模式下逐项探测；NVML 利用率具有自己的采样窗口和定义 | 可选 NVIDIA 后端。[NVML 的 GPU 活跃率](https://docs.nvidia.com/deploy/archive/R510/nvml-api/structnvmlUtilization__t.html)与 Windows 最忙引擎保持分开 |
| Intel IGCL | 引擎/全局 activity、频率、温度、功耗等，依赖设备支持 | [运行库随驱动分发，telemetry 当前限 64 位应用](https://github.com/intel/drivers.gpu.control-library/blob/b6c462933502e13d1537dd5024949a51be30e63d/README.md) | 可选 Intel 后端；独显和核显分别验证，失败时保留 Windows 数据 |
| LibreHardwareMonitor 整库 | 多厂商 GPU、CPU/主板温度、风扇、存储传感器等 | .NET 集成或独立进程；[部分传感器需要管理员权限](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/blob/dc51e75bd97b15ce17ded0885e67bad47be0765b/README.md)，底层依赖按模块核对 | 适合后续可选传感器进程，当前基础指标没有整体替换收益证据。OpenHardwareMonitor 同属硬件传感器库路线，本文未对其当前版本做逐文件审计 |
| WMI / CIM 性能类 | 硬件清单、部分性能计数器的另一层访问方式 | COM/WMI 服务依赖；查询成本和失败边界需测量 | 适合硬件发现、低频兼容补充；包装同源计数器不会自动解决聚合错误 |
| ETW / PresentMon | 帧率、帧时间、图形队列事件与延迟；遥测可另接厂商 API | trace session、事件处理及权限管理；PresentMon 要求相应日志用户组或权限 | 适合按需诊断，不作为每秒基础资源卡片的默认路径 |
| DXGI `QueryVideoMemoryInfo` | 调用进程的显存使用和预算 | 接入简单，但[CurrentUsage 是当前应用用量](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/ns-dxgi1_4-dxgi_query_video_memory_info) | 适合 SnowDesktop 自身显存诊断，不能代替整卡显存占用或 GPU 利用率 |
| 外部命令、HWiNFO / Afterburner 等桥接 | 已安装工具提供的指标；具体来源由工具决定 | 外部安装、版本、进程/共享内存协议和许可依赖 | 可供用户选择集成；不作为便携版必需项。这些工具不能统称为开源采集方案 |

ADLX 允许按接口能力适配新旧驱动，但旧设备/驱动可能只能获得部分功能；应遵循[官方兼容说明](https://gpuopen.com/manuals/adlx/programming-with-adlx/adlx-programming-guide/specifications/compatibility/)，逐项记录支持状态。SDK 提供源码或示例不代表驱动运行库具有相同许可证。

## 4. 现有监控指标逐项判断

源码入口：[共享系统采集器](../src/widget_system_data_provider.cpp)、[系统监控组件](../widgets/system-monitor/main.lua)。以下“风险”是源码和接口语义判断，除 GPU 记录外未进行相应异常场景复现。

| 指标 | 当前方式 | 判断与建议 |
| --- | --- | --- |
| CPU 总占用 | `GetSystemTimes` 的 kernel/user/idle 差分 | 保留低成本系统接口。该指标是忙碌时间；若要与频率归一的性能百分比对齐，应另定义指标。超过 64 逻辑处理器时需要处理 processor groups |
| 逻辑处理器数量、进程 CPU 分母 | `GetNativeSystemInfo` | 核对多 processor group 场景；跨组数量可参考 psutil 的 `GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)`，同时保证总时间和进程时间采用一致的范围 |
| 物理内存、提交内存 | `GlobalMemoryStatusEx` + `K32GetPerformanceInfo` | 保留。物理可用内存和 commit headroom 各自展示，不能把提交额度当物理 RAM；内存百分比取整属于接口精度差异 |
| 进程摘要 | Toolhelp 枚举 + `GetProcessTimes` + `GetProcessMemoryInfo` | 可作为有限权限下的可观测进程集合；受保护进程可能缺失。大量进程时应测量开销，再比较一次性系统快照方案 |
| GPU 利用率 | PDH GPU Engine | 已保存最忙引擎候选；需 AMD 原场景验收。后续区分“0%”与计数器没有有效样本，并检查 adapter 热插拔后的身份稳定性 |
| 专用/共享显存 | DXGI 容量 + PDH GPU Adapter Memory 实际用量 | 采集方向保留。组件多 GPU 汇总目前用量相加、容量取最大值，分子分母范围不一致，应改为按卡显示或采用一致的聚合范围 |
| 网络连通性、计费状态 | Windows 网络连接状态接口 | 保留；连通状态、网卡 link 与互联网可达性各自有不同语义，不能由“流量为零”推断断网 |
| 网络速率 | `GetIfTable2`，过滤后累计字节合计，再按时间差分 | 改为按 InterfaceLuid 保存基线，先逐网卡差分，再汇总。提供明确的网卡选择口径，避免把 VPN、虚拟接口与物理承载重复计入 |
| 卷容量 | `GetLogicalDriveStringsW` + `GetDiskFreeSpaceExW` | 保留；当前跳过部分易阻塞来源有价值。挂载目录、卷重复身份及移动介质慢查询需要独立验证 |
| 磁盘读写速率 | `PhysicalDisk(_Total)` 的 Bytes/sec | 适用于系统总吞吐；若要显示某个盘，应保持容量、吞吐和忙碌率的设备对应关系 |
| 磁盘忙碌率 | `PhysicalDisk(_Total)\% Disk Time` 后截到 100% | 应重新定义为逐盘 active time 或其他明确汇总。建议基于逐盘 `100 - % Idle Time`，不要把排队时间比例直接称为忙碌时间 |
| 电池、电源、节电状态 | `GetSystemPowerStatus` | 保留，正确表达无电池和未知剩余时间。若要健康度、循环数或多电池明细，再扩展 Battery API/设备查询 |
| 温度、功耗、风扇、频率、SMART | 当前基础监控没有统一公开这些指标 | 作为可选扩展，通过厂商 SDK 或独立传感器服务采集，先测休眠 GPU 唤醒、权限和稳定性 |
| 显示器、默认音频、媒体会话 | 各自 Windows 设备/会话接口 | 属于相邻共享数据源，保持原有能力边界；硬件传感器库不能替代它们 |

[GetSystemTimes 官方说明](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getsystemtimes)限定了超过 64 处理器时的分组范围。[GetIfTable2](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/getiftable2)同时返回逻辑和物理接口。微软的[磁盘性能指南，第 64 页附近](https://download.microsoft.com/download/9/b/2/9b205446-37ee-4bb1-9a50-e872565692f1/perftuningguideserver2012r2.pdf)说明 `% Disk Time` 与队列长度有关，可超过 100%；`% Idle Time` 也必须结合 RAID、虚拟盘和汇总对象解释。

## 5. 修正顺序与关闭条件

| 编号 | 优先级与状态 | 具体问题、证据和关闭条件 |
| --- | --- | --- |
| MON-001 | 高；候选完成、AMD 待实机 | GPU 跨引擎求和偏高。生产位置 `SampleGpu`，已增加回归和负向对照。RX 6800、RX 6750 对照通过后再记录场景验证 |
| MON-002 | 高；口径问题、待场景验证 | `SampleStorageIo` 的 `% Disk Time` 不等价于有界 active time。需要明确每盘/总计口径，并用并发 I/O、多盘场景验证 |
| MON-003 | 高；代码推导风险 | `SampleNetworkTraffic` 先合计后差分。新增网卡若带有 1 GiB 历史累计值，会被当作当前采样窗流量；网卡消失可能使合计倒退。逐网卡基线、重连和计数器重置回归通过后关闭 |
| MON-004 | 中；聚合范围不一致 | `summarizeGpu` 专用显存用量 sum、容量 max。两张 8 GiB 卡分别用 4、5 GiB，会形成 9/8 GiB。按卡或一致总量展示后验证多 GPU 与 UMA |
| MON-005 | 中；已知接口边界 | CPU 总量与进程归一使用 group 范围，超过 64 逻辑处理器须验证。关闭条件是跨组机器上的数量、分母和忙碌时间一致 |
| MON-006 | 中；代码推导风险 | `SampleGpu` 获取数组成功即可认定占用可用；全部实例无效/未匹配时可能落为 0。设备按枚举序号生成 id，热插拔可能重排。需有效性和身份回归 |
| MON-007 | 中；待测量 | 系统监控 `setup` 订阅各项来源，隐藏单张卡片与整个组件不可见不是同一件事；共享采集线程上的慢查询也可能影响其他来源。先测启用/停用和延迟，再决定取消订阅或分离任务 |

MON-002 至 MON-007 作为评估发现保留，尚未修改相应生产行为。测试通过不会将这些问题自动关闭。

## 6. 接入和验证建议

基础数据继续由现有 broker 共享、按订阅采样。为每个设备、每项指标内部记录来源、时间戳、有效性和预热状态；错误时呈现不可用，避免沿用过期值或伪装成零。GPU 身份使用完整 LUID 和物理节点信息，厂商设备映射另做校验。采样时间差使用单调时钟。

厂商扩展先作为可关闭的独立采集任务或进程，避免 SDK 阻塞影响 CPU、网络等基础数据。仅启用有订阅的传感器；卸载后释放会话，检测查询是否唤醒休眠独显。此处是架构建议，尚未实现，也没有后端开销排序或节省比例实测。

若公开厂商活跃率、温度等新字段，应先确定单位、设备范围和失效语义，再增加相应 feature/capability；旧宿主缺少能力时组件隐藏该项。当前 `usagePercent` 校正保留字段、范围和订阅协议，不提高 `apiVersion` / `minHostVersion`；依赖旧偏高数值设阈值的调用方仍可能观察到变化。

AMD 验收建议使用 RX 6800、RX 6750 各一台，记录 Windows 构建、驱动版本、HAGS 状态与显卡身份。空闲、桌面动画、硬件解码、3D 游戏、并发复制各采集同窗数据，至少包含原反馈场景。对照 SnowDesktop、PDH 分引擎、任务管理器；ADLX/AMD 软件另列厂商口径，不要求与最忙引擎逐点相等。驱动重启、休眠恢复、计数器缺失、GPU 热插拔和订阅恢复分别检查预热及失效状态。

成本验证应分开记录采集耗时、CPU 时间、分配量、查询实例数、采样迟延和 GPU 唤醒情况，比较只有基础卡片与启用厂商传感器的差异。PDH、PerfLib、D3DKMT 的替换决策以这些测量为依据。

## 7. 本次候选验证记录

- 代码提交：`cb3f46527d085edfeb0317d638b07a121ca9fef4`，类型 `try(gpu)`。
- `scripts/test.bat name "^widget_system_data_provider$"`：定向 Release 编译及 1/1 测试通过。
- `scripts/build.bat --reload-shell`：退出码 0，标准 Release 构建 43.92 秒，确认 `.build/Release/SnowDesktop.exe` 生成。
- `scripts/test.bat full`：退出码 0，120/120 自动测试通过；配置 1.57 秒、测试目标编译 21.74 秒、CTest 98.84 秒。手动 Shell 文件操作诊断不在该集合内。
- 全量第一次尝试被运行中应用的预检拦截，未开始构建或测试；关闭实例后的正式运行取得上述结果。
- 隔离副本对同一组 GPU 用例恢复三种错误：跨引擎求和、同引擎只取进程最大值、忽略物理 GPU 编号。三者均编译通过并以断言失败退出 1；正确实现退出 0。
- 完整构建、定向/全量测试构建和负向对照编译日志未发现编译器或链接器警告；构建脚本的 Shell 重载提示不是编译警告。
- 日志、哈希和环境记录保存在本机 `.codex-probes/gpu-usage/`，JUnit 记录为 `.build/Testing/test-run-55b05d69746b4bd9ac8e493801fd763a.xml`。GPU 修改后的源码和测试已绑定哈希；未提交的既有审计文档未混入代码提交。
- RX 6800、RX 6750 原场景待用户实机对照；候选不宣称 AMD 兼容性问题已经解决。
