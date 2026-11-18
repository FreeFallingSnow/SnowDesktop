# SnowDesktop 代码优化状态

从“下一项”接手。认领时把该行改为“进行中”，填任务 ID 和下一步。实现已提交但仍有验收项时标为“待验证”，写清剩哪一项；确认受阻时标为“阻塞”，写清恢复条件。证据齐全后标为“已验证”。已验证事项若在后续改动中失效，重开原行并注明依据。更新与对应改动一起提交；同次提交的验证可记入提交正文，用 Git 文件历史追溯。

| 状态 | 优化事项 | 任务或证据 | 下次从这里继续 | 更新日期 |
| --- | --- | --- | --- | --- |
| 已验证 | 大图标配置值与 JSON 编解码分离 | `9e3c5e81a`；标准 Release、`behavior_recovery` 和 `behavior_settings-host` 通过；独立头 `/Zs` 通过；与 `501607ae3` 的 5 组往返和 6 项拒绝对照共 11,799 字节一致 | 已收尾；构建和对照记录见本行 Git 历史 | 2026-11-18 |
| 候选 | widget_engine.cpp · 音频任务结果与事件分流 | [ApplyWidgetTaskBrokerActions](src/widget/runtime/widget_engine.cpp) 在开头消费播放消息，末尾派发积存的 Lua 事件。基线 `2f8326173` 共 36,725 行 | 先固定两阶段执行顺序，再把消费逻辑提到 `widget/runtime/` 私有实现。它只借用播放服务和 broker，处理任务完成或取消，按原序返回普通事件。验收正常完成、任务缺失、已请求取消以及嵌套唤醒时的派发次序；引擎保留重入保护与 Lua 回调。 | 2026-11-18 |
| 候选 | widget_view_tree.cpp · 普通及进出场转场校验 | [ValidateNode](src/widget/view/widget_view_tree.cpp) 同时处理整树预算、子节点递归和本节点属性。基线 `2f8326173` 共 6,322 行 | 将 `transition`、`enterTransition`、`exitTransition` 的局部参数校验提到 `widget/view/` 私有实现，只传转场数据并返回校验结果及错误。递归、预算和键集合仍归遍历器；在原调用位置核对合法输入、多项同时非法时的首条错误及文本。 | 2026-11-18 |
| 候选 | widget_package.cpp · 清单预览字段解析 | [ReadManifest](src/widget/packages/widget_package.cpp) 读取预览声明，[ValidateDirectory](src/widget/packages/widget_package.cpp) 随后检查实际文件。基线 `2f8326173` 共 5,134 行 | 将 `previewGallery`、`previewData` 及其 variants 的 JSON 读取提到 `widget/packages/` 私有实现，只产生预览字段和有序诊断。文件检查与发布仍归原入口；对照字段缺省、有效旧清单、部分错误和超限输入的解析值及完整诊断顺序。 | 2026-11-18 |
| 候选 | 存储与设置重复校验 UTF-8 | 已定位事务、值编码、设置服务三处字节校验 | 对照三处边界输入，将键长、NUL 及错误信息留在原调用方 | 2026-11-18 |
| 候选 | 弹窗图标与文字重复计算几何 | [图标和文本矩形](src/app/popup/app_popup_geometry.cpp) 分别推导名称列和扇形条目 | 接手前与当前弹窗改动对照，先选一种几何计算验证 | 2026-11-18 |
| 已验证 | snowwidget 三个契约导出器共用字符串转义 | `cd63cbd24`；Release 定向链接，三条命令合计 363,891 字节与旧工具一致 | 已收尾 | 2026-11-18 |

工具命中保留在审计报告里。能指出具体调用方、可单独处理的边界和一个验收结果时，再登记为候选。[审计与复查方法](scripts/code-health.md)

上述三项按行内职责验收。候选实现应位于原大文件之外，由原生产入口调用，接入实际消费该实现的目标并通过相应行为对照。只在原文件内提取函数可记录为中间进展。关闭一项只表示该职责已拆出，其余职责按各自证据另行登记。

在多个项目的共同父目录运行以下命令，可以按文件路径汇总采用同名状态表的项目。日常只看需要行动的行。

```powershell
rg --with-filename --glob CODE_HEALTH.md '^\| (下一项|进行中|待验证|阻塞) \|' .
```
