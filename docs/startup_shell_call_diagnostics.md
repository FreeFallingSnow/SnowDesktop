# 启动 Shell 慢调用诊断

针对 2026-09-22 14:22:48 开发版启动中约 45 秒的首图提取等待，新增
`Shell call slow:` 记录，细分原来的 `bitmapMs`、`classifyMs` 和桌面读取总耗时。
这组埋点用于定位阻塞位置，不改变取图顺序、线程数、缓存或超时策略。

## 覆盖位置

- `icon.phase1` / `icon.phase2`：系统图标索引、PIDL 解析、文件夹属性；
  快捷方式创建/加载/图标位置/目标路径；资源文件属性和 `SHDefExtractIconW`；
  `SHBindToParent`、`IExtractIconW`；ImageFactory 图标/缩略图；图像列表和位图转换。
- `icon.shortcut`：快捷方式加载、属性存储、ShellItem 属性、目标 PIDL/名称/类型查询。
- `desktop.full` / `desktop.local`：桌面绑定、已知文件夹路径、命名空间注册信息、
  `EnumObjects`、每次 `Next`、名称/路径/属性/显示元数据及增量发布。
- `desktop.local-user` / `desktop.local-common`：进入本地目录读取前的隐藏项设置和目录路径查询。

## 日志字段

| 字段 | 含义 |
| --- | --- |
| `process`, `thread` | 执行调用的进程和线程 |
| `trace`, `parentTrace` | 本次后台任务的编号及嵌套读取的父任务编号 |
| `kind` | 首图、细节、分类或桌面读取阶段 |
| `call`, `parentCall` | 当前任务内的调用编号和父调用编号 |
| `step` | API 或操作名；`total` 是任务总耗时 |
| `item` | 桌面枚举中正在请求的序号，从 1 开始；非枚举任务为 0 |
| `start_tick`, `elapsed_ms` | 调用开始的系统单调时钟值、持续毫秒数 |
| `calls` | 当前任务已进入的被计时调用数，不等于慢调用数 |
| `path` | 图标任务的源路径或桌面源目录 |
| `detail` | 已知时附加资源文件路径或桌面项解析路径 |

原有 `Shell icon slow` 和 `Shell shortcut slow` 增加 `trace`，可以直接关联到
同一次任务的细分记录。`queueMs` 仍独立反映线程池排队，不算某个 API 的执行时间。

## 读取方式与边界

1. 以同一次 `Run start` 为范围，筛选 `Shell call slow:`，先看耗时最长的叶子调用。
2. 相同 `trace` 下的父子调用耗时有包含关系，不能相加。`Bitmap.GetHighResolution`、
   `Link.SourceIcon` 和具体 API 同时记录约 45 秒时，通常是同一段等待。
3. 比较不同线程的 `start_tick + elapsed_ms`，判断是否同时恢复；同时恢复只能提示
   共同依赖，不能直接认定某个 Shell 扩展、锁或网络位置是根因。
4. `Enum.Next` 在拿到下一项之前可能阻塞，此时只有源目录和请求序号，不能把上一项当作
   被阻塞的对象。虚拟桌面源的目录为空，使用 `kind=desktop.full` 区分。
5. 单次调用或整个任务达到 250 毫秒才写日志，正常调用不写盘。计时精度沿用
   `GetTickCount64`。日志写入开销可能计入外层调用和任务总耗时。
6. 慢调用记录在调用返回或异常展开时写入。持续未返回、进程被强制终止时，不能仅凭没有
   细分日志判断调用未执行；需要线程栈等另外的证据。
7. 未单独包裹的析构/释放等开销仍可能只体现在外层调用或 `total` 中。此次埋点不能证明
   启动等待已解决，须由用户用本次构建重现并检查新日志。

自动回归放在现有 `application_restart_policy` 测试中，只替换时钟和日志落盘端，
覆盖阈值过滤、嵌套关联、线程隔离、异常退出以及原调用返回值/Win32 错误保持。
这些检查验证埋点本身，不替代真实 Shell 阻塞场景。
