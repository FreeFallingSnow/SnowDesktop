local function setup()
    return {
        alternateColor = storage.get("previewColor") == "orange",
        cardVisible = storage.get("previewCard") ~= "hidden",
        columnLayout = storage.get("previewLayout") == "column",
        hitMode = storage.get("previewMode") == "hit",
        hitCount = 0,
        lastHit = "hit_none",
    }
end

local function event(_context, model, value)
    if value.kind ~= "action" then return end

    if value.id == "transition.update" then
        model.alternateColor = not model.alternateColor
    elseif value.id == "transition.enter" then
        model.cardVisible = true
    elseif value.id == "transition.exit" then
        model.cardVisible = false
    elseif value.id == "layout.toggle" then
        model.columnLayout = not model.columnLayout
    elseif value.id == "mode.toggle" then
        model.hitMode = not model.hitMode
        model.hitCount = 0
        model.lastHit = "hit_none"
    elseif value.id == "hit.lower" or value.id == "hit.upper" then
        model.hitCount = model.hitCount + 1
        model.lastHit = value.id == "hit.upper" and "hit_upper" or "hit_lower"
    else
        return
    end
    widget.invalidate()
end

local function command(id, label, row, enabled, width)
    return view.button({
        key = id,
        label = label,
        width = width or "fill",
        height = row,
        fontSize = row * 12 / 28,
        textAlign = "center",
        padding = { horizontal = row * 3 / 28, vertical = 0 },
        enabled = enabled,
        action = { id = id },
        style = {
            foreground = enabled and "textPrimary" or "textDisabled",
            cornerRadius = row * 6 / 28,
        },
    })
end

local function heading(row, text, modeLabel)
    return view.row({
        key = "api-test.heading",
        width = "fill",
        height = row,
        alignItems = "center",
        gap = row * 8 / 28,
        children = {
            view.text({
                key = "api-test.title",
                text = text,
                width = "fill",
                height = "fill",
                fontSize = row * 14 / 28,
                bold = true,
                verticalAlign = "center",
                style = { foreground = "textPrimary" },
            }),
            command("mode.toggle", modeLabel, row, true, row * 4),
        },
    })
end

local function hitButton(row, id, label, x, width, z, background)
    return view.button({
        key = id,
        label = label,
        width = row * width,
        height = row * 1.15,
        alignSelf = "start",
        offset = { x = row * x, y = row * 0.42 },
        zIndex = z,
        fontSize = row * 12 / 28,
        textAlign = z > 0 and "center" or "start",
        padding = { horizontal = row * 0.62, vertical = 0 },
        action = { id = id },
        style = {
            background = background,
            foreground = 0xFFFFFF,
            cornerRadius = row * 5 / 28,
        },
    })
end

local function presenceCard(options)
    options.key = "transition.presence-card"
    return view.box(options)
end

local function hitTestView(row, model)
    local lastHit = l10n.tr("lua_widget.component_api_test.hit_none")
    if model.lastHit == "hit_upper" then
        lastHit = l10n.tr("lua_widget.component_api_test.hit_upper")
    elseif model.lastHit == "hit_lower" then
        lastHit = l10n.tr("lua_widget.component_api_test.hit_lower")
    end

    return view.column({
        key = "api-test.hit",
        width = "fill",
        height = "fill",
        padding = row * 12 / 28,
        gap = row / 3,
        children = {
            heading(row, l10n.tr("lua_widget.component_api_test.hit_heading"), l10n.tr("lua_widget.component_api_test.hit_back")),
            view.row({
                key = "hit.samples",
                width = "fill",
                height = row * 2,
                gap = row / 4,
                children = {
                    view.stack({
                        key = "hit.clip",
                        width = row * 7,
                        height = "fill",
                        overflow = "clip",
                        style = { borderColor = "borderStrong", borderWidth = 1 },
                        children = {
                            hitButton(row, "hit.upper", l10n.tr("lua_widget.component_api_test.hit_upper"), 2.85, 5.2, 3, 0x14765F),
                            hitButton(row, "hit.lower", l10n.tr("lua_widget.component_api_test.hit_lower"), 0.35, 4.2, 0, 0x315B91),
                        },
                    }),
                    view.text({
                        key = "hit.outside",
                        text = l10n.tr("lua_widget.component_api_test.hit_outside"),
                        width = "fill",
                        height = "fill",
                        fontSize = row * 11 / 28,
                        verticalAlign = "center",
                        style = { foreground = "textSecondary" },
                    }),
                },
            }),
            -- Keep the green card key mounted across modes so there is no exit overlay here.
            presenceCard({ visibility = "collapsed" }),
            view.text({
                key = "hit.result",
                text = string.format(l10n.tr("lua_widget.component_api_test.hit_result"), lastHit, model.hitCount),
                width = "fill",
                height = row,
                fontSize = row * 13 / 28,
                verticalAlign = "center",
                style = { foreground = "textPrimary" },
            }),
        },
    })
