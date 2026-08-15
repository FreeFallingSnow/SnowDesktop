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

当前过渡 feature `view.tree.core` 提供 `view.box/row/column/stack/text/image/button/icon/
iconButton/shape/progressBar/progressRing/spacer`；额外的 `view.dataSeries` feature 提供
`sparkline/lineChart/barChart/waveform/spectrum`，`view.statusVisuals` 提供
`badge/divider/meter`，`view.selectionControls` 提供 `toggle/checkbox`，
`view.actionControls` 提供 `link/radioGroup/slider`，
`view.inputControls` 一次提供 `textInput/textArea/searchBox/numberInput/select`，
`view.grid.uniform` 提供基础 `grid`，`view.flow.wrap` 提供横向换行 `flow`。
`view.scroll` 提供宿主滚动视口，`view.collection.basic` 提供基础集合，
`view.collection.virtual` 提供固定行高虚拟集合与可见范围查询；
`view.styledText.basic` 提供有界样式 span，`view.monthCalendar` 提供受控月历日期网格，
`view.logicalSlots` 提供与 manifest 宿主管理槽位严格对应的 `slotSurface/slotItem`，
`view.referenceIcon` 提供只接收实例自有 opaque ref 的宿主图标节点。
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
`contextMenu` 动作默认 `scope="element"`：命中后菜单只显示该元素返回的操作，不混入组件
设置、悬浮和移除等总菜单。覆盖整张组件表面的菜单应显式写
`{ id="component.menu", scope="component" }`，其返回项会附加到组件总菜单。

`toggle` 和 `checkbox` 是受控选择控件：必须提供非空 `label`、显式 `checked`，以及
`action` 简写或 `events.change`；不得绑定 `events.click`。指针完成一次有效点击时，宿主
投递 `event.action == "change"`，并附带当前 `previousChecked` 与建议的新值 `checked`。
宿主不会替组件修改或持久化状态；组件应在 `event` 中更新自己的 model/storage 并调用
`widget.invalidate()`，下一棵树仍以组件提供的 `checked` 为准。`checkedStyle` 先于
`hoverStyle/pressedStyle` 合并，轨道、勾选标记、hover、pressed 和元素命中均由宿主实时
绘制；两类控件也支持各自的 `contextMenu`。直接指针 change 保留可信用户手势，但当前
过渡实现尚未提供键盘操作与 UI Automation 输出。

```lua
view.toggle({
    key = "notifications",
    label = "Notifications",
    checked = model.notifications,
    action = { id = "notifications.change" },
    checkedStyle = { background = 0x4C9AFF },
})
```

`link` 要求非空 `label` 和 click action，宿主使用链接语义、手型光标、强调色与下划线
实时绘制，并支持 hover/pressed 样式和元素级 `contextMenu`。`radioGroup` 与 `slider` 同样是
受控控件，必须使用 `action` 简写或 `events.change`，不得绑定 `events.click`。单选组要求
显式 `selectedValue`（空字符串表示未选择）以及 1–64 个 `{ key, value, label, enabled? }`
选项，key/value 在组内唯一；每个选项都有独立的 `<group-key>/<option-key>` 命中区、
radio 语义、hover/pressed/checked 绘制和右键菜单目标。选中选项时 action 事件附带
`previousSelection/selection`，宿主不写回选中值。

`slider` 要求显式 `value` 和 `accessibility.label`，支持 `min/max/step`（默认
0/1/0.01）及水平/垂直方向。鼠标左键按下后由宿主捕获拖动，持续投递 step 对齐且限制在
范围内的 `previousControlValue/controlValue`；右键只用于菜单，不改变数值。组件收到建议值
后仍需更新自己的 model/storage 并调用 `widget.invalidate()`。这三个节点对应
`view.actionControls`；当前尚不提供键盘调节与 UI Automation 输出。

```lua
view.radioGroup({
    key = "density",
    selectedValue = model.density,
    options = {
        { key = "comfortable", value = "comfortable", label = "Comfortable" },
        { key = "compact", value = "compact", label = "Compact" },
    },
    action = { id = "density.change" },
})

view.slider({
    key = "volume",
    value = model.volume,
    min = 0,
    max = 100,
    step = 5,
    action = { id = "volume.change" },
    accessibility = { label = "Volume" },
})
```

`view.inputControls` 的五类节点都是声明式受控控件。`textInput/textArea/searchBox`
必须提供字符串 `value`，`numberInput` 必须提供有限数值 `value/min/max/step`，四类输入
都要求 `action` 简写或 `events.change` 和 `accessibility.label`，不得绑定 `events.click`。
宿主复用同一套键盘、选择、剪贴板代理和 IME 编辑器：聚焦期间宿主持有编辑缓冲，
`change` 通过 `previousText/text` 只报告建议值；组件更新 model 后下一棵树才成为权威值，
Lua 不会获得剪贴板内容或原生句柄。`liveUpdate=false` 将 change 延迟到提交，Escape 在
实时模式下用 `cancelled=true` 建议恢复初始值。`focus/blur/submit` 为可选动作；单行 Enter
提交，`textArea` Enter 换行而 Ctrl+Enter 提交。`numberInput` 的上下方向键按 step 调整，
文本是完整且位于范围内的数字时，change 还带 `numberValid=true` 和 `controlValue`。

