# 右键扩展改造候选与验证记录

开发分支：`release/v1.0.7.0`；版本保持 `1.0.7.0`。本文记录自动化证据与实机边界，不代表桌面验收通过。

## 实现边界

- 注册目录后台读取 HKCR 通用位置、扩展名、ProgID、PerceivedType、OpenWithProgids、UserChoice 与打包应用清单。名称、图标、注册来源、适用范围和禁用状态独立于菜单快照保存。
- 设置分为“文件与文件夹”“空白处”，通用规则与位置例外只写 SnowDesktop 配置。旧身份记录保留；设置只展示实际观察到的可用菜单根项，隐藏未关联的原始注册行。
- 实际菜单仍由隔离 Shell 进程决定适用性。命中缓存时本次菜单保持固定；无快照时先显示应用菜单，查询完成后在安全时机动态补齐。保留底部“展开更多选项／管理”，没有加载行。
- 设置进程通过私有 IPC 21 使用宿主服务；组件公共 API、apiVersion、minHostVersion 不变。
- 具体选择、位置与 Shift 共同确定查询身份。宿主合并在途任务、最多运行两个查询进程，失败退避，成功进程空闲五分钟回收。
- 显示快照采用完整键与最近使用淘汰，内存 64 条／16 MiB，磁盘 256 条／64 MiB，单条 2 MiB，24 小时有效期。目录资料单独持久化；执行令牌不写磁盘。
- 点击会重新查询并校对名称、命令、父子路径、注册关联与禁用状态；不按菜单位置执行。合法空结果可以发布，失败不能覆盖有效快照。
- 长菜单使用独立的上下滚动提示条，内容裁剪、箭头和命中区域一致；支持点击箭头，保留键盘与滚轮滚动。独立预览程序按 `L` 切换长菜单。

## 前轮自动化检查

定向入口：

```bat
scripts/test.bat name "^(shell_context_menu_invoke|general_settings|settings_controller|modern_menu_interaction|menu_icon_render|localization_contract)$"
```

`6aac0a2c` 对应候选执行 6/6 通过。日志：`.codex-probes/shell-menu-redesign/targeted-final.log`。

| 风险 | 自动化证据与边界 |
| --- | --- |
| 旧配置扩大显示范围 | 旧分类不一致保留例外；恢复跟随后不从旧记录复活例外 |
| 注册身份误合并 | 隔离注册树中同名不同来源、同 CLSID 多位置、类型专属与系统禁用；禁止仅按名称关联 |
| 菜单打开后跳动 | Presentation 实际构造与挂接；异步完成后项目与轮询回调不变 |
| 重复查询、晚到结果 | 可控完成门验证合并、两任务上限、依赖失效后旧结果拒绝、成功空结果 |
| 启动失败丢快照 | 控制启动异常并检查内存保留与自动退避；显式操作可以立即重试 |
| 磁盘与命令混用 | 跨进程读取同一临时缓存，只保留显示数据；新会话令牌、禁用、歧义和命令变化检查 |
| 点击执行前禁用 | 等待中的执行查询完成前隐藏扩展，验证未调用命令边界 |
| 多选与 Shift | 单文件、同类型多文件、混合类型、单文件夹、多文件夹、文件与文件夹混选的普通／Shift 键隔离 |
| 注册变化过度失效 | 控制目录读取边界：图像专属变化保留文本快照；相关文本注册变化使之失效 |
| 缓存容量与损坏 | 270 个独立选择触发磁盘容量淘汰、桌面保留、损坏文件、过期、目标变化、请求序号与依赖校验 |
| 滚动误点 | 独立菜单交互测试在 100%、125%、150%、200% 缩放点击提示条，再验证键盘末项与可视边界 |

最终检查：`scripts/test.bat` 在 `21cd7473` 对应生产输入上执行，119/119 通过，退出码 0，测试执行约 81 秒（日志 `full-final.log`）。随后只将测试局部变量 `file` 改名以清理 C4456；受影响的 `shell_context_menu_invoke` 再次通过 1/1（日志 `final-shell.log`），其余 118 项输入未改变。未运行仓库默认排除的手动诊断。

