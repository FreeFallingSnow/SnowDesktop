# 组件互拖合并与建组验收

日期：2026-09-14。首轮基于 `1214624c`，保存在 `45b70297`；第二轮 `2ed6dc36` 补充映射互拖、单项组还原及 Dock 桌面文件支持；本轮根据反馈调整还原尺寸及桌面文件新建归属。桌面宿主交互尚待用户实机验证。

## 操作约定

“桌面文件”指 `FileCategories` 桌面文件组件。以下入口作用于整个组件拖动（移动手柄、鼠标中键或从 Dock 拖出单个组件），不改变组件内部文件项拖放协议，也不新增公共组件 API。

| 来源 → 目标 | 无修饰键 | Shift 松手 | Ctrl 松手 |
| --- | --- | --- | --- |
| 集合 → 集合 | 提示两种操作，沿用原组件移动 | 合并内容到目标，移除空源集合 | 创建集合组，目标为第一个活动标签 |
| 桌面文件 → 桌面文件 | 提示两种操作，沿用原组件移动 | 合并内容到目标，移除空源组件 | 创建文件组，目标为第一个活动标签 |
| 桌面文件 → 映射组件 | 提示 Ctrl 建组，沿用原组件移动 | 原组件移动 | 创建文件组 |
| 映射组件 → 桌面文件 | 提示 Ctrl 建组，沿用原组件移动 | 原组件移动 | 创建文件组 |
| 映射组件 → 映射组件 | 提示 Ctrl 建组，沿用原组件移动 | 原组件移动 | 创建文件组，保留两边映射路径 |

Alt、Ctrl+Shift 及含 Alt 的组合不启用合并或建组。有效单键启用后，提示框及目标轮廓使用黄色；松开修饰键恢复移动预览。原有拖入已存在的集合组、文件组和 Dock 的行为继续走原路径。

合并保留目标名称、显示设置和已有顺序，随后追加源内容；按既有大小写归一化规则去重，保留暂时找不到实际文件的键。操作只调整逻辑归属，不读写、合并或删除实际文件。建组保留两个子组件的 ID、内容、映射路径、配置和原始跨度。

创建组优先使用目标位置和跨度；集合组最小尺寸可能比小集合更大。若扩展范围被其他组件占用，不启用黄色建组预览且提交不修改数据。允许的桌面图标避让仍使用既有组件放置入口。隐藏子组件、自身、不兼容目标、已失效目标不能执行互拖事务。

自动还原时，剩余子组件继承组此时的宽高及落点，保留子组件的 ID、类型、内容、映射路径和设置。用户改变组尺寸后，还原采用调整后的组尺寸；网格占用检查也使用同一尺寸。

## 首轮自动化证据（45b70297）

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

## 第二轮行为与验证边界（2ed6dc36）

- 映射组件互拖支持 Ctrl 建立文件组，禁止 Shift 合并物理目录。
- Dock 中的集合、桌面文件、映射组件拖到桌面组件时，使用相同的类型和修饰键规则。只接收捕获的单个组件引用，松手时重新核对源引用、目标与按键；成功后移除 Dock 引用，取消或拒绝时保留。集合和桌面文件没有物理目录导出；Dock 映射文件夹跨外部窗口时沿用既有 OLE 与原生拖拽交还流程，该跨窗口边界仍待实机。
- 桌面文件可从移动手柄、中键或文件组来源标签拖入 Dock 文件区，保留唯一组件实例。使用集合式 2×2 内容图标及逻辑文件弹窗，支持排列、拖出与已有文件组插入。文件区位置与真实文件夹弹窗分别判断，桌面文件不被当成可打开的物理目录。
- Ctrl 新建的组持久化 `dissolveWhenSingle=true`。子组件移出、转组或删除之后，待拖拽、菜单和重命名结束，再按当前成员重新判断。剩一个时保留该子组件 ID、原类型、尺寸、内容、路径及设置，删除组外壳；优先使用组位置，空间不足时用空位／溢出页。剩零个时仅清除空组。
- 旧布局和手动组默认 `false`，不推断已有组的创建来源；此前 `45b70297` 创建但没有标记的组需重新建立才能获得自动还原。旧宿主重新保存可能丢失标记。Dock 桌面文件沿用已有 `collection` 引用字段，由实际 `FileCategories` 组件恢复运行时类型；旧宿主保留引用和组件数据，但没有本轮交互支持。无 Lua API／capability 变化。

影响映射：`widget_pair_drop` → 鼠标释放／Dock 来源 → 逻辑归属及容器重建；`LayoutItems` → 延迟还原 → 网格落点及布局保存；Dock 载荷与槽位契约 → 文件区、弹窗、转组与回桌面；布局读取 → 标记和 Dock 类型恢复；渲染和提示 → 颜色、图标与键盘刷新。涉及跨模块所有权和布局数据，本轮交付需标准构建及一次完整测试。