```lua
view.searchBox({
    key = "query",
    value = model.query,
    placeholder = "Search",
    maxBytes = 256,
    action = { id = "query.change" },
    events = { submit = { id = "query.submit" } },
    accessibility = { label = "Search" },
})
```

`select` 需要 `selectedValue`、1–64 个稳定选项、`events.change`（可用 `action` 简写）、
`events.click` 和 `accessibility.label`。展开状态同样由组件通过 `expanded` 控制：触发区 click
报告 `previousExpanded/expanded`，展开后每个 `<select-key>/<option-key>` 选项 change 报告
`previousSelection/selection`。宿主在组件内表面顶层绘制选项并优先命中，不调用阻塞式系统
菜单；组件收到 click/change 后应更新 model 并 invalidate。当前弹层仍受组件及父滚动视口
裁剪，跨组件表面的通用 popover 属于后续宿主 surface API。

`grid` 是行优先的均匀网格容器，必须提供 1–64 的整数 `columns`；每列等宽，
`columnGap/rowGap` 分别控制水平和垂直间距，未提供时回退到 `gap`。隐藏子节点不占格，
其余子节点保持原顺序，现有 `alignItems/alignSelf` 控制格内拉伸或对齐，
`justifyContent` 控制整组行在纵向剩余空间中的位置。对应 feature 为
`view.grid.uniform`。该 feature 不包含自定义 track、显式行定义、跨行/跨列、自动填充、
瀑布流或虚拟化；需要这些能力时不得假设基础 `grid` 会静默模拟。

`flow` 按子节点原顺序从左到右放置，当前行剩余宽度不足时整体换到下一行；隐藏子节点
不占位置，单个超宽子节点钳制到内容区宽度。`columnGap/rowGap` 独立控制项间距与行间距，
未提供时回退到 `gap`；`justifyContent` 分别作用于每一行的横向剩余空间，
`alignItems/alignSelf` 控制行内纵向对齐。自动高度采用“每项单独一行”的保守固有高度，
避免测量阶段因未知最终宽度截断换行内容；在固定或 fill 高度中，超出的行高会按可用高度
收缩。对应 feature 为 `view.flow.wrap`，不包含纵向 flow、masonry、滚动或虚拟化。

`scroll` 是宿主管理的有界滚动视口，只允许一个子节点且该节点必须可见（额外隐藏节点也会
拒绝）；默认纵向，也可使用
`orientation="horizontal"`。宿主测量完整内容、按实例和稳定 key 保存偏移、处理滚轮/
触控板 wheel、钳制到内容边界、移动子树并同时裁剪绘制和元素命中。滚出视口的按钮或
列表项不能 hover、点击或打开右键菜单。`showScrollbar=false` 可隐藏宿主滚动条，但不会
关闭滚动。每棵树最多 32 个 scroll，单轴内容 extent 最大 1,000,000 逻辑单位；该能力
对应 feature `view.scroll`，当前不提供 Lua 自绘滚动条、惯性动画、滚动链或程序化定位。

`list` 是纵向有界集合，`gridList` 是要求 `columns=1..64` 的行优先等宽集合；两者的直接
子节点必须全部是 `listItem`。每个 `listItem` 要求全树唯一稳定 key、只含一个可见内容子节点
（额外隐藏节点也会拒绝）和 `accessibility.label`，可以使用 `action`/`events.click`、doubleClick、pointer 状态与
独立 contextMenu；宿主默认赋予 `listitem` 语义。一个树最多 256 个 listItem，仍受 512
总节点和 256 交互区域上限约束。对应 feature 为 `view.collection.basic`。这是非虚拟化
基础集合；大量或远程分页数据应使用下述 `virtualList/virtualGrid`，不能通过超配额树模拟。

```lua
view.scroll({
    key = "feed-scroll",
    height = "fill",
    children = {
        view.list({
            key = "feed",
            gap = 6,
            children = {
                view.listItem({
                    key = "article:" .. article.id,
                    action = { id = "article.open",
                        value = { articleId = article.id } },
                    events = { contextMenu = { id = "article.menu",
                        value = { articleId = article.id } } },
                    accessibility = { label = article.title },
                    children = {
                        view.text({ key = "title:" .. article.id,
                            text = article.title }),
                    },
                }),
            },
        }),
    },
})
```

`virtualList` 和 `virtualGrid` 是固定行高的纵向虚拟集合，对应 feature
`view.collection.virtual`。Lua 先用 `view.virtualRange()` 查询当前宿主滚动位置需要实体化的
1-based 闭区间，只为该区间创建连续 `listItem`；再把同一 `key/itemCount/itemExtent/
rowGap/columns/overscan`、返回的 `firstIndex` 和窗口 children 提交给虚拟节点。宿主按全局
索引布局这些项、使用完整逻辑 itemCount 计算滚动范围，并验证提交窗口覆盖真实可见行；
窗口缺项会拒绝整棵树，不能显示错误但可点击的空洞。

