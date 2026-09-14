# 组件互拖合并与建组验收

本轮日期：2026-09-14。基于 `1214624c` 开发；桌面宿主交互尚待用户实机验证。

## 操作约定

“桌面文件”指 `FileCategories` 桌面文件组件。以下入口作用于整个组件拖动（移动手柄或鼠标中键），不改变组件内部文件项拖放协议，也不新增公共组件 API。

| 来源 → 目标 | 无修饰键 | Shift 松手 | Ctrl 松手 |
| --- | --- | --- | --- |
| 集合 → 集合 | 提示两种操作，沿用原组件移动 | 合并内容到目标，移除空源集合 | 创建集合组，目标为第一个活动标签 |
| 桌面文件 → 桌面文件 | 提示两种操作，沿用原组件移动 | 合并内容到目标，移除空源组件 | 创建文件组，目标为第一个活动标签 |
| 桌面文件 → 映射组件 | 提示 Ctrl 建组，沿用原组件移动 | 原组件移动 | 创建文件组 |
| 映射组件 → 桌面文件 | 提示 Ctrl 建组，沿用原组件移动 | 原组件移动 | 创建文件组 |

Alt、Ctrl+Shift 及含 Alt 的组合不启用合并或建组。有效单键启用后，提示框及目标轮廓使用黄色；松开修饰键恢复移动预览。原有拖入已存在的集合组、文件组和 Dock 的行为继续走原路径。

合并保留目标名称、显示设置和已有顺序，随后追加源内容；按既有大小写归一化规则去重，保留暂时找不到实际文件的键。操作只调整逻辑归属，不读写、合并或删除实际文件。建组保留两个子组件的 ID、内容、映射路径、配置和原始跨度。

创建组优先使用目标位置和跨度；集合组最小尺寸可能比小集合更大。若扩展范围被其他组件占用，不启用黄色建组预览且提交不修改数据。允许的桌面图标避让仍使用既有组件放置入口。隐藏子组件、自身、不兼容目标、已失效目标不能执行互拖事务。

## 自动化边界

- `widget_interaction_rules` 调用宿主松手路径复用的 `widget_pair_drop::Apply`，使用真实 `DesktopWidget` / `DockEntry` 模型；只替代测试键的大小写归一化。覆盖支持和拒绝的类型对、8 种修饰键、合并后的唯一归属和顺序、空源、索引先后、保留暂缺文件键、重复提交拒绝、子组件完整设置、无关组件及 Dock 引用。此测试不调用桌面窗口消息、保存文件或渲染。
- `widget_composition_layer_rules` 检查静止指针下启用／释放修饰键会请求重新呈现；`drag_hint_rules` 检查样式变化使提示位图缓存失效。它们不证明实机颜色、位置或换行正确。
- `localization_contract` 覆盖全部语言的键契约。新增 7 条文案已人工审阅全部 10 种语言；长提示使用换行布局。
- 既有 `slot_contract_matrix` / `slot_runtime_contract` 检查原槽位路由和执行模块边界，不能代替新增整个组件原生消息分支的实测。
- 隔离副本负向对照：跳过目标内容提交后，合并数据断言失败 2 项；遗漏源子组件后，建组保留断言失败 4 项。两者编译成功、测试退出码均为 1。生产文件未被覆盖。

本次定向命令：`scripts/test.bat name "^(widget_interaction_rules|widget_composition_layer_rules|localization_contract|slot_contract_matrix|slot_runtime_contract)$"`，5/5 通过，退出码 0。配置 0.97 秒，测试目标编译 13.79 秒，CTest 1.13 秒。日志 `.codex-probes/pair-drop-targeted.log`；JUnit `.build/Testing/test-run-f3e20151b76a4d8b9678bb03511cd177.xml`。

负向对照命令：`python .codex-probes/run_pair_negative.py`；日志 `.codex-probes/pair-drop-negative.log` 和 `.codex-probes/pair-negative/`。临时探针不随仓库分发。

本次 `scripts/build.bat --reload-shell` 退出码 0，生成 `.build/Release/SnowDesktop.exe`；日志 `.codex-probes/pair-drop-build.log`，日志时间跨度约 409 秒。预检发现宿主运行，已按仓库规则提示并终止宿主、重启 Explorer。构建只观察到既有 WinUI 生成头 `GetCurrentTime` 的 C4002 警告。

本次 `scripts/test.bat full` 于 19:21:36–19:23:19（UTC+8）完成，118/118 通过，退出码 0；配置 1.07 秒，测试目标构建 32.41 秒，CTest 67.96 秒，默认排除 `manual` 诊断。日志 `.codex-probes/pair-drop-full.log`；JUnit `.build/Testing/test-run-c81e1b0b2c044a85b1cd96b4956a5b26.xml`。

输入为本提交的源码、测试、语言资源，MSVC 19.50.35730.0、Windows SDK 10.0.26100.0、VS 18 2026、Release 配置；检查期间这些输入未再修改，原始文件哈希快照保存在 `.codex-probes/pair-drop-inputs.json`。全量检查完成后的宿主 SHA-256：`f78121921f5b9eea68349ca1518355b10db3c0cfa5181de98432ca1fabfafa49`。自动化通过不代表完整宿主交互验收，以下实机项仍待验证。

## 待实机步骤

1. 用独立测试文件建立两个不同内容的集合；先观察两种按键提示，再保持指针不动切换 Shift、Ctrl、无键。核对黄色提示与目标高亮同步、文案不截断，Esc 和释放修饰键均不合并或建组。
2. 分别对上表四种方向执行有效操作；核对仅提交一次，合并目标内容及顺序、源组件移除、实际文件仍可打开。集合和桌面文件各测非空源、空源、目标先／后创建。
3. 对集合组和文件组检查目标标签默认打开、两个子组件的内容与设置保留；从组中释放后检查原始尺寸和映射路径。刷新／重新启动后复核结果。
4. 测无键、Alt 和 Ctrl+Shift，不应执行合并或建组。检查自身、不兼容组件、隐藏子组件、移出目标后立即松手，以及新组被邻接组件阻挡时数据不变。
5. 回归拖入已有组与 Dock、翻页、移动手柄与中键拖动、多屏不同 DPI。上述交互均待实机，未使用桌面自动化操作宿主。
