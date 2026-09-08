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
- 普通文件系统目录使用 `ShellExecuteExW(open)`，跳过宿主的完整菜单枚举。
  其他带 PIDL 的请求保留 Shell Open 命令兼容路径；快捷方式提权检测也可在辅助进程完成。
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

原用户安装百度网盘扩展后的文件夹打开、Windows 10 文档 DDE/执行委托、前台焦点及 UAC
仍需实机验证。单元测试和构建通过不能替代这些兼容性场景。

机制参考：[DLL 初始化约束](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices)、
[ShellExecute 的同步交接标志](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-shellexecuteinfow)、
[Job 子进程边界](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)。