end

local function sample(key, title, row, card)
    return view.column({
        key = key,
        width = "fill",
        height = "fill",
        gap = row * 4 / 28,
        transition = { durationMs = 450, easing = "easeInOut", properties = { "layout" } },
        children = {
            view.text({
                key = key .. ".label",
                text = title,
                width = "fill",
                height = row * 0.62,
                fontSize = row * 10 / 28,
                verticalAlign = "center",
                style = { foreground = "textSecondary" },
            }),
            card,
        },
    })
end

local function buildView(_context, model)
    local row = ui.metrics().layoutRowHeight
    if model.hitMode then return hitTestView(row, model) end

    local radius = row * 6 / 28
    local gap = row * 8 / 28
    local duration = 850
    local styleCard = view.box({
        key = "transition.style-card",
        width = "fill",
        height = "fill",
        style = {
            background = model.alternateColor and 0xE6813B or 0x317CE2,
            cornerRadius = radius,
        },
        transition = { durationMs = duration, easing = "easeInOut", properties = { "background" } },
    })
    local presenceChildren = {}
    if model.cardVisible then
        presenceChildren[1] = presenceCard({
            width = "fill",
            height = "fill",
            style = { background = 0x1F9E81, cornerRadius = radius },
            enterTransition = { durationMs = duration, easing = "easeOut", opacity = 0 },
            exitTransition = { durationMs = 1900, easing = "easeIn", opacity = 0 },
        })
    end
    local presenceSlot = view.box({
        key = "transition.presence-slot",
        width = "fill",
        height = "fill",
        style = {
            borderColor = "border",
            borderWidth = row / 28,
            cornerRadius = radius,
        },
        children = presenceChildren,
    })

    return view.column({
        key = "api-test",
        width = "fill",
        height = "fill",
        padding = row * 12 / 28,
        gap = gap,
        children = {
            heading(row, l10n.tr("lua_widget.component_api_test.heading"), l10n.tr("lua_widget.component_api_test.hit_open")),
            view.row({
                key = "transition.samples",
                flexDirection = model.columnLayout and "column" or "row",
                width = "fill",
                height = "fill",
                gap = gap,
                children = {
                    sample("transition.style", l10n.tr("lua_widget.component_api_test.style_sample"),
                        row, styleCard),
                    sample("transition.presence", l10n.tr("lua_widget.component_api_test.presence_sample"),
                        row, presenceSlot),
                },
            }),
            view.row({
                key = "transition.commands",
                width = "fill",
                height = row,
                flexShrink = 0,
                gap = gap,
                children = {
                    command("transition.update", l10n.tr("lua_widget.component_api_test.update"), row, true),
                    command("transition.enter", l10n.tr("lua_widget.component_api_test.enter"), row, not model.cardVisible),
                    command("transition.exit", l10n.tr("lua_widget.component_api_test.exit"), row, model.cardVisible),
                    command("layout.toggle", l10n.tr("lua_widget.component_api_test.layout"), row, true),
                },
            }),
        },
    })
end

return widget.define({
    name = l10n.tr("lua_widget.component_api_test.name"),
    setup = setup,
    event = event,
    view = buildView,
})
