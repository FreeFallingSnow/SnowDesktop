# 状态栏实施与验收记录

本文件用于保留用户要求、实现状态和证据，不能将“实现”“编译”“离线通过”或“用户实测”互相替代。最后更新：2026-09-26。

## 当前优先级

1. **托盘零图标是最高优先级**。对照固定版本 YASB，验证连接、初始重报、图标增删改、点击回调与恢复，不以占位面板代替采集验收。
2. 稳定性优先：顶栏刷新与提示闪烁、弹窗空白／关闭崩溃、原生绘制与窗口生命周期。
3. 收敛交互、主题、设置页和离线视觉测试。控制中心能力扩展不能挤占以上问题。

## 用户反馈清单

| 要求／反馈 | 当前状态 | 验收要求 |
| --- | --- | --- |
| 顶部／底部 AppBar 占位，左右位置取消；按屏全屏隐藏并保留占位 | 已有实现，待多屏实测 | 最大化／贴靠、混合 DPI、非前台屏全屏、热插拔、Explorer 重启 |
| 顶栏和弹窗空白、关闭无响应／崩溃 | 用户报告此前候选未再出现；`2f11ce65` 有限验证记录 | 不能外推为全面稳定；本轮生命周期调整后重新验收 |
| 频繁刷新、提示闪烁 | 栏体重复输入像素一致、相同悬停目标的提示保持稳定；托盘实际控件的未变化图像／提示／布局回归通过，桌面悬停仍待实测 | 离线帧差、交互状态测试；悬停实测 |
| 左中右排布，中间日期时间 | 生产原生布局已离线验证整栏居中、窄屏避让及 1.5 倍缩放 | 各缩放／长文本不重叠、不漂移 |
| 左首按钮为任务管理器、终端、系统设置、锁定、睡眠、重启、关机菜单 | `43b94d67` 已编译，待交互验收 | 与右键顶栏菜单分离；危险操作确认 |
| 右键打开顶栏设置菜单 | 已有实现 | 菜单关闭／全屏切换生命周期 |
| 信息区在托盘左侧，固定宽度 | 生产渲染验证 9%／100% 和不同速率单位更新不改变命中矩形，长网速固定区域增加余量 | 0／9／100%、不同网速单位不改变布局 |
| 信息点击无需面板 → 最新改为简洁曲线＋指标面板 | 四类面板与一分钟共享历史已接入；浅深色实际渲染、断点和逐卡切换检查通过，真实采集交互待实机 | 不进入控制中心；无数据明确不可用；GPU 可选择适配器 |
| 网络、音量、电量为一个固定控制中心按钮 | `43b94d67` 已编译，待交互验收 | 一个点击区域／唯一控制中心，无重复入口 |
| 音量图标滚轮调节 | `43b94d67` 已编译；高精度滚轮与边界定向回归通过，待设备实测 | 连续与高精度滚轮、上下界、设备失效 |
| 满电、充电、接电状态 | 满电／充电原生图标的实际像素已区分，普通电池／不可用状态已导出；真实电池与无电池状态待验收 | 区分充电、满电接电、未充电接电、无电池与未知 |
| 最右通知按钮打开 Windows 通知栏 | `43b94d67` 已编译，待交互验收 | Win11 通知中心／Win10 操作中心 |
| 控制中心参考 Windows／macOS／MyDockFinder／Linux／Android，稳定优先 | 唯一面板已整理无线开关、带数值滑条、媒体与电源入口；两种主题实际控件预览已检查，桌面／设备待实测 | 唯一面板，常用开关、滑条和设备详情；无复杂多弹窗 |
| 顶栏不用 WinUI，顶栏和面板均支持离线渲染 | 顶栏、日历、控制中心、托盘及信息面板均已复用生产绘制离线导出；栏体使用 D2D／DirectWrite，预览不注册 AppBar | 栏体不初始化 XAML；面板共享宿主与主题，生产绘制路径可离线验收 |
| 弹窗四角，尤其底部圆角 | 共用主题边框及窗口区域已接入；日历／控制中心／托盘／信息面板浅深色 PNG 四角已检查，玻璃与桌面仍待验证 | 透明角像素／主题与裁剪离线测试 |
| 日历参考图：压缩左侧当日信息、月历、日程；管理跳转设置页 | 紧凑当日区、完整六周月历、只读日程和管理跳转已实现；两种主题／DPI 离线图片已检查 | 设置跳转及真实日程交互仍待实机；不重复实现日程 CRUD 编辑器 |
| 设置只保留左按钮与信息区显示开关；控制中心不配置细项 | `43b94d67` 已编译，待设置界面验收 | 折叠分组，移除独立网络／音量／电量和控制能力开关 |
| 缩放数值、恢复默认、控件右对齐；排序不可用 | `43b94d67` 已编译，待设置界面验收 | 沿用其他页 NumberBox／Slider／Reset；移除失效排序 |
| 主题在主题与材质页，全局／独立／自定义 | 已有实现 | 浅／深／高对比、弹窗主题统一 |
| 设置分组在桌面与系统界面之前，名称不叫桌面栏 | 已改为 Dock 与系统栏 | 旧路由、搜索、历史、全部语言 |
| 状态栏／任务栏图标不统一 | `43b94d67` 已编译；浅深色 20／40 px 离线图已检查，待设置窗口验收 | 同版 Fluent 顶／底面板 Filled＋Regular、原始路径、同尺寸配色 |
| 托盘参考 YASB，关于页与相关文档注明 | 已包含关于页、文档与许可；`043cb72b` 追加弹窗参考、固定／折叠分工、稳定更新与实际控件离线预览 | 固定提交、版权、分发许可和链接；真实第三方菜单及焦点仍待实测 |
| 用 Markdown 跟踪全部提示；原生离线渲染与其余可自动化验收 | 持续在本文件跟踪；栏体和四类面板均接入生产渲染，窗口／设备实机项继续保留 | 同一生产绘制路径；不能用另画的示意图冒充 |
| 菜单→搜索后菜单残留选中，搜索不能随其他应用激活关闭 | `ca15c27b` 已编译延迟切换；`be8432e1` 已编译鼠标焦点框处理，待原场景实测 | 覆盖嵌套消息循环后的焦点归还；原场景实测 |
| 亮度实际成功却提示失败 | `ca15c27b` 已编译有界延迟回读、按控制和设备过滤过期结果；定向和负向测试通过，待实机 | 延迟状态、快速滑动、失败后成功、设备失效 |
| 控制子页需合理分组，切换面板大小立即更新 | 已接入切页同步测量定位及分组设备子页；实际控件离线检查通过，桌面尺寸响应待实测 | 设备列表／操作／滑条分区；切页同步测量和定位，不等待采样 |
| 顶栏边框只要面向桌面的一条边 | 共用原生边框函数，关闭四边高光；浅深色顶／底边像素检查与反向边框负样本通过，玻璃／实机待验证 | 顶部下边／底部上边；左右和屏幕外沿无边框 |
| Ctrl＋点击控制中心入口打开系统控制中心 | `43b94d67` 已编译；仍需处理按住 Ctrl 对 SendInput 的修饰键影响，再验收 | Win+A；普通点击仍打开自有面板；不能把 Ctrl＋Win＋A 当作 Win＋A |
| 左侧“更多”按钮不好看 | `ca15c27b` 已编译：固定官方 Fluent `apps_16_regular` 四格图标 | 保持系统快捷菜单语义与相邻搜索图标尺寸一致 |
| 点击状态栏自身空白也关闭菜单／弹窗 | `ca15c27b` 明确关闭动作并取消等待中的切换，`be8432e1` 包含鼠标焦点框调整；本轮重核空白命中与关闭路由、菜单关闭回归及完整测试通过，桌面点击仍待实测 | 空白命中发送关闭动作，不需要先点桌面或其他应用；兼容菜单嵌套循环 |

