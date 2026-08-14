# SnowDesktop Lua 组件 API v2

本文档描述当前宿主已经实现并放入 API v2 沙箱的接口。清单中保留的权限名、
路线图能力或 API v1 全局库不代表 v2 组件已经可以调用它们。可在运行时使用
`widget.apiInfo()`、`widget.hasFeature(id)` 和 `system.capabilities()` 探测能力。

编辑器类型定义位于 `library/snowdesktop-v2.lua`，其函数签名是本文档的配套
机器可读契约。

## 入口契约

`main.lua` 必须返回 `widget.define({...})` 的结果，并且必须且只能提供
`render` 或 `view`。宿主会把当前上下文和实例 model 传给所选回调：

```lua
local function setup(context)
    schedule.every("refresh", 60000)
    return { createdOn = context.surface }
end

local function render(context, model)
    draw.text(layout.cu(12), layout.cu(12),
        "Hello from " .. model.createdOn)
end

local function dispose(context, model, reason)
    -- Optional. Host resources are released automatically after this returns.
end

local function event(context, model, value)
    if value.kind == "schedule" and value.id == "refresh" then
        state.set("lastRefresh", time.monotonic())
    end
end

return widget.define({
    name = l10n.tr("lua_widget.example.name"),
    setup = setup,
    render = render,
    event = event,
    dispose = dispose,
})
```

`setup(context)` 最多执行一次，返回值作为实例 model 传给每次 `render(context,
model)`、`panel(context, model)`、`event(context, model, event)` 和最终的 `dispose(context, model,
reason)`。没有 `setup` 时 model 为
`nil`；没有 `dispose` 时宿主仍会自动回收实例资源。`reason` 当前可能是
`unload`、`hotReload` 或 `shutdown`。setup 失败时新 VM 不会替换热重载前的可用
VM。

event 覆盖宿主 surface 级事件：`visibility`、`resize`、`pointer`、`timer`、
`schedule`、`action`、`selection`、`environment`、`panel`、`data.change` 和
`task.complete`。
指针事件包含 `action`、`surface`、`x/y`、`button`、`delta`，命中即时绘制
region 时还包含 `targetKey`；schedule 事件包含 `id`、UTC epoch 毫秒 `now`、
`missed` 和 `coalesced`。
region 绑定的 hover、pressed、click、doubleClick、wheel 和菜单选择统一以
`event.kind == "action"` 投递。
事件驱动的数据 topic 发生变更时，持有对应订阅的组件收到 `data.change`，其中包含
`topic/revision`；组件可在该事件中重建依赖日期范围等参数的订阅。

`menu(context, model, request)` 同时用于即时绘制 region 和声明式节点的独立右键菜单。
不要把 API v1 的全局回调迁入 v2 描述符。

## 已实现能力

### `widget`

- `widget.define(definition)`：校验并返回 v2 描述符。`render` 与 `view` 必须二选一，可选
  `setup`、`panel`、`event`、`menu` 和 `dispose`。`panel` 只在
  `widget.openPanel` 打开的宿主辅助面板中绘制，收到的 `context.surface` 为
  `panel`；面板中的 `control.textInput/textArea` 与桌面 surface 使用同一套
  storage-bound 输入契约。
- `widget.apiInfo()`：返回当前 API 版本、支持版本和 feature ID。
- `widget.hasFeature(id)`：探测 feature。
- `widget.context()`：返回逻辑/像素尺寸、DPI、网格跨度、显示器范围、主题、
  辅助功能、语言区域、时区、可见/预览/选择状态和 surface。
- `widget.info()`、`widget.theme()`：兼容的实例与外观快照。
- `widget.hasPermission(name)`：查询当前实例已授予权限。
- `widget.setTitle(text)`、`widget.invalidate()`、`widget.log(level, text)`。
- `widget.setTimer`、`widget.cancelTimer`：API v1 兼容入口；新 v2 组件使用
  `schedule`。
- `widget.openSettings()`、`widget.openPanel(options)`、`widget.closePanel()`。

### `view.tree.core` 声明式视图

当前过渡 feature `view.tree.core` 提供 `view.box/row/column/stack/text/image/button/
iconButton/shape/progressBar/progressRing/spacer`。
每次 `view(context, model)` 返回一棵完整树；所有节点必须提供全树唯一、1–128 字节的
稳定 `key`。宿主先完整解析、校验和布局，再原子替换上一棵成功树；回调或校验失败时
继续显示上一棵树，不留下半棵树或空白交互区。

```lua
local function buildView(context, model)
    return view.column({
        key = "root",
        width = "fill",
        height = "fill",
        padding = 12,
        gap = 8,
        alignItems = "stretch",
        justifyContent = "center",
        children = {
            view.text({
                key = "status",
                text = model.status,
                width = "fill",
                textAlign = "center",
                fontSize = 18,
                style = { foreground = 0xFFFFFF },
            }),
            view.button({
                key = "refresh",
                label = l10n.tr("lua_widget.example.refresh"),
                height = 36,
                action = { id = "refresh" },
                events = {
                    contextMenu = { id = "refresh.menu" },
                },
                style = { background = 0x365F86, cornerRadius = 8 },
                hoverStyle = { background = 0x477FB5 },
                pressedStyle = { background = 0x315F8F },
            }),
        },
    })
end
```

尺寸接受有限非负数字、`auto` 或 `fill`；线性布局支持 `padding`、`gap`、
`flexGrow`、`alignItems/alignSelf` 和 `justifyContent`。文本支持 `fontSize`、`bold`、
`textAlign`；基础样式支持 RGB 前景/背景/边框、边框宽度、圆角、0–1 opacity 及
hover/pressed 覆盖。按钮 `action` 是 click 简写；events 还支持 pointer enter/leave/
down/up、doubleClick 和 contextMenu，动作通过 `event.kind == "action"` 投递。

