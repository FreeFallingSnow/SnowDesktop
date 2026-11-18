local function setup()
    return {
        alternateColor = storage.get("previewColor") == "orange",
        cardVisible = storage.get("previewCard") ~= "hidden",
        columnLayout = storage.get("previewLayout") == "column",
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
    else
        return
    end
    widget.invalidate()
end

local function command(id, label, row, enabled)
    return view.button({
        key = id,
        label = label,
        width = "fill",
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
        presenceChildren[1] = view.box({
            key = "transition.presence-card",
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
            view.text({
                key = "transition.heading",
                text = l10n.tr("lua_widget.component_api_test.heading"),
                width = "fill",
                height = row * 0.78,
                fontSize = row * 14 / 28,
                bold = true,
                verticalAlign = "center",
                style = { foreground = "textPrimary" },
            }),
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