## 原始计划仍待完成的项目

- 技术范围以用户原文为准：原计划控制中心使用 WinUI，后续明确要求“顶栏不用 WinUI”和顶栏／面板支持离线渲染；不把这自动扩大为必须重写所有弹窗。栏体保持原生绘制，当前面板仍共享现有 WinUI 宿主；后续优先处理布局、圆角、日历简化、信息曲线及离线验收。
- 共享设备服务已接入原生界面；六个 Lua 数据主题由 `ccfdd996` 引入（API v2、feature 门控）。十九个新增控制任务、独立写权限和可信手势／宿主确认尚未完成。
- AMD GPU 90%：原始实机样本仍未取得；GPU 拓扑缓存、按适配器／引擎有效性、PDH 缓冲复用、诊断与系统监控选择仍待处理。不得声称已校准。
- 最新标准构建通过；本轮完整测试 120/120 通过，先前的三项失败已处理并保留原始失败记录。后续代码变化须按依赖更新证据，不能直接沿用本次结果。
- 首版不承载任意 Lua 顶栏布局、不做自有通知历史。系统通知使用 Windows 入口。

## 离线面板预览

`snowwidget preview-native status-bar <输出目录> --appearance light --locale zh-CN --dpi 96 --transparent --canvas-width 1968 --canvas-height 256 --padding 24 --host <SnowDesktop.exe>` 直接调用生产 `BuildStatusBarItems`、命中布局和 `DrawStatusBarContent`，背景复用应用原生主题绘制及 `DrawStatusBarEdge`。输出普通、全部信息、数值更新、悬停、满电、充电、不可用、底栏、窄屏、1.5 倍缩放、固定高对比调色板和重复帧十二种状态。使用固定设备数据、Windows 库存图标，不创建桌面窗口／AppBar、不连接 Explorer 或启动采样。显式 CLI 目标不改变旧 `all`／Lua API v2，旧宿主不支持该目标。预览支持 light／dark；高对比样本仅注入调色板，没有更改系统高对比设置。桌面玻璃模糊不能被该位图导出代表，相关预设明确拒绝，真实系统高对比与玻璃仍待验收。

