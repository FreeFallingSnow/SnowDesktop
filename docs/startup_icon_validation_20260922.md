# 2026-09-22 开发版启动图标等待验证

对 `5a0467fcbf0eb9358dfb6b1c7eea9d30ad88bca2` 的结论：**原问题验收失败**。
本地条目已快速发布，但首阶段位图仍等待快捷方式分类，四个首阶段线程同时被占用约 54.4 秒。
本轮只读取用户运行的开发版日志和相关源码，不修改生产代码，不启动、重启或自动化操作桌面宿主。

## 样本与绑定

- 宿主：`.build/Release/SnowDesktop.exe`，宿主 PID `16616`。
- 启动日志：`.build/Release/data/SnowDesktop.log`，UTF-16LE；本次 `Run start` 为 `2026-09-22 10:00:55.001`，原日志第 1443 行，诊断 run ID 为 `16616-163331437`。
- 读取时 EXE SHA256：`7FE927DBFC595A2FC33738885154176D470745BBD9054D307367BBFAFAAF9E73`；与 `5dab88e9` 记录的当前开发版候选一致。它包含后续 Shell 启动变更，不是上一轮单独构建的候选哈希。
- `app_icon_loader.cpp`、`shell_icon_work.h`、`app_desktop_enumeration.cpp`、`app_quick_navigation_interaction.cpp`、`background_work.h` 与被验证尝试无差异。
- 完整日志副本、分段摘录、任务统计、读取时 HEAD/进程/源码哈希保存在本地忽略目录 `.codex-probes/startup-icons-20260922-runtime/`。原始日志包含用户路径，不加入仓库。

## 实际记录

| 事件 | 日志行 | 结果 |
| --- | --- | --- |
| 公共桌面读取结束 | 1484 | 40 条，16 ms |
| 用户桌面读取结束 | 1485 | 70 条，16 ms |
| 首批条目应用 | 1595 | 10:00:56.117，113 条；布局 265 ms、重建 78 ms |
| 首帧准备完成 | 1596 | 启动计时 1250 ms |
| 桌面接管完成 | 1601 | 启动计时 1516 ms |
| 四个首阶段快捷方式任务结束 | 1654–1657 | 均为 `queueMs=0 bitmapMs=0 shortcutMs=54437 totalMs=54437 bitmap=1` |
| 后续任务 | 1658 起 | 主要等待队列；首阶段最大排队 54875 ms |
| 完整 Shell 桌面读取结束 | 1991 | 113 条，55766 ms |
| 完整快照应用 | 2051、2169–2170 | 10:01:52.780 完成，应用阶段 282 ms |

四个阻塞任务的文件名是 `Adobe Photoshop 2025.lnk`、`Adobe Premiere Pro 2025.lnk`、`Adobe Media Encoder.lnk`、`AfterFX.lnk`。
`bitmapMs=0` 表示没有跨越计时器刻度，不表示提取操作绝对零耗时；`bitmap=1` 确认任务已取得位图。

本次启动后记录到首阶段慢任务 169 个、第二阶段慢任务 131 个；这只是超过日志阈值的任务数，不是唯一桌面条目数。
已完成慢任务中，任务自身处理超过 250 ms 的只有上述四个任务。其余首阶段慢任务主要被队列拖延；第二阶段最大排队 875 ms、总耗时 890 ms。

## 代码与等待的对应关系

`src/app/app_icon_loader.cpp` 的 `QueueIconTask` 先获取位图，再在 Phase1 执行快捷方式分类，最后才返回 `IconLoadResult`，由完成回调进入 `OnIconLoaded`。
因此，即使位图已经提取成功，`shortcutMs` 覆盖的分类尚未返回时也不能提交该位图，且当前工作线程一直被占用。

分类包括 `IPersistFile::Load`、`IsApplicationsShellLinkTarget` 和 `IShellLinkW::GetPath`。
`src/app/app_quick_navigation_interaction.cpp` 中的 `IsApplicationsShellLinkTarget` 还会访问属性存储、Shell 项目属性、目标 PIDL 的名称和 `SHGetFileInfoW` 类型信息。
当前日志只能定位到这段分类链，不能断言其中某个 COM/API 调用耗尽了 54.4 秒，也不能归咎于 Adobe 软件本身。

`shell_icon_work.h` 为首阶段保留四个线程、第二阶段保留两个线程。此次四个首阶段任务都卡在自身分类中，故隔离第二阶段并不能避免本次排队。
完整桌面读取同时达到 55.8 秒，但现有探针只细分到 `EnumObjects`，不能证明它与快捷方式分类等待共享同一个底层原因。

## 后续修正边界

优先让首阶段位图提交不依赖应用快捷方式分类，将分类作为独立补充结果，并保证其延迟不会覆盖新请求、已显示图标或错误改变快捷方式箭头。
补充分类内部和完整桌面逐条元数据调用的计时，进一步定位 Shell 等待。仅增加首阶段线程数无法消除已经取得位图仍不提交的问题。

回归需要控制分类任务不返回，验证真实图标请求路径仍能提交已提取的首阶段位图、继续处理后续图标，并覆盖分类迟到、取消和对象销毁。
上一轮工作池隔离测试仅覆盖第二阶段被占满；它没有覆盖首阶段自身还包含慢分类这一入口。最终仍需同一桌面启动实测，不以自动化测试代替可见结果。

本轮未运行构建或测试：仅新增诊断文档和验证记录，没有修改运行输入。上一轮测试通过不代表本次原问题验收通过；日志也不提供每个图标最终绘制到屏幕的时刻。

## English

Runtime validation of `5a0467fc` failed for the original long startup placeholder wait. Local desktop reads completed in 16 ms and 113 items were applied around 1.1 seconds after run start, but four Phase1 shortcut classification operations each took 54,437 ms after their bitmaps had already been obtained. They occupied all four first-stage workers; later tasks waited up to 54,875 ms in the queue. Full Shell desktop reading took 55,766 ms; applying that snapshot took 282 ms.

The source still returns the bitmap result only after shortcut classification. Separate first/detail worker pools do not protect against slow classification inside the first stage. Prioritize decoupling bitmap delivery from classification, and cover this production boundary with controlled slow-classification regression tests and the same real desktop scenario.

The exact internal COM/API call and any shared cause with full desktop reading remain unproven. Timing resolution and completion-only slow logs do not establish exact on-screen paint times. This is a documentation-only diagnosis of the user's running development host; no build, tests, restart or desktop UI automation was performed. Original evidence and candidate/source hashes are archived locally, outside version control.
