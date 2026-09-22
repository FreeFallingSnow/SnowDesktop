# 文件夹、库和文档类型的本地首图

## 改动与边界

针对 `2026-09-22 22:39:51` 启动中下载和图片占满首图回退线程的记录，扩展已有
`ReadLocalIconResources` 快路径，不修改模型提交、取消、线程数量或公共组件 API。

- 普通文件夹及指向文件夹的原始 `.lnk`：读取启用自定义属性的文件夹中的
  `desktop.ini`，优先 `IconResource`，兼容 `IconFile` / `IconIndex`。
  没有可用自定义资源时，使用注册表 `Folder\DefaultIcon` 的静态系统文件夹图标。
- `.library-ms` 及其快捷方式：用 Windows XmlLite 解析本地文件，只采用正确命名空间、
  根节点直属的 `iconReference`。不绑定库对象，不读取其目录、搜索连接或网络位置。
  类似 `imageres.dll,-1003` 的无目录资源名只在 System32 中查找。
- 普通文档及其原始快捷方式目标：读取当前用户 `UserChoice` 的 ProgId，或扩展名默认
  ProgId 下的静态 `DefaultIcon`，不调用关联解析接口或图标处理程序。`%1` 等动态资源
  不能被当作本地图标加载。
- 保持固定本地盘、祖先重解析点、脱机 / 按需召回和路径形式检查。库文件最大 1 MiB，
  XML 深度最大 32，禁止 DTD；配置 `desktop.ini` 最大 64 KiB。
  这些检查不是对慢本地驱动或并发文件替换的硬超时保证。
- 获得首图后沿用已有独立分类、Phase2 精细图标和缩略图更新。无有效本地资源的条目
  仍进入独立 Shell 回退队列。特殊文件夹可能先显示普通文件夹图标，最终由 Shell 更新。

格式依据：[Microsoft 文件夹自定义说明](https://learn.microsoft.com/en-us/windows/win32/shell/how-to-customize-folders-with-desktop-ini)、
[库描述 schema](https://learn.microsoft.com/en-us/windows/win32/shell/library-schema-entry)、
[XmlLite 说明](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ms752838(v=vs.85))。

## 原始样本对照

从上述启动归档提取 174 个不同的原始路径，分别调用旧、新生产读取器，再实际执行
`SHDefExtractIconW` 提取资源。原始文件仅被读取。结果如下：

| 指标 | 旧实现 | 新实现 |
| --- | --- | --- |
| 取得可提取图标的路径 | 97 / 174 | 160 / 174 |
| 无本地首图的路径 | 77 | 14 |
| 下载.lnk | 无候选 | 7.881 ms，系统文件夹图标 |
| 图片.lnk | 无候选 | 8.451 ms，库声明的图片图标 |
| 文档.lnk | 无候选 | 7.528 ms，库声明的文档图标 |

新增成功 63 个，原先成功的 97 个全部保留；新增路径提取最大 16.502 ms，无单项超时。
每个路径在独立探针进程中执行；表中时间是读取及提取区间，不含进程创建。
这是本机一次顺序对照，不是统计基准，也不等于宿主启动、呈现或交互耗时。

剩余 14 个包括特殊 / 应用快捷方式、缺少静态关联的日志文件及虚拟系统图标。
完整桌面枚举第 19 次 `Next` 的长等待没有在本轮消除。后台分类和精细图标仍可能等待
共同 Shell 依赖；不能据此宣称启动或用户此前反馈的交互冻结已经解决。

## 回归证据

新增用例复用 `shortcut_application_rules`，从真实临时文件进入生产读取器，覆盖文件夹
及库快捷方式、相对资源、负图标索引、UTF-8 / UTF-16 XML、目录后续节点、损坏 / 重复 /
错误命名空间 / DTD / 超限库文件和远程资源拒绝。静态关联测试使用当前进程的 HKCR
覆盖及唯一临时易失注册表键；退出恢复覆盖并删除测试键，不修改真实文件关联。

在隔离副本恢复旧生产读取器，同一组用例出现 8 个预期行为失败；初次夹具错误已纠正后
重新执行负向对照。当前定向回归 `scripts/test.bat name shortcut_application_rules` 为
1/1 通过。首次候选 `e7dfb550` 的定向测试失败已如实记录；后续调整修正 XmlLite 结束节点
深度处理和易失注册表子键夹具，没有把失败计入通过。

本地证据目录 `.codex-probes/startup-folder-icons-20260922/` 保存：原始路径对照、旧读取器
负向对照、构建 / 测试日志及退出码、候选输入哈希。测量工具在未初始化 Shell COM 的
独立进程中运行；没有启动或操作 SnowDesktop 桌面宿主作自动化验收。

最终候选的本次验证（Release，Windows SDK 10.0.26100.0）：

- `scripts/build.bat --reload-shell`：退出 0，34.594 秒，标准宿主及配套目标构建完成。
- `scripts/test.bat full`：退出 0，120/120 通过，总计 124.234 秒；配置 1.59 秒、
  测试目标构建 21.14 秒、CTest 执行 100.52 秒。手动诊断条目未运行。
- 定向、标准及全量构建、独立探针和负向对照的编译 / 链接日志均无警告。
- 测试构建重新链接宿主；最终 `.build/Release/SnowDesktop.exe` SHA256 为
  `e51272c162179ca757108127e0b4faba8b44fd3b4374c3711cb870f62c4b119e`。
  标准构建时的哈希另保存在 `build-result.json`。从原始样本测量到全量结束，候选源码、
  测试、脚本及 CMake 输入哈希一致；`validation.json` 记录各阶段日志和产物哈希。
- 标准构建前已检查进程和 Hook 占用，提前告知并使用 Shell 重载。脚本中的 `timeout`
  在重定向输入下提示无法等待，但 Explorer 重启及构建完成；这不是编译 / 链接警告。
- 桌面实际启动观感、精细图标变化和点击 / 拖动 / 右键验收仍待用户复测；上述组件测量
  和自动测试不替代该验收。

## English

The local first-image reader now supports folder desktop.ini resources, library iconReference
and static registered document-type icons, including raw shortcut targets. It does not bind
libraries, visit their search locations, activate association providers or change scheduling.
Bounded XmlLite parsing prohibits DTDs, and resource paths retain local/reparse/offline checks.
Folders without usable customization may show the registered generic folder image first; existing
Shell refinement still updates them later.

On the same 174 original paths from the reported startup, independent extraction succeeded for
160 instead of 97, with no previously successful path lost. Downloads, Pictures and Documents
extracted in 7.881, 8.451 and 7.528 ms. These are single-run component measurements, not startup
or interaction acceptance. Fourteen paths still need Shell fallback, and the full desktop
enumeration wait is not eliminated.

The standard Release build passed in 34.594 s; the targeted test passed 1/1 and the full suite
passed 120/120 in 124.234 s (CTest 100.52 s). No compiler/linker warnings occurred. The isolated
old-reader negative control failed eight expected behavior checks after fixture correction.
The first compiled attempt's failures remain recorded separately. Final executable and input
hashes are archived above; the test build relinked the host without source/input changes.
Manual diagnostics and desktop runtime acceptance were not performed. Shell was reloaded for
the occupied build; its timeout command reported redirected input and skipped the delay, while
Explorer restart and the standard build completed.
