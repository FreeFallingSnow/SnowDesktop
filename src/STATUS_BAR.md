# 状态栏实施与验收记录

本文件用于保留用户要求、实现状态和证据，不能将“实现”“编译”“离线通过”或“用户实测”互相替代。最后更新：2026-09-26。

## 当前优先级

1. **托盘零图标是最高优先级**。对照固定版本 YASB，验证连接、初始重报、图标增删改、点击回调与恢复，不以占位面板代替采集验收。
2. 稳定性优先：顶栏刷新与提示闪烁、弹窗空白／关闭崩溃、原生绘制与窗口生命周期。
3. 收敛交互、主题、设置页和离线视觉测试。控制中心能力扩展不能挤占以上问题。
4. 用户要求集中完成已收到的修改后再统一编译；停止逐项构建。41 号首轮标准构建因用户重新启动宿主而 LNK1104 失败，尚无成功候选，不作为交付；继续合并处理反馈后统一验证。

### 当前集中候选（44，编译通过，待验证）

- 按用户要求暂停未定位的鼠标卡顿调查，保留 43 号转储分析；没有证据将约 400 MiB 私有提交归因为内存泄漏，也没有宣称卡顿已解决。
- 同侧靠边 Dock 自动合并到状态栏，中央保留原生 Dock，栏体统一背景／边线，按屏匹配并共用 AppBar 占位；合并时关闭会侵入侧区的图标放大。不同侧和悬浮 Dock 保留原布局，无合并开关。
- 托盘改为紧凑直接拖拽：在图标间排序、拖到栏体固定、拖到展开入口或展开面板取消固定；过滤控制中心重复的系统音量／网络／电池 GUID。关闭与失去捕获清理拖拽；第三方菜单激活期间保留展开面板。
- 对照固定 YASB 补齐新版托盘原始右键 down/up 及 context 回调；微信菜单仍待真实应用验收。WPS 越界场景尚未取得，不对任意第三方窗口实施无依据的强制移动。
- 控制区三个图标使用独立提示，音量分档图形并保持滚轮调节；充电采用官方 Filled 几何与绿色。GPU 仅合并重复身份和没有独立有效样本的别名，保留同型号有效多卡。
- 收拢控制页设备／网络列表与底部入口；Wi-Fi 去重并移除已保存列表，日历选择／第二历法与今天入口同步；弹窗使用现有动画调度器滑入／收回，窗口区域随内容裁剪。
- 设置 IPC 补齐菜单和搜索开关及排列字段，信息默认关闭；完整主题预设保留旧映射；普通菜单只留设置及任务管理器，电源操作确认。此前各条“待实现”以本候选为待编译实现，不能算通过。
- 离线 status-bar 预览追加 merged 样本，验证侧区不侵入中央，不把空出的区域冒充已渲染的真实 Dock。该命令输出数量增加，旧样本保留。设备、真实第三方菜单、多屏合并及玻璃动画仍需实机。
- 初步独立证据：`42-ipc-repro.py after` 使用与旧版失败样本相同的 Pack/Unpack 输入，结果由开关被还原为 true 改为两项均 false，退出 0；不是设置窗口交互验收。`44-negative.py` 正确 GPU 展示函数退出 0，隔离副本恢复原始重复列表时以“duplicate GPU presentation”退出 1；两次探针编译 `/W4 /WX` 通过。标准构建、生产渲染与整体回归结果尚待补充。
- `scripts/build.bat --reload-shell` 退出 0，Release 宿主生成，无编译／链接警告（`44-batch-build.log`）；输入绑定 `44-checkpoint-validation-inputs.json`。脚本的控制台等待出现 input-redirection 提示，但实际构建成功，不将此误作编译错误；生产控件渲染和回归尚未运行。按仓库规则先保存此编译候选，验证中如需调整再独立记录。
- `b2c4a63e` 检查结论：定向测试在 GeneralSettingsTests 编译阶段失败，新增用例缺少 `snowdesktop::` 命名空间，未执行 CTest，不计为通过。栏体 13 状态、日历、紧凑托盘和资源面板的浅深色导出成功；控制面板只导出 overview，预览收尾仍按旧 ScrollViewer 根节点转换而报 E_NOINTERFACE（`44-render-controls.py.log`），后续页未验证。审查另发现新音频／Wi-Fi ListView 的 SelectionChanged 捕获自身形成引用环，需要去掉强自引用；这不是用户卡顿的已确认原因。下一候选集中处理这些验证发现。
- 45 号集中调整：去掉列表事件的强自引用，改从事件 sender 读取选择；离线预览追加关闭后实际列表弱引用释放检查。同步移除预览旧根节点转换、补齐测试命名空间，合并侧区预留增至 480 DIP，使普通宽屏可同时容纳四类信息；窄屏仍整项避让。待本轮构建和测试。
- 45 号 `scripts/build.bat` 退出 0，无编译／链接警告（`45-batch-build.log`）。预检宿主未运行、Explorer 未加载 Hook，无需再次重启 Shell；后续渲染及回归仍待执行。

## 用户反馈清单

