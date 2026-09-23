# GitHub Desktop 固定项与运行项重复排查（2026-09-23）

## 现场与原因

用户报告 GitHub Desktop 固定在 Dock 后，窗口仍单独出现在运行区。只读检查本机布局确认固定项为桌面的 `GitHub Desktop.lnk`，并取得以下原始输入：

- 快捷方式目标：`C:\ProgramData\guoyunzhe\GitHubDesktop\GitHubDesktop.exe`。
- 窗口进程：`C:\ProgramData\guoyunzhe\GitHubDesktop\app-3.6.6\GitHubDesktop.exe`，PID 29644。
- 快捷方式 AUMID：`com.squirrel.GitHubDesktop.GitHubDesktop`。
- 窗口属性存储中的 AUMID 为空；`GetApplicationUserModelId` 返回 15703，无法通过现有窗口/进程 AUMID 查询关联。
- 主进程记录的父 PID 51528 已不存在，根目录启动器也未存活。

旧 `ReadDockAppIdentity` 对普通 EXE 快捷方式不读取 AUMID。旧 `MatchesRunningApp` 仅接受相同 EXE 路径，或安装目录内且祖先进程中仍能查到启动器的窗口。上述现场两项条件均不成立，固定项运行状态与运行区去重共同失配。

[Squirrel 启动器源码](https://github.com/Squirrel/Squirrel.Windows/blob/develop/src/StubExecutable/StubExecutable.cpp)显示，它从根目录选择 `app-<version>` 下的同名程序，启动后等待输入就绪并退出。此流程解释了为什么不能要求启动器一直存活。Windows 的 [AppUserModel.ID 属性说明](https://learn.microsoft.com/en-us/windows/win32/properties/props-system-appusermodel-id)描述了显式标识的分组用途，但不能据快捷方式存在该属性就推断窗口可读到相同标识。

## 调整范围

普通 EXE 快捷方式也读取并缓存现有 AUMID 字段。共享匹配器增加 Squirrel 启动器路径关系：

1. 快捷方式必须具有非空 `COM.SQUIRREL.` 后缀的规范化标识。
2. 运行程序必须在启动器同一安装根目录的直接 `APP-<version>` 子目录中。
3. EXE 文件名必须完全相同。版本目录以数字开头，允许数字、字母及版本常用的点、减号、加号；这不是完整的 SemVer 校验器。

不依赖窗口 AUMID 或存活的启动器祖先。原有普通 EXE 路径/进程族、Applications 和 Steam 匹配保持原逻辑。直接固定 EXE 而没有 Squirrel 快捷方式标识的情况仍走原规则，不凭同名或整个安装目录合并。

调用链涵盖固定项运行指示、运行区去重、常用项去重、运行项查找桌面快捷方式，以及预览/关闭等路径使用的 `DockWindowMatchesAppIdentity`。这些入口继续复用同一规则；没有修改公共组件 API、布局格式或用户配置。

## 验证

- 只读现场探针对同一快捷方式和 GitHub Desktop 窗口读取属性，将真实路径与标识传入生产匹配器：修改前 `matched=0`、退出 1；修改后 `matched=1`、退出 0。探针未枚举或操作 SnowDesktop 桌面宿主。
- 复用 `dock_and_window_rules` 增加启动器退出、窗口无 AUMID、版本升级/预发布目录，以及不同安装目录、相似目录前缀、其他 EXE、嵌套辅助程序、缺失 Squirrel 标识和路径穿越的回归。
- 将上述新增断言提取到隔离探针：当前生产头文件退出 0；使用修改前头文件的隔离副本退出 1，失败点为实际关联及升级后的关联。两份探针均使用 MSVC `/W4 /WX` 成功编译，没有用编译失败代替缺陷复现。

本次在 Windows / MSVC Release 配置执行：

| 检查 | 结果 |
| --- | --- |
| `scripts/build.bat` | 退出 0，330.41 秒，确认 `.build/Release/SnowDesktop.exe` 生成；完整日志无编译或链接警告 |
| `scripts/test.bat full` | 退出 0，120/120 自动条目通过，入口总耗时 160.42 秒；不包含 `manual` 诊断 |
| 全量分段计时 | 配置 2.39 秒、测试聚合构建 26.23 秒、CTest 130.40 秒；构建日志无编译或链接警告 |
| `dock_and_window_rules` | 包含在上述全量中，退出 0，1.03 秒 |
| 运行输入核对 | 构建前后及全量后，已跟踪生产代码、测试、脚本、资源、依赖和配置的 SHA-256 无变化 |

标准构建第一次预检之后，另一任务短暂启动了预览导出进程，脚本以预检退出码 3 拒绝构建。只读确认该进程已经退出后，重新运行标准入口取得上表结果；没有终止它或重启 Explorer。这次预检阻断单独保留日志，不计为通过。

探针、原始样本哈希、构建/测试完整日志、计时、候选差异、运行输入指纹、CMake 缓存与最终产物哈希保存在 `.codex-probes/github-dock-20260923/`。JUnit 为 `.build/Testing/test-run-673955cc77014d9abce8c671ae5a8436.xml`。有效源码相当于 `53f84024` 加本次代码和测试；另一任务的标题阴影改动在构建期间提交，但文件内容未改变。既有 `docs/testing_audit_product_issues.md` 修改未改动，也未纳入本次提交。

## 待实机

使用本轮标准构建检查已有固定项：运行时只保留固定图标，显示运行状态；点击能切换/最小化/恢复 GitHub Desktop，多窗口预览与关闭入口保持一致；退出应用后固定项保留、运行标记消失，再从固定项启动后仍合并。新规则自动应用于已有快捷方式，不需要重新固定。

现场匹配探针通过不等于 Dock 最终视觉和交互验收通过，本轮按 `try` 保存，待用户原场景反馈。
