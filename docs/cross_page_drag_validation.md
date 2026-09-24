# 跨页拖拽坐标验证（2026-09-24）

本记录对应 `release/v1.0.7.0` 的跨页拖拽候选。用户报告鼠标、虚影和落位之间出现偏差；桌面原场景尚待用户实机验证。

## 调整与影响范围

- 普通图标和整块组件的网格落位先按鼠标选择页面，再将起拖偏移换算得到的原点吸附到最近的格子原点。此前使用原点选择页面，并按格子的右/下边界命中：跨屏边界可能命中旧页，吸附也可能落后近一格。
- 热边和点击翻页不再提前迁移、重排或保存选中图标。它们与快捷键翻页一样只切换视图，起拖原点、相对布局和源归属保留到松手提交。去除隐藏组件 bounds 变化引发的按下点重设。
- 普通桌面图标保留起拖时实际图标矩形，避免目标页的图标布局参数改变虚影内部的图像位置。大图标仍使用现有缩略预览规则。
- 多选虚影明确记录按下的项目索引，文字区起拖及翻页重绑后不依靠缩小的图标矩形重新猜测主项目。列表及扇形来源已启用的指针锚定继续使用鼠标所在格。
- 拖动整块组件期间不删除临时引导组件，以免重排组件数组后使拖拽索引指向其他对象。
- 修改宿主内部几何与交互，不涉及公共组件 API、清单、权限或持久化格式。

首两轮候选的调用链：图标提示、预览和提交均经 `ResolveDesktopRequestCell`；整块组件预览和松手采样均经 `CellFromDragOrigin`。两者复用 `ResolveGridDragCell`。列表、Dock、外部来源及组标签的指针落位规则保留。后续组件跨屏比例调整见文末。

## 自动验证

- `scripts/test.bat name "^(dock_and_window_rules|slot_runtime_contract|desktop_drop_cache|widget_interaction_rules)$"`：4/4 通过，Release，退出码 0。测试执行 2.60 秒；日志 `.codex-probes/cross-page-drag/targeted.log`，JUnit `.build/Testing/test-run-d4181b3bc5554fc5a81d08cbfc0ec7fb.xml`。
- `grid_drag_geometry_cases.h` 调用生产几何函数，覆盖跨屏接缝、反向负坐标、格内与间隙吸附、同屏替换不同尺寸网格、翻回原页、组件跨度与边缘约束。会话重绑测试检查起拖虚影及提交坐标在运行时对象重建后保留。
- 隔离副本用相同用例恢复旧选页规则，2 项失败；恢复旧吸附规则，3 项失败；未变异基线退出码 0。各副本以 MSVC `/W4 /WX` 编译，日志 `.codex-probes/cross-page-drag/negative-controls.log`。这些失败来自落点断言，不是编译或环境错误。
- 翻页路径的源码负向边界禁止直接迁移/移动选中项、保存布局及重新计算组原点；此检查只保护入口边界，不代表桌面交互已经执行。
- 旧翻页入口的隔离源码副本触发 6 处上述边界失败，当前源码通过；日志 `.codex-probes/cross-page-drag/boundary-negative.log`。
- 补齐主虚影识别后的 `scripts/test.bat name slot_runtime_contract`：1/1 通过，Release，退出码 0，测试执行 1.61 秒；日志 `.codex-probes/cross-page-drag/primary-targeted.log`。使用同一测试函数在生产会话头文件的隔离副本中恢复旧识别逻辑，两项行为断言失败；基线通过，日志 `.codex-probes/cross-page-drag/primary-controls.log`。

最终候选（首轮提交 `11d0369e` 加本记录所属后续 `try` 提交的源码）：

- `scripts/test.bat full`：本次 120/120 通过，退出码 0；配置 1.71 秒、编译 473.90 秒、测试执行 131.75 秒、入口总计 608.57 秒。日志 `.codex-probes/cross-page-drag/full.log`，JUnit `.build/Testing/test-run-ee8bf576cd8f4f8b82e8c956d3c83250.xml`。默认排除的 `manual` 诊断未运行。
- `scripts/build.bat`：本次标准 Release 构建通过，退出码 0，35.20 秒；日志 `.codex-probes/cross-page-drag/build-final.log`。最终输入与上述全量一致。首轮另已执行 `scripts/build.bat --reload-shell`，498.90 秒通过；该轮关闭了 SnowDesktop 并重启 Explorer，最终构建预检无占用。
- 完整编译日志未发现编译或链接警告。工具链：CMake 4.3.1、MSVC 19.50.35730.0、Windows SDK 10.0.26100.0；使用仓库 Release/tests 预设，未改变构建开关。
- 最终 `.build/Release/SnowDesktop.exe`：17,091,072 字节，SHA-256 `9137adf24bc7f9517fc7940bef473de572472a13993e78e43956548fc6710977`。
- 未启动桌面宿主进行视觉验证。用户原有的审计文档修改与附件目录未纳入提交。

## 待实机矩阵

使用本轮标准构建生成的 `.build/Release/SnowDesktop.exe`，保留真实布局，优先在目标页空白区域比较，避免自动避让占用格干扰判断。

