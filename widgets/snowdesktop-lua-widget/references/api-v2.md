# SnowDesktop Lua 组件 API v2

本文档描述当前宿主已经实现并放入 API v2 沙箱的接口。清单中保留的权限名、
路线图能力或 API v1 全局库不代表 v2 组件已经可以调用它们。可在运行时使用
`widget.apiInfo()`、`widget.hasFeature(id)` 和 `system.capabilities()` 探测能力。

编辑器类型定义位于 `library/snowdesktop-v2.lua`，其函数签名是本文档的配套
机器可读契约。

## 入口契约

`main.lua` 必须返回 `widget.define({...})` 的结果，并且当前版本必须提供
`render`。宿主会把当前上下文和实例 model 传给 `render`：

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
model)`、`event(context, model, event)` 和最终的 `dispose(context, model,
reason)`。没有 `setup` 时 model 为
`nil`；没有 `dispose` 时宿主仍会自动回收实例资源。`reason` 当前可能是
`unload`、`hotReload` 或 `shutdown`。setup 失败时新 VM 不会替换热重载前的可用
VM。

event 覆盖宿主 surface 级事件：`visibility`、`resize`、`pointer`、`timer`、
`schedule`、`action`、`selection`、`environment`、`panel` 和 `task.complete`。
指针事件包含 `action`、`surface`、`x/y`、`button`、`delta`，命中即时绘制
region 时还包含 `targetKey`；schedule 事件包含 `id`、`missed` 和 `coalesced`。
region 绑定的 hover、pressed、click、doubleClick、wheel 和菜单选择统一以
`event.kind == "action"` 投递。

`menu(context, model, request)` 已用于即时绘制 region 的独立右键菜单；`view`
仍是后续声明式视图契约预留项，当前宿主会拒绝使用。
不要把 API v1 的全局回调迁入 v2 描述符。

## 已实现能力

### `widget`

- `widget.define(definition)`：校验并返回 v2 描述符。当前必需 `render`，可选
  `setup`、`event`、`menu` 和 `dispose`。
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

### `schedule`

- `schedule.every(id, milliseconds, options?)`：创建或替换一个重复计划。
- `schedule.after(id, milliseconds, options?)`：创建或替换一个单次计划。
- `schedule.cancel(id)`：取消计划，存在并取消时返回 `true`。

ID 为 1–128 字节，每个实例最多 32 个计划；周期请求范围是 1 ms–24 小时，
宿主最小实际周期为 100 ms。跨过多个重复截止时间时只分发一个
`event.kind == "schedule"` 事件，并通过 `missed` 和 `coalesced` 报告合并结果。
`options.whenHidden` 支持 `pause`、`throttle`（默认）和 `continue`。pause 隐藏时不
保留宿主唤醒，恢复后只发送一个合并事件并报告 `missed`；throttle 隐藏时使用
5000 ms 最小周期；continue 保持请求周期。卸载、热重载和关闭会自动取消实例计划。

当前尚不支持绝对时间 `at`、timeline 或预览虚拟时钟。API v1 继续使用 `onTimer`
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
selectedDate value 返回 `date/revision`。创建、修改、删除或选择日期仍不由这些只读
topic 执行；预览返回固定日期和事件。

`app.indexStatus` 受 `app.discovery` 保护，value 返回 `state/revision`。当前宿主
应用搜索提供者就绪时为 `ready`，缺失时以 `available=false` 和
`state="unavailable",error="providerUnavailable"` 明确报告；应用索引变更推进
revision。该 topic 不携带完整应用目录，搜索结果仍应由后续有界任务获取。

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

当前公开首批异步媒体动作：`media.toggle`、`media.next` 和 `media.previous`。
它们对应 feature ID `task.start` 与 `task.media.control`，要求 `media.action`
权限，而且只能在 `click/doubleClick/pointerDown/pointerUp/wheel`、宿主按钮、
菜单命令或由宿主明确标记来源的打开回调同步调用栈内启动：

```lua
local taskId, err = task.start("media.toggle")
if not taskId then
    widget.log("warn", "media task rejected: " .. tostring(err))
end
```

启动成功只表示任务进入宿主队列。WinRT 媒体调用在独立工作线程执行；完成后由
`event.kind == "task.complete"` 串行投递，事件包含 `taskId/task/ok`。成功时
`event.value.accepted == true`；失败时 `event.error` 为稳定错误码，例如
`notAvailable`、`actionUnsupported`、`actionRejected`、`mediaActionFailed`、
`permissionRevoked` 或 `canceled`。完成事件不继承原始用户手势，不能借完成回调
连续启动更多高风险动作。

`task.cancel(taskId)` 只接受当前 Lua VM 自己持有的任务。卸载、热重载、撤权和
宿主关闭会自动取消；热重载使用 VM owner token，旧任务结果不会投递给新 VM。
预览不会访问系统媒体会话，而是异步返回确定性的 `accepted=true` mock。
当前三个媒体动作不接受参数，第二个参数只能省略、为 nil 或空表。API v1 的
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
- `draw.icon(...)`：当前仍要求 `desktop.read`，但 v2 尚未开放 desktop 查询库；
  不应作为新 v2 组件的基础能力。

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

API v2 暂未向沙箱提供声明式 `view` 控件树、即时 region 的键盘焦点/UIA 输出、
受控二级菜单、`desktop`、旧的同步 `media` 库、HTTP、尚未列出的系统状态、
剪贴板、文件选择和应用启动库。它们将在对应宿主实现、配额与按需生命周期完成后
再加入 feature 目录和 LuaLS 定义；不要根据权限词汇自行推测函数名。