`snowwidget preview-native resource-panel <输出目录> --appearance light --locale zh-CN --dpi 96 --transparent --canvas-width 1000 --canvas-height 1000 --padding 24 --host <SnowDesktop.exe>` 使用生产 `SystemResourceView` 与共用弹窗边框，覆盖 CPU、物理／提交内存、逐卡 GPU、上下行网速、零占用、预热、不可用、采样断点与部分 GPU 不可用九种状态。只替换采集边界；不读取真实设备、不创建桌面 AppBar。显式目标不加入旧 `all`，Lua API v2 与现有数据字段不变；旧宿主不支持这个新目标。CPU 只展示已有真实采集字段，不用占位频率或物理核心数填卡片。GPU 独立有效性目前仅用于原生界面，公共可选能力仍待实现。共享历史按实际时间保存最近一分钟，缺失样本留断点，最后一个消费者离开时释放历史。九种状态各有浅深色图片，实际控件验证断点、零值、GPU 切换、刷新对象稳定、订阅清理与四角；真实设备、桌面交互、高对比及玻璃仍未验收。

`snowwidget preview-native tray-panel <输出目录> --appearance light --locale zh-CN --dpi 96 --transparent --canvas-width 1000 --canvas-height 1000 --padding 24 --host <SnowDesktop.exe>` 使用生产 `SystemTrayView` 和共用弹窗边框，输出网格、动态更新、整理、空列表、连接中与采集受限六个状态。仅替换托盘快照与执行边界，不启动桌面宿主、Hook 或 Windows 托盘。Windows 库存图标是明确的渲染夹具，不是实际采集证据。整理页使用静态图钉切换按钮，避免动画复选框在 XAML 离线图中丢失勾选；浅深色固定／未固定符号均检查实际像素，移除符号的输出变异会失败。无障碍操作验证固定、排序、原应用激活和关闭后的回调失效；鼠标多击、第三方菜单定位、玻璃和桌面焦点仍属于独立验收。显式目标不进入旧 `all` 集合，不改变 Lua API v2，旧宿主不支持该目标。

控制中心本轮收敛为无线开关、带数值的音量／亮度滑条、媒体和电源入口；子页按设备、状态和操作分组。新增显式 `control-panel` CLI 目标，原有 `all` 与 Lua API 不变。该目标使用生产 `SystemControlView`，仅在设备服务边界替换固定数据，禁止执行真实设备控制／打开设置；验证切页立即请求尺寸更新、Wi-Fi 页之外不扫描、关闭清理订阅。当前已编译并输出 16 张浅深色／不同 DPI 图片；音频和 Wi-Fi 短设备列表底部入口已完整显示，增加真实滚动范围检查。无线按钮现已在浅深色离线图中显示实际选中背景；已补充能拒绝旧错误图片的像素回归，稳定候选完整测试 120/120 通过。玻璃、高对比度和桌面交互尚未验收。

`snowwidget preview-native calendar-panel <输出目录> --appearance light --locale zh-CN --dpi 96 --transparent --canvas-width 1000 --canvas-height 1000 --padding 24 --host <SnowDesktop.exe>` 使用生产 `SystemCalendarView` 和 `CreateSystemPanelFrame`，输出空日程与有日程两张 PNG。该显式目标不属于旧 `all` 集合，不改变已有目标及参数；旧宿主不支持新目标。预览在独立离屏渲染窗口运行，不创建桌面宿主／AppBar、不连接托盘或读取用户日程，使用固定日期和示例日程。控件通过 WinUI `RenderTargetBitmap` 导出；目前只支持 light／dark，桌面玻璃模糊不属于此 XAML 导出范围，玻璃预设会明确拒绝。

离线检查使用真实月历视觉树确认固定月份最后一天完整可见，像素检查四个透明圆角、预期宽度和日程引起的高度变化。该检查不代表日程管理跳转、桌面焦点、高对比度、玻璃模糊、窗口区域或其他面板均已验收。

## 证据与检查点