`shape` 支持 rectangle、roundedRectangle、circle 和 ellipse；填充与描边来自 style。
`image` 的 `source` 只接受入口加载期间创建的 `resource.image()` 句柄，必须显式提供
`alt`（装饰图片使用空字符串），支持 `fill/contain/cover/none` fit、
`start/center/end` alignment 和 `nearest/linear` interpolation；对应 feature 为
`view.image`。`text` 和 `button` 可通过 `font` 使用 `resource.font()` 返回的包私有字体
句柄，对应 feature 为 `view.font`。这些属性不接受文件路径或跨包句柄。
`icon`/`iconButton` 的 `glyph` 使用宿主 Font Awesome 或 Fluent 字体，`iconButton` 必须
提供 `accessibility.label`。`progressBar`/`progressRing` 接受 0–1 的 `value`、正数
`thickness`、track/fill opacity，并分别使用 style.background/foreground 作为轨道和
进度色。这些节点均由宿主直接绘制，不开放路径、字体文件或原生绘图对象。

树限制为 512 节点、32 层、单节点 4 KiB 文本、全树 64 KiB 文本和最多 256 个交互
区域。未知字段、错误枚举、非连续 children、重复 key、NaN/Infinity 和越界值会拒绝
整次提交。桌面树只布局在底部标题栏之上的内容区。

该 feature 不是完整 `view.tree`：当前每帧重建树，尚无 grid/scroll/list/
input/chart/slot 节点，也没有键盘焦点、UIA 输出、RTL、文本换行、主题
token、差量资源复用或声明式 panel。需要这些能力的组件应继续使用 v2 即时绘制或等待
对应 feature；不得把 `view.tree.core` 当作稳定完整控件集声明。

### `interaction` 与元素级菜单

即时绘制没有宿主可识别的元素。`interaction.region(spec)` 在 `render` 内为当前
桌面 surface 提交语义命中区域；一次成功 render 会原子替换上一成功帧的完整集合，
render 抛错时继续使用上一集合。首版最多 256 个 region，稳定 `key` 为 1 到 128
个 UTF-8 字节，后提交的重叠区域位于上层。

```lua
local function render(context, model)
    local key = "primary-action"
    local hovered = interaction.isHovered(key)
    local pressed = interaction.isPressed(key)
    draw.rect(12, 12, 120, 36,
        pressed and 0x315F8F or (hovered and 0x477FB5 or 0x365F86), 8)
    draw.text(28, 20, "Open", 15, 0xFFFFFF)
    interaction.region({
        key = key,
        shape = { type = "roundedRect", x = 12, y = 12,
            width = 120, height = 36, radius = 8 },
        cursor = "hand",
        events = {
            click = { id = "item.open", value = { itemId = "primary" } },
            contextMenu = { id = "item.menu",
                value = { itemId = "primary" } },
        },
        accessibility = { role = "button", label = "Open" },
    })
end
```

shape 首版支持 `rect`、`roundedRect` 和 `circle`；cursor 支持 `default`、`hand`、
`text`、`crosshair`。events 支持 `pointerEnter`、`pointerLeave`、`pointerDown`、
`pointerUp`、`pointerMove`、`click`、`doubleClick`、`wheel` 和 `contextMenu`。
动作 `value` 会被深拷贝，只允许 nil、布尔、有限数字、字符串、连续数组和字符串键
对象，限制 8 层、256 个节点和合计 16 KiB 字符串。普通 hover/click 不需要权限。

即时绘制的纵向滚动区域使用 `interaction.scroll(spec)`，不要调用 v1
`ui.scrollArea`：

```lua
local scroll = interaction.scroll({
    key = "items",
    shape = { type = "rect", x = 0, y = 0,
        width = layout.width(), height = layout.height() },
    contentHeight = 1200,
})
draw.pushClip(0, 0, layout.width(), layout.height())
-- 使用 scroll.offset 把内容坐标换算为视口坐标后绘制。
draw.popClip()
```

scroll key 同样是实例内稳定的 1–128 字节 UTF-8 字符串；只接受正尺寸 rect，
`contentHeight` 上限为 1,000,000 逻辑单位。返回值包含 `offset`、`maximum`、
`viewportHeight` 和 `contentHeight`。宿主处理滚轮和触控板 wheel 增量、钳制偏移、重绘及
滚动条；组件必须用成对的 `draw.pushClip/popClip` 裁剪内容。
`interaction.setScrollOffset(key, offset)` 只在当前 render 已注册同 key 区域后设置并
返回实际偏移。滚动不需要 `ui.input` 权限，对应 feature 为 `interaction.scroll`。

右键命中带 `contextMenu` 绑定的 region 后，宿主同步调用 descriptor 的 `menu`：

```lua
local function menu(_context, _model, request)
    if request.id ~= "item.menu" then return nil end
    return ui.menu({
        { id = "item.open", label = "Open" },
        { id = "item.pin", label = "Pin", checked = false },
        { type = "separator" },
        { id = "item.remove", label = "Remove" },
    })
end
```

首版菜单项支持唯一字符串 `id`、`label`、`enabled`、`checked`、separator 和宿主
字体 glyph。回调必须同步、快速且不执行 I/O，最多读取 64 项。用户选择后收到
`event.kind == "action"`，其中 `id` 为菜单项 ID，`source == "contextMenu"`，并带
原 region 的 `targetKey` 与 `value`。菜单打开后只要 region 集合产生新一代提交，
旧菜单动作就会失效，避免重排或复用 key 后误操作。SnowDesktop 的设置、授权、
诊断和移除入口始终保留。该 API 不要求 `ui.contextMenu` 权限；对应 feature 为
`interaction.region`、`interaction.pointerActions` 和 `interaction.contextMenu`。
- `widget.editText(...)`：旧宿主编辑器兼容调用，不建议新 v2 组件依赖。

### `control` 文本编辑

`control.textInput(spec)` 和 `control.textArea(spec)` 是当前即时绘制 surface 的
宿主管理文本编辑器。组件在每次 `render` 中提交稳定描述符，宿主继续使用 Direct2D
绘制透明背景、光标、选择、占位文本和 IME 组合下划线；输入值绑定到实例
`storageKey`，函数返回当前字符串。它们不是 `view.tree.core` 节点，也不向
Lua 暴露剪贴板内容或原生窗口句柄。

