# 隐藏桌面表面与毛玻璃工厂回收

## 策略与边界

桌面组件隐藏后先保留绘图表面，隐藏满 10 秒后由现有每秒维护定时器回收。
隐藏表面的 BGRA 像素容量总预算为 16 MiB；超过预算时，下次维护优先回收最早隐藏的实例。
实际回收时间受 UI 消息调度影响，10 秒不是实时期限；可见表面不参与此预算。
计量包含组件主表面和原生滚动文字子表面，不包含驱动额外分配、共享包图或 Lua 堆。

隐藏时即解除滚动文字的原生位移动画，短期保留表面不继续运行动画。
回收先解除 visual 的内容引用，再释放宿主表面引用。
保留组件所有权、轻量 visual/clip、滚动位置的时间状态和 Lua 运行时。
不卸载组件，不取消提醒、网络任务或辅助面板，也不清理仍被其他实例使用的图片缓存。
隐藏期间不创建新的滚动文字表面。恢复可见时主动排入该组件的绘制队列，即使它在当前
脏矩形之外，也会重新生成表面。原生图形调用失败沿用合成恢复路径，不把失败计作完整回收。

`IDCompositionVisual::SetContent(nullptr)` 才能解除 visual 对内容的引用；仅重置宿主
`ComPtr` 不足以证明表面已经失去引用。[Microsoft 文档](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-idcompositionvisual-setcontent)

毛玻璃每个面板继续独占 effect brush 与 backdrop source；只共享模糊工厂。
在一帧面板收集结束后，清理没有面板使用的半径工厂；帧外移除面板则当场检查。
同帧移动或替换面板可复用工厂，拖动预览撤下面板也纳入该帧收集；最后一个使用者消失后工厂数量应为零。
目标、根 visual 以及线程共享 compositor/dispatcher 继续存在，避免影响其他窗口的合成。
隐藏但留待复用的弹窗面板仍是使用者，本轮不改变其保留策略。

## 诊断指标

沿用默认关闭的 `scripts/profile.bat`，CLI 和报告 schemaVersion 1 不变，新增指标可由旧分析器忽略。
没有新增 Lua API、清单字段或 capability，不需要改变组件 `apiVersion`。

| module / phase | owner | 含义 |
|---|---|---|
| `widget.memory / surface_bgra_bytes_estimate` | 组件 ID | 原有主表面容量；没有表面时为零 |
| `widget.memory / marquee_surface_bgra_bytes_estimate` | 组件 ID | 原生滚动文字表面容量合计 |
| `widget.memory / hidden_surface_bgra_bytes_estimate` | 组件 ID | 隐藏实例主表面与滚动文字容量合计，可见时为零 |
| `widget.composition / surface_visible` | 组件 ID | 桌面合成可见性，区别于包含辅助面板的 Lua hostVisible |
| `widget.composition / surface_resident` | 组件 ID | 是否持有主表面 |
| `widget.composition / surface_created` | 组件 ID | 采集期间的主表面成功分配事件，值为 1 |
| `widget.composition / surface_reclaimed_expired_bytes` | 组件 ID | 采集期间因隐藏到期回收的容量，每次事件记录本次容量 |
| `widget.composition / surface_reclaimed_budget_bytes` | 组件 ID | 采集期间因预算压力回收的容量，每次事件记录本次容量 |
| `composition.memory / hidden_surface_bgra_bytes_estimate` | 空 | 全部隐藏实例的表面容量 |
| `composition.memory / hidden_surface_instance_count` | 空 | 仍持有至少一个表面的隐藏实例数量，不是纹理数量 |
| `composition.memory / surface_reclaim_count` | 空 | 进程生命周期内完整回收实例表面的累计次数，可重复计算同一实例 |
| `composition.memory / surface_reclaimed_bytes` | 空 | 进程生命周期内回收容量累计值，包含局部成功回收 |
| `backdrop.state / available` | 合成目标 | 当前目标是否可用 |
| `backdrop.state / panels` | 合成目标 | 当前保留面板数量，包括复用中的隐藏弹窗面板 |
| `backdrop.state / blur_factories` | 合成目标 | 当前宿主持有的模糊工厂数量 |

