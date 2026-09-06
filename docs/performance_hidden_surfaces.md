# 隐藏桌面表面与毛玻璃工厂回收

## 策略与边界

桌面组件隐藏后先保留绘图表面，隐藏满 10 秒后由现有每秒维护定时器回收。
隐藏表面的 BGRA 像素容量总预算为 16 MiB；超过预算时，下次维护优先回收最早隐藏的实例。
实际回收时间受 UI 消息调度影响，10 秒不是实时期限；可见表面不参与此预算。
计量包含组件主表面和原生滚动文字子表面，不包含驱动额外分配、共享包图或 Lua 堆。

回收先解除 visual 的内容引用，再释放宿主表面引用；滚动文字同时解除原生位移动画。
保留组件所有权、轻量 visual/clip、滚动位置的时间状态和 Lua 运行时。
不卸载组件，不取消提醒、网络任务或辅助面板，也不清理仍被其他实例使用的图片缓存。
隐藏期间不创建新的滚动文字表面。恢复可见时主动排入该组件的绘制队列，即使它在当前
脏矩形之外，也会重新生成表面。原生图形调用失败沿用合成恢复路径，不把失败计作完整回收。

`IDCompositionVisual::SetContent(nullptr)` 才能解除 visual 对内容的引用；仅重置宿主
`ComPtr` 不足以证明表面已经失去引用。[Microsoft 文档](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-idcompositionvisual-setcontent)

毛玻璃每个面板继续独占 effect brush 与 backdrop source；只共享模糊工厂。
在一帧面板收集结束后，清理没有面板使用的半径工厂；帧外移除面板则当场检查。
同帧移动或替换面板可复用工厂，最后一个使用者消失后工厂数量应为零。
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
构建、运行时回收与视觉验收结果待补充；暂不声称 CPU、GPU 或 DWM 内存收益。
