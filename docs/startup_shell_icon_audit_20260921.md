# 启动与图标 Shell 阻塞排查（2026-09-21）

前半部分保留原始排查证据；文末记录用户要求“全优化”后的实施和验证状态。

## 结论与证据边界

20:56:26 启动的宿主 `19236` 在 66,313 ms 后完成桌面交接。其中，
`Dock.FolderIcon.SHGetFileInfo` 单次耗时 63,844 ms，占交接总耗时约 96.3%。
该调用从 20:56:28.342 持续到 20:57:32.186，位于主线程 Dock 绘制链。
此前 20:31:11 的启动交接耗时为 3,484 ms，同类首次查询为 16 ms。

因此，本次一分钟等待的直接阻塞点已有日志证据。Shell 内部具体等待哪个提供程序，
现有日志没有记录；不能据此认定是某个第三方扩展、网络目录或特定文件损坏。

前述“桌面和组件内部已移出，只漏 Dock”的判断需要修正：后台位图提取已经存在，
但返回主线程后的路径解析、显式刷新、文件夹弹层和绘制回退仍保留同步路径。

下表中“代码风险”表示已经核对生产入口、调用链和线程边界，**没有**表示该路径在本机
已发生秒级阻塞。只有 SBI-01 具有本轮慢启动实测日志；不能将表中风险耗时相加。

源码基线为 `d96421ff10a9cbc2a5121e16f106c0fd53d601fe` 加审查时工作区内容。
工作区另有 Dock 动画等未提交修改，本次没有改动或暂存它们。
原始检索、源码哈希、Git 状态和带原始行号的日志摘录位于本地
`.codex-probes/startup-shell-audit-20260921-211505/`。该目录为诊断产物，不参与构建。

## 遗漏清单