- `1af7c4a7`、`867e9132`、`b1d32282`：此前原生顶栏／WinUI 弹窗候选。
- `2f11ce65`：用户“打开日历／控制中心再关闭顶栏，没出现空白、无响应或闪退”的有限反馈；无二进制哈希和重复次数，不涵盖新改动。
- `ccfdd996`：六个设备数据主题；标准构建退出 0；相关 8/8 测试通过，尚未完整测试／真实设备 Lua 验收。
- 临时原始日志、负向对照、转储分析保存在 `.codex-probes/statusbar-implementation/`，不作为运行依赖、不写入用户数据。
- 本轮只读证据：宿主 PID 21156，托盘启用；Explorer 51436；实际部署 Hook 的托盘导出存在，DLL 可加载；未发现存活的托盘共享连接，尚不能确定失败阶段。已增加分阶段连接诊断和接收／解析计数，下一候选需读取真实结果。
- 本轮生产采集服务无界面诊断（未启动桌面宿主）：原始解析器连接成功但 `received=88, decoded=0, rejected=88, lastSize=1484, icons=0`。隔离副本仅调整已知前缀的有界复制／扩展包长度后：`received=44, decoded=44, rejected=0, resync=0, icons=15, pixelIcons=15, callbacks=14`，程序退出 0。原始日志：`tray-live/read-df51dce0a7ef.log` 与 `tray-extended-diagnostic/read.log`。**目前仅诊断副本验证，尚待接入生产源码和回归测试**；不涵盖实际菜单交互。
- 2026-09-26 栏体反馈候选：`scripts/build.bat --reload-shell` 退出 0，`SnowDesktop.exe` SHA256 `e118f9f54436bd4477c9299e64ae27613ad79c07b10b3b7f7b1b40237eb18f93`，完整日志 `10-bar-feedback-build.log` 无编译／链接警告。Shell 重载脚本的 `timeout` 输入重定向提示不属于编译失败，最终构建成功。定向测试实际选中 7 项全部通过（`10-bar-feedback-tests.log`，12.76 秒），不是完整测试。隔离负向对照恢复“同目标反复替换提示文本”及“滚轮每次依据旧采样”后各自触发对应失败；正确实现通过（`presentation-negative/`）。状态栏空白关闭和托盘扩展包接入尚未包含在此候选中。
- `43b94d67` 保存上述栏体候选。随后生产源码已接入 1484 字节通知前缀解析；扩展包回归通过，隔离恢复旧长度限制后同一回归失败（`tray-negative/`）。
- 真实 Explorer 链路测试使用独立隐藏测试进程注册临时图标，仅操作自有图标。生产 Hook `4015330d16c9a0aa40cceeac267f7057bbd39ba6bb0b78b3352792ed341c0054` 已收到初始重报、版本 4、动态图像／提示和隐藏状态；**矩形回查失败**：要求 `(120,40,152,72)`，得到 `(120,40,272,112)`。诊断副本将坐标查询第二段改为宽高后，矩形、全部测试点击、新旧版本回调、删除后拒绝旧句柄、连接释放、同进程重连及无重复图标均通过（`tray-geometry-diagnostic/lifecycle-final.log`）。该几何修正尚待接入生产；不以隔离副本通过关闭生产问题。
- 空白关闭／托盘前缀／亮度反馈候选：`scripts/build.bat --reload-shell` 退出 0，日志 `11-tray-dismiss-build.log` 无编译／链接警告，宿主 SHA256 `6ce5cd093672e1501d0ae368d2c344d58fe4af47fe2d6fdaf644724badb6fa43`。定向 5/5 通过：`widget_system_data_provider`、`localization_contract`、`settings_controller`、`dock_and_window_rules`、`ui_animation_scheduler`（`11-tray-dismiss-tests.log`，12.96 秒）。亮度隔离负向对照恢复立即回读／保留过期结果时分别失败，正确实现通过（`brightness-negative/`）。浅深色 20／40 px 设置图标已离线检查（`settings-icons.png`），不等同于应用设置窗口实测。空白关闭、亮度实机与切页尺寸仍待实测；矩形回查问题留给下一次独立修改。

- 托盘几何候选：生产代码已改为向 Shell 返回原点与宽高。`scripts/build.bat --reload-shell` 退出 0，`12-tray-geometry-build.log` 无编译／链接警告；宿主 SHA256 `32eb67113909f9ffdedf9bbad98c76996cce52218867cefd8bd9c810e44d38cf`，Hook SHA256 `92b09ab8e73277cbed2d07b4942e8fe0147c9cb6f8175568e13357c61f125381`。定向 `dock_and_window_rules`、`tray_live_integration` 2/2 通过（9.10 秒），后者未跳过：实际生产 Hook 与自有临时图标验证初始注册、版本、动态更新、隐藏、精确矩形、点击回调、删除和重连。恢复旧长度限制／错误矩形的隔离负向对照分别失败。第三方应用菜单视觉和宿主空白关闭仍待用户实测。
- 同一候选完整检查 `scripts/test.bat full`：117/120 通过，174.18 秒，CTest 退出 8（外层脚本退出 1）；`builtin_widget_source_contract` 缺 `data.audio.devices` 作者清单，`test_selection` 错误假设只有一个 manual 测试，`dock_and_window_rules` 顶层窗口对初始化和稳定刷新失败。完整日志 `12-full-tests.log`，输入绑定 `12-validation-inputs.json`；不得将此次完整检查报告为通过。继续修正前单独保存编译通过的尝试。