```lua
local itemExtent = 44
local rowGap = 4
local viewportExtent = math.max(1, context.logicalHeight - 8)
local range = view.virtualRange({
    key = "feed-virtual",
    itemCount = #articles,
    itemExtent = itemExtent,
    viewportExtent = viewportExtent,
    rowGap = rowGap,
    overscan = 2,
})
local children = {}
for index = range.firstIndex, range.lastIndex do
    local article = articles[index]
    children[#children + 1] = view.listItem({
        key = "article:" .. article.id,
        action = { id = "article.open", value = { index = index } },
        accessibility = { label = article.title },
        children = {
            view.text({ key = "title:" .. article.id,
                text = article.title }),
        },
    })
end
return view.virtualList({
    key = "feed-virtual",
    height = "fill",
    itemCount = #articles,
    itemExtent = itemExtent,
    firstIndex = range.firstIndex,
    rowGap = rowGap,
    overscan = 2,
    children = children,
})
```

`virtualGrid` 另要求 `columns=1..64`，`view.virtualRange` 必须收到同一 columns；
`itemExtent` 表示行高而不是单格宽度。虚拟集合最多表示 1,000,000 项，但总逻辑 extent
仍不得超过 1,000,000；每帧最多实体化 128 项，overscan 为 0–16 行，空集合使用
`firstIndex=0` 和空 children。虚拟节点必须有固定或 fill 高度，`viewportExtent` 是扣除
节点 padding 后的实际内容高度。当前不支持可变行高、横向虚拟集合、sticky header、
程序化定位或保留已回收项的 Lua 局部状态；稳定状态应放在 model/state 并以 item key 索引。

`shape` 支持 rectangle、roundedRectangle、circle 和 ellipse；填充与描边来自 style。
`image` 的 `source` 只接受入口加载期间创建的 `resource.image()` 句柄，必须显式提供
`alt`（装饰图片使用空字符串），支持 `fill/contain/cover/none` fit、
`start/center/end` alignment 和 `nearest/linear` interpolation；对应 feature 为
`view.image`。`referenceIcon` 使用相同的 `alt/fit/alignment/interpolation`，但以当前
组件实例从宿主搜索、文件引用任务或逻辑槽位获得的 1–128 字节 opaque `reference`
代替图片资源句柄；宿主在异步 Shell 图标缓存就绪后重绘，不在渲染热路径同步解码，
也不会把目标路径交给 Lua。该节点本身不授予启动、打开、定位或文件内容权限，对应
feature 为 `view.referenceIcon`。

```lua
view.referenceIcon({
    key = item.id .. ".icon",
    reference = item.reference,
    alt = item.title,
    width = 64,
    height = 64,
})
```

`text`、`badge`、`button`、`link`、`toggle`、`checkbox` 和
`radioGroup` 可通过 `font` 使用
`resource.font()` 返回的包私有字体
句柄，对应 feature 为 `view.font`。这些属性不接受文件路径或跨包句柄。
`icon`/`iconButton` 的 `glyph` 使用宿主 Font Awesome 或 Fluent 字体，`iconButton` 必须
提供 `accessibility.label`。`progressBar`/`progressRing` 接受 0–1 的 `value`、正数
`thickness`、track/fill opacity，并分别使用 style.background/foreground 作为轨道和
进度色。这些节点均由宿主直接绘制，不开放路径、字体文件或原生绘图对象。

`styledText` 要求 1–64 个非空 `spans`，每个 span 可独立指定
`foreground/fontSize/bold/italic/underline/strikethrough`。宿主把全部 span 合并为一个 DirectWrite layout，统一
执行换行、裁剪、对齐和包私有字体解析，而不是为每段创建子节点。该首批契约通过
`view.styledText.basic` 探测；它刻意不包含 inline icon 和可点击 action span，作者不得
把整个节点的命中区误当作行内链接语义。

```lua
view.styledText({
    key = "status",
    spans = {
        { text = "Build ", foreground = 0x94A3B8 },
        { text = "passed", foreground = 0x4ADE80, bold = true },
    },
    accessibility = { label = "Build passed" },
})
```

`monthCalendar` 是受控的六周 Gregorian 日期网格。它要求 `year/month/selectedDate`、
按周日至周六排列的七个本地化 `weekdayLabels`、change `action` 和
`accessibility.label`；`firstDayOfWeek=1..7` 决定显示顺序。`todayDate`、最多 366 个唯一
`eventDates`、`showAdjacentDates` 以及 `selectedStyle/todayStyle/adjacentStyle/eventStyle`
用于有限状态绘制。每个可见日期拥有稳定的 `<calendar-key>/<YYYY-MM-DD>` 命中目标和
独立 hover/pressed/contextMenu；点击投递 `previousSelection/selection`，组件必须写回
自己的 model，宿主不会修改持久状态。

```lua
view.monthCalendar({
    key = "month",
    year = 2026, month = 8, firstDayOfWeek = 2,
    selectedDate = model.selectedDate,
    todayDate = model.todayDate,
    weekdayLabels = { "日", "一", "二", "三", "四", "五", "六" },
    action = { id = "calendar.select" },
    accessibility = { label = "2026 年 8 月" },
})
```

`badge` 要求非空 `text`，默认使用 4 单位 padding 和胶囊圆角，适合紧凑状态标记；
`divider` 通过 `orientation="horizontal"|"vertical"` 表示分隔方向，以 `thickness` 和
`style.foreground` 控制线宽与颜色，垂直分隔线未显式指定尺寸时使用 intrinsic width 并
填满父级高度；`meter` 接受 0–1 `value`，绘制方式与确定进度条相同，但语义是当前读数
而不是任务完成进度，因此必须提供 `accessibility.label`。三者对应
`view.statusVisuals`，不创建原生窗口或逐帧 Lua 回调。