```lua
local value = control.textArea({
    key = "note",
    storageKey = "text",
    shape = {
        type = "rect",
        x = layout.cu(12),
        y = layout.cu(12),
        width = layout.width() - layout.cu(24),
        height = layout.height() - layout.cu(48),
    },
    placeholder = l10n.tr("lua_widget.note.placeholder"),
    fontSize = layout.fontCu(15),
    maxBytes = 65536,
    liveUpdate = true,
})
```

共同字段为 `key/storageKey/shape`，以及 `placeholder/fontSize/textColor/
placeholderColor/backgroundColor/borderColor/focusedBorderColor/backgroundAlpha/
focusedBackgroundAlpha/borderAlpha/focusedBorderAlpha/radius/padding/
borderThickness/selectAll/liveUpdate/maxBytes`。shape 只接受正尺寸 `rect`；key 和
storageKey 是 1–128 字节有效 UTF-8。颜色是 `0xRRGGBB`，alpha 是 0–1，字号范围
9–96。单行 `maxBytes` 默认 4096，多行默认 65536，允许范围 1–65536；粘贴、普通
输入和 IME 提交按编辑后的最终 UTF-8 大小原子接受或拒绝，不会先删除选择再留下
半次修改。旧存储若已经超限仍可删除内容，宿主不会静默截断。

`textArea` 额外支持 `placeholderWhenWhitespace`。两个控件都支持点击定位、拖选、
Shift 选择、Ctrl+A/C/X/V、Escape 恢复焦点前内容；多行 Enter 插入换行、
Ctrl+Enter 提交，滚轮与光标跟随会调整实例内滚动位置。普通文本输入不要求权限，
剪贴板只由宿主在聚焦控件内处理，并没有开放通用剪贴板 API。

`control.focus(key)` 只能在直接 click/doubleClick/pointerDown/pointerUp/wheel、菜单命令
或宿主明确标记的打开回调同步栈中成功；render、schedule、data.change 和
task.complete 不能抢走桌面键盘焦点。返回 `(focused, error)`，稳定失败码为
`trustedGestureRequired`、`controlNotFound` 或 `hostUnavailable`。对应 feature 为
`control.textInput`、`control.textArea` 和 `control.focus`。

### `schedule`

- `schedule.every(id, milliseconds, options?)`：创建或替换一个重复计划。
- `schedule.after(id, milliseconds, options?)`：创建或替换一个单次计划。
- `schedule.at(id, epochMilliseconds, options?)`：按 UTC epoch 毫秒创建或替换一个
  单次绝对时间计划，最远可设置 366 天；过去时间在下一次宿主唤醒时合并触发。
- `schedule.cancel(id)`：取消计划，存在并取消时返回 `true`。

ID 为 1–128 字节，每个实例最多 32 个计划；周期请求范围是 1 ms–24 小时，
宿主最小实际周期为 100 ms。跨过多个重复截止时间时只分发一个
`event.kind == "schedule"` 事件，并通过 `now`、`missed` 和 `coalesced` 报告实际分发
时间与合并结果。
`options.whenHidden` 支持 `pause`、`throttle`（默认）和 `continue`。pause 隐藏时不
保留宿主唤醒，恢复后只发送一个合并事件并报告 `missed`；throttle 隐藏时使用
5000 ms 最小周期；continue 保持请求周期。卸载、热重载和关闭会自动取消实例计划。

`schedule.at` 对应 feature `schedule.absolute`；系统时钟在宿主重新计算截止时间时会重新
投影到单调时钟，避免用可回拨的 wall clock 计算经过时长。当前尚不支持 timeline 或
预览虚拟时钟。API v1 继续使用 `onTimer`
兼容路径；新 v2 组件不得再依赖清单 `refreshIntervalMs` 过渡事件。

### `data`

当前公开二十二个按需数据源：`system.cpu`、`system.memory`、`system.gpu`、`system.power`、
`system.network.status`、`system.network.traffic`、`system.storage.volumes`、
`system.storage.io`、`system.display.topology`、`system.display.current`、
`audio.output.default`、`audio.output.volume`、`audio.output.analysis`、
`media.sessions`、`media.current`、`media.timeline`、`desktop.items`、
`desktop.selection`、`desktop.changes`、`calendar.events` 和
`calendar.selectedDate`，以及 `app.indexStatus`。在 `setup` 或模块
入口创建订阅，不要在每次 `render` 中重复订阅：

```lua
local cpu

local function setup()
    cpu = data.subscribe("system.cpu", {
        maxAgeMs = 1000,
        whenHidden = "throttle",
    })
end

local function render()
    local snapshot = cpu:value()
    if snapshot.available then
        draw.text(12, 12, string.format("CPU %.1f%%",
            snapshot.value.usagePercent))
    elseif snapshot.warmingUp then
        draw.text(12, 12, "CPU …")
    end
end
```

`data.subscribe(topic, options?)` 返回句柄。`options.maxAgeMs` 为 1–86400000，
同时表达请求采样周期与快照过期阈值；CPU 最快 500 ms，内存最快 1000 ms，
电源、存储卷和显示拓扑最快 2000 ms；存储 I/O、默认音频端点和主音量最快
1000 ms，媒体三个 topic 最快 500 ms，桌面和日历事件 topic 最快 100 ms，
音频分析最快 16 ms。`whenHidden` 可为
`pause`、`throttle`（默认）或 `continue`；
当前系统 provider 不承诺后台 continue，因此会收敛为隐藏 throttle。
`handle:value()` 返回
`available/value/timestamp/stale/warmingUp/error` 包络，CPU value 包含
`usagePercent/logicalProcessors/name`，内存 value 包含
`totalBytes/usedBytes/freeBytes/usagePercent`，电源 value 包含
`acPower/charging/saver/batteryPercent/estimatedRemainingSeconds?`。CPU 首次
差分采样可能暂时 `warmingUp=true`；无电池设备返回
`available=false,error="notPresent"`。`handle:unsubscribe()` 主动释放；卸载、
热重载和关闭也会自动释放。