毛玻璃 owner 为 `desktop`、`collection_popup`、`quick_navigation` 和 `dock:<宿主地址>`。
Dock 地址仅在该进程内标识实例，不能跨重启连接数据。累计回收计数无需在回收发生时开启探针；
表面创建与回收事件只在采集中记录。关闭探针后没有新增采样线程或计时器。

## 验证记录

2026-09-06 优化前，同一主进程 PID 25312，二进制 SHA256：
`12BE2EA7EF60F9BEEC921AD5773965BEBFE85C421AEE3C7D45BD299599AC05F5`。

- 可见页：`artifacts/v1.0.5.0/performance/hidden-retention-before/`，12 秒，12 个 Lua 实例可见。
- 用户手动切页后：`artifacts/v1.0.5.0/performance/hidden-retention-before-hidden/`，12 秒，
  10 个 Lua 实例隐藏，它们的主表面仍合计 6,908,260 字节（6.59 MiB）。
- 两份采集的 droppedEvents、droppedGroups、droppedLinks、probeErrors 均为零。

回归测试纳入已有 `widget_composition_layer_rules` 和 `dock_and_window_rules` 目标，
覆盖隐藏时限、预算、可见实例豁免、再次隐藏、时钟边界，以及工厂最后使用者的释放。
这些规则测试不能证明桌面首帧、动画或毛玻璃显示正确，仍需实际采集和用户实机观察。

### 编译与自动化检查

- `76d19903`：`scripts/build.bat --reload-shell` 通过；两个定向规则目标各 1/1 通过。
- `550a7c73`：`scripts/build.bat` 通过；`scripts/test.bat full` 为 **114/115**，35.64 秒。
  唯一失败为 `steam_entitlement` 的临时断连保留离线租约断言。
  随后 `scripts/test.bat name steam_entitlement` 独立诊断 1/1 通过，未改授权模块或测试，
  不能用这次通过替换首次完整测试失败；该不稳定性尚未消除。
- 渲染相关测试均通过。构建仅观察到既有 WinUI `GetCurrentTime` 宏 C4002 警告；
  第一次 Shell 重载的 `timeout` 提示不支持重定向输入，后续标准构建步骤全部成功。
- `git diff --check` 通过。复测主进程 PID 44056，最终运行二进制 SHA256：
  `5356C663E327FE4735F8AF8C50E2B677D36998EDF1008FE8BC1E0DFBE25E04A3`。
  完整测试入口重新链接了主程序，因此该运行哈希与测试前哈希不同。

### 实际隐藏与释放

用户手动从 12 个 Lua 实例可见的页面切换到另一页，等待至少 15 秒后回复“已隐藏”。
60 秒过渡采集 `hidden-retention-after-transition` 记录了 10 次到期回收，
事件发生在采集开始后约 19.837 秒，共解除 **7,112,256 字节（6.78 MiB）** 的表面引用。
其中主表面与旧版同一批实例对应，合计 6,908,260 字节；其余为滚动文字表面。
滚动文字宽度会随内容变化，过渡期回收量不必与更早的静态采样完全相等。

| 同一批 10 个隐藏 Lua 实例 | 旧版稳定隐藏 | 新版回收后稳定隐藏 |
|---|---:|---:|
| 主表面容量 | 6,908,260 字节 | 0 |
| 滚动文字表面容量 | 旧探针未统计 | 0 |
| 仍加载的全局 Lua 实例数 | 12 | 12 |
| 全局可见 Lua 实例数 | 2 | 2 |
| 隐藏实例 `widget.render / desktop` | 0 次 | 0 次 |
| 隐藏实例 `widget.view / draw` | 0 次 | 0 次 |
| 隐藏实例主表面新分配 | 旧探针未统计 | 0 次 |

新版稳定隐藏采集为 `hidden-retention-after-hidden`，12 秒，隐藏表面容量持续为零，
累计回收计数保持 10，没有反复分配；采集期间没有合成错误日志。
`hidden-retention-after-return` 的首轮 60 秒窗口仍处于隐藏页，没有捕获到用户切回，
只能作为进一步的隐藏稳定性证据，不能用它声称恢复验证通过。
以上新旧采集的丢弃计数与 probeErrors 均为零。

这些字节是宿主持有表面的像素容量，不能直接等同于实际显存释放。
暂不声称固定 CPU、GPU 或 DWM 内存收益。