五个数据图形节点只接受 `values` 连续数值数组，每节点 1–512 个有限样本、全树最多
4096 个样本，并要求 `accessibility.label`。`sparkline` 和 `lineChart` 默认按当前数列
自动取值域；`barChart` 自动包含零基线；`waveform` 默认范围为 -1–1，`spectrum` 默认
范围为 0–1。需要固定尺度时必须同时提供有限且满足 `min < max` 的 `min/max`；超出范围
的样本只在绘制时钳制，不修改 Lua 数据。`lineChart` 绘制有界参考线，`waveform` 和跨零
柱图绘制零线；`style.foreground`、`thickness`、`trackOpacity` 和 `fillOpacity` 控制前景。
节点由宿主在已提交树内直接绘制，不创建逐样本子节点或逐样本事件区域：

```lua
view.waveform({
    key = "waveform",
    values = audio.waveform,
    height = 64,
    style = { foreground = 0x72C7FF },
    trackOpacity = 0.5,
    accessibility = { label = "Output audio waveform" },
})
```

树限制为 512 节点、32 层、单节点 4 KiB 文本、全树 64 KiB 文本和最多 256 个交互
区域；数据图形另有上述逐节点和全树样本额度。未知字段、错误枚举、非连续 children、
重复 key、NaN/Infinity 和越界值会拒绝整次提交。桌面树只布局在底部标题栏之上的内容区。

该 feature 不是完整 `view.tree`：当前每帧重建树，尚无可变高度虚拟集合、
可操作行内 span，也没有通用键盘焦点、UIA 输出、RTL、主题
token、差量资源复用或声明式 panel。需要这些能力的组件应继续使用 v2 即时绘制或等待
对应 feature；不得把 `view.tree.core` 当作稳定完整控件集声明。

### `slots.model` 与 `view.logicalSlots`

API v2 包可在 `widget.json` 中静态声明单项绑定或有界集合。当前只支持
`operation="reference"`，引用原对象而不移动、复制或删除它：

```json
"slots": {
  "primaryApp": {
    "kind": "binding",
    "accepts": ["app.reference"],
    "operation": "reference",
    "replacePolicy": "allow",
    "allowClear": true
  },
  "favorites": {
    "kind": "collection",
    "accepts": ["desktop.item", "app.reference", "filesystem.reference"],
    "operation": "reference",
    "capacity": 32
  }
}
```

每包最多声明 16 个槽位，集合容量为 1–64。`slots.binding(id)` 与
`slots.collection(id)` 只接受 manifest 中同 kind 的 ID。读取方法 `id/revision/state/
capacity/item/items` 可在 view 中调用；`bind/add/clear/remove/move` 会持久化宿主管理模型，
只能由当前可信用户 action 调用，预览和普通 render/data/timer 回调会以
`userGestureRequired` 或 `previewReadOnly` 拒绝。`bind/add` 接受 `app.search`、
`desktop.search`、`everything.search` 或文件引用任务返回的当前实例 opaque ref，成功后
返回 `SnowLogicalSlotChange`；槽位会发出新的持久 opaque `item.reference`，可继续交给
`view.referenceIcon` 显示宿主图标，或交给对应的 `app.launch`、
`shell.openItem/revealItem` 任务执行受权限和可信手势约束的操作。

声明式树必须准确反映同一宿主快照。binding 的 surface 可有一个 placeholder 或一个
`slotItem`；有绑定时必须提交该 item。collection 的直接 children 必须按宿主顺序完整提交
为 `slotItem`。伪造、漏掉、重复或交换 reference，以及可选 `revision` 不匹配，都会拒绝
整棵新树并保留上一棵成功树：

```lua
local favorites = slots.collection("favorites")
local children = {}
for _, item in ipairs(favorites:items()) do
    children[#children + 1] = view.slotItem({
        key = item.id,
        reference = item.reference,
        accessibility = { label = item.title },
        child = view.text({ key = item.id .. ".title", text = item.title }),
    })
end

return view.slotSurface({
    key = "favorites",
    collection = "favorites",
    revision = favorites:revision(),
    children = children,
})
```

探测 `slots.pointerReorder` 后，collection 中有至少两个项目时，用户可从任一已提交
`slotItem` 内直接按住拖动。未越过系统拖动阈值时仍按普通声明式 click 处理；越过后由宿主
接管捕获，实时绘制源项目轮廓和插入线，释放时按 item ID 原子调用同槽 move，并写入同一
undo/redo 历史。成功变化通过 `slot.changed` 以 `source="host.pointer"` 投递；Lua 不接收
高频拖动坐标，也不能伪造插入位置。binding、单项目 collection、槽位外拖出和跨槽拖动
不属于该 feature。

