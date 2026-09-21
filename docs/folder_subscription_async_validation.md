# 目录订阅后台解析验证（2026-09-21）

本次独立处理 Steam 更新审查中的 `SHELL-01`。基于 `a2a913cd`，用户验收记录另见
[实机验收补记](steam_update_runtime_validation_20260921.md)。不修改公共组件 API、权限、清单或 capability。

## 变更与风险边界

- `FolderNotifications::Sync` 仅整理当前目录、接收结果和注册通知；
  `SHParseDisplayName` 及 Shell 长短路径解析由两个后台 STA 工作线程执行。
- 线程复用同一个队列，不因重建、关闭弹窗或 `Clear` 创建更多线程。失败目录在后续同步时
  至少间隔 30 秒才重试；没有新增定时目录扫描。
- 每次请求有独立编号。移除、切换、清空及重新加入同一路径后，旧结果不能注册当前订阅。
  后台线程只持有独立邮箱，不持有宿主对象；销毁不等待卡住的 Shell 解析。
- 完成消息不携带指针，UI 重新核对当前模型后处理。订阅成功仍请求一次限定目录刷新，
  补足初始读取与后台订阅完成之间的间隔；原有通知路由、合并和状态保留继续生效。
- 不能强行取消已进入 Shell 提供程序的调用。若两个提供程序都永久不返回，新的路径解析会
  等待工作线程空闲；界面同步和对象销毁不等待它们，已有订阅继续保留。
  `SHChangeNotifyRegister/Deregister` 仍在 UI 线程，本次没有证明这些 API 在所有系统环境下都有时限。

## 回归设计与失败证据

复用 `slot_runtime_contract` 和现有 `tests/shell_folder_refresh_cases.h`，不新增 CTest 程序。

- 本地隔离目录和消息窗口验证真实 Shell 通知、长短路径别名、去重、切换、移除及重新注册。
  等待后台订阅完成后才写入测试文件，普通文件写入不替换为合成 Shell 通知。
- 受控阻塞仅替换 Shell 解析边界；队列、完成处理、通知注册及对象生命周期仍使用生产实现。
  检查同步调用返回、另一目录独立完成、失败重试限制、同路径不同代际及销毁后返回。
- 独立探针保存在 `.codex-probes/folder-subscription-async-20260921/negative/`。
  `/W4 /WX` 编译的原实现基线退出 0；隔离副本把解析恢复到 `Sync` 调用线程后退出 1，
  明确触发“解析在 UI 调用线程执行”“Sync 等待受控阻塞”“对象无法在阻塞期间退役”断言。
  探针协调成功，失败不是编译错误或未执行到变异点。未覆盖工作区源码制造失败。

## 构建记录

日志目录：`.codex-probes/folder-subscription-async-20260921/`。

- 首次 `scripts/build.bat --reload-shell` 按预检关闭运行中的宿主并重启 Explorer。
  编译发现新增 PIDL 智能指针丢失 `__unaligned` 限定符的 C4090，主动停止该次构建。
  已将释放器的 `pointer` 类型定义为 `PIDLIST_ABSOLUTE`，保留 Windows SDK 的限定符；
  没有抑制警告或改动生成头文件。
- `scripts/build.bat`：退出 0，518.85 秒；完整 `build-2.log` 无编译/链接警告。
  生成的 Release 宿主 SHA256 为 `FBF40B5641F41377921B1AAED830F62B29434D1441F40C615411346111C91759`。
- `scripts/test.bat name "^(slot_runtime_contract|shell_integration_contract)$"`：编译通过，无警告；
  CTest 1/2 通过，退出 8（脚本退出 1），见 `targeted-2.log`。唯一失败仍是历史断言
  `external file creation wakes the mapped-folder subscription`，新增受控阻塞及生命周期断言没有失败。
  配置/目标编译/CTest 分别见日志；不能据此写成定向测试整体通过。