最终标准 `scripts/build.bat` 通过，输出 `.build/Release/SnowDesktop.exe`；日志 `build-delivery.log`。单独编译 `SnowDesktopModernMenuPreview` 通过，输出 `.build/Release/tests/SnowDesktopModernMenuPreview.exe`；日志 `preview-build.log`。最终增量构建及警告清理后的定向测试没有新警告；较早完整编译出现过既有 WinUI 生成头 `GetCurrentTime` C4002 和既有 settings_controller C4456。

环境：Release、Windows SDK 10.0.26100.0、目标系统 10.0.26200、MSBuild 18.5.4。宿主 SHA256：`1FAF576ED803CF4FD9C00BA4D353D3F13837FF4CA6D7D9BA2E770771987C6839`；预览 SHA256：`9607188DC89437DAB81AB9989D363FC6C0491189C7B04527B72D0FD9BBDFF5E3`。

负向对照在 `.codex-probes/shell-menu-redesign/negative/` 隔离副本中进行：只在失败分支加入清除快照，未改正式生产文件。诊断程序编译成功，退出码 1，确定失败信息为 `launch failure does not clear a valid memory snapshot`；未通过超时或无条件重试判定。日志为同目录上级的 `negative-build.log` 和 `negative-run.log`。

## 同机性能对照

每种对象分别执行 5 次冷查询与 30 次热访问；使用临时本地文件／文件夹，不执行安装的第三方命令。冷启动指隔离查询进程／查询服务，不是启动桌面宿主。P95 采用最近秩法。基线为 `402bd673`，最终测量使用 `21cd7473` 的生产输入及仅变量重命名后的测试；原始日志保存在 `.codex-probes/shell-menu-redesign/baseline.log`、`comparison-delivery.log`，汇总为 `metrics-delivery.json`。另保留中间候选 `6aac0a2c` 的 `comparison.log`，不与最终数据混用。

| 测量（毫秒，P95） | 文件 | 文件夹 |
| --- | ---: | ---: |
| 原 Session 冷查询基线 | 1297 | 1219 |
| 原 Session 热查询基线 | 219 | 188 |
| 改造后同入口冷查询 | 1329 | 1968 |
| 改造后同入口热查询 | 156 | 266 |
| 统一服务完整冷查询 | 1106.070 | 1529.740 |
| 统一服务完整热查询 | 185.765 | 279.663 |
| 内存快照读取（热） | 0.031 | 0.011 |
| 内存命中菜单内容准备（热） | 0.438 | 0.506 |

“菜单内容准备”经过生产 Presentation 构造、过滤、图标转换和挂接入口，不包含窗口呈现和第三方查询。该样本达到 P95 ≤ 50 毫秒目标；不能由这些数据推断桌面实际显示延迟或所有第三方扩展的速度。第三方完整查询存在波动，最终文件夹查询高于基线，不能声称其冷查询本身已经提速；改造主要将这部分等待移出打开菜单的路径。

无副作用的点击边界替身保留真实调度、重新查询、身份校对和回调流程，30 次 P95 为 16.672 毫秒；它不是实际第三方命令耗时。本机注册目录读到 2852 条记录，其中 2802 条未标记系统禁用，首轮读取约 3835.03 毫秒。Shell 最终适用性仍在查询时决定。

## 待实机验收

- 原截图场景：不同菜单样式、深浅色、上下滚动边界和点击箭头，确认对比度、圆角、定位及不误点。
- 设置页两分类、卡片、图标、搜索、位置例外、多选检查和管理按钮跳转。
- 真实 7-Zip、终端及用户安装的其他扩展：冷启动、快速关闭再开、Shift、单选与混选、真实命令执行。
- 未关联注册记录不再显示；实际动态根项可用独立菜单身份开关，仍需验证类型专属项目发现与同名不同来源行为。
- 标准 Release 构建与自动化结果见上文。桌面宿主不使用 UI 自动化验收；用户反馈后才记录 `verify`。

