# SnowDesktop Lua Widget API

## Contents

- [Runtime model](#runtime-model)
- [Callbacks](#callbacks)
- [Drawing](#drawing)
- [Widget and system](#widget-and-system)
- [Storage](#storage)
- [Desktop integration](#desktop-integration)
- [Settings UI](#settings-ui)
- [Manifest and permissions](#manifest-and-permissions)
- [Troubleshooting](#troubleshooting)

## Runtime model

Scripts run in a sandbox containing:

- Base functions: `assert`, `error`, `ipairs`, `next`, `pairs`, `pcall`, `select`, `tonumber`, `tostring`, `type`, `xpcall`.
- Libraries: `string`, `table`, `math`, `utf8`.
- Host APIs: `draw`, `sys`, `layout`, `storage`, `widget`, `desktop`.
- `imgui` only when the manifest declares `ui.input`.
- `widgetId`, a unique string for the current component instance.

Coordinates passed to drawing and mouse callbacks are local to the component. The origin is its upper-left corner.

The host checks the script timestamp and hot-reloads it while rendering. Persistent storage is scoped by component instance ID, so two instances of the same script keep separate values.

## Callbacks

All callbacks are optional except `render()` for visible output.

```lua
function render() end
function imguiRender() end
function onOpen() end
function onClick(x, y, button, delta) end
function onDoubleClick(x, y, button, delta) end
function onMouseDown(x, y, button, delta) end
function onMouseMove(x, y, button, delta) end
function onMouseUp(x, y, button, delta) end
function onWheel(x, y, button, delta) end
function onDesktopChanged(reason) end
function getContextMenu() return {} end
function onMenu(id) end
```

- Mouse callbacks receive four arguments even if the script only declares `x, y`.
- For wheel handling, use the sign of `delta`.
- `onDesktopChanged(reason)` requires `desktop.read`.
- `imguiRender()` requires `ui.input`.
- Context-menu callbacks require `ui.contextMenu`.

Menu example:

```lua
function getContextMenu()
    return {
        { id = 1, label = "执行操作", icon = "" },
        { separator = true },
        { id = 2, label = "不可用项", icon = "", enabled = false }
    }
end

function onMenu(id)
    if id == 1 then widget.log("info", "menu action") end
end
```

Menu item fields:

- `id`: integer passed to `onMenu(id)`.
- `label`: displayed text.
- `icon`: optional Font Awesome 6 Free Solid glyph rendered by the host menu.
- `enabled`: optional boolean; defaults to `true`.
- `separator`: set to `true` for a separator and omit the other fields.

The built-in Lua widget settings command is labeled **详细设置** and opens
`imguiRender()`.
Use the debug page's **Font Awesome 图标字符** grid to preview the embedded font
and click-copy a glyph for the `icon` field. To unlock **调试**, open
**设置 → 关于** and click the version number five times.

## Drawing

Colors use `0xRRGGBB`.

### `draw.text`

```lua
draw.text(x, y, text, size?, color?, maxWidth?, bold?, singleLine?)
```

- Defaults: `size=14`, `color=0xFFFFFF`, `maxWidth=0`.
- `maxWidth > 0` enables wrapping.
- `singleLine=true` disables wrapping and adds character ellipsis trimming.

### `draw.measureText`

```lua
local metrics = draw.measureText(text, size?, maxWidth?, bold?)
-- metrics.width, metrics.height
```

Defaults match `draw.text`. Use this before drawing centered or fitted text.

### Shapes

```lua
draw.rect(x, y, width, height, color?, radius?, alpha?)
draw.strokeRect(x, y, width, height, color?, radius?, thickness?, alpha?)
draw.line(x1, y1, x2, y2, thickness?, color?, alpha?)
draw.circle(centerX, centerY, radius, color?, alpha?)
```

Defaults:

- Color: `0xFFFFFF`.
- Alpha: `1.0`.
- Radius: `0`.
- Thickness: `1.0`.

`draw.circle` draws a filled circle.

### Images and shell icons

```lua
draw.image(relativePath, x, y, width, height, alpha?)
draw.icon(pathOrDesktopItem, x, y, size?, alpha?)
```

- `draw.image` accepts only a path relative to the root executable `widgets` directory.
- Supported image decoding is provided by Windows Imaging Component.
- `draw.icon` resolves a Windows shell icon and requires `desktop.read`.
- `pathOrDesktopItem` may be a path string or an item table returned by `desktop`.

## Widget and system

```lua
local info = widget.info()
-- info.id, info.width, info.height

widget.setTitle("新标题")
widget.invalidate()
widget.log("info", "message")

local theme = widget.theme()
-- theme.bg, theme.border, theme.alpha, theme.gradientEndA

widget.editText(key, x, y, width, height, multiline,
    initialText?, selectAll?, textColor?)
```

`widget.editText` opens a host edit control and saves the result to `storage` under `key`. Defaults:

- `initialText`: current stored value.
- `selectAll`: `true`.
- `textColor`: `0x000000`.

Time:

```lua
local t = sys.getTime()
-- year, month, day, wday (1=Sunday), hour, min, sec
```

Notification:

```lua
sys.notify(title, message)
```

Shows a system tray balloon notification. Both arguments are required strings.

Layout:

```lua
local width = layout.width()
local height = layout.height()
```

## Storage

```lua
local value = storage.get("key") -- string or nil
storage.set("key", tostring(value))
storage.remove("key")
local keys = storage.keys()
```

All values are strings. Each `storage.set` and `storage.remove` persists immediately, so call them on user actions or actual changes rather than every render.

Common conversions:

```lua
local count = tonumber(storage.get("count")) or 0
local enabled = storage.get("enabled") ~= "0"
storage.set("enabled", enabled and "1" or "0")
```

## Desktop integration

Reading requires `desktop.read`:

```lua
local all = desktop.items()
local selected = desktop.selection()
local matches = desktop.find("query")
```

Each item contains:

```lua
{
    id = "...",
    title = "...",
    path = "...",
    source = "...",
    type = "...",
    selected = false
}
```

Actions require `desktop.action`:

```lua
local opened = desktop.open(itemOrPath)
local revealed = desktop.reveal(itemOrPath)
desktop.refresh()
```

`desktop.open` and `desktop.reveal` return booleans.

## Settings UI

Declare `ui.input`, then define `imguiRender()`.

```lua
imgui.text(text)
imgui.textWrapped(text)
imgui.separator()
imgui.sameLine(offset?, spacing?)
imgui.spacing()

local open = imgui.collapsingHeader(label)
local treeOpen = imgui.treeNode(label)
imgui.treePop()

local clicked = imgui.button(label)
local text = imgui.input(label, currentText)       -- multiline
local text = imgui.inputText(label, currentText)   -- single line
local checked = imgui.checkbox(label, checked)
local color = imgui.colorEdit3(label, color)
local number = imgui.sliderFloat(label, value, min, max)
local integer = imgui.sliderInt(label, value, min, max)
local index = imgui.combo(label, oneBasedIndex, { "A", "B" })
local clicked = imgui.selectable(label, selected)
local clicked = imgui.radio(label, active)
imgui.beginDisabled(disabled)
imgui.endDisabled()
```

Controls return the new/current value. Persist a value only when it differs from the previous value.

## Manifest and permissions

The manifest filename is derived by replacing `.lua` with `.widget.json`.

```json
{
  "name": "示例组件",
  "version": "1.0.0",
  "description": "示例说明。",
  "defaultSize": { "columns": 2, "rows": 1 },
  "minSize": { "columns": 2, "rows": 1 },
  "maxSize": { "columns": 4, "rows": 3 },
  "permissions": ["ui.input"]
}
```

`minSize` and `maxSize` are optional grid-span constraints. If omitted, the
component can use any valid grid span from `1 x 1` up to the current page size.
A `maxSize` dimension of `0` is also treated as unrestricted. The host adjusts
`defaultSize` into the declared range and enforces the limits while resizing,
restoring saved layouts, and reacting to grid changes.

| Permission | Required for |
|---|---|
| `ui.input` | `imgui`, `imguiRender()` |
| `ui.contextMenu` | `getContextMenu()`, `onMenu(id)` |
| `desktop.read` | `desktop.items`, `selection`, `find`, `draw.icon`, desktop-change callback |
| `desktop.action` | `desktop.open`, `reveal`, `refresh` |

Missing permissions produce a runtime error for guarded APIs. Context menus and desktop-change callbacks are skipped by the host when their permission is absent.

## Troubleshooting

### Component is absent from **添加组件**

- Put the `.lua` file directly in the executable's `widgets` directory.
- Do not put runnable scripts in a subdirectory.
- Rebuild or run the widget sync script after editing the source repository's `widgets` folder.
- Confirm the extension is exactly `.lua`.

### Manifest is ignored

- Match `example.lua` with `example.widget.json`.
- Validate JSON syntax.
- Keep `defaultSize` values between 1 and 8.
- Ensure `defaultSize` is compatible with optional `minSize` and `maxSize`.

### `imgui` is nil

Add `"ui.input"` to the manifest permissions.

### Permission denied

Add the exact permission required by the API and reload the widget.

### Widget shows a red error panel

- Inspect syntax and argument types.
- Use `widget.log` before the failing branch.
- Save the correction, then refresh/re-add the widget if it was marked invalid.

### Image does not render

- Use a relative path under the root `widgets` directory.
- Do not pass an absolute path.
- Verify Windows Imaging Component supports the file.

### Settings or render stutters

- Remove unconditional `storage.set` calls from `render()`.
- Cache or limit desktop queries where possible.
- Avoid loading many shell icons every frame.
