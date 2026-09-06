# 探针模式验证记录

验证对象：`f6304bd4`。2026-09-06，Windows 11，Release 实际宿主 PID 17792。
可执行文件 SHA-256：`F24226158DA44008605A489F5CF55FDF0A74F96E4A8DBB4D25AE4879763F107B`。

`scripts/build.bat --reload-shell` 通过，`scripts/test.bat full` 115/115 通过。
构建仍有既有 WinUI `GetCurrentTime` C4002 和 `snapshot` C4456 警告。
控制窗口夹具验证了 CLI 默认 summary、trace、trace+CPU、旧 v1 start、主动 stop、
超时关闭、report 和跨配置 compare；默认及结束状态均为 idle。

## 固定工作量

执行已有诊断程序的 `--performance-overhead`，每种配置三轮，每轮计时 40000 个
嵌套作用域。结果在 `artifacts/v1.0.5.0/performance/probe-overhead-modes/`，
`benchmark.jsonl` 保存逐轮记录。中位经过时间：

| 配置 | 毫秒 |
|---|---:|
| 关闭 | 0.0444 |
| summary | 3.6115 |
| trace | 14.2648 |
| trace + ScopeCpu | 34.1744 |

summary 的计时循环比 trace+CPU 短约 89.4%。这是固定嵌套调用的观测开销结果；
不包含采样器初始化和报告导出，也不是应用 CPU/GPU 降幅。

## 耦合运行

同一次宿主启动中，按 summary → trace → trace+CPU，再反向轮换；每次采集
10 秒，结束后外部读取 5 秒进程累计 CPU。没有操作桌面宿主进行视觉验证。
当前音乐实例 `widget-133060937-33` 对应包
`20437e72-af2b-4e19-9f6b-30efb9bb5f6e`，仍与音频频谱等组件共同运行。

| 配置 | 进程 CPU 两轮 | 音乐绘制次数 | 音乐 frame 次数 | 报告字节 |
|---|---|---|---|---|
| summary | 1.593%、1.510% | 1108、1107 | 598、597 | 32487、32409 |
| trace | 1.677%、1.659% | 1109、1108 | 598、598 | 5920793、5915509 |
| trace + ScopeCpu | 1.737%、1.620% | 1108、1109 | 597、598 | 5894345、5904287 |

六次采集的 droppedEvents、droppedGroups、droppedLinks、probeErrors 全部为 0。
summary 有 162 组、无事件，资源和耦合组件耗时仍可读取；未查询的作用域 CPU
在 CLI 摘要中为 null。关闭探针时 CPU 为 1.429%～1.652%，与采集数值存在重叠，
因此不以这六次短样本宣称固定的应用 CPU 降幅。音乐仍约 111 次绘制/秒；探针
变轻没有消除组件的重复音频/动画刷新或背景模糊开销。

报告位于 `artifacts/v1.0.5.0/performance/probe-live-modes/`，`evidence.json`
记录所有 capture 的 SHA-256、原始次数和资源值。代表报告：

- `2-summary/capture.json`：`16b7bc66c791ed7925fab50586af9e60f11393ca0331b953f20287a990cf66a2`
- `2-trace/capture.json`：`a1c352280a8f165ebf2e4c68fc0569b1532da9eac842409cb607d58c676dac8f`
- `2-trace-cpu/capture.json`：`3d565206ff4f47da7be5eb9c747bc3a8c17dd35a4d481e35881f8c387a2bd12b`

这是探针功能和开销的验证记录，不是音乐绘制优化或桌面视觉/交互验收。