## 参考


注册目录与状态读取参考 [ContextMenuManager ShellList](https://github.com/BluePointLilac/ContextMenuManager/blob/master/ContextMenuManager/Controls/ShellList.cs)；进程复用与失败处理思路参考 [TortoiseGit RemoteCacheLink](https://github.com/TortoiseGit/TortoiseGit/blob/master/src/TortoiseShell/RemoteCacheLink.cpp)。扩展批准策略参见 [Microsoft 的 Shell 注册文档](https://learn.microsoft.com/en-us/windows/win32/shell/reg-shell-exts)。实现沿用本项目隔离辅助进程与 IPC，没有复制其业务缓存或超时数值。

## 设置列表与首次动态补齐跟进（2026-09-20）

用户反馈前轮候选在设置中渲染约 2800 条原始注册记录，出现大量不可操作的“待关联”行，
打开页面卡顿。用户进一步确认设置页和实际右键菜单均需首次动态补齐，取代之前冷菜单冻结的要求。
候选 `dc7da2f2` 将设置投影改为实际可用根项，四个真实本地场景后台发现，按身份合并范围，
只传根项文字与图标；UI 分批创建行，位置控件按需创建。冷菜单在既有安全更新条件下补齐，
缓存命中仍保持本次内容；没有加载占位。全部十种语言同步指南并移除“待关联”标签。

| 本次执行 | 结果 |
| --- | --- |
| `scripts/test.bat name "^(shell_context_menu_invoke|modern_menu_interaction|localization_contract|settings_controller)$" | 4/4 通过，CTest 16.60 秒 |
| `scripts/build.bat --reload-shell` | 退出码 0，标准 Release 宿主及 WinUI 设置页编译通过 |
| `scripts/test.bat` | 119/119 通过，退出码 0，CTest 79.63 秒 |
| `SnowDesktopShellContextMenuInvokeTests.exe --benchmark-menu-settings` | 私有空缓存与真实 Shell 查询通过，未执行第三方命令 |

受控回归注入 3000 条未关联注册记录与两个同名不同身份的真实菜单根项，最终只返回 2 行、
298 字节 IPC；验证同身份范围合并、图标保留、子菜单令牌不传设置、开关实际控制可见性。
首次无缓存设置查询不要求已有开启项；同一服务四个场景各查询一次。
菜单回归覆盖冷查询主动启动、不安全时机延迟补齐、缓存命中保持固定和关闭后查询继续。

本机真实发现返回 51 个菜单根项，IPC 为 54899 字节；初次服务 Inspect 调用 0.0265 毫秒，
后台发现约 3694.13 毫秒，30 次热 Inspect 为约 0.0085–0.0175 毫秒。
这是服务数据准备测量，不是 WinUI 控件创建或桌面显示耗时；测量期间宿主构建及 Shell 重载，
不能据此声称实际设置页卡顿已验收或第三方完整查询达到固定耗时。

日志位于 `.codex-probes/menu-settings-useful/` 的 `targeted.log`、`build.log`、`full.log`、
`real-discovery.log`。全量报告：`.build/Testing/test-run-784ec6da2e014f3bb624fd768b587f36.xml`。
标准构建出现既有 WinUI GetCurrentTime C4002，重载脚本非交互 timeout 输出输入重定向提示，
均未阻止构建完成；没有观察到新增编译警告。手动诊断未运行。
设置实际响应、专属类型补充、桌面首次补齐及第三方执行仍待用户实机验证。

### 最终跟进候选

代码候选 `e94b7444` 进一步保留后台解析中的桌面选择，避免“检查桌面菜单”返回旧文件而跳错分类；
主动刷新重新查询四个基础场景，不受自动查询的新鲜缓存节流影响。语言与指南候选为 `7f957476`，
本轮主要设置与菜单调整为 `dc7da2f2`。版本号仍为 1.0.7.0。

| 最终输入验证 | 本次执行结果 |
| --- | --- |
| 三项受影响定向回归 | 3/3 通过，退出码 0，CTest 15.08 秒 |
| `scripts/build.bat` | 退出码 0，35.08 秒，预检无宿主或 Hook 占用，标准 Release 产物已生成 |
| `scripts/test.bat` | 119/119 通过，退出码 0，配置 1.76 秒、编译 21.22 秒、CTest 77.63 秒 |

隔离对照保留真实服务调度、缓存、投影和 IPC 打包，仅替换 Shell 查询边界。
正确副本通过；三个故障副本均正常编译，并分别以退出码 1 命中以下确定断言，未使用超时或重试代替失败信号：

- 返回原始注册目录：`only the two actionable observed identities reach settings`。
- 忽略主动刷新标记：`explicit settings refresh queries all four locations despite the fresh-cache throttle`。
- 桌面路径未解析时返回旧文件：`desktop inspection never routes settings back to the previous file while resolving its path`。

对照日志为 `.codex-probes/menu-settings-useful/negative-*.log`，结果为 `negative-results.json`；
最终构建与全量日志为同目录的 `build-final.log`、`full-final.log`，附有起止时间与退出码 JSON。
全量报告为 `.build/Testing/test-run-c8159329c4ae4dfb880f4c5218a3af3e.xml`。
Release、MSBuild 18.5.4、Windows SDK 10.0.26100.0；最后增量构建和完整回归无新增编译警告。
`final-inputs.json` 记录 991 个源码、测试、传递依赖、资源和构建输入；结束时全部哈希一致。
最终宿主 SHA256：`C085E689E260731A422BCD680DF55B3ED0D00B3774A1DECB317AF7B9F75BADF8`。

手动诊断未运行；设置页实际打开耗时、卡片视觉、首次菜单补齐的鼠标交互以及第三方命令执行
仍待用户实机验收。本轮只记录编译及自动化通过，未创建声称实机通过的 verify。

## 文件类型发现、滚动边界与 Win10 样式跟进（2026-09-20）

用户反馈图片和文档菜单仍缺失，并要求到达滚动边界后隐藏该侧提示；随后要求 Win10 深浅
样式文字稍小、图标两侧间距稍宽、圆角适度增大。候选 `abb9244c` 保持版本号 1.0.7.0，
公共组件 API 和私有设置 IPC 均未变化。

- 有注册记录的支持类型使用有效应用样本，后台分批查询普通／Shift、单选／同类型多选。
  仍只展示实际菜单根项；相同身份合并，类型范围显示在专属行中。十种语言指南同步。
- 管理项目独立保存在有界 observed.dat 中；只保留根项显示资料，具体文件快照淘汰和查询失败
  不再导致开关消失。相关系统注册变化撤销受影响记录，其余类型保留。
- 提示绘制直接使用真实滚动偏移与上限，边界侧不再绘制箭头及分隔线；预留区域保持不动。
- Win10 字号 12 → 11 DIP，左留白 6 → 8、图标列 18 → 20、图文间距 5 → 6 DIP；
  面板圆角 2 → 4、高亮圆角 1 → 2 DIP，行高和图标尺寸保持 24／16 DIP。

五项定向回归本次通过 5/5，退出码 0，CTest 17.23 秒，报告为
`.build/Testing/test-run-09e71c76aa9f4c08bccfea27cd8af433.xml`。
覆盖类型专属、多选和 Shift 专属根项、共用身份合并、系统禁用、70 个后续选择导致缓存淘汰、
删除隔离快照后的独立恢复，以及新查询失败时保留管理项目。绘制回归覆盖浅深色、
100%／125%／150%／200% 缩放下的顶端、中间、底端与中文、长文字、快捷键、图标和勾选。

独立解析器从最终源码的样本字节验证：PNG/JPEG/BMP/GIF/TIFF 全部实际解码为 2×2 像素；
PDF 为一页，python-docx 读取段落，openpyxl 读取单元格，python-pptx 读取一张幻灯片。
不使用修改扩展名的空文件充当这些格式。

隔离正向副本通过；四种故障副本正常编译，均以退出码 1 命中对应行为断言：

| 故障 | 确定失败信号 |
| --- | --- |
| 不启动文件类型发现 | 类型专属、多选和 Shift 项未进入设置 |
| 每次重建临时管理目录 | 文件缓存淘汰后图片开关消失 |
| 不读取持久化管理目录 | 删除具体快照、辅助进程失败后无法恢复图片与文档开关 |
| 边界继续绘制提示 | 已到边界仍有箭头／分隔线像素 |

本机实际查询、私有空缓存、未执行第三方命令：从 51 个基础场景根项增至 117 个，IPC 115554 字节；
首个 Inspect 为 0.0351 毫秒，全部后台发现 18693.8 毫秒。此数值是后台完整发现时间，
不是窗口首屏等待时间，也不是所有机器的性能保证。实际设置控件创建与桌面视觉仍待验收。

标准 `scripts/build.bat --reload-shell` 本次通过，退出码 0，用时 46.46 秒；执行前已检查占用并
告知关闭 SnowDesktop／重启 Explorer 的副作用。构建包含 WinUI 设置页及宿主产物。
仅出现既有 WinUI GetCurrentTime C4002 与重载脚本的非交互 timeout 提示，无新增编译警告。
日志、时间、故障对照和样本解析记录位于 `.codex-probes/menu-types/`。

限制：自动发现覆盖上述明确支持的常见格式；旧版二进制 Office 格式、其他格式以及依赖内容、
位置或特殊混合选择的动态菜单，仍通过“检查文件”或实际右键的管理入口补充。不得把样本覆盖
写成全部第三方菜单穷举。扩展保持默认隐藏，用户开启后实际右键仍由 Shell 判断当前适用性。
所有视觉及真实第三方命令执行待用户实机，手动诊断未运行。

最终输入的 `scripts/test.bat` 本次通过 119/119，退出码 0；配置 1.83 秒、编译 20.98 秒、
CTest 82.75 秒。报告为 `.build/Testing/test-run-ef0b584208a94ed2b23393209fc9dfab.xml`，
日志为 `.codex-probes/menu-types/full.log`，没有新增编译警告。
MSBuild 18.5.4、Windows SDK 10.0.26100.0、Release；994 个运行／测试／资源／构建输入
在标准构建和完整回归结束后保持一致，清单为 `final-inputs.json`，核对为 `final-verification.json`。
宿主 SHA256：`CDA51843F4C41F83D8C7448CA59B917E6DC3D4A2648E3240EF3096149E78ADDE`。
此记录只补充自动化和构建证据，设置与桌面实机待验证状态不变。


## 首次设置增量更新与等价命令分组（2026-09-20）

对应用户提供的首次持续刷新和图片壁纸重复行。候选 `bfa52116` 将目录更新改为按身份增量
协调，仅创建、更新或移除实际变化的行；现存 Expander、ToggleSwitch 及位置选项保留。
有可用行后撤下读取提示。`a6b6da1d` 保留相关注册依赖失效时的重查，普通轮询不再因十秒
新鲜期结束或快照被淘汰而重复查询。新增成员、图标和类型文字在原行更新，分类切换及语言
切换仍允许重建分类内容。

静态注册按完整 verb、执行命令、处理器／委托和参数比较，只合并能够证明等价的注册。
原始注册 ID 及配置保持不变；合并开关更新成员原规则，位置选项只修改适用成员。
原状态不一致时显示十种语言的混合提示，不静默统一；后来发现的类型保留各自原状态。
单凭名称或动态菜单文本不合并。私有 IPC 19、目录缓存格式 3、管理目录格式 2，旧格式
回退后台发现；版本 1.0.7.0、公共组件 API、用户开关和布局格式不变。

本轮定向命令 `scripts/test.bat name "^(shell_context_menu_invoke|settings_controller|settings_search_index|localization)$"`
实际选择三项，3/3 通过，退出码 0，CTest 12.59 秒；`localization` 未匹配条目，本地化契约由
最终完整测试覆盖。定向报告为 `.build/Testing/test-run-4236d576419a4f8cb878f23a0f27a5b1.xml`。
先执行 `scripts/build.bat --reload-shell`（已预检并提醒关闭宿主／重启 Explorer），
后续最终输入执行 `scripts/build.bat`，均退出码 0；最终标准构建 40.92 秒。
既有 WinUI GetCurrentTime C4002 仍存在，没有新增正式构建警告。

隔离正向副本通过。四个故障副本均正常编译，以退出码 1 命中确定行为断言：

| 故障对照 | 检出的回归 |
| --- | --- |
| 不按相同执行身份合并 | 图片类型仍产生多行 |
| 每次协调清空已有控件 | 无关查询元数据导致重建，丢失现有控件身份 |
| 每次轮询都调用查询调度 | 原快照被淘汰后设置轮询重新排队 |
| 不允许已检查对象失效重查 | 注册依赖变化后当前选择无法更新 |

列表测试使用生产协调器及轻量控件替身，验证身份、展开标记和新增／移除操作；不能代替
真实 WinUI 焦点、布局或滚动验收。服务测试保留生产调度、关联、缓存和序列，只替换外部
Shell 边界。断言不依赖固定重试或超时作为预期失败。

本机只读发现用于核对用户截图的实际命令，使用隔离空缓存，不执行菜单命令：117 个原始
根项，文件分类为 93 个显示行，包含四组等价命令；抖音壁纸的 `.jpeg/.jpg/.png` 合为一组，
系统壁纸的 `.bmp/.gif/.jpeg/.jpg/.png/.tif/.tiff` 合为一组。该查询使用 bfa52116 的相同
扫描与分组实现；后续跟进未修改这两条数据路径。后台完整发现约 18.77 秒，首个 Inspect
约 0.03 毫秒；此诊断与编译同时运行，不作为 WinUI 首屏性能验收或速度提升百分比。

日志、隔离副本和结果保存在 `.codex-probes/menu-incremental/`。设置页真实首次打开、持续
滚动、展开位置选项、合并开关和第三方命令执行待用户实机；不创建声称实机通过的 verify。
手动诊断未运行。

最终 `scripts/test.bat` 本次通过 119/119，退出码 0，CTest 77.25 秒，包含
`localization_contract`；报告 `.build/Testing/test-run-f73929cbb92445918e0b1e7ebaaddbad.xml`。
正式构建及全量日志分别为 `build-final.log`、`full.log`，故障对照为 `negative-final-results.json`。
Release、MSBuild 18.5.4、Windows SDK 10.0.26100.0；源码、测试、传递依赖、资源和构建脚本
清单共 993 个文件，最终与 `final-inputs.json` 哈希一致，核对记录 `final-verification.json`。
最终宿主 SHA256：`B8C9E979B2047261CC1A4B1992C1F4BE6BEF6E9286003644A4D43EFE5CCBAE13`。

## 提供应用、后缀筛选与批量开关（2026-09-21）

候选 `79966de2` 在原设置卡片中加入提供应用和文件后缀筛选，与名称搜索取交集；每行显示
来源和完整适用类型。后缀筛选包含通用文件动作，排除仅适用于文件夹的动作；空白处分类
只显示应用筛选。筛选基于现有内存目录，不触发注册扫描、磁盘读取或额外 Shell 查询。

批量显示／隐藏只修改点击时当前分类筛选结果的通用开关，保留位置例外和其他分类规则；
后来发现的项目不随本次操作改变。筛选先按现有命令身份合并再判断，选中合并行时全部
成员共用该开关，不能将后缀筛选理解为新增按后缀独立规则。空结果禁用批量入口；来源
消失时保留选中的筛选值，避免默默回到全部项目后扩大批量操作范围。

后台从注册 EXE/DLL 的产品资料或打包应用身份读取来源，共享到宿主缓存和设置 IPC；
不按菜单标题猜测应用。私有 IPC 20、目录缓存格式 4、管理目录格式 3；旧目录重新发现，
用户配置、公共组件 API 和版本 1.0.7.0 不变。十种语言、设置搜索和使用指南同步更新。
`aeeffc9c` 过滤版本资源中未修改的 TODO／尖括号占位值，回退文件说明或文件名。

| 本次执行 | 结果 |
| --- | --- |
| `scripts/test.bat name "^(shell_context_menu_invoke\|settings_controller\|settings_search_index\|localization_contract)$"` | 4/4 通过，退出码 0，CTest 16.62 秒 |
| `scripts/test.bat name "^shell_context_menu_invoke$"` | 名称回退跟进 1/1 通过，退出码 0，CTest 12.15 秒 |
| `scripts/build.bat --reload-shell` | 首候选标准 Release 构建通过，退出码 0；已预检并告知关闭宿主／重启 Explorer |
| `scripts/build.bat` | 最终候选标准 Release 构建通过，退出码 0，30.21 秒；预检无进程或 Hook 占用 |
| `scripts/test.bat` | 最终输入 119/119 通过，退出码 0；配置 1.69 秒、编译 21.01 秒、CTest 79.99 秒 |

完整报告为 `.build/Testing/test-run-a7a4f9fcea774859a22d017e586faa01.xml`，最终输入对应
`aeeffc9c`。测试覆盖应用身份和后缀交集、通用与专属类型、同名不同应用、合并成员、
批量捕获范围、保留例外、实际可见快照、延迟来源资料更新、缓存恢复和私有 IPC 往返。
注册测试使用独立临时文件及私有注册树，不执行第三方命令。

隔离正向副本通过；四个故障副本均正常编译并以退出码 1 命中行为断言：忽略应用筛选、
忽略精确后缀筛选、批量写到错误分类、按菜单标题猜测应用。副本不覆盖生产文件，编译
失败与超时不作为检出证据。日志和结果位于 `.codex-probes/menu-filters/` 的
`negative-build-delivery.log`、`negative-delivery-results.json`。

本机空临时缓存只读发现 117 个根项，文件分类合并后 93 行；其中 18 行可可靠归属到
9 个应用，75 行显示“未识别应用”，后者仍可按名称、后缀筛选及批量控制。PNG 筛选得到
44 行，包含通用文件动作。本机 MiDropShellExt 的 `TODO: <Product name>` 已在相同
诊断入口回退为 `MiDropShellExt.dll`。这些计数只代表本机安装状态，不代表全部第三方
动态菜单均能归属。应用名使用模块产品资料，可能是产品族名称而非软件商店展示名。

最终只读诊断首个 Inspect 为 0.0315 毫秒，后台完整发现 16071.8 毫秒；与首候选同机
诊断比较只用于检查来源资料，不作为 WinUI 首屏延迟或性能提升的证据。真实命令未执行。
原始记录为 `real-applications.log`、`real-applications-delivery.log`。

构建与完整日志为 `build-followup.log`、`full.log`，起止时间保存在对应 `*-result.json`。
Release、MSBuild 18.5.4、Windows SDK 10.0.26100.0；985 个运行／测试／资源／构建输入
在全量前后哈希一致，见 `final-inputs.json`、`final-verification.json`。语言文件另存于
`final-language-verification.json`，所有相关跟踪文件与被测提交一致。首候选只出现既有
WinUI GetCurrentTime C4002 及重载脚本非交互 timeout 提示；最终增量构建和完整测试无
新增编译警告。宿主 SHA256：
`B4BB7C35F9FE421C417FE546F4C56EFCD7E41928B076772A78ECD0CC4A896B91`。

设置页浅深色、窄窗口、筛选展开时后台补齐、批量结果反馈、位置例外和真实第三方命令
仍待用户实机验收；自动化没有证明这些视觉和实际交互结果，未创建实机 `verify`。
仓库默认排除的手动诊断未运行。
