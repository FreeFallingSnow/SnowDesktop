# 2026-09-22 22:39 启动 Shell 等待复验

最新开发版仍复现约 39 秒的 Shell 等待，本地首图优化尚未通过整体启动验收。
本次采样到等待期间的左键消息快速返回，不能把后台等待全部认定为主线程连续冻结；
也不能用这些采样否定此前用户反馈的点击、拖动和右键失去响应。

## 运行身份与证据

- 启动时间 `2026-09-22 22:39:51.052`，PID `42320`，run `42320-208865812`，
  主线程 `37552`。进程路径为 `D:\Code\Other\SnowDesktop\.build\Release\SnowDesktop.exe`。
- 采集时 HEAD 为 `0ccf38bd69363a0129bea312be929e0d45b4d84d`，产物修改时间
  `22:36:22.030897`，EXE SHA256 为
  `b8b3bd7291eb6899743a34ae0515091ac9f5813f1fecb687082a37786472951b`，
  与 `.codex-probes/20260922-uac-foreground/final-hashes.json` 的构建产物记录相符。
- 本地首图尝试 `c70a0c4218fc5df3b1c909d2a0338158c6ad2621` 至上述 HEAD 之间，
  `app_icon_loader.cpp`、`shell_icon_work.h`、`shortcut_icon_resource.cpp` 没有新提交。
- 原始 UTF-16LE 日志、进程身份、解析收据和消息配对结果保存在
  `.codex-probes/startup-runtime-20260922-2242/`，采集时间 `22:42:03.103267+08:00`。
  本轮启动位于归档 `SnowDesktop.log` 第 358 行，没有混入 `.1` 中较早的运行。
- 归档当前日志 SHA256：`b584fb5bc19b4a1adcee1c61db339314de1458bc6285475da51bec16d3efd8ef`；
  `.1`：`bf0cd108d1d18b7928fb5b3baf79e4ba30a495ad3ac67a78781fd8334dce3764`。

## 启动耗时

首帧 `1265 ms`，桌面交接 `1468 ms`。174 次本地首图请求中，97 次得到位图、77 次
需要回退；本地读取最大 `16 ms`，排队最大 `156 ms`。这是后台请求统计，不是唯一条目
数量，也不能直接证明这些位图已经呈现给用户。

| 对象 / 通道 | 慢调用 | 耗时 | 当前日志行 |
| --- | --- | --- | --- |
| 完整桌面，第 19 次请求 | `IEnumIDList::Next` | 39156 ms | 1038 |
| 下载.lnk，首图回退线程 51788 | `IPersistFile::Load` | 38454 ms | 1041 |
| 图片.lnk，首图回退线程 5308 | `IShellItemImageFactory::GetImage` | 38516 ms | 1056 |
| Adobe Media Encoder / AfterFX，分类 | `SHCreateItemFromParsingName` | 各 38797 ms | 1039 / 1040 |
| Adobe Media Encoder / AfterFX，细节 | `SHGetFileInfoW` | 38828 / 38843 ms | 1049 / 1055 |

下载和图片再次占满两个首图回退线程，后续文档请求仅排队就达到 `39250 ms`。
不同通道的底层调用在 `22:40:30.951–22:40:31.024` 集中返回，支持共同 Shell 依赖的
推测，但尚不能确定具体服务、扩展、锁或网络目标。第 19 次请求也不能确定将返回哪个对象。

完整桌面读取共 `40266 ms`，于 `22:40:31.911` 结束；最终快照于 `22:40:32.667` 应用，
应用本身耗时 `203 ms`。当前主要长等待仍发生在 Shell 读取和回退队列。

## 等待期间的交互证据

本轮配对到 77 组消息进入 / 返回日志，其中 45 组发生在 Shell 长等待期间；
该期间配对消息的单调时钟耗时最大 `47 ms`。4 次 `WM_LBUTTONDOWN` 的日志范围为：

| 点击时间 | 日志行 | 日志时间差 | 单调时钟差 |
| --- | --- | --- | --- |
| 22:39:55.510 | 909 → 929 | 48 ms | 47 ms |
| 22:40:00.079 | 945 → 962 | 19 ms | 31 ms |
| 22:40:00.397 | 977 → 980 | 4 ms | 0 ms |
| 22:40:08.694 | 1002 → 1015 | 8 ms | 0 ms |

两种时钟的分辨率不同，`0 ms` 不表示处理没有成本。期间也有绘制和成功的
`composition-commit` 记录。这些证据表明采样点的主线程仍能处理消息，不能支持
“整段 39 秒连续主线程卡死”的判断；消息返回也不等于操作达到用户期望的结果。

交互追踪每次仅覆盖 2 秒、最多 64 条窗口事件和 12 条呈现记录，不是全量消息监控。
本轮没有拖放和右键功能验收，也没有冻结现场的线程栈。采集时长等待已经结束，
没有再次对恢复后的进程附加调试器，也没有启动或操作桌面宿主。

本轮只读取日志、源码和进程身份，并记录复验结论；未修改生产代码，未运行新的构建或测试。
整体慢启动验收仍失败。后续需要减少文件夹等首图对 Shell 的依赖，并继续定位完整枚举
的长等待；仅增加回退线程不能证明能够消除共同依赖的阻塞。

## English

The development host started at 22:39:51.052 and still reproduced approximately 39 seconds of
Shell waiting. Its executable hash matches the recorded artifact for the inspected HEAD. Of 174
local first-image requests, 97 produced bitmaps and 77 needed fallback; local reading took at most
16 ms and local queueing 156 ms. These are worker results, not proof of presentation or acceptance.

Downloads and Pictures occupied both fallback workers for 38.454 and 38.516 seconds. Desktop
enumeration waited 39.156 seconds on its nineteenth Next request. Classification and metadata
calls waited about 38.8 seconds and returned together, suggesting an unidentified shared Shell
dependency. Full enumeration took 40.266 seconds; first frame took 1.265 seconds and final
snapshot application itself took 203 ms. Overall slow-startup acceptance remains failed.

During the Shell wait, 45 sampled message pairs returned within 47 ms on the monotonic clock,
including four left-button-down messages with wall-clock spans of 4–48 ms. Presentation commits
also occurred. This contradicts a continuous 39-second UI-thread freeze at those sampled points,
but does not establish successful click behavior, dragging or right-clicking, or disprove earlier
user-reported freezes. Sampling is limited to two seconds and bounded event counts per interaction.
No in-freeze stack was captured; the already recovered process was not attached or operated.

This verification changed only documentation. No new production changes, build or tests were run.