| 要求／反馈 | 当前状态 | 验收要求 |
| --- | --- | --- |
| 顶部／底部 AppBar 占位，左右位置取消；按屏全屏隐藏并保留占位 | 已有实现，待多屏实测 | 最大化／贴靠、混合 DPI、非前台屏全屏、热插拔、Explorer 重启 |
| 顶栏和弹窗空白、关闭无响应／崩溃 | 用户报告此前候选未再出现；`2f11ce65` 有限验证记录 | 不能外推为全面稳定；本轮生命周期调整后重新验收 |
| 频繁刷新、提示闪烁 | 栏体重复输入像素一致，托盘仅改提示不再使绘制失效，图像变化仍更新，相同悬停目标的提示保持稳定；托盘实际控件的未变化图像／提示／布局回归通过，桌面悬停仍待实测 | 离线帧差、交互状态测试；悬停实测 |
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
| Ctrl＋点击控制中心入口打开系统控制中心 | 本轮候选记住点击意图，异步等待修饰键松开再发送 Win+A；空白点击／隐藏／前台切换取消，只在实际点击入口读取一次；编译与真实调度／失败边界检查通过，系统实机仍待验收 | Win+A；普通点击仍打开自有面板；不能把 Ctrl＋Win＋A 当作 Win＋A |
| 左侧“更多”按钮不好看 | `ca15c27b` 已编译：固定官方 Fluent `apps_16_regular` 四格图标 | 保持系统快捷菜单语义与相邻搜索图标尺寸一致 |
| 点击状态栏自身空白也关闭菜单／弹窗 | aeaeda62 已将空白关闭提前到按下，清除悬停与键盘高亮并取消等待中的切换；26d313a2 追加撤销托盘焦点票据，避免旧请求再次激活。最新定向／完整回归通过，桌面点击仍待实测 | 不依赖窗口失焦或完整抬起；不需要先点桌面或其他应用；保留按钮点击及空白右键菜单 |
| 充电图标内部填满绿色 | 新反馈，待实现 | 顶栏和控制中心，浅深色可见；保留充电闪电识别 |
| 托盘应用右键菜单打开时展开面板保持 | 新反馈，待实现 | 第三方菜单显示／关闭、面板外点击、焦点和全屏清理 |
| 托盘外露和排序改为直接拖拽；移除打开系统托盘按钮和标题，紧凑布局 | 新反馈，待实现 | 固定／取消固定及排序拖拽，不经整理页；窄屏、DPI、空列表与受限降级 |
| 控制中心箭头跟随蓝色背景，使用 >／< 折线符号 | 新反馈，待实现 | 选中按钮与详情入口背景统一，返回和前进符号、浅深色与点击区域 |
| GPU 选择列表重复同名设备去重 | 新截图反馈，待实现 | 剔除没有独立有效数据的重复展示项；保留真正可用的同型号设备并区分身份 |
| 控制中心各子页先对齐 Windows 原生布局，声音页保留音量调节 | 新截图反馈，待实现 | 统一头部／开关、可选择设备和网络列表、底部设置入口；音量／静音，空状态和长列表 |
| 日历左侧显示设置的第二历法；日期选中描边不应断续 | 新截图反馈，待实现 | 复用日历设置、日期切换；浅深色和混合 DPI 的连续描边 |
| 日历左侧显示选中日期而非始终今天；今天按钮移到月历顶栏 | 新反馈，待实现 | 星期／日／年月／第二历法同步选择；今天入口回到当月并选中当天，午夜更新不覆盖用户选择 |
| WPS 托盘菜单忽略顶栏高度而溢出 | 用户确认是菜单向上超出屏幕、部分内容看不到，不是状态栏遮挡；待核对 | 检查实际图标矩形、点击坐标、工作区及 YASB 行为；须取得 WPS 原始菜单场景，原应用自绘菜单存在适配边界，不承诺强制移动所有第三方菜单 |
| CPU／内存／GPU／网速占地过大，全部移到左侧 | 40 号已编译；浅深色、中文／德语、96／192 DPI 生产离线渲染通过，桌面待验收 | 位于菜单和搜索之后；CPU／GPU 68 DIP、网速 152 DIP，内存按本地化最大值预留；时钟居中，右侧托盘、控制中心和通知；窄屏与 DPI 离线渲染 |
| 状态栏弹窗接入边缘滑出动画 | 新反馈，待实现 | 复用现有动画设施；顶栏向下、底栏向上，关闭收回；数据刷新与子页切换不重复播放，遵循系统动画设置，快速切换和销毁安全 |
| 蓝牙关闭后按钮直接不可用，不能重新打开 | 40 号已编译；无线电和设备读取错误边界测试、关闭状态实际控件离线渲染通过，实际关→开待验收 | 区分无线电关闭、硬件缺失、权限拒绝；关闭状态保持可开启，验证关→开及设备消失 |
| 状态栏默认跟随全局主题，不是跟随弹窗主题 | 40 号已编译；全局／独立／自定义解析与负向回归通过，桌面待验收 | 栏体独立解析全局／预设／自定义；日历、托盘与控制中心等自有面板才复用弹窗主题；默认项复用全部语言已有“跟随全局主题”文案 |
| 状态栏主题缺少全局预设，Dock 的“跟随组件”应改为“跟随全局主题” | 新反馈，待实现 | 跟随全局、完整全局预设、自定义；保留旧设置映射，不将旧模式静默解释成不同外观 |
| Wi-Fi 不需要已保存网络区 | 新反馈，待实现 | 展开只展示当前网卡的可用网络；管理入口交给系统设置，后端功能不因界面简化丢失 |
| 面板滚动条应贴近外缘 | 新截图反馈，待实现 | 内部内容保留间距、滚动区域延伸至外缘；浅深色、DPI、圆角和长列表 |
| 可用 Wi-Fi 重复显示 HIT-WLAN，空名称只显示信号百分比 | 新截图反馈，待实现 | 明确网卡与网络身份去重，隐藏 SSID 使用真实本地化名称；当前连接状态、连接按钮与参数一致 |
| 状态栏系统菜单与快捷搜索两个开关无法关闭 | 新截图反馈，待排查保存链路 | 设置控件→模型→规范化→持久化→生产栏体；关闭、重新打开设置和重启保持 |
| CPU／内存／GPU／网速信息默认关闭，不自动开启 | 新反馈；模型已有默认 false，待补全首次启用和缺省配置回归 | 首次启用状态栏不自动开启四项，离线预览样本不得修改用户设置 |
| 状态栏常规右键菜单只保留“状态栏设置”和“任务管理器” | 新反馈，待实现 | 删除关闭、位置和主题等重复入口；左侧系统快捷菜单独立保留 |
| 左侧系统菜单电源操作必须二次确认 | 新反馈，待核对并补全 | 睡眠／重启／关机确认后才执行，取消不发出操作，保持唯一确认入口 |
| 微信托盘右键无法唤起菜单 | 新反馈，正在检查回调协议、鼠标事件与面板失焦链路 | 微信原场景，栏上与折叠菜单分别验证；不以消息已发送当作菜单成功显示 |
| Dock 靠边且与状态栏同侧时合并一条栏，Dock 居中、其他控件两侧 | 用户明确要求满足条件直接合并，不提供关闭开关；实现中 | 共用背景和占位、时钟避让、DPI／窄屏、多屏、全屏与拖拽；不同侧或离边自动分离 |
| 隐藏与控制中心重复的系统托盘音量入口；三个状态图标提示不同，音量按档位显示、支持滚轮 | 新反馈，待实现 | 保留同组唯一控制中心入口；分别命中网络／音量／电量提示，静音与音量档位，滚轮只在音量区域生效 |
| 软件输入无响应、整机鼠标卡顿、约 400 MB 内存 | 已保存用户转储并检查线程和内存，根因未确认；用户要求暂时继续新功能 | 保留原始场景证据，下次复现需卡顿期间的线程／输入延迟与退出前后对照；不得标作已解决 |