探测 `slots.keyboardNavigation` 后，选中单个 Lua 组件时按 `Tab` / `Shift+Tab` 可进入并循环
当前已提交且可见的 `slotItem`，方向键按屏幕空间位置移动宿主焦点，`Escape` 退出。若焦点项
自己声明了 `events.click`，`Enter` 或空格会以可信 `source="keyboard"` action 激活它；宿主
不会猜测或代替触发其任意子控件。collection 焦点项可用 `Alt+方向键` 在同槽内原子移动，
`Delete` 可移除 collection 项或 `allowClear=true` 的 binding，二者都进入同一 undo/redo
历史，并以 `slot.changed source="host.keyboard"` 通知 Lua。宿主直接绘制焦点轮廓，不向 Lua
投递高频按键；该 feature 不表示通用声明式控件键盘焦点或 UI Automation 已完成。

提交成功的 `slotSurface` 现在也是宿主原生拖放面。桌面项目、应用快捷方式或 Explorer
文件拖到该区域时，宿主先按 manifest 的 `accepts`、binding 替换策略及 collection 容量
进行命中判断，再显示插入预览并原子保存引用；不会移动、复制或删除真实对象。当前原生
入口一次只接收一个对象，多选拖入会在命中前拒绝。组件或组件分组标签不会进入逻辑槽位。

探测 `slots.hostPicker` 后，可在当前可信 action 中调用 binding 或 collection 句柄的
`pick()`。宿主会打开复用快速导航索引的选择界面，只显示 manifest `accepts` 允许的应用、
桌面项目或文件候选；选择结果直接成为持久化 opaque reference，collection 默认追加一项。
选择器不会授予文件内容权限，取消也不会产生事务；collection 满容量时不会打开。

宿主拖放、选择器或槽位项菜单提交后会派发 `event.kind == "slot.changed"`，字段为 `slotId`、
`slotKind`、`revision`、`operation`、opaque `itemIds`，以及 `source == "host.drop"`、
`"host.picker"`、`"host.menu"`、`"host.pointer"` 或 `"host.keyboard"`。Lua 应重新读取对应句柄并重算 view，不能把事件内容
当作可写模型。可分别探测 `slots.nativeDrop`、`slots.nativeContextMenu`、
`slots.pointerReorder`、`slots.keyboardNavigation` 与 `slots.event.changed`。原生槽位项菜单只显示该项的向前/向后移动和移除操作，不会附加
组件总菜单；binding 是否能移除遵守 manifest 的 `allowClear`。原生槽位项拖出和指针
跨槽重排仍未接入。

探测 `slots.history` 后，可在当前可信用户 action 中调用 `slots.undo()` / `slots.redo()`；
它们按组件实例维护最近 32 次宿主槽位事务，返回 operation 为 `undone` / `redone` 的
`SnowLogicalSlotChange`。`slots.canUndo()` / `slots.canRedo()` 可在 view 中读取。新事务会清空
redo 栈，热重载或重启不会恢复历史。探测 `slots.hostHistory` 后，选中单个 Lua 组件时
宿主还会将 Ctrl+Z、Ctrl+Shift+Z 和 Ctrl+Y 路由到同一历史；没有可用槽位历史时不会
吞掉原桌面快捷键。键盘槽位操作与 Lua 主动调用的历史操作共享相同的最近 32 次边界。

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
旧菜单动作就会失效，避免重排或复用 key 后误操作。`request.scope` 为 `element` 或
`component`；元素级菜单独立显示，组件级菜单才与 SnowDesktop 的设置、授权、诊断和
移除入口合并。该 API 不要求 `ui.contextMenu` 权限；对应 feature 为
`interaction.region`、`interaction.pointerActions` 和 `interaction.contextMenu`。
- `widget.editText(...)`：旧宿主编辑器兼容调用，不建议新 v2 组件依赖。

### `control` 文本编辑

`control.textInput(spec)` 和 `control.textArea(spec)` 是即时绘制 surface 的兼容入口；新声明式
组件应优先使用 `view.inputControls`。这两个函数仍是
宿主管理文本编辑器。组件在每次 `render` 中提交稳定描述符，宿主继续使用 Direct2D
绘制透明背景、光标、选择、占位文本和 IME 组合下划线；输入值绑定到实例
`storageKey`，函数返回当前字符串。它们不属于声明式节点，也不向
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
或宿主明确标记的打开回调同步栈中接受；render、schedule、data.change 和
task.complete 不能抢走桌面键盘焦点。若该操作同时把目标输入框加入界面树，宿主会把
最新一次聚焦请求保留到同一 surface 的下一次成功渲染，并在提交控件后聚焦；若届时
仍未提交目标控件，则清除请求并记录诊断，不会在更晚的无关界面中意外聚焦。返回
`(accepted, error)`，稳定失败码为 `trustedGestureRequired`、`controlNotFound` 或
`hostUnavailable`。对应 feature 为 `control.textInput`、`control.textArea` 和
`control.focus`。

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

当前公开二十四个按需数据源：`system.cpu`、`system.memory`、`system.gpu`、`system.power`、
`system.network.status`、`system.network.traffic`、`system.storage.volumes`、
`system.storage.io`、`system.display.topology`、`system.display.current`、
`audio.output.default`、`audio.output.volume`、`audio.output.analysis`、
`media.sessions`、`media.current`、`media.timeline`、`media.artwork`、`desktop.items`、
`desktop.selection`、`desktop.changes`、`calendar.events` 和
`calendar.selectedDate`、`app.indexStatus`，以及 `filesystem.watch`。在 `setup` 或模块
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
`sharedMemoryBytes/sharedUsedBytes`。两个容量来自 DXGI adapter 描述；两个 used 字段
分别来自 Windows `GPU Adapter Memory` 的 Dedicated Usage 和 Shared Usage，并按
adapter LUID 归属，不能把核显 LOCAL segment 当作专用显存。宿主不会只返回第一块
GPU；首次 PDH 差分样本为 `warmingUp=true`。最后一个 GPU 订阅释放后会关闭 PDH
query，不会因 CPU、内存或网络仍有订阅而继续采样 GPU。

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