- `be8432e1` 保存托盘几何与鼠标焦点候选。随后定位完整检查中的真实层级缺陷：在没有其他置顶窗的隔离桌面上，把背景放到唯一置顶内容窗之后，不会提升背景的置顶属性。生产实现改为在一个事务中分别提升两窗；回归在独立、从不激活的测试桌面执行，不再依赖其他应用的层级。隔离恢复旧实现稳定触发两条原断言失败，新实现通过（`pair-negative/`）；手动筛选遗漏托盘的负向对照触发对应断言（`selection-negative/`）。六个已有数据主题的文档列出完整 feature ID，无新增 API。
- 2026-09-26 最新候选：`scripts/build.bat --reload-shell` 退出 0，无编译／链接警告（`13-regression-build.log`）；定向 `dock_and_window_rules`、`builtin_widget_source_contract`、`test_selection` 3/3 通过（9.98 秒）。`scripts/test.bat full` **120/120 通过**，165.08 秒，退出 0，无编译／链接警告；完整日志 `13-full-tests.log`，JUnit `test-run-5aef9dda91f0418abf5ab4c92f07544f.xml`。输入快照为 `13-validation-inputs.json` 和 `13-final-validation-inputs.json`。完整测试的聚合构建重新链接了相同生产源码：标准构建宿主 SHA256 `7410c8d6655ecde4774487132079b687513e5a9214d6ff73964468bededa3650`，最终宿主 `a499ae71678aad50b61631883d973ad975bce9d5df70639866fc5647689f3cab`；最终 Hook `6b2f04c978639411fc97575d8e6572cbc50dda8c8a43c153595b154ea8dbd527`。生产采集与协议源码未变，托盘真实链路引用上一候选有效结果；本轮未再次广播重注册。空白点击关闭与真实面板外观仍待用户反馈。

2026-09-26 日历简化候选：日历改为压缩当日信息、月历和只读日程，管理入口进入现有设置；新增共享弹窗边框和离线 `calendar-panel` 目标。`scripts/build.bat` 增量重试退出 0，无编译／链接警告（`14-calendar-build-retry.log`；首次失败的 SDK 属性名和头文件问题记录于 `14-calendar-build.log`）。实际预览在 `panel.bitmap` 失败，未产生图片。定向筛选实际命中 4 项，3/4 通过，`widget_author_preview_cli` 在同一尺寸检查失败；6.02 秒，CTest 退出 8，外层退出 1（`14-calendar-tests.log`，JUnit `test-run-1599914e453845babb71204342917ef7.xml`）。记录输入 `14-validation-inputs.json`；此候选未运行完整测试，也未实机验收，先保存编译通过的尝试再继续定位。

- 日历最终候选：`e55f54bd` 保存首次编译通过／渲染失败；`b4e96c6b` 记录实际请求 520×406、返回 780×609 的 150% 缩放证据；`353ec2cd` 导出成功但目检发现月末裁切。本次按默认日期格最小高度 40 DIP 定位，将日期格调整为 28 DIP，复用官方模板与主题并移除内层背景框；相同示例已完整显示六周日期及日程。实际视觉树检查月份最后一天，外层像素检查四角、宽度和日程高度，方形底角像素变异被拒绝。图片位于 `calendar-render-17/`（浅色中文 96 DPI、深色英文 144 DPI；各含空／有日程），不是桌面截图。
- 本候选 `scripts/build.bat` 退出 0，无编译／链接警告（`17-calendar-build-retry.log`；首次缺 Interop 头文件的编译失败保留在 `17-calendar-build.log`）。定向 `widget_author_preview_cli|calendar_service|localization_contract|settings_controller` **4/4 通过**，54.93 秒，退出 0（日志命名为 `16-calendar-tests.log`，实际绑定本候选）。`scripts/test.bat full` **120/120 通过**，170.38 秒，退出 0，无编译／链接警告（`17-full-tests.log`，JUnit `test-run-43555b30d28f4c64bdd8e99ea857b44e.xml`）。输入快照 `17-validation-inputs.json`／`17-final-validation-inputs.json`；最终宿主 SHA256 `7e4e5a7d8066223e72f7e24775f65b1aae82d5febc9f39638ebf4c4bb5432dec`，Hook SHA256 `dc8e41c993e8f632aeebf500aba79f2fd81d60a76fa261cdb848449162b59da7`。日历选择／设置跳转、桌面焦点、窗口区域、高对比与玻璃仍待实机；其他面板和栏体的离线绘制仍未接入。

- 控制中心首轮候选：生产视图整理常用开关、滑条、媒体与设备子页，增加只替换设备边界的实际控件离线 `control-panel` 目标。`scripts/build.bat` 退出 0，无编译／链接警告（`18-controls-build.log`）；定向 `widget_author_preview_cli|widget_system_data_provider|localization_contract|settings_controller` **4/4 通过**，60.72 秒，退出 0（`18-controls-tests.log`，JUnit `test-run-634ffc40d88242fd9f282976427d585d.xml`）。16 张实际控件图片位于 `controls-render-18/`，目检仍发现选中无线按钮背景未稳定、长子页底部入口裁切，保持开放；不将 PNG 生成或外框断言视作视觉通过。本候选未运行全量和实机；输入绑定 `18-final-validation-inputs.json`，最终宿主 SHA256 `24de2fb9d5f357495aca65c6e4b9eafc19178e165160ea89dd101d074a3c35a3`。