| 编号 | 生产入口及位置 | 触发条件与影响 | 证据与处理方向 |
| --- | --- | --- | --- |
| SBI-01 | `DrawDockEntry`，`src/app/app_dock_render.cpp:308`、`:325` | 文件夹映射入口图标缓存未命中时，在绘制线程调用真实路径的 `SHGetFileInfoW`。`ReloadItems` 在 `app_desktop_reload.cpp:773`、`:820` 清空缓存，风险不只限于第一次启动。 | **已测到 63,844 ms**。绘制只读缓存或画占位；将真实图标查询放到后台，完成后刷新。不能只依靠缓存降低复发概率。 |
| SBI-02 | `RefreshDockRunningWindows → CreateDockWindowIconBitmap → CreateDockShellIconBitmap`，`app_dock_window_tracking.cpp:728`、`dock_platform_helpers.h:314`、`:401` | 未固定的运行中应用首次出现或图标尺寸不足时，主线程依次解析路径、查询系统图标、提取图标/资源。由启动完整快照应用及运行期定时器调用。窗口提供图标的 `WM_GETICON` 分支有 80 ms 单次等待，前面的 Shell 路径没有相同保护。 | 代码风险。运行中应用图标也需后台结果缓存；不能只修固定文件夹入口。此次最终快照中的整个运行窗口刷新为 31 ms，不能说本次也在这里卡了一分钟。 |
| SBI-03 | `DockMainEntryCount/IsFolderDockEntry → ResolveDockFolderTarget → item_location::ResolveFolderTarget`，`app_dock_model.cpp:31`、`:88`、`item_location.cpp:49`、`:55`、`:113`；另有 `ResolveDockAppIdentity`，`app_dock_launch.cpp:270` | Dock 布局分类、命中判断及运行窗口匹配在缓存未命中时同步读取快捷方式、解析目标并检查文件属性；虚拟应用另查 Shell 属性，Steam 项会读取安装信息。慢目标可在图标查询之外阻塞 UI。 | 代码风险。分类、身份和图标应分别有后台准备结果，失效时保留旧结果或未知状态；不可因超时认定目标已删除。`SLR_NOSEARCH/SLR_NOTRACK` 限制解析行为，不构成整条调用的时间上限。 |
| SBI-04 | `OnIconLoaded`，`app_desktop_reload.cpp:1232`、`:1246`；`RefreshIconBitmapResolution`，`app_icon_loader.cpp:458`、`:473` | 组件/文件夹弹层第一阶段位图回来后，UI 再调用 `SHParseDisplayName` 才排入第二阶段；尺寸变化也逐项同步解析。`app_run.cpp:1510` 在首帧前最多消费 256 个完成消息，因此首帧也可能进入此路径。 | 代码风险。复用已有独立 PIDL，或让后台任务自行解析；结果接收只做代际校验和模型更新。“最多处理 256 个”限制数量，未限制单次 Shell 调用时长。 |
| SBI-05 | `ReloadItems` 无快照分支、`EnumerateFolderMappingEntries` 无快照分支、`RefreshDockFolderPopup`，`app_desktop_enumeration.cpp:277`、`app_widget_refresh.cpp:110`、`app_popup_transition.cpp:654` | F5/菜单刷新、部分设置变更、创建/手动刷新映射组件、打开 Dock 文件夹弹层可同步枚举目录，并逐项 `SHGetFileInfoW + SHParseDisplayName`。`OpenDockFolderPopupAt` 在 `app_popup_lifecycle.cpp:366` 先同步刷新内容，再启动弹出动画。 | 代码风险。这些入口也须请求异步快照。初始 `LoadLayoutSlots` 的 `initialShellReadPending_` 守卫会跳过同步枚举，不能把启动保护误认为运行期入口也受保护。 |
| SBI-06 | `DrawPlaceholderIcon`，`app_icon_render.cpp:569`、`:606`、`:624`；`DrawQuickNavSysIcon`，同文件 `:653`、`:675`、`:686` | 绘制回退在缓存未命中时同步取系统图像列表、`GetIcon`，再转换位图并进行美化。普通占位仅在 `sysIconIndex < 0` 或初始枚举仍未完成时使用纯绘制；状态切换后会走系统图标分支。快捷方式箭头在 `:357` 首次同步取 stock icon。 | 代码风险，未测到慢调用。应保证占位和冷缓存绘制不触发 Shell 取图；预加载或后台生成可复用位图。stock icon 不访问目标文件，应与真实路径查询区别评估。 |
| SBI-07 | `ApplyQuickNavigationEverythingSearchResult → GetQuickNavigationEverythingIconIndex`，`app_quick_navigation_window.cpp:593`、`:405` | Everything 搜索虽异步，应用结果时仍在 UI 对 EXE、LNK、DLL、ICO、SCR、MSI、CPL 逐项查询真实文件图标。普通扩展名和目录分支使用 `SHGFI_USEFILEATTRIBUTES`，风险不同。 | 代码风险。结果先展示，再异步补具体图标；需拒绝查询变化或面板关闭后的旧结果。应用索引的独立后台线程不能覆盖 Everything 结果这条路径。 |
| SBI-08 | `StartIconLoader/StopIconLoader`，`app_icon_loader.cpp:240`、`:498`；Lua `AsyncShellIconLoader`，`widget_engine.cpp:615`；应用索引 `app_quick_navigation_model.cpp:170`；大图标 `large_icon_assets.cpp:615` | 普通图标单工作线程会被一个慢请求阻塞，后续图标排队；这些加载器在停止/析构时存在 `join`。停止标记只能在 Shell 调用返回后被检查，不能中断已经进入的同步调用。 | 代码风险，未复现退出挂起。需分别处理请求间隔离和宿主退出边界；将 SBI-01 简单塞进现有串行队列不能证明其他图标及退出均不再受阻。不能强杀线程，也不能让持有宿主裸指针的线程直接脱离生命周期。 |
| SBI-09 | `ReadSources`，`app/shell_refresh_snapshot.h:49`；`StartupRead`，`app/startup_shell_read.h:30` | 初始完整读取在一个任务中依次读取桌面、各映射目录和 Dock 路径；完整快照需等整个任务返回。桌面有增量发布与两个本地读取任务，映射目录没有同等的逐目录完成发布。慢目录可能推迟其他映射内容及完整状态。 | 代码风险，属于内容迟到，与 UI 卡死分别记录。后续优化需验证慢目录不阻止其他目录就绪，失败/未完成不得提交为空目录或触发数据清理。本次 70 秒左右才应用完整快照，现有日志不足以定位其后台子步骤。 |

## 已核对的后台边界

这些结论来自调用链审查，不代表本次重新执行了其历史测试。