四个媒体 topic 受 `media.read` 保护，并在同一 provider 采样周期内合并读取：
`media.sessions` value 返回最多 32 个会话和当前会话的不透明 ID，`media.current`
value 通过 `session` 返回当前会话，`media.timeline` value 通过 `timeline` 返回当前
时间线。每个会话包含受限到 4096 字节的 `sourceName/title/artist/album`、播放状态、
逐动作 `can*`、相对 `positionMs/durationMs` 和 seek 范围；没有当前会话时 current
和 timeline 返回 `available=false,error="notPresent"`，会话列表则是可用的空数组。
`media.artwork` 只返回当前会话的 `sessionId`、临时 `image` resource handle 和不超过
512×512 的 `width/height`。宿主在工作线程读取最多 4 MiB 的编码数据，拒绝边长超过
16384 的源图，并解码为有界 PBGRA 像素；Lua 不取得编码原图、缓存路径或像素字节。
该句柄可直接传给 `draw.image` 或 `view.image.source`，最后一个订阅取消后对应 CPU/GPU
缓存立即清除，因此不应持久化句柄。无封面使用 `notPresent`；读取、查询、解码、尺寸等
失败分别使用稳定错误码。预览返回固定 64×64 模拟封面，不读取开发机媒体状态。

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
`data.media.sessions`、`data.media.current`、`data.media.timeline` 和
`data.media.artwork`。
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
任务 `network.request`、外部链接动作 `shell.openUri`、受控设置动作
`system.openSettings`、有界剪贴板任务 `clipboard.read/write/clear`，以及用户选择文件
范围的 `filesystem.pickOpen/pickSave/pickFolder`。它们对应
feature ID `task.start`、`task.media.control`、`task.audio.output.control`、`task.app.search`、`task.app.launch`
、`task.notification.show`、`task.calendar.write`、`task.network.request` 和
`task.shell.openUri`、`task.system.openSettings`、`task.clipboard.text`、
`task.clipboard.image`、`task.clipboard.fileReference`、
`task.filesystem.picker`、`task.filesystem.access`，以及 `task.desktop.search`、`task.everything.search`、
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

`system.openSettings` 要求 `shell.launch` 和当前可信用户手势，只接受宿主固定枚举的
`page`：`notifications/audio/display/network/bluetooth/power/storage/apps/personalization`。
宿主把枚举映射到微软公开的固定 `ms-settings:` 页面；Lua 不能传 URI、查询参数或
任意设置页名称：

```lua
local settingsTask = task.start("system.openSettings", {
    page = "audio",
})
```

成功值为 `{ accepted = true }`，表示 Windows 接受打开请求；稳定错误包括
`openRejected`、`permissionDenied`、`userGestureRequired` 和 `canceled`。预览只返回
确定性成功结果，不启动 Windows 设置。

剪贴板任务都要求当前可信用户手势，并按读取与修改分别检查 `clipboard.read` 和
`clipboard.write`。读取必须显式请求 `text`、`image` 或 `file-reference` format；写入
仍只接受 `text`，且最多 262144 字节、无 NUL、有效 UTF-8；清空不接受参数：

```lua
local readTask = task.start("clipboard.read", { format = "text" })
local imageTask = task.start("clipboard.read", { format = "image" })
local filesTask = task.start("clipboard.read", {
    format = "file-reference",
})
local writeTask = task.start("clipboard.write", {
    format = "text",
    text = "SnowDesktop",
})
local clearTask = task.start("clipboard.clear")

-- readTask 成功：event.value = { format = "text", text = "..." }
-- imageTask 成功：event.value = {
--     format = "image", image = imageHandle, width = 256, height = 256,
-- }
-- filesTask 成功：event.value = {
--     format = "file-reference",
--     items = { { ref = itemRef, name = "photo.png", type = "file" } },
-- }
-- write/clear 成功：event.value = { accepted = true }
```

任务在独立 worker 访问 Win32 剪贴板，同一实例最短间隔 100 ms；稳定错误包括
`rateLimited`、`clipboardBusy`、`formatUnavailable`、`clipboardTooLarge`、
`clipboardReadFailed`、`clipboardWriteFailed`、`clipboardImageDecodeFailed`、
`clipboardImageDimensionsInvalid`、`clipboardReferenceUnavailable`、
`permissionRevoked` 和 `canceled`。预览按所请求 format 返回确定性 mock，不访问真实
剪贴板。

图片输入块上限为 64 MiB，源尺寸每边上限 16384；宿主解码并等比缩放到每边不超过
512 像素，返回的临时 `SnowImageResource` 可直接用于 `draw.image` 或
`view.image.source`。句柄绑定当前组件实例，在热重载、实例卸载或运行时资源限额回收
后失效，不得持久化。文件引用一次最多返回 32 项，每项仅有 `{ref,name,type}`；`ref`
可用于 `draw.icon`、`shell.openItem` 和 `shell.revealItem`，但不暴露路径、不允许读取
文件内容，也不等同于 `filesystem` 授权。剪贴板历史仍未开放，组件不得用文本路径
冒充文件引用。

