# 调试环境与模拟桌面

在“关于”连续点击版本五次进入调试设置。开启“调试模式”会保存当前环境并重启；关闭后恢复正常环境。模式会跨普通退出、重启和系统启动保留。调试模式中调试页始终可访问。

正常数据目录旁的 `<数据目录名>.debug/` 是固定调试根目录：`data/` 保存调试应用状态，`Desktop/` 为默认模拟桌面，`FullBackups/` 和 `TempState/` 保存调试备份与恢复暂存，`PrivateState/` 保存调试组件密钥。首次启动使用全新状态，不复制正常配置。只读内置组件资源继续共用。

可以选择其他现存、可读写的模拟桌面目录。切换目录后重启，不搬迁或删除已有文件；旧普通桌面项目的位置记录失效，Dock、组件及其他设置保留。现有 Shell 路径接口要求所选目录短于 MAX_PATH。真实桌面、公共桌面、受管理数据目录及其重叠路径不可选择。

文件操作会真实修改目标文件。系统图标仍访问 Windows，主动操作目录外文件仍遵循原规则，因此这不是安全沙箱。原“演示模式”仍只替换快捷方式的显示外观。临时初始化与调试模式互斥。

“清空调试数据”经确认后仅清理 `data/`、`FullBackups/`、`TempState/`、`PrivateState/`，不清理调试根目录、默认/自选模拟桌面或正常应用数据。正在调试时在重启后清理，普通模式下直接清理。存在目录链接或其他重解析点时拒绝清理并提示错误。控制文件 `SnowDesktop.debug-profile.json` 位于正常数据目录，保留目录选择和当前模式。

调试启动失败不会静默改用真实桌面；错误对话框允许恢复正常模式，保留调试文件。

## 验证与实机验收

自动验证复用 `application_data_lifecycle`、`single_instance`、`settings_ipc` 等现有目标；生命周期夹具验证默认初始化、持久化、重置边界、无效目录、重解析路径及真实目录哨兵文件。它不等同于宿主 Shell/OLE 交互验收。

实机验收使用专门的演示文件，依次检查：

- 进入、普通退出再启动、退出调试：两套布局、设置、组件数据独立且恢复正确。
- 模拟目录文件枚举、外部新增后的刷新、新建、粘贴、重命名、删除、拖放及右键“更多”；真实桌面文件不被默认桌面动作修改。
- 系统图标可见且可操作；模拟文件、系统图标和混合选择的 Shell 菜单、拖放与重命名正确。
- 切换模拟目录后旧普通桌面位置不恢复；确认保留 Dock 与组件设置。
- 正常模式和调试模式分别清空，核对演示文件、目录选择和正常状态保留；清空失败不得显示成功。
- 设置页浅色、深色、高对比度、键盘访问及长路径显示。新增文本同步提供全部界面语言的翻译。

桌面宿主不使用自动桌面控制工具验收；上述交互由用户实机反馈确认。

## 2026-09-18 自动验证记录

- 宿主实现提交：`87572a79`、`12a61c07`；中文目录回归提交：`0f41ef58`。均位于 `release/v1.0.7.0`，未推送。
- `scripts/build.bat` 标准 Release 构建通过，生成 `.build/Release/SnowDesktop.exe`。新增 UTF-8 路径弃用警告已消除；此前重编译可见既有 `GetCurrentTime` 宏警告和未修改源文件中的变量遮蔽警告。
- `scripts/test.bat name "^(application_data_lifecycle|single_instance|settings_controller|localization_contract|winui_backup_data_page_backend|winui_settings_navigation|winui_settings_window_host)$"`：本次 7/7 通过，退出码 0；定向编译 49.71 秒，测试 5.30 秒。
- `scripts/test.bat full`：本次 118/118 通过，退出码 0，无跳过；配置 1.58 秒，聚合构建 83.27 秒，测试 86.22 秒。按规则排除手动诊断，不代表 `shell_file_operation_worker` 手动条目已执行。
- 清空负向对照：隔离目录中真实 `Clear` 入口通过；隔离源码副本故意把默认模拟桌面加入删除集合后，以“演示文件被删”的断言失败退出 1。该对照覆盖清空边界，不证明宿主交互。
- 环境：Windows x64、Release、MSVC 14.50、Windows SDK 10.0.26100.0。全量聚合构建按现有流程重新整理运行目录，测试完成时宿主 SHA-256 为 `97E4D63D569084993DE15932D4154AFE138C6C77284ABF5639B0E212F831C623`。
- 本地日志：`.codex-probes/debug-profile-build-final.log`、`.codex-probes/debug-profile-targeted-final.log`、`.codex-probes/debug-profile-full.log`；完整 CTest 报告：`.build/Testing/test-run-87105ec60dff41c7815d026195295aa5.xml`。
- 未运行：上节列出的桌面实机及设置页视觉验收。当前代码提交仍为 `try`，没有把自动测试通过记为桌面功能验收通过。
