# 2026-09-22 14:22 开发版启动图标等待复验

对 `cb537517daf353f08d3c98f67a4f99e282d1309b` 的原场景验收：**长时间占位仍存在，验收失败**。
本次首阶段的四个工作线程阻塞在位图获取阶段，后续图标等待约 45 秒。
分类已独立调度，但拆分队列没有为位图获取自身建立返回时限。

## 样本

- 用户运行 `.build/Release/SnowDesktop.exe`，宿主 PID `66748`，run ID `66748-179044406`。
- `Run start` 为 `2026-09-22 14:22:48.013`，原日志第 49 行。
- 源码 HEAD：`c696e13001a5ed9a0762c28215e61f06ad002a7c`，包含后续毛玻璃变更。
- EXE SHA256：`e9a7863f9737841c51cda2aa71df5554748af92f0de0023f574f322226d839b3`，修改时间 `14:17:14.706`，早于本次启动。
- 与 `cb537517` 比较，图标加载、结果应用、桌面枚举、工作队列、`utils.cpp` 取图和 Shell 读取调度源码均无差异。后续毛玻璃变更未修改上述路径。
- 原始 UTF-16LE 日志副本、完整任务统计、进程路径和哈希保存在本地忽略目录 `.codex-probes/startup-icons-latest-20260922/`；不将含用户路径的原始日志提交仓库。

## 耗时与等待位置

| 位置 | 原日志行 | 实测 |
| --- | --- | --- |
| 首次本地桌面读取 | 96–97 | 公共 40 条、用户 70 条，各 31 ms |
| 首批条目应用 | 201 | 113 条，14:22:49.109 |
| 首帧 / 桌面交接 | 202、243 | 1219 ms / 1344 ms |
| CC Switch 首阶段 | 287 | 排队 32 ms，位图获取 45578 ms，分类 0 ms |
| Docker Desktop 首阶段 | 281 | 排队 63 ms，位图获取 45516 ms，分类 0 ms |
| Excel 首阶段 | 282 | 排队 125 ms，位图获取 45454 ms，分类 0 ms |
| KOOK 首阶段 | 286 | 排队 219 ms，位图获取 45375 ms，分类 0 ms |
| AfterFX / Adobe Media Encoder 独立分类 | 284–285 | 加载 0 ms，分类各 45234 ms |
| 同两项第二阶段 | 312–313 | 元数据查询 45297 / 45312 ms |
| 期间触发的两个本地重读 | 267–280、283 | 各 35000 ms，等待发生在 `EnumObjects` 探针进入前 |
| 第一次完整 Shell 桌面读取 | 548 | 46906 ms |
| 最终完整快照应用 | 722、823–824 | 14:23:37.528 完成，应用 203 ms |

本次记录到首阶段慢任务 152 个，最大排队 45860 ms；第二阶段慢任务 171 个，最大排队 45469 ms。
这些是超过阈值的任务数，包含集合等来源，不是唯一桌面条目数量。
四个首阶段任务自身的慢耗时均落在 `bitmapMs`，`metadataMs` 和 `shortcutMs` 为 0；不能再将其归因于首阶段等待分类。

## 已定位与未定位

`src/app/app_icon_loader.cpp` 的首阶段会调用 `GetHighResolutionShellIconBitmap`，并执行颜色键处理。
在 `src/utils.cpp` 中，快捷方式会先尝试直接读取图标资源，失败后还会进入父目录绑定、`IExtractIconW`、`IShellItemImageFactory`、系统图像列表及 `SHGetFileInfoW` 等路径。
本次 45 秒落在这段取图计时内，尚没有内部各分支计时，不能断言已经走到某个回退分支，也不能确定具体慢 API。

本地重读于 14:23:34.317 恢复，四个首图于 14:23:34.328–.351 返回，两个分类于 .334–.335 返回；第二阶段元数据随后也恢复。
这提示多个调用可能等待同一 Shell 依赖，但“共享锁、COM 服务、Shell 扩展或离线路径”目前均只是可能性，没有阻塞线程栈或内部探针证据。
不把几个不同线程的等待相加，也不将 Adobe、Docker、Excel 等应用本身认定为原因。

完整桌面读取没有输出 `Shell desktop metadata slow`，说明新增的逐项 `SHGetFileInfoW` 慢查询日志未定位出其主要等待。
仍需检查 `Next`、名称/路径解析、属性读取，以及本地来源进入 `EnumObjects` 前的初始化和绑定。

上轮独立分类调度仍在使用，分类只在首阶段结果被主线程接受后开始；这一边界不等于每张图最终已绘制到屏幕。
现有回归覆盖“分类卡住但取图提供程序正常返回”，未覆盖“首阶段取图提供程序自身占满所有线程”的本次故障。

## 后续定位方向

先给取图分支和枚举子调用补充分段耗时，或在下一次实际阻塞期间取得稳定的线程栈，确定具体等待位置。
后续优化需要让慢 Shell 取图无法长期占满首图通道，评估真正独立的缓存/直接资源快路径及有明确超时和资源所有权的隔离机制；不能仅靠增加线程数或再拆分类队列。
所有结果仍需通过同一桌面场景验收。

本轮只读取日志、进程路径及源码并记录失败结论；未修改生产代码、未构建或测试、未重启宿主，也未自动化操作桌面界面。

## English

Runtime acceptance of `cb537517` failed again in the user-run development host started at 14:22:48. Four first-stage jobs (CC Switch, Docker Desktop, Excel and KOOK) spent 45.375–45.578 seconds inside bitmap acquisition, with zero recorded classification time. They occupied all four first-image workers; later first-stage queue delays reached 45.860 seconds. The first frame took 1.219 seconds and desktop handoff 1.344 seconds; full Shell desktop reading took 46.906 seconds, while final snapshot application took 203 ms.

Independent classification and second-stage metadata queries also took about 45 seconds. Two later local desktop reads waited 35 seconds before entering the enumeration probe. Their near-simultaneous recovery suggests a shared Shell dependency, but no specific lock, COM service, extension, file or internal API is established by the current timings. The existing tests cover blocked classification with responsive image providers, not saturation by blocked first-image providers.

This is a read-only diagnosis with archived log/process/binary evidence and unchanged relevant icon source relative to the attempt. No production code, build, tests, host restart or desktop UI automation were performed. Further per-call timings or a blocked-thread trace and original-scene runtime validation are required.