GPU value 的 `adapters` 是数组；每项包含不透明 `id`、显示 `name`、
`usagePercent`、`dedicatedMemoryBytes/dedicatedUsedBytes` 和
`sharedMemoryBytes/sharedUsedBytes`。宿主不会只返回第一块 GPU；首次 PDH 差分
样本为 `warmingUp=true`。最后一个 GPU 订阅释放后会关闭 PDH query，不会因 CPU、
内存或网络仍有订阅而继续采样 GPU。

网络 status value 包含 `connectivity`（`none/local/internet`）、`transport`
（`none/ethernet/wifi/cellular/other`）、`costKnown/metered/roaming/overLimit`。
traffic value 包含 `connected/receivedBytes/sentBytes/downloadBytesPerSecond/
uploadBytesPerSecond`，首次差分样本为 `warmingUp=true`。状态和流量是两个独立
topic；只订阅状态不会启动流量差分采样。两者都不会返回 IP、MAC、SSID、BSSID
或主机名。

存储卷 value 的 `volumes` 是当前可访问挂载卷数组。每项包含不透明 `id`、
`displayName/mountPoint/kind/capacityBytes/freeBytes/capacityAvailable/removable/readOnly`；
`mountPoint` 只用于显示，不能作为文件 API 的路径或授权句柄。空光驱等无法读取
容量的设备以 `capacityAvailable=false` 表示；宿主不会为了刷新快照同步访问远程卷，
避免断开的网络映射拖住其他共享 provider。预览使用固定模拟卷，不枚举开发机。

存储 I/O value 是所有物理磁盘的有界聚合，包含 `readBytesPerSecond`、
`writeBytesPerSecond` 和钳制到 0–100 的 `busyPercent`，不包含磁盘序列号或文件路径。
首次 PDH 差分样本为 `warmingUp=true`；最后一个 I/O 订阅释放后立即关闭该 PDH query，
不会因卷列表或其他系统 topic 仍有订阅而继续采样。

显示拓扑 value 的 `displays` 是所有活动显示器数组；每项包含不透明 `id`、显示
`name`、`primary`、逻辑 `bounds/workArea`、像素 `pixelBounds/pixelWorkArea`、
`dpiX/dpiY/scale/refreshHz/orientation`，以及 `hdrKnown/hdrSupported/hdrEnabled`。
Windows 无法报告高级颜色状态时 `hdrKnown=false`，不能把两个 false 当成设备明确
不支持 HDR。预览返回单个固定显示器，不读取开发机拓扑。

`system.display.current` 使用同一份共享显示元数据，但按订阅所属实例的 surface
边界选择当前显示器，value 通过 `display` 返回单项 `SnowDisplayDataValue`。组件跨屏
移动后宿主会在下一次拓扑快照匹配新显示器；匹配前返回
`available=false,error="currentDisplayUnavailable"`，不会错误回退到主显示器。

`audio.output.default` value 包含默认 multimedia render endpoint 的不透明 `id`、
Windows 友好 `name` 和 `state`；`audio.output.volume` 包含匹配的 `endpointId`、
0–1 主音量 `volume`、`muted` 和 `minimum/maximum`。没有输出设备时返回
`available=false,error="notPresent"`。这两个 topic 只读取 endpoint 元数据与主音量，
不会启动 loopback、取得 PCM 或暴露原生 endpoint ID；预览使用固定模拟设备。

`audio.output.analysis` 使用独立 WASAPI loopback 线程，value 返回固定 128 点
`waveform`、64 个 `spectrum` bin、`rms/peak/silent/deviceChanged`、不透明
`endpointId` 及源 `sampleRate/channels`。waveform 已下混为 mono 且限制在 -1–1，
频谱和电平限制在 0–1；Lua 不取得 PCM、无限历史或每进程音频。当前配置通过
`maxAgeMs` 选择 16–1000 ms 发布周期，点数和特征选择仍固定，后续再开放有上限的
`features/waveformPoints/spectrumBins/updateHz` 选项。

三个媒体 topic 受 `media.read` 保护，并在同一 provider 采样周期内合并读取：
`media.sessions` value 返回最多 32 个会话和当前会话的不透明 ID，`media.current`
value 通过 `session` 返回当前会话，`media.timeline` value 通过 `timeline` 返回当前
时间线。每个会话包含受限到 4096 字节的 `sourceName/title/artist/album`、播放状态、
逐动作 `can*`、相对 `positionMs/durationMs` 和 seek 范围；没有当前会话时 current
和 timeline 返回 `available=false,error="notPresent"`，会话列表则是可用的空数组。
这些只读订阅不会执行播放动作或取得封面原图，预览使用一个固定模拟会话。

三个桌面 topic 受 `desktop.read` 保护且由宿主变更事件驱动，不启动轮询线程。
`desktop.items` 返回最多 2048 项，`desktop.selection` 返回最多 512 项；每项只有
稳定宿主引用 `id` 和 `title/source/type/selected` 展示字段，不向 v2 返回绝对路径。
`desktop.changes` 返回单调 `revision` 与最长 64 字节的宿主 `reason`，用于判断何时
重新读取列表。预览使用固定项目；取消最后订阅时没有后台 worker 或系统句柄残留。

`calendar.events` 和 `calendar.selectedDate` 受 `calendar.read` 保护，同样不启动
轮询线程。events 可在订阅选项中同时传入 `fromDate/toDate`（ISO `YYYY-MM-DD`、
闭区间、最长 366 天）；未传时使用当前选中日期前后各 62 天。value 返回实际
`fromDate/toDate`、最多 512 个本地事件、`revision` 和 `truncated`，事件包含
`id/revision/title/date/allDay/startMinutes/endMinutes/notes/reminderMinutes`。
selectedDate value 返回 `date/revision`。创建、修改和删除日程仍不由这些只读 topic
执行；`calendar.selectDate(date)` 只改变 SnowDesktop 内部共享选中日期，不修改事件，
因此不要求 `calendar.write`。纯 `calendar.dateInfo/addDays` 也不读取用户数据、不要求
权限。对应 feature 为 `calendar.selection` 和 `calendar.dateMath`；预览返回固定日期
和事件。