| 路径 | 当前边界与限制 |
| --- | --- |
| 初始桌面枚举 | `StartInitialShellRead` 启动 Shell 和两个本地来源读取；`StartupRead` 的后台线程只持有独立状态，销毁不 join。UI 的 1,500 ms 预算只限制 `TakeReady`，不限制其后 `ReloadItems`、绘制与回调。 |
| Shell 通知后的自动刷新 | `app_shell_file_operation.cpp:651` 将快照读取排入 `shellRefreshWorker_`；UI 负责应用结果。其后布局/绘制仍能进入 SBI-01、02、03、06，不能用“读取异步”概括整个刷新。 |
| 映射目录订阅解析 | `shell_folder_notifications.h:222` 的两个后台 STA 工作者解析路径；UI 只收结果并注册。销毁不等待解析。`SHChangeNotifyRegister/Deregister` 仍在 UI；两个工作者同时阻塞时，新解析仍会等待。 |
| 普通桌面/文件夹内容位图 | `app_icon_loader.cpp:243` 后台执行高分辨率提取；桌面第二阶段复用 PIDL。组件第二阶段的 UI 解析例外见 SBI-04，通用绘制回退见 SBI-06。 |
| 快捷导航应用列表 | `app_quick_navigation_model.cpp:141` 在线程中构建 AppsFolder 索引、提取应用图标；Everything 补图和绘制回退是另外两条路径。 |
| Lua Shell 图标 | `widget_engine.cpp:679` 在线程内完成路径解析和取图；这是 Lua 引擎的请求路径，不等于原生文件夹映射组件所有步骤均异步。生命周期限制见 SBI-08。 |
| 大图标资产 | `large_icon_assets.cpp:612` 工作线程执行图片/资源/Shell 提取；UI 请求结果。停止仍等待工作线程，见 SBI-08。 |
| 首次导入 Explorer 布局 | `InitializeGridFromWindows` 只在无已有布局等条件下调用；`windows_desktop_layout.cpp:118` 后台捕获，默认等待预算 2,500 ms，超时返回默认布局。与无限同步 Shell 取图区别记录。 |

## 邻接入口与范围限制

- `UpdateCutState` 在刷新链上同步 `OleGetClipboard/GetData`
  （`app_item_sorting.cpp:334`、`:344`、`:362`）。本次记录均为 0 ms，未构成当前主因；
  剪贴板提供方慢响应仍应有独立诊断，涉及 OLE/STA，不能直接把 UI COM 对象交给普通线程。
- 托盘非系统桌面图标菜单调用 `LoadDesktopNamespaceRegistrations`
  （`app_tray.cpp:138 → desktop_namespace_registry.cpp:231`）时也会同步解析 Shell 命名空间。
  初始枚举中同一 helper 可在线程内执行，必须按调用者区分。
- 调试配置启用时，`RegisterShellChangeNotifications` 在 `app_desktop_reload.cpp:140`
  还会同步解析模拟桌面目录。审查时配置为 `enabled=false`，不是这次阻塞原因。
- 启动日志的 TaskScheduler `0x800401F0` 是另一已观察错误：
  `app_run.cpp:353` 初始化设置、查询自启动，发生在 `:360` 的 `OleInitialize` 前。
  本次错误很快返回，不解释一分钟等待。
- 原生 Shell 菜单、数据对象、拖放目标的绑定/调用仍有同步接口。本次识别其入口，
  没有审计整个 OLE 协议或承诺可任意换线程；涉及线程单元、消息循环、对象寿命和交互顺序，
  后续应按独立行为范围处理。
- 本次覆盖启动与图标相关 Shell 查询、绘制回退、结果应用、显式刷新和加载器停止路径。
  这不是所有 UI 磁盘 I/O、Lua 执行、图片解码、网络请求、驱动或系统服务的全量性能审计。
  关键 API 检索是发现入口的方法，不是“所有代码实际执行且无阻塞”的证明。

Microsoft 明确建议在后台调用
[SHGetFileInfoW](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shgetfileinfow)
和 [SHParseDisplayName](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shparsedisplayname)，
避免阻塞界面。`SHGFI_USEFILEATTRIBUTES` 不访问所给目标文件；这不等于整个 Shell API 具有严格返回时限。

## 后续修改与验收清单

1. 先封住绘制、布局分类、结果回调中的 Shell/目标路径查询：SBI-01、02、03、04、06。
   UI 只消费已有快照、位图或占位；冷缓存与缓存失效必须覆盖。
2. 将显式刷新、组件创建/刷新、弹层打开和 Everything 补图接入相同的结果校验约束：SBI-05、07。
   异步失败保留旧内容；完成前删除、移动或切换对象后，旧结果不能写回。
3. 独立审查后台慢请求之间的隔离、完整快照发布和退出等待：SBI-08、09。
   设线程数上限，避免为每个卡住请求无界创建线程；停止等待与在途任务资源所有权同时设计。