| 场景 | 检查结果 |
| --- | --- |
| 单个图标从图像或文字区起拖，热边/快捷键翻页 | 虚影与鼠标关系不跳动；松手与落位提示一致 |
| 多选图标，从非第一项起拖，目标页原位置有占用 | 翻页不提前拆散或迁移来源；最终保持相对布局；Esc 后来源不变 |
| 组件从不同位置抓取，跨页后立即松手、翻回原页 | 按下偏移不累计变化；落位与预览一致，跨度不被裁剪 |
| 不同屏幕布局、网格密度、DPI，含左侧/上方负坐标屏 | 页面跟随鼠标选择；接缝处不会因组件原点仍在旧屏而选错页 |
| 大图标、混合选择、集合/文件夹条目、Dock 拖出与 OLE 返回 | 原有缩略、指针落位和会话恢复行为仍可用 |
| 目标格占用、边缘剩余空间不足、取消 | 允许现有避让/拒绝和边缘限制；不丢失来源数据，不把这些情况计作无约束坐标对齐 |

这些桌面视觉、交互和兼容性检查尚未执行，自动化结果不能替代实机验收。

## 组件跨屏比例抓取的后续尝试

用户在 `9cd36209` 后反馈同屏“好像没啥问题”，但不同屏幕间仍有偏移。这是部分场景的初步反馈，不视为上述完整矩阵已经通过。

- 旧路径一直使用源屏像素抓取偏移，而组件预览会随目标屏网格改变宽高。现有 CU 是横纵网格比例取较小值的单一缩放，不能表达横纵比例各不相同的目标网格。
- 左键移动手柄的两个入口及中键移动入口现在都通过 `CaptureGridDragAnchor`，在按下时从原始组件矩形记录横纵抓取比例。手柄在矩形外时保留有符号比例；翻页后不再读取可能已隐藏或重排的源组件矩形。
- `ResolveGridSpanDragCell` 按鼠标选择目标页，分别用目标网格的实际跨度宽高恢复抓取点，再选择距离该抓取点最近的可放置行列。尺寸计算包含间距和网格取整，与 `GetGridRect` 一致；边缘保持完整跨度，超过整页时沿用页面裁剪规则。
- 组件移动预览和松手时的最后一次 `OnMouseMoveAt` 采样共用这个函数，提交沿用同一个 `widgetPreviewCell_`。本轮不改变图标的像素虚影策略、Dock/分组/合并路由、持久化格式或公共组件 API。
- 影响映射：`app.h` 中的私有组件状态、两个按下处理文件、共享网格几何头及其现有 `dock_and_window_rules` 测试目标；原有图标选页和吸附函数只抽出共用选页步骤，计算规则保持。由于共享几何和应用状态参与多个模块，本候选重新运行全量测试，不把前轮全量当成本轮结果。
- 新回归直接调用生产捕获/落位函数，以独立的源矩形与目标落点覆盖大屏到小屏、小屏到负坐标大屏、非等比网格、隐藏源页后换网格、往返、矩形外手柄及边界约束。它们只证明几何规则，不代表宿主实际交互已经执行。
- 隔离副本恢复固定像素偏移后，7 项行为断言失败；错误裁剪手柄抓取比例后，1 项失败；同一输入的候选基线退出码 0。全部以 MSVC `/W4 /WX` 编译通过，日志 `.codex-probes/widget-drag-anchor/controls.log`。

本轮验证绑定 `d40f35ac` 后本节所属 `try` 提交的原生源码、测试及共享几何头；工具链、SDK 和 Release/tests 预设沿用上文记录，没有修改构建开关或运行时资源。用户原有审计文档和附件修改不纳入本次提交。

- `scripts/test.bat name "^(dock_and_window_rules|slot_runtime_contract|desktop_drop_cache|widget_interaction_rules)$"`：本次 4/4 通过，退出码 0；配置 2.04 秒、编译 17.00 秒、CTest 2.31 秒。日志 `.codex-probes/widget-drag-anchor/targeted.log`，JUnit `.build/Testing/test-run-9f526eb5ca6343ce87768d1747226d18.xml`。
- `scripts/test.bat full`：本次 120/120 通过，退出码 0；配置 1.67 秒、编译 24.05 秒、CTest 130.55 秒、入口总计 157.32 秒。日志 `.codex-probes/widget-drag-anchor/full.log`，JUnit `.build/Testing/test-run-f31fdfb56c974ae2b0813c6b702dcb8e.xml`。默认排除的 `manual` 诊断未运行。
- `scripts/build.bat --reload-shell`：本次标准 Release 构建通过，退出码 0，484.86 秒；日志 `.codex-probes/widget-drag-anchor/build.log`。执行前已告知并关闭 SnowDesktop、重启 Explorer。脚本内 `timeout` 在重定向输入下提示不能等待，后续配置、编译和产物整理正常完成。
- 全量后执行 `scripts/build.bat` 完成最终 Release 构建和产物整理，退出码 0，33.50 秒；日志 `.codex-probes/widget-drag-anchor/build-final.log`。预检无占用，最终构建与上述测试的原生源码和测试输入一致。
- 已检查两次标准构建、定向/全量测试构建及隔离对照的完整编译日志，未发现编译或链接警告。最终 `.build/Release/SnowDesktop.exe` 为 17,092,096 字节，SHA-256 `5c4de9563fa2372e87c4e343cf8b789a9b6080b90b23a1b3e3a55b6c2420357e`。

原始跨屏交互仍待用户使用本轮产物实测，重点核对两屏双向移动、靠近边缘及跨屏后翻回；网格吸附和页面边缘约束造成的离散位移仍属预期规则。未启动宿主做桌面交互自动化，不把自动测试通过记为原问题已经修复。