`app.indexStatus` 受 `app.discovery` 保护，value 返回 `state/revision`。当前宿主
应用索引真实返回 `indexing`、`ready` 或 `unavailable`；缺失时以
`available=false,state="unavailable",error="providerUnavailable"` 明确报告，
应用索引变更推进 revision。该 topic 不携带完整应用目录，搜索结果由有界
`app.search` 任务获取。

CPU、内存和 GPU 受 `system.performance.read` 保护，电源受 `system.power.read` 保护，
两个网络 topic 受 `system.network.read` 保护，两个存储 topic 受
`system.storage.read` 保护，显示拓扑受 `system.display.read` 保护。
基础两个音频输出 topic 受 `audio.output.read` 保护；分析 topic 单独受
`audio.output.analyze` 保护。分析是高风险 provider：只有已授权且可见的实例持有
订阅时运行，隐藏、撤销、卸载或取消最后一个订阅会立即停止并清空，忽略
`whenHidden="continue"`；预览只返回确定性模拟波形。
需要无权限降级的组件应把对应权限声明在 `optionalPermissions`，并处理
`available=false,error="permissionDenied"`；预览返回稳定模拟值且不会读取本机
状态。对应 feature ID 是 `data.subscribe`、`data.system.cpu`、
`data.system.memory`、`data.system.gpu`、`data.system.power`、
`data.system.network.status` 和
`data.system.network.traffic`、`data.system.storage.volumes` 和
`data.system.storage.io`、`data.system.display.topology` 和
`data.system.display.current`，以及 `data.audio.output.default`、
`data.audio.output.volume` 和 `data.audio.output.analysis`，以及
`data.media.sessions`、`data.media.current` 和 `data.media.timeline`。
桌面 topic 对应 `data.desktop.items`、`data.desktop.selection` 和
`data.desktop.changes`。
日历 topic 对应 `data.calendar.events` 和 `data.calendar.selectedDate`。
应用索引状态对应 `data.app.indexStatus`。

### `task`

当前公开异步媒体动作 `media.play/pause/toggle/stop/next/previous/seek/setRate/setShuffle/setRepeat`，
默认音频输出动作 `audio.output.setVolume/setMute`，应用任务
`app.search`、`app.launch`，桌面项目任务 `desktop.search`、`everything.search`、
`shell.openItem`、`shell.revealItem`、`desktop.refresh`，一次性通知任务
`notification.show`，以及本地日历写入任务 `calendar.create/update/remove`、公网读取
任务 `network.request` 和外部链接动作 `shell.openUri`。它们对应
feature ID `task.start`、`task.media.control`、`task.audio.output.control`、`task.app.search`、`task.app.launch`
、`task.notification.show`、`task.calendar.write`、`task.network.request` 和
`task.shell.openUri`，以及 `task.desktop.search`、`task.everything.search`、
`task.shell.item`、`task.desktop.refresh`。媒体动作要求 `media.action` 权限，而且只能在
`click/doubleClick/pointerDown/pointerUp/wheel`、宿主按钮、菜单命令或由宿主明确
标记来源的打开回调同步调用栈内启动：

```lua
local taskId, err = task.start("media.toggle", {
    sessionId = session.id,
})
if not taskId then
    widget.log("warn", "media task rejected: " .. tostring(err))
end
```

默认音频输出动作要求 `audio.output.control` 和同样的可信用户手势，只作用于调用时
Windows 当前默认的 multimedia render endpoint：

```lua
local volumeTask = task.start("audio.output.setVolume", { volume = 0.65 })
local muteTask = task.start("audio.output.setMute", { muted = true })
```

`volume` 必须是有限数值，宿主在 Core Audio 调用前钳制到 0–1；`muted` 必须是布尔值。
宿主用自己的事件来源 GUID 标记修改，并对同一组件实例的音频修改设置 100 ms 最小
间隔。任务成功值为 `{ accepted = true }`；稳定错误包括 `rateLimited`、`notPresent`、
`audioEnumeratorUnavailable`、`audioEndpointUnavailable`、`audioVolumeUnavailable`、
`audioControlRejected`、`permissionRevoked` 和 `canceled`。该权限不授予非默认设备、
逐进程音频会话、默认设备切换或系统音频策略控制。

`app.search` 要求 `app.discovery`，不要求用户手势；参数是严格的普通表：`query`
为 1–256 字节有效 UTF-8，`limit` 默认为 50、范围 1–100，`offset` 默认为 0、范围
0–10000。结果按宿主名称/拼音匹配排序并分页，只返回展示字段和实例作用域的不透明
`ref`：

```lua
local searchId, err = task.start("app.search", {
    query = "music",
    limit = 20,
    offset = 0,
})

-- 对应 task.complete 成功值：
-- event.value.items[i] = { ref, title, source, type }
-- event.value.nextOffset / hasMore / catalogRevision
```

如果应用索引仍在构建，完成事件返回 `appIndexNotReady`；可订阅
`app.indexStatus`，在 revision/state 变化后重试。目录在 UI 线程复制成不可变快照，
实际匹配在独立任务线程完成，因此不会让 Lua 或 worker 直接读取桌面应用容器。

`app.launch` 要求独立的 `app.launch` 权限和当前可信用户手势，只接受同一组件实例
先前搜索得到的 `ref`：

```lua
-- 必须位于直接 click/action 回调的同步调用栈：
local launchId, err = task.start("app.launch", { ref = item.ref })
```