4. 验证使用生产入口和受控 Shell 边界阻塞，分别覆盖冷启动、完整/部分初始快照、
   F5、文件通知刷新、Dock 运行应用变化、映射弹层、图标尺寸变化、Everything 查询变化及退出。
   阻塞一个查询时，检查 UI 调用是否及时返回、无关结果能否应用、旧代际结果是否被拒绝。
5. 测试数据包含本地目录、失效快捷方式、离线 UNC/映射盘和返回失败；
   不访问真实用户文件做破坏性测试。先确认错误实现会触发正确的失败信号，再验证修改。
6. 后续原生跨模块/异步行为修改按仓库规则完成标准构建和稳定候选全量测试。
   桌面启动、Dock 与弹层实机反馈交给用户或使用稳定等价证据，不能以构建成功代替验收。

上述内容为修改前的审计结论，不能作为后续实现的验证证据。

## 实施记录

实现基线为 `adb64f29`。本任务保留用户已有的
`docs/testing_audit_product_issues.md` 修改，不将其纳入提交。
没有调整 Lua 公共 API、组件清单、能力声明或版本号。

| 审计项 | 本轮代码调整 |
| --- | --- |
| SBI-01 | Dock 映射图标后台查询，绘制只读取缓存和占位图；后台诊断名称为 `Dock.FolderIcon.Async` |
| SBI-02 | 运行中应用图标、窗口 AUMID 后台获取，结果校验身份、路径、尺寸及进程 |
| SBI-03 | Dock 文件夹目标、快捷方式及应用身份后台解析，缓存键包含来源路径和可用的文件戳 |
| SBI-04 | 补图回调与尺寸刷新只传递路径/PIDL 值，路径解析由图标工作线程完成 |
| SBI-05 | F5、映射组件、弹层使用异步读取；保留旧内容，失败不会应用为空目录 |
| SBI-06 | 系统图像列表、快捷方式箭头后台取得 HBITMAP，D2D 位图仍在 UI 线程创建 |
| SBI-07 | Everything 的文件图标后台获取，只更新当前查询中仍存在的行 |
| SBI-08 | 图标、应用索引、组件 Shell 图标和大图标加载器退出不再等待不可取消的 Shell 调用；值数据和资源由工作状态持有 |
| SBI-09 | 桌面、各映射目录及 Dock 路径检查分别调度；目录列表不等待逐项 Shell 图标查询 |

公共内部调度器 `BackgroundWork` 使用固定数量的 STA 工作线程、有限队列、请求键去重、
取消序号校验及无指针唤醒消息。UI 按批消费结果，并在拖放、菜单、重命名、绘制及文件操作期间
延后应用。目录版本和图标文件戳防止迟到结果覆盖新状态；关闭弹层会取消其排队任务。
饱和时未入队的桌面/映射图标由维护周期重试。

另外取消启动阶段固定的 1.5 秒 Shell 等待，将剪贴板剪切状态读取移入独立后台队列，
将托盘命名空间和调试目录监听解析移到后台。COM 初始化移到设置初始化之前；
启动阶段使用保存的自启动偏好，外部任务计划状态及旧任务协调在打开设置时核对。

### 验证证据与边界

- 生产调度器受控阻塞回归：`.codex-probes/run-startup-queue-probe.cmd`，退出码 0。
  覆盖独立 STA、阻塞源隔离、去重、容量、UI 应用边界、取消后重新请求、迟到结果和析构。
- 文件戳回归：`.codex-probes/run-startup-stamp-probe.cmd`，退出码 0。
  同一路径修改时间或大小改变、未知元数据时，旧请求不得替换新图标。
- 隔离负向对照：将工作线程限制为 1、保留取消请求、绕过文件戳校验，分别产生 2、2、4 个
  确定断言失败，测试进程均正常返回 1。没有修改生产文件，也未用编译失败充当缺陷复现。
- 第一轮标准构建因托盘命名空间的不可复制 PIDL 发生编译错误；改为只复制菜单值字段。
  文件戳探针首次链接缺少 Shell32，修正探针链接输入后通过。这两次失败均不计为通过。
- 标准构建、仓库定向/全量测试及真实启动日志的最终结果在交付时补录。

这些检查不能证明所有第三方提供程序始终返回。工作线程和排队数量有限；若同一队列的工作线程
全部被系统提供程序阻塞，其待加载内容会继续保留占位或旧内容，主线程和析构不会加入等待。
没有强杀线程，也没有引入可终止的独立 Shell 查询进程。
Dock 分类、补图视觉、弹层、拖放和设置交互仍需原始桌面场景实机验收。