- `1459e787` 保存控制中心首轮。后续将设备页视口上限调整到 540 DIP（仍受实际屏幕工作区限制），没有位置权限错误时收起空错误行和位置设置按钮，避免占用常用控制空间；保持实际模板，仅在离线树完成 Storyboard 动画。`scripts/build.bat` 退出 0，无编译／链接警告（`19-controls-build.log`）；相关 6/6 测试通过，63.71 秒，退出 0（`19-controls-tests.log`）。其中 `modern_menu_interaction` 检查显式关闭正在运行的菜单循环，`dock_and_window_rules` 覆盖状态栏基础交互规则；这些并非用户桌面空白点击端到端验收。浅深色图片 `controls-render-19/` 已确认音频／Wi-Fi 底部入口不裁切，控件树检查短列表不存在额外滚动；首页无线选中背景仍异常，继续定位。未运行本候选全量或实机；输入绑定 `19-final-validation-inputs.json`，宿主 SHA256 `58e57bec947230a4e18e84b030065678a7b6245c91899c7fc6d1a01da542b270`。

- `67a68421` 保存设备页可见范围候选。随后离线渲染在移除画刷过渡后重新进入控件原有视觉状态，不触发 `IsChecked`／点击操作；浅深色实际首页图片已正确出现选中背景，19 号图片保留为失败对照。`scripts/build.bat` 退出 0，无编译／链接警告（`20-controls-build.log`）；显式 `control-panel` 浅色 zh-CN 96 DPI／深色 en-US 144 DPI 各输出 8 状态，均退出 0（`20-render.log`，`controls-render-20/`）。本候选暂未重跑 CTest／全量，待补充像素回归；不沿用 19 号 CTest 为本次执行。输入绑定 `20-validation-inputs.json`，宿主 SHA256 `a68340d1650f9ef0560ade4fe962295bbf4fcbbd8b35d65cf7d7b6100903b6c5`。

- `a9007432` 保存离线选中捕获。新增无线按钮像素回归的首次定向构建成功，但 CTest 1/1 失败（6.33 秒，退出 8／外层 1；`21-controls-tests.log`）。原始 19 号浅深色图被预期拒绝，20 号正确图片也被误拒绝：断言错误要求不可用按钮背景 alpha > 240，实际浅色为 210、深色为 109，这是主题透明度，不是渲染失败（`read-radio-pixels.py` 输出、`21-radio-negative.log`）。先记录编译通过但测试失败的独立尝试，再修正该断言；不得把这次测试当作产品问题复现或通过。

- `e3e2c434` 记录首次像素断言失败；后续保留透明材质的正确预期。定向 `scripts/test.bat name "^widget_author_preview_cli$"` **1/1 通过**，50.43 秒，退出 0，无编译／链接警告（`22-controls-tests.log`）。同一检查对 19 号浅深色错误图退出 1，对 20 号正确图退出 0，失败信号为选中填充不可区分（`22-radio-negative.log`）；原先 alpha 错误不算产品复现。
- 稳定候选 `scripts/test.bat full` **120/120 通过**，165.83 秒，退出 0，无编译／链接警告（`22-full-tests.log`，JUnit `test-run-c0eb34e2517d4f8eb1bec54a7018073b.xml`）。生产源码仍对应 `a9007432`，标准构建引用本轮 `20-controls-build.log` 的有效结果，21／22 仅调整测试和文档，没有重跑或声称新的标准构建。最终宿主 SHA256 `a42c13250e9d69a46be1014a97fad730ca716e963e1c86b577b0fe3135954e70`，Hook `b8722e4092fcb09659c010b1a0f77da9f3761659946907affbadd71b2aaef15c`；输入绑定 `22-before-full-validation-inputs.json`／`22-final-validation-inputs.json`。
- 本检查点已覆盖菜单显式关闭、生产控制中心 8 状态 × 浅深色／不同 DPI、四角、短设备列表底部入口、切页尺寸请求和无设备副作用；用户状态栏空白点击、真实设备操作、玻璃、高对比和桌面尺寸响应仍待实机。栏体／托盘／信息面板的离线绘制及 AMD 原始样本等开放项目不因本次通过而关闭。