- 最初定向命令因 PowerShell 向批处理传递正则时丢失引号而在执行测试前退出 255；
  改用保留正则引号的包装批处理调用同一标准测试入口。未将该次命令失败算作测试执行。
- 本次尝试先按 `try` 独立保存，随后继续定位通知用例失败；完整测试尚未运行。

此修改的离线 UNC、映射盘恢复和真实桌面交互尚待实机；受控阻塞证明本处解析不再阻塞调用方，
不等同于证明所有慢启动反馈均已解决。用户对修改前目录通知的验收保持有效，不能预先扩大为
对本次新代码所有环境的验收。自启动仍等待原用户配合。

## 通知用例的后续定位

`c8f68429` 保存以上编译通过、定向存在失败的独立尝试。后续只调整测试准备阶段，
没有继续改动通知生产实现。

- 隔离插桩在 `notify-diagnostic.log` 中记录：注册返回和首次 mapped 文件写入均为
  `107038859`，六秒内没有收到该目录事件；随后 Dock 写入收到真实 `SHCNE_CREATE` 和
  `SHCNE_UPDATEITEM`，路径已被正常展开为长路径。本次失败并非已有别名路由断言误判。
- 对照仅在注册后使用 `SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSH, ...)`
  完成准备阶段，再清除准备通知。`notify-barrier.log` 中真实文件写入分别收到 CREATE 和
  UPDATEITEM，退出 0。由此推断失败来自注册返回与底层通知监视就绪之间的初始化时序窗口；
  没有测量 Windows 内部 watcher，也不把此推断写成该 API 提供的正式就绪保证。
- 测试现在先刷新、分发并清空准备通知，之后仍用普通文件写入触发待测事件。
  这明确区分准备与观察阶段，不使用固定睡眠或失败重跑。`SHCNF_FLUSH` 的语义依据
  [Microsoft 文档](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shchangenotify)。
- 隔离副本取消 `SHCNRF_InterruptLevel` 后（`notify-no-interrupt-2.log`），两项真实外部写入
  断言均失败、退出 1；证明准备通知不能替代真实文件系统通知而使测试通过。
  首次控制探针因宏重复定义 C4005 被 `/WX` 拒绝；改为在探针中先 `#undef` 后定义所需变异，
  重编译无警告。该控制只在隔离探针内取消事件源，没有抑制生产构建警告。
- 更新后的标准定向入口退出 0，2/2 通过，配置 1.57 秒、编译 7.65 秒、CTest 1.57 秒；
  `targeted-3.log` 无编译/链接警告，报告 `test-run-e8699ee64359474891703907ccf1213f.xml`。
- 原始失败日志完整保留；本次检查覆盖准备完成后的通知，不声称冷启动窗口中的每次写入
  均可单独产生事件。生产逻辑保留订阅后补读，此次没有为测试改变产品行为。

最终 `scripts/test.bat full`：退出 0，120/120 通过，无跳过，默认排除手动诊断
`shell_file_operation_worker`；配置 1.57 秒、聚合目标编译 36.70 秒、CTest 93.55 秒，
总流程 132.78 秒。完整 `full.log` 无编译/链接警告，报告为
`.build/Testing/test-run-2f9c4341115543b793d34c271c8c9a8a.xml`。
本次最终输入清单保存为 `inputs-final.json`（945 个源码、测试、配置及资源文件），
绑定 `c8f68429` 加本次测试准备阶段改动，未把旧全量当作本次通过。

全量后再次执行标准 `scripts/build.bat`，退出 0，28.19 秒，完整 `build-final.log` 无编译/链接
警告，运行时目录整理完成。最终宿主 SHA256 为
`40C70CA6C2EDF7F6A1B089BE70BE9809EB99981201CE496C92A9DF4A14CC7308`。
重新核对 945 个被测输入均未变化；原有 DND-05 未提交段落保持原样，未纳入本次提交。
没有运行桌面宿主自动化、Steam 打包、Git 推送或线上上传。
