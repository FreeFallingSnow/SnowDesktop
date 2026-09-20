# 右键扩展改造候选与验证记录

开发分支：`release/v1.0.7.0`；版本保持 `1.0.7.0`。本文记录自动化证据与实机边界，不代表桌面验收通过。

## 实现边界

- 注册目录后台读取 HKCR 通用位置、扩展名、ProgID、PerceivedType、OpenWithProgids、UserChoice 与打包应用清单。名称、图标、注册来源、适用范围和禁用状态独立于菜单快照保存。
- 设置分为“文件与文件夹”“空白处”，通用规则与位置例外只写 SnowDesktop 配置。旧身份记录保留；设置只展示实际观察到的可用菜单根项，隐藏未关联的原始注册行。
- 实际菜单仍由隔离 Shell 进程决定适用性。命中缓存时本次菜单保持固定；无快照时先显示应用菜单，查询完成后在安全时机动态补齐。保留底部“展开更多选项／管理”，没有加载行。
- 设置进程通过私有 IPC 18 使用宿主服务；组件公共 API、apiVersion、minHostVersion 不变。
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