- 托盘视图首轮候选：生产托盘面板提取为 `SystemTrayView`，固定项只留在栏体，展开网格复用未变化的图像／提示；整理页保存固定与顺序，保留离线应用身份，像素缺失／异常清除旧图。新增显式 `snowwidget preview-native tray-panel`，旧 `all` 和 Lua API v2 不变；使用 Windows 库存图标夹具与动作记录边界，不接入 Explorer、不启动桌面宿主。YASB 固定版本的 popup／widget 参考已补入许可说明。
- `scripts/build.bat` 重试退出 0，无编译／链接警告（`23-tray-build-retry.log`）；初次编译的 SDK SymbolIcon 属性／枚举错误保留于 `23-tray-build.log`。定向 `widget_author_preview_cli|localization_contract|settings_controller|modern_menu_interaction|dock_and_window_rules` **5/5 通过**，64.68 秒，退出 0，无编译／链接警告（`23-tray-tests-retry.log`，JUnit `test-run-0610c33467dd4cb6b2d1491f5ba145e6.xml`；初次命令被 shell 的管道引用拒绝，未执行测试，记录于 `23-tray-tests.log`）。宿主 SHA256 `a5969013e6029b718f86037ad79f6743e8130746b3a449de894961a1e4253445`；输入 `23-final-validation-inputs.json`。
- 浅色 zh-CN 96 DPI／深色 en-US 144 DPI 各 6 状态实际控件图在 `tray-render-23/`，内置检查通过：固定与隐藏项排除、动态像素／提示、异常图清理、未变化对象及布局稳定、无障碍激活、固定保存、排序边界／离线身份、关闭后旧控件不再执行。目检仍发现 WinUI 复选框动画勾选未进入离线图片，只显示选中底色；先保存本次编译通过的尝试，再收敛该控件。完整测试未运行；实际第三方菜单、鼠标多击／焦点、玻璃及高对比仍待验收。


- `043cb72b` 保存托盘首轮候选；随后整理页改用官方标准 ToggleButton 和静态图钉，固定与排序图标按列对齐。实际浅深色图片 `tray-render-24/` 中，固定／未固定图钉均可见，六种状态保持紧凑布局及四角。`scripts/build.bat` 退出 0，无编译／链接警告（`24-tray-build.log`）；定向 `scripts/test.bat name widget_author_preview_cli` **1/1 通过**，57.22 秒，退出 0，无编译／链接警告（`24-tray-tests.log`，JUnit `test-run-cef0afc09c2446659753d1632a5cd136.xml`）。新增符号像素检查同时拒绝“删除图钉但保留按钮／面板”的输出变异；这验证输出检查的失败信号，不等同于真实设备或鼠标点击验收。
- 稳定候选 `scripts/test.bat full` **120/120 通过**，166.20 秒，退出 0，无编译／链接警告（`24-full-tests.log`，JUnit `test-run-4f5f36ed35b9463689bf7fdd16ba06ce.xml`）。输入绑定 `24-before-full-validation-inputs.json`／`24-final-validation-inputs.json`；最终宿主 SHA256 `469ae0e8bdeff47346686187d9bfa2e2e511a838f9b5b8a402200a99c4863abc`，Hook `f142832d4668333ef2873f40dd4604c29b6fa5710e72017ed731868617d67b9f`。当前采集／Hook 源码没有修改，真实 Explorer 链路沿用 12 号有效证据，本轮未广播重注册；UIA 动作使用执行边界记录，未控制真实应用。
- 本检查点覆盖实际托盘控件的固定／折叠、动态更新与不变对象稳定、异常像素清理、整理／排序保存、关闭后的旧回调失效，以及日历、控制中心与托盘的离线回归。状态栏空白点击、第三方托盘菜单及焦点仍待用户实机；信息曲线、栏体离线绘制、修饰键、Lua 写任务与 AMD 样本等开放条目保持开放。


- 信息面板首轮：`scripts/build.bat` 退出 0，无编译／链接警告（`25-resource-build.log`），宿主 SHA256 `cfa727f4a5d19292918adf97009486bf256bbeab60f4478a9551d9caeca38d49`。实际生产控件首次离线导出失败：`resource metric card clipped its value`（`25-render.log`）；尚未确定是布局裁切还是检查边界问题，不能声称视觉通过。隔离副本使用同一历史单元断言，正确实现通过，恢复缺失值填零、取消秒级合并、保留过期样本三个错误均产生预期失败（`history-negative-25/`）。本候选定向 CTest 和完整测试尚未运行，先按编译成功尝试保存，再修改。桌面空白关闭保持待实机。