本轮定向：`scripts/test.bat name "^(widget_interaction_rules|slot_contract_matrix|slot_runtime_contract|application_data_lifecycle|localization_contract)$"`，5/5 通过、退出码 0；配置 1.07 秒、测试目标编译 22.56 秒、CTest 3.75 秒。日志 `.codex-probes/pair-extension-targeted-final.log`，JUnit `.build/Testing/test-run-dae5a1968cf743a984e46dab4f71d4b0.xml`。首次新增测试的布局样本缺少必需坐标、来源样本未绑定 Dock 面，修正夹具后通过；这两项初次失败不作为产品缺陷复现。

映射互拖测试先更改独立预期，观察原实现拒绝该组合（2 条行为断言失败），再修改生产规则通过：`.codex-probes/mapping-pair-before.log`、`mapping-pair-after.log`。本轮隔离负向对照分别保留错误 Dock 源引用、遗漏自动还原标记、跳过还原事务；均编译成功且相关行为断言失败，退出码 1。命令 `python .codex-probes/run_pair_extension_negative.py`，日志 `.codex-probes/pair-extension-negative.log`。未覆盖生产文件。

新增自动化使用宿主实际调用的模型事务、载荷分类器和布局存储入口；替身仅限临时样本、键归一化和运行时容器外壳。覆盖来源保留、事务后唯一归属、三种子组件还原、手动组保持、空组和失效子引用、重启保留标记。它们不执行宿主计时器、桌面消息或实际绘制，不能证明黄色提示、Dock 弹窗、还原时机和落点的实机效果。

最终 `scripts/build.bat` 于 19:57:21–19:58:06（UTC+8）完成，退出码 0，生成 `.build/Release/SnowDesktop.exe`；日志 `.codex-probes/pair-extension-build-final.log`，约 45 秒。此前 `--reload-shell` 已按提示结束宿主并重启 Explorer，但首次编译因错误调用私有 `InvalidatePresentation` 失败（C2248）；改用既有宿主刷新入口后重新通过，失败中间状态未提交。最终构建只观察到既有 WinUI 生成头 `GetCurrentTime` 的 C4002 警告。

本轮 `scripts/test.bat full` 于 19:58:28–20:00:09（UTC+8）完成，118/118 通过、退出码 0，默认排除 `manual` 诊断。配置 1.05 秒、测试聚合构建 32.57 秒、CTest 65.56 秒；日志 `.codex-probes/pair-extension-full.log`，JUnit `.build/Testing/test-run-01ec06289cdd41058247751eaf9ba31a.xml`。源码、测试、语言资源及依赖输入在检查期间保持一致，复核 46 个修改的非文档文件哈希无变化，快照 `.codex-probes/pair-extension-inputs.json` 绑定基线 `45b70297`、Release、Windows SDK 10.0.26100.0、MSBuild 18.5.4。测试聚合会重新链接宿主，完成后的最终 EXE SHA-256 为 `6acd67ce26dc22c29e0003ff46654b3429e69a8223217c2d4bd5adf186f9fbf3`。

## 待实机步骤

1. 用独立测试文件建立两个不同内容的集合；先观察两种按键提示，再保持指针不动切换 Shift、Ctrl、无键。核对黄色提示与目标高亮同步、文案不截断，Esc 和释放修饰键均不合并或建组。
2. 分别对上表五种方向执行有效操作；核对仅提交一次，合并目标内容及顺序、源组件移除、实际文件仍可打开。集合和桌面文件各测非空源、空源、目标先／后创建；映射组件互拖核对两个标签仍分别打开各自目录。
3. 对集合组和文件组检查目标标签默认打开、两个子组件的内容与设置保留；剩一个自动还原时检查它继承组的当前尺寸，映射路径保持原样。刷新／重新启动后复核结果。
4. 测无键、Alt 和 Ctrl+Shift，不应执行合并或建组。检查自身、不兼容组件、隐藏子组件、移出目标后立即松手，以及新组被邻接组件阻挡时数据不变。
5. 回归拖入已有组与 Dock、翻页、移动手柄与中键拖动、多屏不同 DPI。上述交互均待实机，未使用桌面自动化操作宿主。
6. 把桌面文件组件拖入 Dock 文件区：核对集合式图标显示内容、单击弹窗、弹窗内文件的打开／拖放／排序、重启后仍可打开。从 Dock 拖出后逐项执行上表适用操作；检查静止指针切换按键、Esc、目标失效时 Dock 源保留，成功后 Dock 源消失且内容只有一个归属。
7. 新建集合组及三种组合的文件组，先缩放组，再分别把其中一个标签拖回桌面、移入另一个组、从文件组拖入 Dock 或删除，核对剩余项自动还原为原类型且宽高与还原前的组一致。分别留下集合、桌面文件和映射组件；核对路径、文件、名称、显示设置，刷新与重启后不恢复组。测试组附近拥挤及入组前子组件大于／小于组；手动创建的单项组保持原状。
8. 在独立桌面文件组件、文件组的桌面文件来源标签、Dock 桌面文件弹窗分别通过“新建”创建文件夹和本机可用的文件模板，并测试 Ctrl+Shift+N。核对新项目只属于发起组件、不会留在自由桌面或另一自动收集组件；连续从不同组件新建、取消菜单、等待向导完成，以及刷新／重启后再次核对。自由桌面和映射目录的“新建”继续遵守各自原落点。

