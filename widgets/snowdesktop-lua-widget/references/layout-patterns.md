# Information layout patterns

Use these structures for lists, searches and management tools after selecting
the model in [layout-system.md](layout-system.md). They are view-building
functions, not a complete package or a host-provided helper library. Use
`informationMetrics` from that reference and read it inside each surface callback.
Check the installed `view-contract` for the required features of each selected
node; require `ui.semanticMetrics.rowUnit` and semantic foreground tokens.

Visible interface copy belongs in literal `l10n.tr` calls at the caller.
`item.label` represents real item data. Wire the emitted `event.id`/`event.value`
to the package's event handler; keys identify the item, not its display index.

## Selectable row with a trailing command

Use an explicit text inset inside the selectable control. A fixed trailing
command keeps its own width; the name ellipsizes in the remaining space.

```lua
local function itemRow(item, commandLabel, m, width)
    local key = "item." .. item.id
    local controls = {
        view.button({
            key = key .. ".open", label = item.label,
            width = "fill", minWidth = 0, height = m.row,
            fontSize = m.body, textAlign = "start",
            padding = { horizontal = m.textInset, vertical = 0 },
            style = { foreground = "textPrimary", borderColor = "border",
                borderWidth = m.stroke, cornerRadius = m.radius },
            events = { click = { id = "item.open", value = item.id } },
        }),
        view.button({
            key = key .. ".command", label = commandLabel,
            width = m.row * 3, height = m.row, flexShrink = 0,
            fontSize = m.body, textAlign = "center",
            padding = { horizontal = m.textInset, vertical = 0 },
            style = { foreground = "textPrimary", borderColor = "border",
                borderWidth = m.stroke, cornerRadius = m.radius },
            events = { click = { id = "item.command", value = item.id } },
        }),
    }
    -- Reserve two row units of width for a useful name before keeping commands inline.
    if width < m.row * 5 + m.gapSmall then
        controls[2].width = "fill"
        local height = m.row * 2 + m.gapSmall
        return view.column({
            key = key, width = "fill", height = height, flexShrink = 0,
            gap = m.gapSmall, children = controls,
        }), height
    end
    return view.row({
        key = key, width = "fill", height = m.row, flexShrink = 0,
        gap = m.gapSmall, children = controls,
    }), m.row
end
```

The command width and name budget are examples for a short localized verb,
not universal button widths. Measure the actual labels and adjust those budgets;
the narrow branch places the command below the name. A glyph command can use a
square row box only after its meaning, font mapping and accessible label are verified.

## List body and one outer scroll owner

The body below intentionally has no outer inset and owns no scrolling, so it
can also be composed into a form. Empty content still has a positive line budget.

```lua
local function listBody(items, commandLabel, emptyLabel, m, width)
    local rows = {}
    local minimumItemHeight = m.row
    for _, item in ipairs(items) do
        local node, height = itemRow(item, commandLabel, m, width)
        rows[#rows + 1] = node
        minimumItemHeight = math.max(minimumItemHeight, height)
    end
    if #rows == 0 then
        minimumItemHeight = m.row * 2
        rows[1] = view.text({
            key = "items.empty", text = emptyLabel, width = "fill",
            height = m.row * 2, flexShrink = 0, fontSize = m.body,
            textWrap = "wrap", maxLines = 2, verticalAlign = "center",
            style = { foreground = "textSecondary" },
        })
    end
    return view.column({
        key = "items.body", width = "fill", height = "auto",
        gap = m.gapSmall, children = rows,
    }), minimumItemHeight
end

local function scrollList(body, m)
    return view.scroll({
        key = "items.viewport", width = "fill", height = "fill",
        children = {
            view.column({
                key = "items.inset", width = "fill", height = "auto",
                padding = m.inset, children = { body },
            }),
        },
    })
end
```

Use `scrollList` only when its allocated bounds remain positive after the child
inset. For an extremely narrow host-constrained surface, reduce the content
inset from actual width as in the management pattern below. For large collections,
select the documented virtual-list contract instead of emitting an unbounded
node tree; a virtual list owns its viewport and replaces this outer scroll.

## Management form with constrained-height fallback

This pattern has a fixed header, one body viewport and a fixed footer when room
allows. `headerHeight` and `footerHeight` are the actual budgets of the supplied
nodes, including any internal gaps. All three nodes use `width="fill"`; the body
is auto-height and does not contain its own scrolling region.
`minimumBodyHeight` comes from `listBody`, including a reflowed or empty item;
for a settings form, pass the height of its smallest useful complete field group.

