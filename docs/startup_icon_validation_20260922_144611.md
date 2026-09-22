# 2026-09-22 14:46 启动 Shell 调用细分复验

`9830e4e1ab9186189ccda78f9e2b1f42a82f0954` 的细分日志在真实启动中生效，
已将首图的主要等待定位到 `IPersistFile::Load`，完整桌面读取的主要等待定位到
`IEnumIDList::Next`。启动长时间占位仍复现，不能据此将前面的性能尝试判为通过。

## 样本与身份

- 用户启动：`2026-09-22 14:46:11.326`，原始日志第 865 行。
- 宿主 PID `53256`，run ID `53256-180447687`；查询到实际进程路径为
  `D:\Code\Other\SnowDesktop\.build\Release\SnowDesktop.exe`。
- 源码 HEAD 为 `9830e4e1`；工作区仅有用户原有测试评估文档修改，没有原生代码差异。
- EXE SHA256：`7f6bc85b118c5d026d84a1473dcfa49a22a8cfe238933b0d1b6a9a0016c0aace`，
  修改时间 `14:42:34.231`，与细分日志提交的构建/测试收据中最终产物一致。
- 原始 UTF-16LE 日志、解析结果、进程路径和产物信息保存在本地忽略目录
  `.codex-probes/startup-shell-detail-runtime-20260922/`。
- 原始日志副本 SHA256：`5395be84ffcffd2c7ef4388c0cec95f29a89cd21c7312c2889e95527003e9093`。

## 主要等待位置

| 阶段和对象 | 具体调用 | 耗时 | 原日志行 |
| --- | --- | --- | --- |
| CC Switch 首图 | `IPersistFile::Load` (`Link.Load`) | 34766 ms | 1092 |
| Docker Desktop 首图 | `IPersistFile::Load` | 34718 ms | 1074 |
| Excel 首图 | `IPersistFile::Load` | 34640 ms | 1075 |
| KOOK 首图 | `IPersistFile::Load` | 34594 ms | 1093 |
| 完整桌面第 19 次请求 | `IEnumIDList::Next` (`Enum.Next`) | 34797 ms | 1073 |
| AfterFX 快捷方式分类 | `SHCreateItemFromParsingName` | 34422 ms | 1077 |
| Adobe Media Encoder 快捷方式分类 | `SHCreateItemFromParsingName` | 34406 ms | 1076 |
| 上述两个 Adobe 项的第二阶段元数据 | `SHGetFileInfoW` | 各 34500 ms | 1123、1127 |

`Link.Load` 对应 `src/utils.cpp` 的 `ExtractShellLinkSourceIcon` 中
`persistFile->Load(shortcutPath.c_str(), STGM_READ)`。这个函数先用 Shell 加载 `.lnk`，
然后才通过图标位置或目标路径提取资源。
四项 `Link.SourceIcon` 总耗时分别为 34781、34734、34656、34594 ms；扣除 `Load`
后，其余步骤共约 0–16 ms（含计时粒度和日志开销）。本次首图等待因此可以从笼统的
“位图获取”收窄到“加载快捷方式”，不应归因于后续 ImageFactory 或高分辨率资源提取。

四个慢首图任务对应四个不同线程，占满 `shell_icon_work.h` 默认的四个首图工作线程。
后续首图最大排队 35047 ms；两个元数据任务也占用两个细节线程，后续细节最大排队
34641 ms。日志中的首图慢任务 153 个、细节慢任务 170 个包含不同来源和重复阶段，
不是唯一桌面条目数。

## 其余启动时间

- 本地用户桌面 70 项、公共桌面 40 项，各读取 31 ms。
- 首批 113 项于 14:46:12.379 应用；首帧 1187 ms，桌面交接 1406 ms。
- 完整 Shell 桌面读取 36000 ms，于 14:46:47.879 结束。
- 最终完整快照应用 203 ms，于 14:46:48.909 结束。

`Enum.Next` 与 Docker/Excel 的 `Load`、Media Encoder 的解析调用均在单调时钟
`180482593` 附近返回；其他慢调用的返回值集中在之后 94 ms 内。枚举等待的开始时间
早于上述取图等待，但时序不能证明它持有阻塞其他调用的锁。

## 结论与边界

1. 细分埋点验收通过：同一次实际启动中已经获取可关联的具体 API、线程、文件及耗时。
2. 首图慢的直接原因已定位：首图通道仍同步等待 Shell 的快捷方式加载，四个并发任务
   全部等待时，其余图标没有可用工作线程。独立分类队列无法消除这类首图内部等待。
3. 完整桌面读取的慢点是第 19 次 `Next`；这代表请求序号，日志无法确定下一项的身份。
   不能将该序号或上一个枚举项解释为“某个坏快捷方式”。
4. 跨 API 的同时恢复支持“共同 Shell 依赖或串行等待”的推测，但具体内部锁、COM 服务、
   扩展或网络依赖仍未确认。本次没有阻塞期间的线程栈，不归责于上述应用本身。
5. 后续优化应优先评估首图缓存或直接读取本地 `.lnk` 图标字段，避免首图必须等待
   `IPersistFile::Load`；不能解析的项目应通过有资源边界的 Shell 回退处理。
   这只是针对已观察瓶颈的实现方向，本轮未实施或验证。
6. 约 35 秒与上次约 45 秒是不同运行样本；本轮只有诊断埋点，不能将差值当成已验证提速。

本轮仅读取日志、进程路径和源码并保存验收记录，未修改生产代码、未重启宿主，
未运行构建/测试或桌面 UI 自动化。上轮标准构建、定向测试 1/1、完整测试 120/120
是 `9830e4e1` 同一输入的已有证据，本轮没有重跑。

## English

The diagnostic instrumentation in `9830e4e1` worked in the user-run development host started at
14:46:11. Its binary hash matches the recorded candidate. All four first-image workers waited
34.594–34.766 seconds in `IPersistFile::Load`; the remaining shortcut resource work took about
0–16 ms at the existing timer resolution. Subsequent first-image queueing reached 35.047 seconds.
The full desktop reader waited 34.797 seconds on its 19th `IEnumIDList::Next` request. Adobe shortcut
classification waited about 34.4 seconds in `SHCreateItemFromParsingName`, while detail metadata
queries waited 34.5 seconds in `SHGetFileInfoW`.

Local desktop reads took 31 ms each, first frame 1.187 seconds, full desktop reading 36 seconds,
and final snapshot application 203 ms. Near-simultaneous returns suggest a shared Shell dependency
but do not identify a specific lock, service, extension, next item or application fault. The long
startup wait remains reproduced; the difference from the earlier 45-second sample is not proof
of improvement. A cache/direct shortcut-file path and bounded Shell fallback are prospective work,
not changes or validated results of this diagnosis.

This turn performed read-only inspection and recorded runtime validation. It did not change
production code, rebuild, rerun tests, restart the host or automate its desktop UI. The prior
standard build, 1/1 targeted tests and 120/120 full tests remain the same-input evidence for this
diagnostic candidate; they were not rerun here.
