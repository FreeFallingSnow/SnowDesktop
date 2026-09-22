# Shell 打开请求隔离

桌面打开文件夹时，`IContextMenu::QueryContextMenu(CMF_NORMAL)` 会枚举第三方菜单扩展。
2026-09-07 的用户转储显示，百度网盘 `YunShellExtV164.dll` 在初始化期间间接等待新线程，
而新线程正在等待加载器。主线程持有加载锁，导致桌面无响应。同进程工作线程也无法隔离
这类进程级死锁。

## 执行边界

`ShellLaunchWorker::Execute`、`ExecuteInteractive` 和 `ExecuteRunAsAdministrator`
现在只派发请求。返回 `true` 表示辅助进程已经启动，不代表文件已经打开。
原有 `Enqueue` 接口仍表示请求已入队；默认执行器派发辅助进程后即返回，不等待 Shell。

每个请求启动同一运行时的 `SnowDesktop.exe`，通过内部 `--shell-open-helper` 入口处理。
入口位于单实例、桌面状态、Steam 和崩溃重启初始化之前。该协议只供同构建宿主使用，
不是公开 CLI，不改变 Lua 组件 API 或 capability。

- 路径和绝对 PIDL 通过只读继承内存映射传递；命令行只有句柄值。
- 继承列表只包含请求映射与父进程句柄。辅助进程校验父进程身份、协议版本、长度、
  PIDL 边界及窗口所属进程，并清除传输句柄的继承标志，再执行 Shell 代码。
- 每个请求独立执行，最多同时保留 16 个辅助进程，默认期限为 120 秒。
- 宿主授予辅助进程前台资格；辅助进程在 STA 上等待 Shell/DDE 交接完成后退出。
- 默认打开统一使用 `CMF_DEFAULTONLY | CMF_OPTIMIZEFORINVOKE` 查询必要的 Shell 命令，
  不用 `CMF_NORMAL` 构造完整菜单。普通目录、目录/文件/应用快捷方式及带 PIDL 的命名空间
  对象使用同一路径；只有路径的请求也先在辅助进程解析 PIDL，不因入口不同绕开限制。
  保留原快捷方式对象、参数、工作目录和提权检测，不手工替换为快捷方式目标。
- 查询或执行已支持的命令失败时直接返回失败，不再用另一个 API 重试。
  无法取得 `IContextMenu` 的对象或协议保留 `ShellExecuteExW(open)` 兼容路径；这条路径仍
  可能加载处理器，由辅助进程期限限制其影响。受限查询并不保证任何第三方处理器都遵守标志。
- 超时仅终止对应辅助进程。其私有 Job 使用 `KILL_ON_JOB_CLOSE` 和
  `SILENT_BREAKAWAY_OK`；通过它正常启动的程序不会因辅助进程回收而被一同终止。
  打开失败或超时通过调试输出记录进程 ID 和错误码，不自动重试，避免重复执行用户操作。

这次隔离覆盖上述打开入口。用户显式显示的系统右键菜单、属性窗口和其他 Shell 调用仍按
各自现有路径执行，不能据此认为所有第三方扩展风险都已消除。已有外层 Job 的约束仍可能
影响子进程的启动；辅助进程无法创建时返回失败，不回退到主线程执行。

## 验证

`scripts/test.bat name "^shell_launch_worker$"` 使用真实子进程覆盖请求传输、
阻塞请求的期限回收、后续请求独立执行和 `.lnk` 启动；同时检查传输句柄不会继续继承，
以及辅助进程超时后已启动的目标程序仍能继续运行。测试注入仅存在于测试执行器，
生产辅助进程不接受模拟阻塞开关。

2026-09-10 补充：此前只检测路径自身的目录属性，指向文件夹的 `.lnk` 因而仍进入完整菜单
枚举。原机探针在 `QueryContextMenu` 内重复捕获到百度网盘 `YunShellExtV164.dll` 加载期间
的线程等待；新建目录的路径式打开也出现阻塞。现在已有目标中的命令边界测试模拟拒绝完整
枚举的处理器，验证默认动作、无规范名称的默认项和失败不执行；真实集成测试创建独立目录，
分别核对带 PIDL 的 `.lnk`、不带 PIDL 的 `.lnk`、普通目录实际进入资源管理器。
观察按文件身份比较目录，避免 Windows 短路径/长路径差异造成误判；不操作真实用户目录。

同类入口检查覆盖桌面/Dock 打开、快速导航、映射目录、组件“打开文件夹”和 Lua 宿主打开
请求，它们复用上述打开执行器。快速搜索的应用列表原先直接执行 Shell、在 UI 线程读取
提权元数据，现已合入同一辅助入口；纯 PIDL 对象也能传输，不要求凭空提供文件路径。
设置中的数据目录、备份位置、组件开发目录、新建开发项目目录和本地许可文档同样接入。
这些入口返回成功表示请求已派发，不能解释为目标已打开；宿主不等待辅助进程的执行结果。
用户主动展示的完整菜单继续保留菜单项；“新建”仅创建
`CLSID_NewMenu`。系统图标自定义菜单读取管理/清空/属性等非默认动作，需要相应 Shell
命令，不能套用仅默认动作标志；这些显式菜单仍有扩展兼容性边界，未声称一并消除。