Lua 不会取得可执行文件路径、参数、Shell verb 或工作目录，也不能伪造其他实例的
引用。应用目录 revision 改变后旧引用返回 `staleReference`；未知、被回收或跨实例
引用返回 `invalidReference`。成功值与媒体动作一样为 `accepted=true`，只表示宿主的
Shell 启动队列已接受请求，不代表目标进程最终成功启动。

`desktop.search` 与 `everything.search` 提供有界、可取消的项目搜索。两者都接受
`query/limit/offset` 严格参数；query 为 1–256 字节有效 UTF-8，limit 范围 1–100，
offset 范围 0–100。`desktop.search` 要求 `desktop.read`：宿主最多复制 2048 个当前
桌面项目为不可变快照，实际名称/拼音匹配在任务线程完成。`everything.search` 要求
`everything.search` 权限，每实例最多一个并发任务，在后台调用本机 Everything
索引；宿主把进程级 Everything SDK 调用串行化，避免与 SnowDesktop 自身搜索互相覆盖。

```lua
local desktopTask = task.start("desktop.search", {
    query = "report", limit = 50, offset = 0,
})
local everythingTask = task.start("everything.search", {
    query = "report", limit = 50, offset = 0,
})

-- 两者的 task.complete 成功值结构相同：
-- event.value.items[i] = { ref, title, source, type }
-- event.value.nextOffset / hasMore / revision
```

结果只含展示字段和当前组件实例可用的不透明 `ref`，不含文件系统路径。组件可将该
ref 直接传给 `draw.icon(ref, ...)`；不得持久化、解析或自行构造。桌面 revision 变化
后旧桌面引用返回 `staleReference`；未知、已回收或跨实例引用返回
`invalidReference`。取消 Everything 搜索会抑制结果投递，但不能保证中断已经进入
Everything IPC 的一次查询。

项目打开、定位和桌面刷新要求 `desktop.action`，并且只能在直接指针动作或菜单命令
的可信手势调用栈中启动。`shell.openItem/revealItem` 只接受同一实例先前由
`desktop.search/everything.search` 返回的 ref；`desktop.refresh` 不接受参数：

```lua
local openTask = task.start("shell.openItem", { ref = item.ref })
local revealTask = task.start("shell.revealItem", { ref = item.ref })
local refreshTask = task.start("desktop.refresh")
```

成功值为 `{ accepted = true }`。稳定错误包括 `invalidReference`、`staleReference`、
`openRejected`、`revealRejected`、`permissionDenied`、`userGestureRequired` 和
`canceled`。应用 ref 仍只能交给 `app.launch`，项目 ref 不能交给 `app.launch`。

`notification.show` 要求 `notification.post` 权限，但不要求用户手势，因此可以从
`schedule` 到期事件启动。参数是只允许 `title/message` 两个字段的严格普通表：标题为
1–256 字节、正文为 1–2048 字节的有效 UTF-8，均不得包含 NUL。宿主沿用每实例每分钟
最多 5 次的通知配额；预览会异步返回确定性的成功结果，但不会产生系统通知：

```lua
local notificationId, err = task.start("notification.show", {
    title = l10n.tr("widget.name"),
    message = l10n.tr("widget.completed"),
})
```

组件应把 `notification.post` 放在 `optionalPermissions`，在授权拒绝、撤销或超额时
继续完成自身主功能。成功值为 `accepted=true`；除通用的 `permissionRevoked` 和
`canceled` 外，通知还可能返回 `quotaExceeded`、`providerUnavailable` 或
`notificationFailed`。当前 v2 只开放一次性 `show`；更新、关闭、预约和操作按钮仍在
后续计划内，不得使用 API v1 `system.notify` 代替。

`calendar.create/update/remove` 要求 `calendar.write`，参数只接受严格字段。
create/update 共用 `title/date/allDay/startMinutes/endMinutes/notes/reminderMinutes`；
update 另需宿主事件 `id` 和正整数 `expectedRevision`，remove 只接受 `id`。提醒值
限定为 `-1/0/5/15/30/60/1440`，日期必须是有效 `YYYY-MM-DD`，文本和时间范围在
进入日历服务前完成边界检查。新增和更新不要求手势；删除必须从直接指针动作或菜单
命令的可信调用栈启动：

```lua
local updateId, err = task.start("calendar.update", {
    id = item.id,
    expectedRevision = item.revision,
    title = item.title,
    date = item.date,
    allDay = item.allDay,
    startMinutes = item.startMinutes,
    endMinutes = item.endMinutes,
    notes = item.notes or "",
    reminderMinutes = item.reminderMinutes,
})
```

成功结果为 `{ id, revision }`；删除的 revision 为 0。更新冲突返回
`error="conflict"`，并在完成事件的 `currentRevision` 给出宿主当前 revision。
其他稳定错误包括 `not_found`、`title_required`、`text_too_long`、`invalid_date`、
`invalid_time`、`invalid_reminder`、`event_limit`、`save_failed`、
`permissionDenied`、`userGestureRequired` 和 `previewReadOnly`。

`network.request` 要求 `network.internet`，当前只提供无凭据、无 Cookie、无自定义头部和
请求体的公网 HTTPS `GET`。默认可访问任意公网 HTTPS 主机；如果组件功能固定依赖少数服务，
可以在 `networkDomains` 中逐项声明精确主机名，主动把自身网络范围收窄。域名限制不支持
通配符或子域继承；无论是否收窄，localhost、局域网地址和指向非公网地址的解析或重定向
都被拒绝。每实例该任务最多并发 2 个，参数只接受：

- `url`：1–2048 字节有效 UTF-8 公网 HTTPS URL；清单声明了 `networkDomains` 时主机必须精确命中；
- `timeoutMs`：1000–30000，默认 15000；
- `cacheSeconds`：0–86400，默认 0，缓存按实例、请求与响应上限隔离；
- `maxBytes`：4096–1048576，默认 524288。

```lua
local requestId, err = task.start("network.request", {
    url = "https://www.example.com/feed.xml",
    timeoutMs = 15000,
    cacheSeconds = 120,
    maxBytes = 512 * 1024,
})

-- 成功：event.value = { status, body, fromCache }
-- HTTP 响应失败时：event.error == "httpStatus"，event.status 可用
```

