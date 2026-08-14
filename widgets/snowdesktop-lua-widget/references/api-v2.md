# SnowDesktop Lua 组件 API v2

本文档描述当前宿主已经实现并放入 API v2 沙箱的接口。清单中保留的权限名、
路线图能力或 API v1 全局库不代表 v2 组件已经可以调用它们。可在运行时使用
`widget.apiInfo()`、`widget.hasFeature(id)` 和 `system.capabilities()` 探测能力。

编辑器类型定义位于 `library/snowdesktop-v2.lua`，其函数签名是本文档的配套
机器可读契约。

## 入口契约

`main.lua` 必须返回 `widget.define({...})` 的结果，并且当前版本必须提供
`render`：

```lua
local function render()
    draw.text(layout.cu(12), layout.cu(12), "Hello")
end

return widget.define({
    name = l10n.tr("lua_widget.example.name"),
    render = render,
})
```

`view`、`setup`、`event`、`dispose` 和 `menu` 是后续声明式视图与生命周期契约
预留项，当前宿主会拒绝使用。不要把 API v1 的全局回调迁入 v2 描述符。

## 已实现能力

### `widget`

- `widget.define(definition)`：校验并返回 v2 描述符。当前必需 `render`。
- `widget.apiInfo()`：返回当前 API 版本、支持版本和 feature ID。
- `widget.hasFeature(id)`：探测 feature。
- `widget.context()`：返回逻辑/像素尺寸、DPI、网格跨度、显示器范围、主题、
  辅助功能、语言区域、时区、可见/预览/选择状态和 surface。
- `widget.info()`、`widget.theme()`：兼容的实例与外观快照。
- `widget.hasPermission(name)`：查询当前实例已授予权限。
- `widget.setTitle(text)`、`widget.invalidate()`、`widget.log(level, text)`。
- `widget.setTimer(name, milliseconds, repeat)`、`widget.cancelTimer(name)`。
- `widget.openSettings()`、`widget.openPanel(options)`、`widget.closePanel()`。
- `widget.editText(...)`：旧宿主编辑器兼容调用，不建议新 v2 组件依赖。

定时器会触发宿主已有的 `onTimer` 调用路径，但 v2 生命周期描述符尚未定稿；
新模板不依赖定时器回调，直到统一的 `setup/event/dispose` 契约开放。

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
  "requiredFeatures": ["draw.immediate", "l10n.basic"],
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

API v2 暂未向沙箱提供声明式 `view` 控件树、元素事件/hover/独立右键菜单、
`desktop`、`media`、HTTP、系统性能、网络状态、音频分析、剪贴板、文件选择和
应用启动库。它们将在对应宿主实现、配额与按需生命周期完成后再加入 feature
目录和 LuaLS 定义；不要根据权限词汇自行推测函数名。
