# 数据资源自动回收实测（2026-10-07）

验证对象：`1ae5adaa3d696786449a55a1d54bb615e3773bac` 和 `f711e07bb0aa3a4ad56d9c40b2b5586939f2452d` 的数据回收实现。标准构建发生在最终源码提交之前；构建批次 `9ac281c78c0a47f9a376d512369b5169` 记录输入稳定，当前程序哈希与该批次一致。

本次只通过目录、文件及进程状态复查正常运行中的应用自动回收结果，没有操作桌面宿主窗口。初次清点来自本对话的目录核对；复查保存在 `.codex-probes/data-cleanup-live-after.json`。

| 路径（相对 `.build/Release/data`） | 初次清点 | 2026-10-07 09:40 前复查 |
| --- | --- | --- |
| large-icons | 550 个文件，约 33.15 MiB | 0 个文件 |
| website-icons | 14 个文件，约 0.64 MiB | 0 个文件 |
| DropContent | 11 个历史文件 | 目录不存在 |
| crashdumps（包括 wer） | 9 个文件，约 448.28 MiB | 2 个文件，共 102,892,138 字节（约 98.12 MiB） |
| ThemeWorkshop/staging | 旧发布预览中转目录 | 0 个文件 |
| SteamWorkshopManager/staging | 发布/打包中转目录 | 0 个文件 |
| initialization-experiments | 历史 session 目录 | 0 个文件 |

应用主进程、监视进程和 Shell 菜单辅助进程仍运行。布局、组件存储、备份、字体、创作项目、迁移标记和未明确归属的目录仍保留。ShellHook 没有代码或清理规则改动。

验证：

- 初次应用全量 127/128，通过项保留；唯一失败为 `shell_launch_worker` 中过早的一次性 Shell 视图身份检查。
- 修正原有有界就绪观察后，最终标准 Release 构建通过，编译/链接警告为 0。定向补验 `application_data_lifecycle`、`icon_render_rules`、`large_icon_shell_assets`、`large_icon_rendering`、`shortcut_application_rules`、`shell_launch_worker` 为 6/6，通过并行度 4、21.91 秒。没有再次运行全量。
- 隔离 C++ 负向对照：禁用历史中转回收产生 2 个业务断言失败，禁用小容量孤儿图标回收产生 3 个业务断言失败；相同夹具下正常实现均通过。记录 `.build/verification/data-cleanup-negative-e8d4c716ee6b4457832db1dc445b5aae.json`。
- 输出目录隔离检查通过：`.codex-probes/data-cleanup-output-isolation.json`。

限制：自动资源回收已有目录实测证据；真实图片、文本、URL 拖放以及 Dock/组件的最终路径持久化仍待用户实机验证。此记录不将这些交互或公共调用方兼容性写成通过。仍被引用的历史中转文件和无法确定归属的数据采取保留策略。

English: The normal running application automatically collected the orphan icon resources and legacy scratch data shown above. The current executable matches the final standard-build batch. The original full application run passed 127/128 tests; its one Explorer readiness failure was corrected and the affected six tests passed in a separate supplemental run. Both isolated negative controls demonstrated meaningful business failures, and output isolation passed. Real image/text/URL drops, Dock/component final-path persistence and public-caller compatibility still require runtime acceptance. Referenced legacy backing files and data with unknown ownership are preserved. ShellHook was excluded at the user's request.

Executable SHA-256: `8817787ab1fad42da9dec01facca4aac912d43e3aaa2a67303bccf21319a38b9`