重定向的每一跳都重新检查 HTTPS、可选的精确域名范围、DNS 解析地址和实际连接地址。响应在
worker 中读取，超限立即失败，不会把慢网络 I/O 放进 UI/render 线程。稳定完成错误包括
`requestRejected`、`networkError`、`redirectRejected`、`responseTooLarge`、
`httpStatus`、`permissionRevoked` 和 `canceled`。预览返回确定性的最小 RSS mock，不发起
网络连接。声明了 `networkDomains` 的组件如果请求范围外主机，会得到 `requestRejected`；
RSS、Webhook 阅读器等允许用户填写地址的组件不应声明固定域名范围。

`shell.openUri` 要求 `shell.launch` 和当前可信用户手势，只接受不含用户名/密码的公网
HTTPS URL；`http:`、`file:`、自定义 scheme、localhost、局域网和 IP 内网地址均被拒绝。
成功值为 `{ accepted = true }`，仅表示宿主 Shell 队列接受请求。稳定错误包括
`invalidUrl`、`openRejected`、`permissionDenied`、`userGestureRequired` 和 `canceled`。
阅读器等非核心打开场景应把 `shell.launch` 放入 `optionalPermissions`，无授权时仍显示内容。

六个直接动作 `play/pause/toggle/stop/next/previous` 的参数表可省略，也可只传
`sessionId`。`seek` 需要非负整数 `positionMs`，其含义是相对媒体时间线起点的位置；
`setRate` 需要有限正数 `rate`；`setShuffle` 需要布尔 `shuffle`；`setRepeat` 需要
`mode="none"|"track"|"list"`。所有动作都可选传入 `media.sessions/current` 返回的
不透明 `sessionId`，不传时控制 Windows 当前会话。目标会话已经消失时返回
`notAvailable`；宿主在执行前检查该会话对应的 `can*` 能力，时间线越界返回
`seekOutOfRange`，不支持的控制返回 `actionUnsupported`。会话 ID 仅用于当前快照和
后续短时交互，不应解析或持久化。

启动成功只表示任务进入宿主队列。WinRT 媒体调用在独立工作线程执行；完成后由
`event.kind == "task.complete"` 串行投递，事件包含 `taskId/task/ok`。成功时
`event.value.accepted == true`；失败时 `event.error` 为稳定错误码，例如
`notAvailable`、`actionUnsupported`、`actionRejected`、`mediaActionFailed`、
`permissionRevoked` 或 `canceled`。完成事件不继承原始用户手势，不能借完成回调
连续启动更多高风险动作。

`task.cancel(taskId)` 只接受当前 Lua VM 自己持有的任务。卸载、热重载、撤权和
宿主关闭会自动取消；热重载使用 VM owner token，旧任务结果不会投递给新 VM。
预览不会访问系统媒体会话、音频端点或系统通知，而是异步返回确定性的 `accepted=true` mock。
媒体参数表只接受上述动作对应字段；其他任务同样拒绝未知字段、错误类型和越界数值。API v1 的
`media.playPause/next/previous` 不会注册进 v2 VM，不能绕过任务的手势门禁。

### `draw`

即时绘制坐标以组件左上角为 `(0, 0)`：

- `draw.text(x, y, text, size?, color?, maxWidth?, bold?, singleLine?,
  maxHeight?, alpha?, font?)`
- `draw.measureText(text, size?, maxWidth?, bold?, font?)`
- `draw.rect(...)`、`draw.strokeRect(...)`、`draw.line(...)`、`draw.circle(...)`
- `draw.pushClip(x, y, width, height)`、`draw.popClip()`
- `draw.fa(...)`、`draw.fluent(...)`
- `draw.image(imageHandle, x, y, width, height, alpha?)`
- `draw.icon(ref, x, y, size?, alpha?)`：要求 `desktop.read`，只接受当前实例由
  `app.search`、`desktop.search` 或 `everything.search` 返回且仍有效的不透明 ref；
  不接受路径、v1 项目表或其他实例的引用。

颜色是 `0xRRGGBB`，透明度单独传入。`draw.image` 在 v2 中只接受
`resource.image()` 返回的不透明句柄；字体句柄可传给 `draw.text` 和
`draw.measureText`。

### `layout`

`layout.width/height` 返回当前逻辑尺寸。`columns/rows/sizeClass` 返回跨度与尺寸
档位。`cellWidth/cellHeight/cellScale/cellGap/barHeight` 提供宿主网格指标。
使用 `layout.cu(value)` 和 `layout.fontCu(value)` 做 DPI/网格自适应，不要假定
固定像素。

### `storage`

- `storage.get(key) -> string?`
- `storage.set(key, value)`：`value` 必须是字符串，并立即持久化。
- `storage.remove(key)`、`storage.keys()`。
- `storage.transaction(function(tx) ... end) -> changed`：一次原子提交多个字符串
  写入；事务对象提供 `tx:get/set/remove`。

```lua
storage.transaction(function(tx)
    tx:set("item.42.title", "Book tickets")
    tx:set("order", "17,42")
    tx:remove("draft")
end)
```

事务内必须通过 `tx` 访问存储，不能嵌套事务，也不能混用全局
`storage.get/set/remove/keys`。回调抛错、最终快照超过配额或写盘失败时不会暴露部分
修改；配额在最终快照上检查，因此允许先暂存新键再在同一事务删除旧键。事务最多
1024 次操作；每实例最多 256 个键、每键 128 个有效 UTF-8 字节、每值 64 KiB、
总量 1 MiB。`storage.transaction` 对应 feature 为 `storage.transaction`。

API v2 的 `storage.set/remove/transaction` 不能在 `render` 内调用；持久化只允许在
setup、事件、菜单动作或迁移回调等副作用阶段执行。预览和迁移使用隔离覆盖层，成功
后再由宿主决定是否持久化。当前事务值仍是字符串；计划中的 JSON-like 类型化持久值
和 secret reference 尚未开放。