## 本轮还原尺寸与新建归属

用户反馈：还原组件应与还原前的组同尺寸；桌面文件组件通过“新建”创建的项目落入了自由桌面。尺寸回归先修改独立预期，再对旧实现运行 `scripts/test.bat name widget_interaction_rules`，三种子组件均因尺寸不符失败，日志 `.codex-probes/group-size-before.log`，退出码 1。

新建路径保留系统菜单，在发起桌面文件组件时捕获稳定 ID 和插入位置，为 Windows `CLSID_NewMenu` 附加 `INewMenuClient`。仅采用 Shell 回调给出的实际创建路径，统一长／短路径别名；不扫描前后差集猜测归属。回调状态由独立共享对象持有，不捕获宿主指针，通过空 Shell 刷新消息唤醒宿主。刷新尚未枚举到输出时保留该输出；消费时重新查找目标 ID、移除临时自动收集归属，并保存到发起组件。重复回调只提交一次；取消、目标删除和过期输出不修改其他组件。右键入口传递文件组的实际来源，快捷键解析当前弹窗／活动文件来源。此变更只涉及宿主内部接口，没有 Lua 公共 API 或布局 schema 变化。

调用链与风险映射：组件右键／Ctrl+Shift+N → Shell New site → 精确路径队列 → 桌面刷新 → `new_item_placement::Apply` → 既有 `CommitKeyedLanding` → 布局与持久化。模型测试覆盖连续独立请求、重复回调、刷新先于枚举、组件向量重排、取消、目标删除、超时及纠正其他组件的自动收集归属。Shell 集成测试在独立临时目录实际调用 Windows NewFolder，使用测试专用消息窗口，不启动或操作桌面宿主；它验证真实创建结果和生产回调入口，不替代菜单、Dock 与文件组的实机验收。

真实 Shell 测试首次夹具过早销毁菜单、缺少命令所属窗口，均修正后再测，不作为原产品缺陷复现。测试还发现系统临时目录短路径与 Shell 回调长路径不同，补充路径归一化后通过。第三方“新建”模板、快捷方式向导及实际弹窗仍待实机；没有声称所有 Shell 扩展都已验收。

定向 `scripts/test.bat name "^(widget_interaction_rules|slot_runtime_contract|shell_context_menu_invoke)$"`：3/3 通过，退出码 0；配置及编译／CTest 计时见 `.codex-probes/new-item-targeted.log`，JUnit `.build/Testing/test-run-659410baf96e4f6cab18651a38351821.xml`。其中 CTest 1.08 秒、测试目标编译 14.72 秒。

隔离副本负向对照 `python .codex-probes/run_new_item_negative.py`：跳过 `INewMenuClient` 回调的路径记录，真实 Shell 创建测试因缺少准确输出失败；跳过模型归属提交，连续创建的唯一归属断言失败。两个变异均编译成功、运行退出码 1；日志 `.codex-probes/new-item-negative.log`，未覆盖生产代码或用户文件。Windows 接口依据：[INewMenuClient 官方文档](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-inewmenuclient) 及本机 Windows SDK 10.0.26100.0 头文件。

本轮标准构建 `scripts/build.bat --reload-shell` 于 20:21:26–20:28:09（UTC+8）完成，退出码 0，生成 `.build/Release/SnowDesktop.exe`，约 403 秒。预检发现运行中的宿主及 Explorer Hook，已事先说明后关闭宿主并重启 Explorer。日志 `.codex-probes/group-new-item-build.log`；只有既有 WinUI 生成头 `GetCurrentTime` 的 C4002 警告。

本次 `scripts/test.bat full` 于 20:28:25–20:29:55（UTC+8）完成，118/118 通过、退出码 0，默认排除 `manual` 诊断。配置 1.02 秒、聚合构建 22.91 秒、CTest 65.03 秒；日志 `.codex-probes/group-new-item-full.log`，JUnit `.build/Testing/test-run-6f4166df40ea43a49a2aa73e571fb7b7.xml`。检查绑定基线 `2ed6dc36` 及本次 13 个非文档输入，前后哈希全部一致，快照 `.codex-probes/group-new-item-inputs.json`；Release、MSVC 19.50.35730.0、MSBuild 18.5.4、Windows SDK 10.0.26100.0。全量聚合重新链接后的宿主 SHA-256：`23d584c3a662a51a641b07029daf9c3ccca51856a5b7999b34628cc7b1c41e2d`。本轮以 `try` 交付，不将构建与模型／Shell 测试通过写成桌面实机验收通过。