### 输入卡顿转储排查（43，暂缓）

- 用户提供 `SnowDesktop.DMP`，SHA256 `ab16e786871992fa6785846540674558990dc75b5b544a781eb8ca252d2e1760`，副本与解析位于 `.codex-probes/statusbar-implementation/input-hang-43/`。转储时间 2026-09-26 11:15:14，PID 32224，运行 35 分 19 秒，与 10:20:09 编译的宿主符号匹配。
- 两份相邻快照的私有提交内存均约 400.10 MiB；普通 NT 堆提交约 175.87 MiB。转储文件大小不等同于进程工作集，短期稳定也不能排除缓慢泄漏。201 个线程中，134 个的非系统等待栈来自 NVIDIA 用户态驱动，不能全部归因为应用创建。
- 主线程及唯一低级鼠标钩子线程均处于消息等待，未捕获持续死锁；这不能排除间歇阻塞。未发现本次对应的新崩溃记录。旧 Hook 模块被安全固定加载，模块残留本身不能证明采集线程仍在运行。
- 用户自行重启后的只读 10 秒采样：私有内存约 342～351 MiB、196 线程，系统可用内存约 12.5 GiB；不是复现期间的数据，也不是性能验收。没有自动重启宿主、修改用户配置或进行桌面交互验收。
- 用户要求找不到根因就继续新功能，本项暂缓，不添加未经证据支持的“修复”，也不将 400 MB 单一数字认定为根因。当前批次尚未重新编译；后续统一验证时仍需遵守占用预检。

### 左侧信息、全局主题与蓝牙关闭候选（40）

- 信息移到菜单／搜索之后并压缩固定宽度；栏体继承实际全局外观，保留独立预设和自定义。蓝牙无线电与配对设备读取分开处理，关闭时不扫描；成功读取的无线电不再因设备列表读取失败而被丢弃。公共 `bluetooth.devices` 仍为 API v2，通过原有 error 表达部分失败，已在编辑前公告并更新作者文档。
- `scripts/build.bat --reload-shell` 退出 0，无编译／链接警告（40-feedback-build.log）。定向 general_settings、widget_system_data_provider、widget_author_preview_cli、localization_contract、dock_and_window_rules 共 5/5 通过，57.09 秒，退出 0（40-feedback-tests.log；test-run-d9d6b9c4890441f2abf7a1061fbd8deb.xml）。
- 正确生产 helper 隔离副本通过；关闭时扫描、丢弃已读无线电、将全局主题替换为弹窗默认值三种负向变体均在对应断言失败，最终 /W4 /WX 无警告（40-negative.log）。栏体中文 96 DPI／德语 192 DPI 各 12 张、控制中心中文 96 DPI／英文 144 DPI 各 9 张均经生产离线入口生成并检查；蓝牙关闭仍可点击，信息宽度固定。证据为 bar-render-40/、controls-render-40/；不代表真实设备、桌面玻璃和系统高对比验收。
- `scripts/test.bat full` **119/120，通过项不计作完整通过**，退出 1（CTest 8）；40-full-tests.log、test-run-e38e6e9f2b184f19b7477f14d8f15020.xml。steam_workshop_manager 的 7 条失败断言仍将作者工具包修订号写为 13，实际已在 39 号升级为 14；先保存此已编译候选，再修正该检查，保留失败证据。输入及产物哈希见 40-final-validation-inputs.json。
- WPS 已确认是菜单向上超出屏幕，不是层级遮挡；尚未取得实际菜单窗口定位证据，未声称解决。边缘动画、日历、紧凑拖拽托盘、充电填色、控制中心布局与其他开放项继续保留。

### GPU 可选 Lua 详情候选（39）