原用户安装百度网盘扩展后的文件夹打开、Windows 10 文档 DDE/执行委托、前台焦点及 UAC
仍需实机验证。单元测试和构建通过不能替代这些兼容性场景。

### 2026-09-22：重复打开时窗口隐藏

独立临时目录通过真实辅助进程连续打开三次：首次 Explorer 窗口可见，第二、三次辅助进程均
成功退出，但同一个窗口句柄仍存在且 `IsWindowVisible` 为假。由此确认本机复现的是窗口被
隐藏，而非关闭或打开请求排队阻塞；仅检查 `IShellWindows` 中的目录地址无法发现这个问题。

辅助进程原先固定传入 `STARTF_USESHOWWINDOW + SW_HIDE`。仅将该启动显示状态改为请求中的
`showCommand`，保持其余 Shell 调用不变后，同一探针连续三次打开均可见。辅助进程本身不创建
界面，控制台仍由 `CREATE_NO_WINDOW` 抑制。此修改不改变内部请求格式或公开组件 API。

现有 `shell_launch_worker` 集成测试增加普通文件夹的 PIDL 入口，并覆盖五种路径/PIDL 形式
的首次打开、已打开时再次打开、最小化后再次打开。每次均等待对应辅助进程退出，再核对
真实 Explorer 窗口可见且未最小化，防止请求执行前的旧窗口让测试误通过。只使用独立临时
文件夹并清理其窗口。此证据不替代用户原桌面场景、前台焦点及其他 Windows 版本的验收。

### 2026-09-22：管理员快捷方式的前台交接

用户报告 VGN VHUB 双击时 UAC 未进入前台。原机快捷方式未设置 `SLDF_RUNAS_USER`，
目标 EXE 清单声明 `highestAvailable`，因此必须覆盖目标清单触发的提权路径。
`3af68677` 曾记录此对象双击实测通过；`55d888f9` 的隔离改造随后移除了临近 `runas`
的窗口激活，将授权提前到辅助入口。此为代码回归线索，尚未通过原场景二分确认唯一致因。

本轮调整将桌面/Dock 双击、映射路径、快速导航和管理员菜单的启动 owner 收口到已有的
独立输入窗口。隐藏控制窗口、`WS_EX_NOACTIVATE` 窗口和 Explorer 下的渲染子窗口不作为
提权 owner；显式传入的可激活顶层窗口继续保留。快捷方式元数据读取完成后，辅助进程
紧邻 `ShellExecuteExW(runas)` 交接前台；只有前台仍属于 owner 进程时才请求激活 owner。
取消授权不重试，Shell 调用和等待仍保留在独立辅助进程。内部测试边界不属于公开 API，
不改变同构建请求格式或组件 capability。

新增回归位于现有 `shell_launch_worker` 目标，使用独立测试 HWND、真实临时 `.lnk` 和
`highestAvailable` 清单读取，仅替换 Win32 前台操作与 UAC 执行边界。覆盖 owner 选择、
两种管理员快捷方式、显式管理员命令、取消不重试、owner 销毁和前台丢失后的不再激活。
测试程序的 `--elevation-contract [快捷方式路径]` 可只读检查原始快捷方式的实际路由，
管理员分支的授权执行使用替身，意外进入普通 Open 时也由替身直接拒绝；两条分支均不会
启动该目标程序。它不是宿主 CLI。另以强制绕过提权路由的隔离副本验证，此类回归会明确
失败而不会调用真实 Shell；生产默认边界仍使用原来的 Shell Open。

本轮定向测试 4/4 通过：`shell_launch_worker`、`shell_integration_contract`、
`modern_menu_interaction`、`ui_animation_scheduler`。原机 VGN VHUB 快捷方式通过上述
元数据及模拟边界检查。隔离副本中删除即时授权，或恢复无效 owner，均使新回归以退出码 1
失败；两份负向对照编译成功且无编译/链接警告。日志在
`.codex-probes/20260922-uac-foreground/`。

收紧普通 Open 测试边界后，最终候选重新通过 `scripts/build.bat` 和
`scripts/test.bat full`（120/120，CTest 96.32 秒），退出码均为 0，完整日志无编译或
链接警告。原机 VGN 快捷方式探针再次通过；强制错误提权路由的隔离副本编译成功，
并以退出码 1 拒绝测试断言。最终证据分别为上述目录中的 `final-build.log`、
`final-full.log`、`final-original-shortcut.log`、`negative-route.log` 和
`final-hashes.json`；全量默认排除 manual 诊断。

上述自动化结果不能单独证明实际 UAC 的层级和焦点。交付最终候选及 VGN VHUB 双击、
右键管理员启动、快捷启动面板的验证步骤后，用户反馈“没问题了”。据此记录
`7f510f81`、`0ccf38bd` 对应的原始 VGN VHUB 授权框前台问题实机验收通过。
验收记录时再次核对源码、测试和宿主二进制的 9 项 SHA256，与最终候选证据全部一致，
复用上述有效构建与全量结果，不重复编译或测试。

用户未逐项列举各入口、等待/取消/同意、切换到其他程序及连续启动的结果，不将这次反馈
扩展为所有交互分支、第三方程序或 Windows 版本均已验证；也未通过二分确认唯一回归提交。

机制参考：[DLL 初始化约束](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices)、
[ShellExecute 的同步交接标志](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-shellexecuteinfow)、
[Job 子进程边界](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)。