```lua
local function managementPanel(key, width, height, m,
        header, headerHeight, body, footer, footerHeight, minimumBodyHeight)
    local inset = math.min(m.inset, width / 4)
    local fixedHeight = headerHeight + footerHeight
        + inset * 2 + m.gap * 2
    local usefulBodyHeight = minimumBodyHeight + m.gapSmall * 2

    if height - fixedHeight < usefulBodyHeight then
        return view.scroll({
            key = key .. ".all", width = "fill", height = "fill",
            children = {
                view.column({
                    key = key .. ".content", width = "fill", height = "auto",
                    padding = inset, gap = m.gap,
                    children = { header, body, footer },
                }),
            },
        })
    end

    local function fixedRegion(suffix, node, regionHeight)
        return view.column({
            key = key .. suffix, width = "fill", height = regionHeight,
            flexShrink = 0, padding = { horizontal = inset, vertical = 0 },
            children = { node },
        })
    end
    return view.column({
        key = key, width = "fill", height = "fill",
        padding = { vertical = inset, horizontal = 0 }, gap = m.gap,
        children = {
            fixedRegion(".header", header, headerHeight),
            view.scroll({
                key = key .. ".body", width = "fill", height = "fill",
                children = {
                    view.column({
                        key = key .. ".body.inset", width = "fill", height = "auto",
                        padding = { horizontal = inset, vertical = m.gapSmall },
                        children = { body },
                    }),
                },
            }),
            fixedRegion(".footer", footer, footerHeight),
        },
    })
end
```

Read actual width/height in the panel callback. The request passed to
`widget.openPanel` is not a substitute. The fallback changes the scroll owner;
it preserves normal control sizes and keeps the footer reachable by scrolling.
Do not retain both body and whole-form scrolling for the same axis.

## Search or settings form composition

The search is a two-row header (visible label + input); the footer is one row.
Each node declares its budget instead of estimating header height from the
number of children. A settings form can supply labeled field rows as `body`
using the same management structure.

```lua
local function searchPanel(context, model)
    local m = informationMetrics("popup")
    local headerHeight = m.row * 2 + m.gapSmall
    local header = view.column({
        key = "search.header.content", width = "fill", height = headerHeight,
        flexShrink = 0, gap = m.gapSmall,
        children = {
            view.text({
                key = "search.label", text = l10n.tr("example.search.label"),
                width = "fill", height = m.row, flexShrink = 0,
                fontSize = m.heading, verticalAlign = "center",
                style = { foreground = "textPrimary" },
            }),
            view.searchBox({
                key = "search.query", value = model.query,
                width = "fill", height = m.row, flexShrink = 0,
                fontSize = m.body, maxBytes = 256,
                padding = { horizontal = m.textInset, vertical = 0 },
                style = { foreground = "textPrimary", borderColor = "border",
                    borderWidth = m.stroke, cornerRadius = m.radius },
                accessibility = { label = l10n.tr("example.search.label") },
                events = { change = { id = "query.change" }, submit = { id = "query.submit" } },
            }),
        },
    })
    local inset = math.min(m.inset, context.layoutSize.width / 4)
    local body, minimumBodyHeight = listBody(model.items,
        l10n.tr("example.item.command"), l10n.tr("example.items.empty"), m,
        context.layoutSize.width - inset * 2)
    local footer = view.button({
        key = "search.done", label = l10n.tr("example.done"),
        width = "fill", height = m.row, flexShrink = 0,
        fontSize = m.body, textAlign = "center",
        padding = { horizontal = m.textInset, vertical = 0 },
        style = { foreground = "textPrimary", borderColor = "border",
            borderWidth = m.stroke, cornerRadius = m.radius },
        events = { click = { id = "panel.done" } },
    })
    return managementPanel("search.panel",
        context.layoutSize.width, context.layoutSize.height, m,
        header, headerHeight, body, footer, m.row, minimumBodyHeight)
end
```

Replace the example localization keys with the package's literal keys and
provide every declared locale. Populate real or isolated preview data, handle
query/loading/error states that apply, and route the commands through `event`.
When a title must wrap, increase its row budget and `headerHeight` together.
For a read-only desktop list, call `informationMetrics("desktop")` and compose
`scrollList(listBody(...), metrics)` with the body's actual inset width instead
of carrying popup density into the desktop. Verify the selected structure using
the acceptance cases in
[layout-system.md](layout-system.md#layout-acceptance).
