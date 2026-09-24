# 版本切换后重复冲突弹窗排查（2026-09-24）

用户场景：在本地构建与 Steam 版之间切换，目标版本打开后再次出现版本冲突提示。
候选提交为 `2b65b4d0`，分支为 `release/v1.0.7.0`，待原始场景实机验证。

## 现场证据与原因

Steam 数据目录的日志记录 PID `30392` 在 `13:24:53.587` 开始正常退出，
`13:24:53.719` 返回主运行函数，`13:24:53.750` 完成宿主资源释放。
同一 PID 的崩溃记录为 `0xC0000005`，tick 为 `348365734`；调用栈依次经过
`ModuleApplication` 的缓存查询、`Scanner::Root`、`ReadCatalogue` 和
`MenuService::Impl::RefreshCatalogue` 的异步任务。本地版 PID `35456` 也有相同位置的
访问冲突记录，tick 为 `348356234`。

调用链中有两处退出缺口：

1. `SharedMenuService` 是静态服务，宿主退出时未显式关闭。扫描任务随后首次构造
   `ModuleApplication` 的静态缓存；进程退出时缓存先于服务析构，扫描仍可能访问缓存。
2. `MenuService::Shutdown` 只等待调度线程，独立的 `std::async` 扫描仍可存活。
   仅增加宿主的关闭调用不足以封闭扫描生命周期。

崩溃守护逻辑会在观察到 `0xC0000005` 后再次启动原可执行文件。此时目标版本已经接管，
被重新启动的旧版进入单实例检查，便可再次显示冲突弹窗。日志已证实退出阶段的扫描崩溃；
未保存第二次弹窗所属进程的快照，重新拉起与弹窗的关联依据为现有启动和守护调用链。

现场 Steam 可执行文件：
`D:/SteamLibrary/steamapps/common/SnowDesktop/.snowdesktop/runtime/1.0.7.0-06792b7f09a10575/SnowDesktop.exe`，
SHA256 为 `5e15e8bcf0ba2af8e10d1bb9cac3b27c1e5a3762d7e76e9cc57e2146c17407cd`。
原始日志副本保存在本地 `.codex-probes/version-switch-20260924/` 的
`steam-host.log`、`steam-crash.log` 和 `local-crash.log`。

## 调整与验证范围

- `DesktopApp` 析构开始时关闭共享菜单服务，在进程静态对象析构前完成收尾。
- `MenuService::Stop` 等待调度线程后，再等待独立扫描任务结束；重复关闭仍可执行。
- 保留正常崩溃恢复与版本冲突判断，组件 API、公开 CLI 和存储格式均未改变。

新增回归复用 `shell_context_menu_invoke` 目标，以受控读取器代替注册表扫描内容，
保留真实服务、调度线程、异步任务和关闭入口。用例在读取器挂起时要求关闭不能提前返回，
放行后要求扫描已经完成，并执行重复关闭。隔离副本移除扫描等待后，得到明确业务断言失败
和退出码 1；当前实现通过同一用例。隔离探针首次编译缺少 COM 声明，补齐探针头文件后
才取得上述失败信号，首次编译错误不计为缺陷复现。

本次定向回归覆盖菜单服务、单实例接管、崩溃重启策略、应用数据生命周期和 Shell 集成契约。
改动涉及宿主与后台任务生命周期，因此另执行完整自动测试。测试不操作 SnowDesktop 桌面宿主。

## 本次验证结果

| 命令 | 结果 | 总耗时 |
| --- | --- | --- |
| `python .codex-probes/version-switch-20260924/negative.py` | 隔离旧逻辑编译成功，运行因扫描未等待的业务断言退出 1 | 24.687 秒（含编译） |
| `scripts/test.bat name "^(shell_context_menu_invoke\|single_instance\|application_restart_policy\|application_data_lifecycle\|shell_integration_contract)$"` | 5/5 通过，退出码 0 | 43.391 秒 |
| `scripts/build.bat --reload-shell` | Release EXE 已生成，退出码 0 | 57.657 秒 |
| `scripts/test.bat full` | 120/120 通过，退出码 0 | 162.234 秒 |

全量执行的配置耗时为 2.10 秒，增量构建为 25.08 秒，CTest 为 133.88 秒。
最终构建、定向回归、全量和有效负向对照日志均未出现编译或链接警告。
本轮未执行默认排除的 `shell_file_operation_worker` 手动诊断。

定向报告为 `.build/Testing/test-run-b985a670711e4a3589f2c83a477605eb.xml`，
全量报告为 `.build/Testing/test-run-b19073e7e60d463490df070fcfbeeb85.xml`。
完整命令、日志和耗时保存于 `.codex-probes/version-switch-20260924/`。
验证前后核对的 1375 个已跟踪输入文件内容一致，包含源码、测试、资源和构建配置；
对应 `inputs.json` 的 SHA256 为
`69b5fda751ba5e94c53a706fdc289402ceab866eee6aa24c8bff6981e205222b`。
用户已有的问题台账修改未并入本次提交。

交付程序为 `.build/Release/SnowDesktop.exe`，SHA256 为
`84a10e30fc312ea6242692882998860f3e0b9f263df91aa993b22d4369ad88ec`。
本次构建按预检结果关闭了 SnowDesktop 并重启 Explorer，未自动启动桌面宿主。

## 实机验收

本地候选已构建，Steam 安装目录未更新。旧 Steam 程序仍可能在退出时触发同一崩溃；
验收双向切换前，两侧程序都应包含本次修改。

1. 运行更新后的本地版，启动更新后的 Steam 版，在冲突弹窗选择切换；确认目标版正常接管，
   旧版退出后不会重新出现冲突弹窗。
2. 从 Steam 版切回本地版，重复上述检查；同时在打开系统菜单管理、扫描尚未完成时切换。
3. 检查两侧日志和崩溃目录，确认本轮没有新增退出崩溃；另检查托盘退出与重启。

当前退出会等待已启动的扫描结束，尚未增加扫描取消机制。自动化回归不代替上述实机验收。
