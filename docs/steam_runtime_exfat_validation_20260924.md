# Steam 启动器 exFAT 兼容性排查（2026-09-24）

## 用户场景与结论边界

- 用户安装盘为 exFAT，卷状态 `Warning`；`update.lock` 是普通文件，属性为 `Hidden, Archive`，不是只读文件或目录。
- 启动及管理员 `--snowdesktop-launcher-apply-only` 均立即失败，后者退出码为 1603。旧日志只有 `cannot acquire the Steam runtime update lock`，未保存底层 Win32 错误码。
- 用户随后反馈：“用户换到ntfs的c盘可以打开”。该反馈验证的是移动安装位置后的临时解决办法，不是本次新启动器在 exFAT 上的验收。
- 原实现用 `GetFileInformationByHandleEx(FileAttributeTagInfo)` 校验锁文件；相同查询还用于刷新暂存载荷。Boost 的上游实现明确记录 FAT/exFAT 对该查询返回 `ERROR_INVALID_PARAMETER`，并改用基础句柄查询。因此这一兼容性缺口足以解释现有现象，但尚未从用户原机取得底层错误码。
- 卷状态 `Warning` 单独保留，不能据此认定磁盘硬件损坏，也不能由本次启动器修改消除。未执行格式化、磁盘修复、权限修改或用户数据清理。

参考：[Boost Filesystem 实现](https://github.com/boostorg/filesystem/blob/develop/src/operations.cpp)、[GetFileInformationByHandle](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfileinformationbyhandle)。

## 修改与受影响边界

基线为 `0e50995b`，本次生产修改仅在 `src/steam_runtime_manager.cpp`：

- 更新锁与暂存文件刷新改用 `GetFileInformationByHandle` 的 `dwFileAttributes`。保留对同一个已打开句柄的校验，拒绝目录、重解析点和设备；不改为沿路径重新打开目标。
- 保留独占锁、30 秒共享占用等待、校验完成后发布及旧版本回退规则。
- 锁失败详情记录打开、属性查询、对象类型或等待超时阶段，并保留 Win32 错误码和锁路径；清理入口复用相同诊断。
- 未改变公共组件 API、Steam 分发清单格式、数据目录或版本号。

## 本次验证

环境为 Windows 本机 NTFS、MSVC 18.5 MSBuild、Windows SDK 10.0.26100.0、Release。仓库测试聚合清单未修改。既有未提交审计文档及远程附件不属于本次改动。

| 检查 | 结果与限制 |
| --- | --- |
| `scripts/test.bat name "^steam_runtime_update$"` | 退出 0，1/1 通过，测试执行 4.83 秒；覆盖真实句柄、只读锁诊断、回退、清理失败、锁目录和链接拒绝 |
| 隔离旧实现负向对照 | 仅在 Win32 查询边界令 `FileAttributeTagInfo` 返回 `ERROR_INVALID_PARAMETER`；真实 `ApplyDistribution` 首次发布断言失败，程序退出 1；没有模拟掉复制、校验、发布或存储 |
| 相同模拟边界的新实现 | 现有 Steam 运行时测试通过，退出 0；只能证明该 API 不支持时的行为，不是真实 exFAT 驱动测试 |
| `scripts/build.bat --reload-shell` | 退出 0，约 41.03 秒，生成标准 Release 宿主及启动器；编译和链接日志无警告 |
| Release 启动器 `--snowdesktop-launcher-apply-only` | 在隔离 NTFS 安装夹用不可执行的文本载荷连续运行两次均退出 0；核对当前版本指针、载荷哈希；没有启动桌面宿主 |
| `scripts/test.bat full` | 119/120 通过，CTest 退出 8，脚本退出 1；耗时 106.66 秒，`steam_runtime_update` 通过（4.30 秒） |

全量失败为 `component_preview`：`tests/component_preview_tests.cpp:608` 的 `SetCursorPos(0, 0)` 返回失败，断言为 `the preview fixture can move outside the pending preview`。失败发生在测试准备鼠标位置阶段，尚未确认该系统调用失败的具体环境原因；不把它计为通过，不用无条件重试覆盖记录。本条目及其生产依赖不调用本次修改的启动器模块。

原始日志保存在本机 `.codex-probes/steam-lock-exfat/`（不提交）。全量 JUnit：`.build/Testing/test-run-efdb821c44d34ae08b41f298ec1b9634.xml`。关键文件为 `targeted-test.log`、`before-build.log`、`before-test.log`、`after-build.log`、`after-test.log`、`build.log`、`build-timing.txt`、`full-test.log`、`launcher-smoke.json`。

## 尚待验证

本机没有 exFAT 卷，且当前令牌不是管理员，未挂载或格式化测试磁盘。新候选仍须在原 exFAT 环境运行 `--snowdesktop-launcher-apply-only`，确认成功后再由用户从 Steam 启动并核对桌面数据。将应用移到 NTFS 后成功，不能替代新候选在 exFAT 的验证。此改动按 `try` 保存，不声称全量全绿或原机缺陷已修复。