- 信息面板后续候选：`05f460fe` 保存首轮实现及失败记录。首轮检查把短文本宽度误作卡片宽度，现改为检查实际卡片与文字边界；无需放宽卡片最小宽度或忽略裁切。`scripts/build.bat` 退出 0，无编译／链接警告（`26-resource-build.log`）。四类生产面板与零值、预热、不可用、断点、GPU 部分不可用共九状态 × 浅深色／不同 DPI 实际导出成功（`resource-render-26/`、`26-render.log`）；已目检曲线、双通道、指标卡片、不可用提示和四角。CPU 不虚构未采集的频率／物理核心指标。
- 定向 `scripts/test.bat name "^(widget_author_preview_cli|widget_system_data_provider|localization_contract|settings_controller|dock_and_window_rules|modern_menu_interaction)$"` **6/6 通过**，92.61 秒，退出 0，无编译／链接警告（`26-resource-tests.log`；`test-run-7daa05286d074eb1a227c99a9775c1e2.xml`）。新增图表检查确认实际曲线像素变化并拒绝冻结曲线的输出变异；控制树检查 GPU 立即切换、缺失值不填零、相同数据不重建图表、关闭后不保留订阅／读取源。历史单元负向证据沿用相同输入的 `history-negative-25/`。
- 本候选 `scripts/test.bat full` **120/120 通过**，195.68 秒，退出 0，无编译／链接警告（`26-full-tests.log`；`test-run-d6ae6604055744caa49c294e737a044e.xml`）。输入绑定 `26-before-full-validation-inputs.json`／`26-final-validation-inputs.json`，最终宿主 SHA256 `5adfdf10a18533636131f6d6617f797bb1601af8c0990f74f6b768830690e73e`，Hook `11663d6bf5ea5956e171587d29b8ec171d4863976b45cfef8ab690248c9bf376`。采集与面板共享现有单一调度；新增历史及 GPU 原生有效性没有改变已有 Lua JSON。GPU 专用／共享显存仍沿用旧采集成功边界，独立公共能力与 AMD 原始样本校准保持开放。
- 空白点击沿用生产显式关闭路由，本轮菜单关闭与窗口规则回归通过，不等同于用户桌面点击端到端验收。栏体离线绘制、真实第三方托盘菜单、Ctrl 修饰键、Lua 写任务、设备控制与玻璃／高对比等剩余要求继续保留，不能因本轮全量通过而关闭。


- 栏体原生预览候选：窗口与离线导出共用内容构建、D2D／DirectWrite 绘制、布局及点击命中；背景关闭四边高光，只绘制朝向桌面的边线。网速仍保持固定宽度，扩大长单位的余量。原窗口保留 AppBar、DComp 提交及屏幕托盘几何；位图路径没有创建窗口或采样连接。`scripts/build.bat` 退出 0，无编译／链接警告（`27-bar-build.log`）。
- `bar-render-27/` 包含十二状态 × 浅深色、96／192 DPI、中文／德文的 24 张实际原生 PNG；本轮已目检普通、信息更新、窄屏、充电、满电与固定高对比调色板。几何断言检查整栏居中、区域不重叠、长文本容纳、点击与画面一致、空白无命中，以及信息采样更新前后的矩形稳定。像素断言检查真实托盘位图、充电／满电符号、悬停、重复帧一致与顶／底单边边框；恢复反向边框的输出变异被拒绝。该证据不代表真实桌面焦点、AppBar 协商或系统高对比已经验收。
- 定向 `scripts/test.bat name "^(widget_author_preview_cli|dock_and_window_rules|modern_menu_interaction|localization_contract)$"` **4/4 通过**，96.91 秒，退出 0，无编译／链接警告（`27-bar-tests.log`；`test-run-866ead5d09ba459d99f721488156a22f.xml`）。`scripts/test.bat full` **120/120 通过**，203.31 秒，退出 0，无编译／链接警告（`27-full-tests.log`；`test-run-a95edf8f4b9346d5a45a71244e7bc609.xml`）。输入绑定 `27-before-full-validation-inputs.json`／`27-final-validation-inputs.json`，最终宿主 SHA256 `9eb4f43694c5c0fac640ab3752aba6385ff99ec1c604c485fe9d93db3976f7a6`，Hook `78a2b19c4782968a9f6b2609d8ed2afc9f8721c479b0b732c2b6f691f39fe2ba`。
- 栏体和各面板的离线生产路径现已具备；全屏／混合 DPI／热插拔窗口交互、真实第三方托盘菜单、Ctrl 修饰键、Lua 控制写权限与任务、AMD 原始采样校准、玻璃和系统高对比仍未完成。另有托盘提示文字单独变化仍触发内容绘制的效率改进空间，当前重复帧像素检查不等于所有无效重绘已消除。

## YASB 参考边界

固定来源、复用范围及许可见 [third_party/yasb/README.md](../third_party/yasb/README.md)。不能把它的私有协议当作 Microsoft 稳定 API。

需要逐项对照：真实 Explorer 托盘定位、Hook 附着就绪后 TaskbarCreated、旧式工具栏补充、32 位 wire handle 与原生 cbSize、GUID／窗口身份、NIM_SETFOCUS 与未知包、version 4 回调、图标矩形、动态像素、丢包重同步、Explorer 和宿主退出。采集线程不得等待宿主，未知布局必须明确降级。

## 交付规则

每次编译通过后先独立提交 `try` 再进入下一轮代码修改。更新本文件的状态和命令证据；视觉离线测试、逻辑测试、实际托盘读取与用户桌面交互分别记录。尚未实现或失败的条目保持开放。