- 新增 `data.system.gpu.details` 与 `system.gpu` 的 `includeDetails=true` 选项，API v2、旧订阅字段和权限保持；新增逐卡有效性与引擎编号／标签／占用。类型声明、作者文档、注册表和离线宿主预览同步。新组件仍须最低宿主版本和 feature 检测。
- 同一采样保存详细快照，避免旧兼容路径的可用性防抖掩盖首个无效读数；最后消费者退出清理详细快照。不增加 PDH 查询或采样线程。系统监控的适配器选择尚未完成。
- `scripts/build.bat --reload-shell` 退出 0（39-gpu-build-link.log），最终成功编译／链接无警告。最初注册表长度编译错误已纠正；两次宿主重新启动导致的链接占用和一次预检拦截保留在其他 39-gpu-build 日志。Shell 重载时的 timeout 输入重定向提示不属于编译警告。
- `scripts/test.bat name "^(widget_api_registry|widget_author_tools|widget_author_lint|widget_author_preview_cli|widget_system_data_provider|widget_data_broker)$" 6/6 通过，51.17 秒，退出 0，无编译／链接警告；39-gpu-tests.log，test-run-5adeffbe22eb41e3a3c193fa5c62eee6.xml。实际 Lua serializer、旧／新订阅、严格参数、无副作用预览均执行；五种序列化负向变体均被拒绝，正确副本通过（gpu-lua-negative-39/）。
- 输入及产物哈希见 39-final-validation-inputs.json。本检查点未运行新的完整测试，待后续稳定候选补全；不沿用 38 号全量作为本次执行。AMD、真实设备故障恢复及桌面交互待实机；新收集的视觉反馈仍保持开放。

## 原始计划仍待完成的项目

- 技术范围以用户原文为准：原计划控制中心使用 WinUI，后续明确要求“顶栏不用 WinUI”和顶栏／面板支持离线渲染；不把这自动扩大为必须重写所有弹窗。栏体保持原生绘制，当前面板仍共享现有 WinUI 宿主；后续优先处理布局、圆角、日历简化、信息曲线及离线验收。
- 共享设备服务已接入原生界面；六个 Lua 数据主题由 `ccfdd996` 引入（API v2、feature 门控）。十九个新增控制任务、独立写权限和可信手势／宿主确认尚未完成。
- AMD GPU 90%：问题机器原始样本仍未取得。35 号候选已接入拓扑缓存、独立显存有效性、PDH 缓冲复用及内部按需原始诊断；当前 NVIDIA／Intel 实测已取得，但不是 AMD 对照。36 号候选已纳入类型名称为空的合法编号引擎，并用真实原始行与负向对照验证解析，标准构建和完整测试通过。37 号已接入用户可用 gpu-diagnostics 导出入口并完成实际 CLI 采样，38 号标准构建与完整回归通过；Lua 可选详情能力和系统监控适配器选择仍待完成，不得声称 AMD 已校准。
- 38 号曾完整测试 120/120 通过；40 号最新已编译候选的完整测试是 119/120，失败见上方记录。当前未编译编辑不引用旧结果为通过；下一稳定输入须重新验证。
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
- 栏体和各面板的离线生产路径现已具备；全屏／混合 DPI／热插拔窗口交互、真实第三方托盘菜单、Ctrl 修饰键、Lua 控制写权限与任务、AMD 原始采样校准、玻璃和系统高对比仍未完成。该候选仍有托盘提示文字单独变化触发内容绘制的问题，后续轮次单独记录调整证据。


- 快捷键／提示刷新第一候选：点击时保留 Ctrl 系统面板意图，异步等待修饰键与目标字母松开再发送 Win+A／Win+N；空白点击、栏体隐藏或前台切换取消，五秒超时，部分输入失败只补发抬键。托盘提示元数据不再使原生内容绘制失效，位图变化及隐藏仍会更新；没有公共 API 变化。
- 标准 scripts/build.bat 退出 0、无编译／链接警告（28-interaction-build.log）。定向 scripts/test.bat name "^(widget_author_preview_cli|dock_and_window_rules|modern_menu_interaction|ui_animation_scheduler)$" **4/4 通过**，93.40 秒，退出 0（28-interaction-tests.log；test-run-c7fc65e0f3db43a28a28df5d4f281cf3.xml）。interaction-negative-28/ 正常调度及同一内容比较函数通过，忽略修饰键、丢失意图、取消部分输入恢复、恢复旧提示比较四种负样本产生预期失败；仅替换系统键盘状态／输入边界，没有真实桌面按键注入。输入绑定 28-checkpoint-validation-inputs.json。
- 本候选不进入全量检查点：复查发现普通点击在菜单延迟分支仍可能再次读取 Ctrl 状态，下一候选将判定移到真实点击入口，只读取一次。系统快捷键、空白关闭及菜单／搜索焦点仍待桌面实机；现有离线通过不证明 Shell 接受按键。


- 最终点击入口候选：在 StatusBar 的用户输入回调中只解析一次 Ctrl，ActivateStatusBar 的菜单延迟分支直接接收已确定的目标。普通点击与 Ctrl 点击都不会因稍后修饰键变化而重新解释；空白关闭仍取消同一等待 token。基于 [Microsoft SendInput 文档](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput) 对现有键盘状态的说明，等待物理修饰键释放，不合成其释放／恢复；这没有改变公共 API、权限或配置。
- scripts/build.bat 退出 0、无编译／链接警告（29-intent-build.log）。最终输入 scripts/test.bat full **120/120 通过**，191.58 秒，退出 0、无编译／链接警告（29-full-tests.log；test-run-56addd5cf57b4ff0b6579b3086396e76.xml）。第一候选 e9d8011c 的定向 4/4 与负向证据仍适用于未变的调度器、内容比较和离线预览；最终全量另外覆盖了本次入口移动后的输入。证据绑定 29-before-full-validation-inputs.json／29-final-validation-inputs.json；宿主 SHA256 e46b7406229f4892cac974e1445ae964dc128b8e783829f553d4ecc2f028a011，Hook 0b2c965373a908655d2ac79658811dbb6a4132b30fc54f83e6326e2c8b68f73e。
- 系统快捷键、Windows 通知、空白关闭及菜单／搜索焦点仍待桌面实机，自动检查不能证明 Shell 接受按键；其余原始计划开放项继续保留。代码复查还发现托盘固定项动态插入／排序后悬停仍按索引定位，需后续检查其身份对应（推导风险，未实机复现）。


- 动态目标审查：栏体原本将悬停和键盘焦点保存为数组索引，鼠标抬起直接命中当前列表；托盘增删／重排可能把旧提示、焦点或按键释放交给新位置的另一应用。现有固定宽度及绘制设施可以继续复用，不增加界面入口或配置。
- 共享 StatusBarInteraction 已编译并通过下述逻辑／渲染回归：键盘按身份重定位、被移除时清空；目标顺序变化清理原生提示及 Leave 回调；左右键按下／抬起和双击按身份配对。空白完整点击保留关闭路由，隐藏、移出与捕获取消清理未完成手势，已移除固定项清除旧屏幕矩形。target-negative-30/ 已确认正确实现通过、旧索引／残留悬停／释放给替换图标／跨图标双击四种变异失败，标准构建与整体回归证据见下一项，桌面实际交互仍待验证。


- 动态目标候选验证：scripts/build.bat 退出 0、无编译／链接警告（30-target-build.log）。定向 scripts/test.bat name "^(dock_and_window_rules|modern_menu_interaction|widget_author_preview_cli)$" **3/3 通过**，91.09 秒（30-target-tests.log；test-run-3e1174ab271240908ee32ab48544729a.xml）。完整 scripts/test.bat full **120/120 通过**，196.79 秒，退出 0、无编译／链接警告（30-full-tests.log；test-run-6423f0d31f3646c98313ac80d977b95d.xml）。
- 同一生产交互状态配合独立矩形夹具，验证增删／排序后的键盘身份、清空焦点、隐藏项跳过、左右键目标核对、双击身份、空白点击及取消；生产原生渲染路径继续通过实际 PNG 回归。target-negative-30/ 中四种错误变异分别触发确定失败，无模拟 Explorer 菜单或真实输入。模型证据不代表 Windows 实际鼠标消息时序与第三方菜单焦点已经验收。输入绑定 30-before-full-validation-inputs.json／30-final-validation-inputs.json；宿主 SHA256 b14594f2249d689a856db9ee97d643c05a834a7ac6d1be157d8d3e8022c72d45，Hook f8d6496b75e7e9b22defdaebef1cdd2545ee271cf5946b9154187877a4f5c111。
- 后续键盘入口审查：当前栏体 WM_CONTEXTMENU 的键盘分支仍直接打开栏体菜单，应将聚焦托盘图标的上下文操作交给原应用。tray::Activation::ContextKeyboard 后端已有，但本轮没有改变该入口；后续补齐可达性与回调验证。玻璃、设备、Lua 写任务、AMD 原始样本等原始开放项保持。

### 空白按下关闭候选（31）

- 本次反馈聚焦状态栏自身空白：此前已有显式 Dismiss 路由，但触发依赖完整按下／抬起配对；不激活的栏体不会让弹窗自然失焦。改为左键按下空白立即关闭自有菜单、面板及搜索，并取消待执行切换／系统快捷键。清除提示、悬停与键盘高亮，不为这次手势保留抬起激活；空白右键及实际按钮保持原行为。
- 复用 StatusBarInteraction 与既有应用关闭入口，不新增配置、公共 API 或另一套弹窗机制。新增回归在现有窗口规则测试检查按下即关闭、抬起不重复、拖入按钮不激活及空白右键保留；菜单集成测试在真实菜单嵌套循环内使用同一生产交互模型，仅替换栏体窗口和命中矩形。
- 标准 scripts/build.bat 退出 0，无编译／链接警告（31-blank-build.log）。定向窗口规则与菜单交互 **2/2 通过**，13.49 秒（31-blank-tests-retry.log；test-run-dfb1ff69605a40618d574fb4b45e9ee3.xml）；初次 shell 引用被拒绝，未执行测试，原始记录在 31-blank-tests.log。完整 scripts/test.bat full **120/120 通过**，186.21 秒，退出 0，无编译／链接警告（31-full-tests.log；test-run-c57ce5061ec544f4922ad3bb5069366b.xml）。
- blank-negative-31/ 使用生产交互头文件与同一独立矩形测试：恢复“等抬起才关闭”的隔离变异退出 1，触发立即关闭／不重复分发两项预期失败；新实现退出 0，/W4 /WX 编译通过。菜单集成使用真实菜单窗口与嵌套消息循环，验证空白按下隐藏菜单且取消后的抬起不激活；未替代应用级搜索／面板关闭或实际桌面鼠标端到端验收。
- 输入绑定 31-before-full-validation-inputs.json／31-final-validation-inputs.json；最终宿主 SHA256 34dd391b17718226a3ff02539e0a6b19d93ec6264a4b0c3c215eaf37b73f8666，Hook 757dbacae9ba50a19a5e8afd5f3c824bf65061607fc351c9be9aeb78baace816。实际桌面窗口未启动／自动操作，用户原场景仍须手动确认；托盘键盘入口等其他开放项保持。

### 托盘键盘候选（32）

- 上一轮空白按下关闭已保存为 `aeaeda62`，属于已产生代码与验证证据的进展；本轮继续键盘菜单入口审查。栏体此前吞掉所有 WM_KEYDOWN，WM_CONTEXTMENU 的键盘分支固定打开栏体菜单，Enter／空格重复消息也会再次激活。原生 HWND 与隔离窗口现在共用键盘分发和目标解析：保留 Windows 默认键处理，按聚焦身份将托盘上下文交给原应用，使用图标自身矩形；内置按钮仍打开栏体菜单。Escape 复用空白关闭入口；按住激活键只执行一次，方向键继续允许连续移动。
- 根据 [WM_CONTEXTMENU](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-contextmenu) 与 [Shell_NotifyIcon](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shell_notifyiconw)，旧版键盘操作使用完整右键按下／抬起；version 3 保留完整 ID，version 4 保留有符号屏幕锚点与打包 ID。固定 YASB 源码的 mouse handlers 不证明键盘行为；本次为自主实现，并更新参考边界。编辑前已公告对第三方托盘键盘回调的行为调整，未修改 Lua API v2、feature 或配置。
- 独立 Win32 探针确认 Shift＋F10 通过 DefWindowProc 产生 sign-extended (-1,-1)，单纯 SendMessage 的 Apps 抬键不代表 OS 原始输入路径（32-win32-key-probe.log，探索性探针退出 1）。生产解析同时接受两种坐标扩展方式；正式回归用真实 Shift＋F10 默认处理与文档规定的 Apps 上下文消息，替换原始键盘输入边界，不注入全局按键。键盘状态仅改测试线程并恢复。
- scripts/build.bat 退出 0、无编译／链接警告（32-keyboard-build.log）。定向窗口规则／菜单交互 **2/2 通过**，13.62 秒（32-keyboard-tests.log；test-run-2db418ca77a64de9b75a8d1d8daf7f05.xml）。完整 scripts/test.bat full **120/120 通过**，196.56 秒，退出 0，无编译／链接警告（32-full-tests.log；test-run-57b2d31c2f2c4aaebf7e6b43a8d8d8f4.xml）。
- keyboard-negative-32/ 使用生产头文件、回调实现及原测试函数，正确窗口／回调两组退出 0；错误上下文类型、重复激活、吞掉默认按键、缺失右键按下、误发左键手势五个隔离变异均退出 1 并触发相应失败断言；全部 /W4 /WX 编译通过。Windows 真实 Shift＋F10、缺失／隐藏焦点和 Escape 已覆盖，Apps 原始键盘路径仍只验证其文档消息入口。
- 输入绑定 32-before-full-validation-inputs.json／32-final-validation-inputs.json；最终宿主 SHA256 667faeaf3a6b4af46be1257b9dd8ab422ded4d9e9f4f400dd2438b8177600050，Hook 60752e6aa7b59359f0b9259087a30a3c92c4fe03fdaae1630686eb417f9808d1。未启动或自动操作桌面宿主；NIM_SETFOCUS 的焦点返回、第三方应用菜单以及桌面键盘操作仍待处理／实机验收，不能由上述消息验证代替。采集端仍忽略 NIM_SETFOCUS，服务没有返回至原始栏体／弹窗的事件路由；后续需带代次、目标与前台检查，避免抢焦点。

## YASB 参考边界

固定来源、复用范围及许可见 [third_party/yasb/README.md](../third_party/yasb/README.md)。不能把它的私有协议当作 Microsoft 稳定 API。

需要逐项对照：真实 Explorer 托盘定位、Hook 附着就绪后 TaskbarCreated、旧式工具栏补充、32 位 wire handle 与原生 cbSize、GUID／窗口身份、NIM_SETFOCUS 与未知包、version 4 回调、图标矩形、动态像素、丢包重同步、Explorer 和宿主退出。采集线程不得等待宿主，未知布局必须明确降级。

## 交付规则

每次编译通过后先独立提交 `try` 再进入下一轮代码修改。更新本文件的状态和命令证据；视觉离线测试、逻辑测试、实际托盘读取与用户桌面交互分别记录。尚未实现或失败的条目保持开放。

### 托盘焦点返回首轮（33）

- 在 1066ac17 后接入 NIM_SETFOCUS：栏体和托盘展开页记录原栏体、图标身份和用户操作序号；Hook 非阻塞读取票据，后台传输返回事件，UI 单次消费。固定图标返回自身，折叠图标返回现有托盘入口，不重开面板。空白按下关闭继续使用既有 Dismiss 路由，并取消焦点返回；全屏／隐藏、图标失效、Explorer 代次变化和前台切换也会拒绝旧请求。
- 内部宿主／Hook 共享协议升级 v2，成套更新，Lua API v2 未改。YASB 固定版未实现该返回路径，本轮为基于微软文档的独立实现，来源边界同步 third_party/yasb/README.md。
- scripts/build.bat 退出 0、无编译／链接警告（33-focus-build.log）。定向窗口规则通过，modern_menu_interaction 在隔离桌面前台窗口准备步骤失败，未运行到新增 Hook 路径：**1/2 通过**，7.52 秒，CTest 退出 8、外层退出 1（33-focus-tests.log；test-run-49c8140a9852449faa797b7b30048f58.xml）。未切换输入桌面的隔离桌面不能取得全局前台；不能将其记录为生产焦点缺陷复现或通过。下一轮需将此前台边界显式替换为受控状态，并保留实际子类、工作线程和队列。
- focus-negative-33/ 的生产模型正确副本退出 0；忽略前台切换、忽略代次、忽略最后前台检查、重复认领四种变异均退出 1，命中预期断言，/W4 /WX 编译无警告。本候选未运行全量；桌面空白关闭和第三方托盘菜单焦点仍待用户实机。输入与产物绑定 33-final-validation-inputs.json。

### 托盘焦点边界与稳定检查点（34）

- 26d313a2 保存生产实现及隔离前台准备失败。此轮只调整测试边界与 CMake 接入：隔离桌面不切换输入桌面，全局前台查询／授权／激活采用受控状态；直接编译生产 collector.cpp，保留真实 HWND 的归属、可见性、子类、工作线程、共享内存队列、宿主状态机与焦点防护。它验证逻辑和消息传输，不代表 Explorer 实际授予前台权限。
- 定向 **2/2 通过**，13.33 秒，退出 0、无编译／链接警告（34-focus-tests.log；test-run-9b728a926f0d4793819fe2d8f3326e77.xml）。验证 GUID-only NIM_SETFOCUS、返回原图标／托盘入口、重复／隐藏／关闭／代次失效、授权拒绝和原生回退。focus-collector-negative-34/ 正确实现退出 0，丢失序号、接受隐藏目标、重复返回、越过前台保护四种变异均退出 1 命中对应断言；/W4 /WX 无警告。最初负向副本混用了原始与复制头文件而编译失败，修正副本包含路径后才取得上述有效证据，编译失败不计作负向发现。33 号生产模型负向证据仍适用于未改的生产源码与模型测试。
- **scripts/build.bat 退出 0，无编译／链接警告**（34-focus-build.log）；**scripts/test.bat full 120/120 通过**，190.55 秒，退出 0、无编译／链接警告（34-full-tests.log；test-run-63762d805cce4ff0ad3d2d1125d4fdd9.xml）。这是当前稳定输入的有效全量，包含实际原生／WinUI 离线渲染回归。
- 新版宿主／Hook 协议另行执行 scripts/test.bat name "^tray_live_integration$"：**1/1 通过**，0.80 秒（34-live-tests.log；test-run-c7cf07a6e57e48258f5e8b77124f0ab7.xml）。真实 Explorer 连接、先启动应用的重报、动态图标、隐藏标记、矩形、v4／旧回调、删除与同宿主重连全部通过；仅调用测试进程自己的图标，未启动桌面宿主，未操作第三方菜单，未重启 Explorer。该测试没有触发真实 NIM_SETFOCUS，不外推其前台行为。
- 输入绑定 34-before-full-validation-inputs.json／34-final-validation-inputs.json；最终宿主 SHA256 4be18010ea5b371c646fa5850f801688e13284d2e7b91ac344cbbdbeebe3f276，Hook SHA256 b2239682e7515266c48c8055d982fc6dbea2db19f6e18dd22569cc922044f5a2。空白点击关闭、第三方菜单焦点、玻璃／设备、Lua 写任务、AMD 样本等原始开放项保持；自动检查与用户实机验收分别记录。
- 真实 Explorer 诊断使用的 DLL 由定向测试构建再次链接，相同生产输入；实际临时副本与 .build/Release/SnowDesktopTaskbarHook.dll 哈希一致：7aeeb15efa62e9d5130256c5b8ff9836aae0492103f12a1923cdabc226364368。上条 Hook 哈希为标准构建的 Runtime 副本，两者分别保留，不能混作同一二进制。完整原始诊断输出保留在 34-live-raw.log。

### GPU 采集缓存与有效性候选（35）

- 从共享 provider 提取 WidgetGpuSampler，原生界面与 Lua 继续复用同一订阅采样；最后 GPU 消费者离开时释放查询、设备拓扑及缓冲。DXGI 拓扑使用 IsCurrent 与可用的适配器变化事件失效，复用 PDH 查询／数组；挂起前后的 wall／unbiased 时间差触发基线重建，普通长间隔不视作挂起。真实热插拔和系统休眠仍待硬件验收。
- 专用／共享显存按完整 LUID 独立记录有效性，不因另一项缺失丢弃已知读数。整卡仍采用同一物理引擎多进程求和、整卡取最忙引擎；内部保留引擎身份、未截断合计与样本数，仅明确请求时复制原始计数器。Lua API v2、现有 JSON 字段不变；本轮尚未增加公共诊断 CLI／可选详情 feature。
- scripts/test.bat name "^widget_system_data_provider$" **1/1 通过**，1.99 秒，退出 0（35-gpu-tests.log；test-run-081fd7aa32df43e3bcc0d70fcc4d6589.xml），编译目标 SnowDesktopWidgetSystemDataProviderTests 无警告。复用生产缓冲与合计函数，替换的仅是 PDH 读数组返回值／尺寸边界；覆盖部分失败、全 LUID、不可信扩容尺寸、有界重试、零值／溢出和挂起判断。gpu-negative-35/ 四个隔离变体分别恢复耦合显存、每次分配、不重新探测尺寸、长间隔误判，均命中预期断言退出 1；正确副本退出 0，/W4 /WX 编译无警告。
- 生产 sampler 的 12 次真实只读采样保存于 35-live-raw.jsonl／35-live-summary.json：前 10 次查询创建 1 次、拓扑刷新 1 次、数组扩容 6 次后不再增长；第 11 次显式重置查询并预热，第 12 次恢复。第 10 次关闭诊断即无诊断对象，Reset 后资源和统计清空。该探针直接链接生产 sampler，未启动桌面宿主。
- 本机 DXGI 列出 NVIDIA GeForce RTX 5070 Ti Laptop GPU／Intel Graphics，以及两个同名 NVIDIA 条目；分别使用 LUID 106542／112208／168939／163941。后两个无占用或显存计数器，仍为不可用，不借用同名条目。实测样本不能代表 AMD 90% 问题机器；可选机器／型号问题尚未收到回答。
- 真实样本检查发现已有解析会拒绝 pid_4_luid_0x00000000_0x0001B650_phys_0_eng_10_engtype_ 等类型名称为空的合法编号引擎；每个有效间隔有 6 条，本轮均为有效 0，当前显示最大值未受其影响。原始记录 35-empty-engine.json；若这些引擎存在负载则可能漏算，保持开放并在下一候选修正，不能以现有测试通过关闭。
- 35-compare.py 从 c0a072dc 提取原有生产采样方法及原有累加器，与新生产 sampler 使用相同 /O2 选项、逐秒交替顺序采样；排除首次预热的 11 次耗时中位数为旧 3798 us／新 1291 us。数据序列非同一瞬间且系统同时编译，不能外推节省比例；GetThreadTimes 粒度不足以可靠比较单次 CPU，宿主占用／唤醒尚未测量。35-baseline-raw.jsonl 的早期日志包含序列化耗时，不与此基准混算。
- scripts/build.bat --reload-shell **退出 0**，无编译／链接警告（35-gpu-build.log）；Shell 重载的 cmd timeout 因重定向 stdin 报不支持输入，后续 Explorer 启动与标准构建继续完成，该提示不是编译错误。完整测试留待上述新发现处理后的稳定输入，未将 34 号全量作为本次全量。
- 输入绑定 35-final-validation-inputs.json；宿主 SHA256 2674ee654941dcd17a33d5df3072bf5655abe19976a0b2e9f488385904d6dce4，Hook SHA256 7aeeb15efa62e9d5130256c5b8ff9836aae0492103f12a1923cdabc226364368。此轮仅调整共享采集，不改变面板绘制和托盘链路；桌面交互、真实 AMD、设备恢复、公共诊断和剩余 Lua 写任务等开放项仍保留。

### 空类型 GPU 引擎的定向检查点（36）

- 接纳真实 PDH 样本中类型名称为空的有效编号引擎，继续按完整 LUID／物理单元／编号区分；类型标记缺失或编号非法仍拒绝。显存快照填充从共享头移入采集实现，避免宿主各模块依赖累加器实现头。现有 Lua API v2 字段没有变化。
- scripts/test.bat name "^widget_system_data_provider$" **1/1 通过**，1.72 秒，退出 0（36-gpu-tests.log；test-run-6e39f1728a05403c821651724601638e.xml）；已编译目标为 SnowDesktopWidgetSystemDataProviderTests，无编译／链接警告。新增真实名称的有效零值回归，以及明确合成的多进程非零边界。
- gpu-negative-36/ 直接链接生产 sampler，正确副本退出 0；恢复旧空类型拒绝、耦合显存、每次分配、信任失败尺寸、长间隔误判五种变体各命中对应断言退出 1；/W4 /WX 无警告。
- 36-live-raw.jsonl／36-live-summary.json 再次取得 12 个真实采样区间；36-raw-check.py 核对 8 个有效间隔×4 个适配器共 32 组，非 _Total 的有效格式化行、包括无类型名称行均纳入分引擎细节，合计、整卡值和显存有效性一致。真实无名引擎读数为 0，不把合成非零样本当作真实负载，也不将 NVIDIA／Intel 样本当作 AMD 校准。
- 此检查点保存时，标准 scripts/build.bat 仍在运行（36-gpu-build.log，预检无宿主／Hook 占用），尚未运行本候选完整测试；待构建完成后继续绑定稳定输入并执行完整回归。不能用上轮标准构建或 34 号全量代表当前候选。
- 后续仍须实现用户可用诊断入口、Lua 可选引擎详情和系统监控适配器选择；AMD 原始对照、热插拔／真实休眠、宿主 CPU／唤醒及桌面交互待实机。

### 空类型 GPU 引擎与稳定检查点（36）

- c981d389 保存缓存／显存有效性的已编译候选及新发现。此轮解析接纳 Windows 实际返回的空类型名称，继续依靠完整 LUID、物理单元与引擎编号区分，缺少类型标记或编号非法仍拒绝。把显存快照填充移入采集器实现，避免整个宿主依赖累加器实现头；内部快照及旧 Lua 字段不变。
- 在原有 widget_system_data_provider 条目加入真实实例名称的有效零值，以及两进程同引擎 35＋45、另一无名引擎 60、命名引擎 25 的固定边界样本，期望整卡 80，并保留 3 个独立引擎。未复制生产公式计算测试预期，非零输入是明确的合成边界，不冒充实机负载。
- scripts/test.bat name "^widget_system_data_provider$" **1/1 通过**，1.72 秒，退出 0（36-gpu-tests.log；test-run-6e39f1728a05403c821651724601638e.xml），定向编译目标 SnowDesktopWidgetSystemDataProviderTests 无警告。gpu-negative-36/ 直接链接生产 sampler：恢复拒绝空标签的旧判断即在有效零值断言失败；显存耦合、每次分配、信任失败尺寸、长间隔误判四个变异也按预期失败，正确副本退出 0，全部 /W4 /WX 无警告。
- 再次执行生产采集器真实只读探针（36-live-raw.jsonl／36-live-summary.json）取得 12 个区间，确认查询／拓扑／缓冲复用、诊断退出及显式重置。36-raw-check.py 核对 8 个有效间隔×4 个适配器共 32 组：除 _Total 外的有效格式化行均纳入引擎详情，包含空标签行；原始格式化读数、分引擎合计、最忙引擎结果、专用／共享有效性相符。真实无名引擎读数仍为 0；没有实机非零负载、AMD 90% 或任务管理器同时间对照结论。
- **scripts/build.bat 退出 0，无编译／链接警告**（36-gpu-build.log）；**scripts/test.bat full 120/120 通过**，196.97 秒，退出 0、无编译／链接警告（36-full-tests.log；test-run-cfea2cda201749fa86d9e7e24c1a4c7b.xml）。包含栏体及各 WinUI 弹窗实际离线绘制回归；未再次触发真实 Explorer 诊断，GPU 改动没有修改托盘生产源码或协议。
- 输入绑定 36-before-full-validation-inputs.json／36-final-validation-inputs.json，并核对完整测试前后全部源码／资源哈希一致。最终宿主 SHA256 9922366a28324806137dafc88fb8ba7536a30a56380d784f22670493cfc214cc，Hook SHA256 6af226574c891b28cdc1de864e79ca9c316d4c656f8ab6a997d0fc387f2a422e。公共诊断入口、Lua 可选引擎详情、系统监控适配器选择、AMD 原始对照、宿主 CPU／唤醒、真实热插拔／睡眠与桌面交互仍未完成。
- 依据：[微软 GPU 任务管理器说明](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/)规定最忙引擎口径；[PDH 数组文档](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhgetformattedcounterarrayw)要求对不足缓冲重新以零尺寸探测；[IsCurrent](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgifactory1-iscurrent)与可选[适配器变化事件](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_6/nf-dxgi1_6-idxgifactory7-registeradapterschangedevent)用于拓扑失效；[QueryUnbiasedInterruptTime](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-queryunbiasedinterrupttime)不包含睡眠／休眠，用于区分实际挂起与普通长采样间隔。空类型名称依据本机真实 PDH 样本，不声称文档保证未公开实例格式。

### GPU 按需诊断 CLI 检查点（37）

- 已在编辑前公告新增 snowwidget gpu-diagnostics <new-output.jsonl> [--samples N] [--interval-ms N]；capabilities.commands 及 gpuDiagnostics.schemaVersion=1／format=jsonl 可检测，CLI protocolVersion 与 Lua API v2 不变，旧参数／主题未修改。不提高 Lua minHostVersion；旧工具与同版本早期构建须检测命令，不按版本号猜测支持。
- 新入口复用生产采集器但独立运行，记录自身时间与间隔；没有声称读取了桌面显示同一帧。默认 11 次／1000 ms，包含首个预热；限制 2–120 次、250–5000 ms、计划等待不超过 120 秒，OS 采样耗时另计。新文件独占创建，最多 64 MiB；逐行记录 metadata、sample、summary，保存版本／自身哈希／Windows 构建、原始与格式化计数器、状态码、逐引擎合计与最终适配器读数。64 位身份、FILETIME、原始整数与字节数以十进制字符串保精度；无效值为 null，有效零值保留。
- [CLI 文档](../tools/snowwidget/README.md)说明边界、退出码、Ctrl+C 后的部分记录和 AMD 同时段对照方式。导出成功代表文件采集完成，不能替代计数器有效性或 AMD 问题验证。
- scripts/build.bat **退出 0，无编译／链接警告**（37-gpu-build.log）。定向 widget_author_tools／widget_api_registry／widget_system_data_provider **3/3 通过**，1.88 秒，退出 0，无编译／链接警告（37-gpu-tests.log；test-run-db33b7515a6d4a7189fc3c642f5f6a6f.xml）。
- 37-live.py 实际调用构建后的 snowwidget：capabilities 与 --version 正确；含中文／空格的新文件成功写入 4 个样本，3 个有可用占用，6 个有效适配器区间与计数器合计一致。输出 metadata 自身 SHA256 与实际 exe 一致，再次写同一路径退出 1 且原文件哈希不变；5 种错误参数退出 2 且无输出文件。原始记录与证据在 gpu-cli-37/，标准 CLI SHA256 cae5e90442ff9c40885154343dd0295c957d0054223618f0b751d05c34fc8ef7；主 CLI 与分发 Skill 中副本哈希一致。
- 当前保存已编译代码后，下一轮收紧新文件测试的失败清理：现有 Check 使用 exit，若其断言失败会跳过临时目录析构，应先退出文件作用域再断言。随后执行负向对照与稳定输入全量；此检查点没有运行 37 号全量，不将 36 号完整测试算成本次。
- 输入绑定 37-final-validation-inputs.json。AMD 原始机器与任务管理器同时间对照、Lua 可选详情、系统监控适配器选择、Lua 写任务与桌面交互等剩余项保持开放。

### 诊断入口、失败清理与稳定回归（38）

- ed2f7e8d 保存生产 CLI。此轮新增实际 snowwidget 子进程的 capability／命令分发／无效参数／文件保护回归，复用原有离线 CLI 测试进程管理，不读取真实 GPU。校验原协议 2、schema 1 与既有命令保留；声明支持却缺少实际分发会在结构化错误检查处失败。
- 单元文件保护场景先读取结果、离开临时文件作用域，再调用现有 exit 型 Check，确保失败也清理自有目录。生产 CLI 的两个文件只删除多余 EOF 空行；37 号 git diff --check 曾报告这两处空行，本轮已消除，不改变运行语义。
- gpu-cli-negative-38/ 使用实际生产 serializer／参数解析／文件创建路径；仅替换文件哈希边界为空值，让覆盖变体在截断后、硬件采集前停止。恢复缺失值输出旧数据、64 位整数作为浮点消费数字、忽略等待上限、CREATE_ALWAYS 覆盖、直接输出非有限数五个变体均命中对应断言退出 1；正确副本退出 0，所有运行均无临时目录残留，/W4 /WX 无警告。首个隔离编译包含未使用的 Lua API 头而缺 lua.h，移除隔离测试无关 include 后才运行成功，编译失败不计作负向发现。
- 定向 scripts/test.bat name "^(widget_author_tools|widget_author_preview_cli)$" **2/2 通过**，71.45 秒，退出 0（38-gpu-tests.log；test-run-346a20849fe74567b03defba8e6d16c1.xml），包含现有栏体、日历、控制中心、托盘和资源面板离线生产渲染。
- **scripts/build.bat 退出 0，无编译／链接警告**（38-gpu-build.log）；**scripts/test.bat full 120/120 通过**，193.08 秒，退出 0、无编译／链接警告（38-full-tests.log；test-run-649f15edce9441f898ae64495e408f66.xml）。
- 输入绑定 38-before-full-validation-inputs.json／38-final-validation-inputs.json，完整测试前后源码与资源哈希一致。最终宿主 SHA256 e8a05f3daff17ea04ba78bc6069d2c72ab87a7da57f5b0b723aef404502da3f4，CLI SHA256 770a3a2fad0bfac0e9dd971a172c36bbdf4784961b20a98e829466ee7b3530e8；根目录 CLI 与分发 Skill 副本一致。实际硬件导出引用 37 号仍有效的生产语义证据，明确保留当时工具哈希，不将其称为本轮二进制。
- 原始计划继续保留：Lua GPU 可选详情／适配器选择、十九个共享控制写任务与权限／手势确认、AMD 机器同时段对照、真实设备恢复及桌面交互等。导出入口和自动检查完成不等于 AMD 问题已解决，也不等于整项目标完成。