文件选择器只在当前可信用户手势中启动，并且只把用户实际选择的范围授予当前组件
包与当前实例。`filesystem.pickOpen` 要求 `filesystem.userSelected.read`；
`filesystem.pickSave` 要求 `filesystem.userSelected.write`；`filesystem.pickFolder`
的 `access=read/write/readWrite` 分别要求对应的一项或两项权限：

```lua
local openTask = task.start("filesystem.pickOpen", {
    extensions = { "txt", "md" },
})
local saveTask = task.start("filesystem.pickSave", {
    extensions = { "json" },
    suggestedName = "export.json",
})
local folderTask = task.start("filesystem.pickFolder", {
    access = "readWrite",
})

-- 成功：event.value = {
--   handle = "filesystem:...", kind = "file"|"folder",
--   access = "read"|"write"|"readWrite", name = "仅用于显示的名称"
-- }
```

`extensions` 最多 16 项，只接受无通配符的安全扩展名；`suggestedName` 只能是文件名，
不能传路径。句柄使用系统随机 token，保存于 Lua 普通存储之外的宿主注册表，并同时
绑定 package ID 与实例 ID；组件删除或软件包卸载时撤销。Lua 永远得不到绝对路径，
其他实例也不能解析或撤销该句柄。稳定错误包括 `permissionDenied`、
`userGestureRequired`、`userCanceled`、`pickerUnavailable`、`pickerFailed`、
`invalidSelection`、`handleQuotaExceeded`、`handlePersistenceFailed` 和 `canceled`。
预览返回固定虚拟句柄且不打开系统对话框。

`task.filesystem.access` 在同一句柄边界上公开 `stat/list/read/write/release`：

```lua
local statTask = task.start("filesystem.stat", { handle = selectedHandle })
local listTask = task.start("filesystem.list", {
    handle = selectedFolder,
    offset = 0,
    limit = 50,
})
local readTask = task.start("filesystem.read", {
    handle = selectedFile,
    encoding = "utf8",
    maxBytes = 512 * 1024,
})
local writeTask = task.start("filesystem.write", {
    handle = selectedFile,
    encoding = "utf8",
    text = nextText,
    expectedRevision = previouslyReadRevision,
})
local releaseTask = task.start("filesystem.release", {
    handle = selectedHandle,
})
```

`stat`、`list`、`read` 要求 `filesystem.userSelected.read` 和可读句柄；`write` 要求
`filesystem.userSelected.write` 和可写句柄。`release` 只撤销当前实例自己的句柄，不要求
额外权限或手势；句柄仍有任务执行时返回 `handleBusy`。`stat` 返回
`{ handle,kind,name,size?,modifiedMs,readOnly,revision }`；`list` 只枚举一层，分页范围
0–10000、每页 1–100，并把非 reparse 子项转换为同一实例的新 opaque handle；超过
10000 个可枚举子项返回 `directoryTooLarge`。`read` 当前只接受 `encoding=utf8`，调用方
上限与宿主硬上限均不超过 1 MiB，NUL 或非 UTF-8 内容返回 `invalidEncoding`。

`write` 当前是 1 MiB 内的 UTF-8 原子整文件替换，同一实例最短间隔 100 ms；传入
`expectedRevision` 时，文件不存在或 revision 已变化均返回 `conflict`。成功返回新的
`{ accepted,size,modifiedMs,revision }`。其他稳定错误包括 `invalidReference`、
`handleAccessDenied`、`notFile`、`notFolder`、`notFound`、`accessDenied`、
`reparsePointDenied`、`fileTooLarge`、`fileChanged`、`readFailed`、`writeFailed`、
`rateLimited`、`permissionRevoked` 和 `canceled`。这些任务不接受路径，也不递归遍历；
目录监听使用独立权限 `filesystem.userSelected.watch`，只接受
`filesystem.pickFolder` 返回且仍属于当前包、当前实例的 folder handle：

```lua
local changes = data.subscribe("filesystem.watch", {
    handle = selectedFolder,
    whenHidden = "pause",
})

local snapshot = changes:value()
if snapshot.available then
    for _, change in ipairs(snapshot.value.events) do
        -- kind: added / removed / modified / renamed
        -- name/oldName 仅用于显示；仍存在的子项可能带 opaque handle
    end
    if snapshot.value.overflow then
        task.start("filesystem.list", { handle = selectedFolder })
    end
end
```

监听只覆盖所选目录的一层，不递归、不跟随 reparse point，也不返回绝对路径。宿主通过
`ReadDirectoryChangesW` 按 subscription 隔离监听，并把连续通知合并到最多 256 个事件；
内核缓冲溢出时返回 `overflow=true`，调用方必须重新 `filesystem.list`，不能把事件列表
当作完整目录状态。隐藏策略固定收敛为 pause；隐藏、退订、卸载、撤权与关闭都会立即取消
监听并清空待发事件。`data.change` 会带 `topic="filesystem.watch"`、`subscriptionId`、
`revision` 和 `overflow`，同一组件监听多个目录时应按 subscription ID 区分。