### 切回与实机验收

原主题恢复后，`hidden-retention-after-return-stable` 记录到原 10 个 Lua 实例
各一次主表面分配，集中于采集开始后 23.172～23.184 秒，说明恢复没有依赖已有 surface 引用。
该时间区间是分配事件时间，不是切页总耗时或输入到呈现延迟。
原主表面合计恢复为 6,908,260 字节，滚动文字也重新生成。
用户对首帧、组件内容、滚动文字、音乐动画以及拖动/缩放的实机验收回复“正常”。

验收包含继续切页与交互，因此此窗口结束时又有 10 个实例处于短期保留阶段；
不能把它的结束快照误读为一直可见。随后 `hidden-retention-final-state` 采集 12 秒，
最终 12 个 Lua 实例可见，主表面恢复正常，全部隐藏实例表面保留量为零。
进程累计回收实例表面 38 次、29,054,364 字节，包含同一实例多次释放及原生组件；
这些累计值用于确认重复生命周期，不能当作单次省下的内存。
相关采集丢弃计数与 probeErrors 均为零，运行日志未记录合成错误。
最终探针状态为 `idle`，独立设置进程已退出，主进程继续运行。

### 毛玻璃开关与 DWM

通过独立设置进程正常选择“深色”与“深色毛玻璃”，每次关闭设置后采集 3 秒对象计数。
始终保持该桌面页（2 个 Lua 实例可见），没有改变组件级设置。

| 场景 | 桌面面板 / 工厂 | Dock 面板 / 工厂 |
|---|---:|---:|
| 隐藏页，原深色毛玻璃 | 10 / 2 | 1 / 1 |
| 关闭第 1 次 `glass-off-1` | 1 / 1 | 0 / 0 |
| 开启第 2 次 `glass-on-2` | 10 / 2 | 1 / 1 |
| 关闭第 2 次 `glass-off-2` | 1 / 1 | 0 / 0 |
| 恢复原主题 `glass-restored` | 10 / 2 | 1 / 1 |

以上目录均带 `hidden-retention-` 前缀；采集无丢弃和 probeErrors。
Dock 的最后面板移除后工厂也归零，重复开关没有累积工厂；桌面原有两种半径的工厂
在关闭后剩一种，符合按当前使用者保留的规则。

用户指出剩余面板应为新页面指示卡片。源码 `src/widgets/widget_base.cpp` 的
`fixedGuideAppearance` 分支确实固定使用 `AcrylicLightPreset()`，
`src/widgets/guide_widget.cpp` 也明确欢迎卡片不继承全局组件预设。
这与当前页保留的一个面板相符；计数探针没有逐面板身份，归因依据为源码及用户现场定位。
不能把“全局组件毛玻璃关闭”解释为欢迎卡片、实例覆盖、弹窗和任务栏的全部材质关闭。

随后在探针停止、设置子进程关闭后，分别采集 15 秒系统计数（16 个样本，GPU 均值去掉首样本；
内存取末 6 个样本中位数）。以下原始文件位于 `hidden-retention-audit/glass-os-*.json`。

| 顺序 | DWM 私有提交 MiB | DWM 专用 GPU MiB | DWM 3D % | 主进程 CPU % |
|---|---:|---:|---:|---:|
| off-1 | 633.62 | 1917.88 | 4.334 | 0.063 |
| on-2 | 635.15 | 1925.99 | 4.858 | 0.108 |
| off-2 | 635.02 | 1918.93 | 4.569 | 0.142 |
| on-restored | 490.88 | 1727.59 | 3.093 | 0.196 |

3D 活跃引擎为 LUID `0x00000000_0x00016552` 的 `eng_0`，其他采到的 3D 引擎为零。
主进程 CPU 以 24 个逻辑处理器归一化；DWM 的进程 CPU 时间无法读取，记为不可用，未当作零。
最后一次开启时 DWM 内存和 GPU 活动反而明显下降，说明整个桌面的其他活动/缓存变化影响很大。
这组数据不支持固定 GPU 降幅或 DWM 内存释放量；能够确认的是面板和无使用者工厂的宿主引用回收。
结束后逐项比对 `SnowDesktop.personalization.json`、`SnowDesktop.general.json` 与采集前快照，
两者全部属性恢复一致，设置进程关闭。