只在值变化时写入；数值和布尔值应显式序列化，并用 `tonumber` 或明确规则读取。

### `state`

`state` 是仅随当前组件 VM 存活的实例内存状态：

- `state.get(key, default?)` 返回深拷贝，未设置时返回默认值。
- `state.set(key, value)` 保存深拷贝；值没有变化时返回 `false` 且不重复失效。
- `state.remove(key)`、`state.has(key)`、`state.keys()`、`state.clear()`。

值支持 nil、boolean、有限 number、string、连续数组和字符串键对象。循环表、
metatable、混合数组/对象以及超出深度、节点、字符串或 256-key 实例配额的值会
被拒绝。真实变化会合并成宿主失效信号；它不会写盘，热重载或卸载后丢失。

### `system` 与 `time`

- `system.info()`：Windows、架构、宿主版本和部署模式。
- `system.capabilities(feature?)`：列出或查询 feature。
- `system.uptime()`：毫秒与是否包含睡眠时间。
- `time.now()`、`time.monotonic()`。
- `time.parts(epochMilliseconds?, timeZone?)`。
- `time.format(epochMilliseconds?, options?)`。
- `time.add(epochMilliseconds, delta, options?)`、`time.compare(a, b)`。

这些基础环境与时间接口不要求高风险权限。CPU、内存、网络、媒体、音频波形等
按需数据订阅属于后续 API，不要用 API v1 的 `sys` 代替。

### `calendar` 日期计算与选择

- `calendar.dateInfo("YYYY-MM-DD")` 返回年、月、日、星期和当月天数。
- `calendar.addDays(date, offset)` 返回偏移后的 ISO 日期，offset 范围为
  -366000 到 366000。
- `calendar.selectDate(date)` 改变 SnowDesktop 本地共享选中日期。

前两项是纯 Gregorian 日期计算；第三项只用于月历、日程等组件协同，不创建、修改或
删除日程。三者都不要求 `calendar.read/write`；读取选中日期和事件仍必须通过受
`calendar.read` 保护的 `data.subscribe`。

声明式 `select` 设置可用稳定的 `options` 值，并用等长的 `optionLabels` 提供当前语言
显示文本；宿主保存值而不是翻译，切换语言不会使现有设置失效。对应 feature 为
`settings.select.localizedOptions`。

`appSearch` 设置使用 `key` 保存用户选中的应用显示名，使用 `searchKey` 保存搜索文字；
宿主复用应用索引并在后台完成匹配，在设置页直接显示候选项。`emptyLabel` 和
`noResultsLabel` 必须使用组件清单中的本地化文本。该控件只负责设置交互；组件运行时仍应
通过 `app.search` 获取当前实例的 opaque `ref`，并在可信用户动作中用 `app.launch` 启动。
对应 feature 为 `settings.appSearch`。

### `l10n`

- `l10n.tr(literalKey, ...)`、`l10n.language()`。
- `l10n.formatNumber`、`formatBytes`、`formatDuration`、
  `formatRelativeTime`、`formatList`。

所有用户可见文本都应使用清单 `locales` 中存在的字面量 key。宿主语言文件不是
组件翻译目录。

### `module` 与 `resource`

`module.require("modules/example.lua")` 只能在入口脚本求值期间加载包内 `.lua`
模块；结果按实例缓存。禁止循环依赖、包外路径以及 `require/package/io/os/load`。

资源必须在 `widget.json` 中声明，例如：

```json
"resources": {
  "logo": { "type": "image", "path": "assets/logo.png" },
  "display": {
    "type": "font",
    "path": "assets/Display.ttf",
    "license": "OFL-1.1"
  }
}
```

入口加载时创建句柄：

```lua
local logo = resource.image("logo")
local display = resource.font("display")

-- 同一句柄可进入声明式视图；Lua 不会获得资源路径。
view.image({ key = "logo", source = logo, alt = "SnowDesktop" })
view.text({ key = "title", text = "SnowDesktop", font = display })
```

可用 `resource.exists(name)` 和 `resource.status(handle)` 查询。资源路径、数量、
文件大小、图片像素、字体格式与许可字段受包校验器限制；不允许绝对路径、父级
跳转、符号链接、junction 或其他重解析点。

## 清单 v2 最小要求

```json
{
  "schemaVersion": 2,
  "id": "f527797f-a986-4ad1-a58d-250ef91f53d3",
  "slug": "my-widget",
  "name": "My Widget",
  "nameKey": "lua_widget.my_widget.name",
  "version": "1.0.0",
  "apiVersion": 2,
  "dataVersion": 1,
  "entry": "main.lua",
  "minHostVersion": "1.0.4.0",
  "author": "Your Name",
  "license": "MIT",
  "description": "A short English fallback.",
  "descriptionKey": "lua_widget.my_widget.description",
  "requiredFeatures": ["draw.immediate", "lifecycle.event", "lifecycle.model",
    "l10n.basic", "schedule.basic"],
  "optionalFeatures": [],
  "resources": {},
  "permissions": [],
  "optionalPermissions": []
}
```

`schemaVersion` 与 `apiVersion` 必须同时为 2。`requiredFeatures` 不受支持时包
无法激活；`optionalFeatures` 用于可降级能力。基础时钟、绘制、上下文和包资源
不应声明高风险权限。

## 当前明确未开放

API v2 暂未向沙箱提供完整 `view.tree`、通用即时 region/`view.tree.core` 的键盘焦点/UIA 输出、
受控二级菜单、`desktop`、旧的同步 `media` 库、HTTP、尚未列出的系统状态、
通用剪贴板、文件选择和应用启动库。`control.textInput/textArea` 只在聚焦的
宿主管理编辑器内部代理标准剪贴板操作，不允许 Lua 读取剪贴板。其余能力将在对应宿主实现、配额与按需生命周期完成后
再加入 feature 目录和 LuaLS 定义；不要根据权限词汇自行推测函数名。