`filesystem.release` 在对应目录仍被订阅时返回 `handleBusy`。稳定监听错误包括
`invalidReference`、`notFolder`、`permissionDenied`、`invalidDirectory`、
`watchOpenFailed`、`watchStartFailed`、`watchReadFailed` 和 `watchRestartFailed`。
feature 为 `data.filesystem.watch`。

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
预览不会访问系统媒体会话、音频端点、Windows 设置、真实剪贴板或系统通知，而是异步返回确定性 mock。
媒体参数表只接受上述动作对应字段；其他任务同样拒绝未知字段、错误类型和越界数值。API v1 的
`media.playPause/next/previous` 不会注册进 v2 VM，不能绕过任务的手势门禁。

### `draw`

即时绘制坐标以组件左上角为 `(0, 0)`：

- `draw.text(x, y, text, size?, color?, maxWidth?, bold?, singleLine?,
  maxHeight?, alpha?, font?)`
- `draw.measureText(text, size?, maxWidth?, bold?, font?)`
- `draw.rect(...)`、`draw.strokeRect(...)`、`draw.line(...)`、`draw.circle(...)`
- `draw.arc(cx, cy, radius, startDegrees, sweepDegrees, thickness?, color?, alpha?)`
- `draw.path(commands, options?)`
- `draw.gradientRect(x, y, width, height, startColor?, endColor?, direction?, radius?, alpha?)`
- `draw.shadow(x, y, width, height, color?, blur?, radius?, offsetX?, offsetY?, alpha?)`
- `draw.sparkline(values, x, y, width, height, color?, thickness?, min?, max?, alpha?)`
- `draw.pushClip(x, y, width, height)`、`draw.popClip()`
- `draw.fa(...)`、`draw.fluent(...)`
- `draw.image(imageHandle, x, y, width, height, alpha?)`
- `draw.imageFit(imageHandle, x, y, width, height, fit?, alignment?, alpha?, interpolation?)`
- `draw.icon(ref, x, y, size?, alpha?)`：要求 `desktop.read`，只接受当前实例由
  `app.search`、`desktop.search` 或 `everything.search` 返回且仍有效的不透明 ref；
  不接受路径、v1 项目表或其他实例的引用。

颜色是 `0xRRGGBB`，透明度单独传入。`draw.image` 在 v2 中只接受
`resource.image()` 返回的不透明句柄；字体句柄可传给 `draw.text` 和
`draw.measureText`。

上述 `arc/path/gradientRect/shadow/sparkline/imageFit` 属于可探测 feature
`draw.advanced`，仅注册到 API v2。它们遵守以下确定性边界：

- `arc` 的 `0°` 指向右侧，正角度顺时针；非零 sweep 最多一圈，宿主会拆成至多
  3 段安全圆弧。
- `path` 接受 1–256 个严格命令，首项必须是
  `{op="move", x, y}`。后续可用 `line`、`cubic`、`quadratic`、`close`；命令表
  拒绝未知字段、元表和稀疏数组。`options` 只接受 `fillColor/strokeColor/thickness/
  alpha/fillRule`，其中 `fillRule` 为 `alternate` 或 `winding`。没有指定填充或描边时
  默认使用白色描边。
- `gradientRect` 是两色线性渐变，方向为 `horizontal`、`vertical`、
  `diagonalDown` 或 `diagonalUp`；圆角不能超过短边一半。
- `imageFit` 仍只接受当前实例的图片句柄。`fit` 为 `fill/contain/cover/none`，
  `alignment` 为 `start/center/end` 并同时作用于两轴，采样为 `linear/nearest`；
  `cover` 由宿主计算源图裁切，不向 Lua 暴露资源路径或像素。
- `shadow` 的 blur 为 `0–64`，最多产生 16 层宿主受控的柔和衰减；它不是任意
  shader 或无界高斯效果。圆角不能超过短边一半，偏移和扩散后的区域仍受坐标预算约束。
- `sparkline` 接受 1–512 个有限数值。`min/max` 必须成对提供且严格递增；省略时宿主
  自动计算范围，越界样本裁到绘图区。所有宽高、坐标、线宽、颜色和透明度都经过有限值
  与上限检查，不会因一次调用创建无界工作量。

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
  "slots": {},
  "permissions": [],
  "optionalPermissions": []
}
```

`schemaVersion` 与 `apiVersion` 必须同时为 2。`requiredFeatures` 不受支持时包
无法激活；`optionalFeatures` 用于可降级能力。基础时钟、绘制、上下文和包资源
不应声明高风险权限。

## 当前明确未开放

API v2 暂未向沙箱提供完整 `view.tree`、通用即时 region 和非输入节点的键盘焦点/UIA 输出、
受控二级菜单、`desktop`、旧的同步 `media` 库、HTTP、尚未列出的系统状态、
通用剪贴板、文件选择和应用启动库。声明式输入与 `control.textInput/textArea` 只在聚焦的
宿主管理编辑器内部代理标准剪贴板操作，不允许 Lua 读取剪贴板。其余能力将在对应宿主实现、配额与按需生命周期完成后
再加入 feature 目录和 LuaLS 定义；不要根据权限词汇自行推测函数名。
