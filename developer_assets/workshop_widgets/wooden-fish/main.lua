local woodenFishImage = resource.image("wooden-fish")
local malletImage = resource.image("mallet")

local STRIKE_KEY = "wooden-fish.strike"
local MAX_COUNT = 999999999

local palettes = {
    lightForeground = {
        primary = 0xFFF7ED,
        secondary = 0xFED7AA,
        focus = 0xFDE68A,
        feedback = 0xFCD34D,
        glow = 0xF59E0B,
    },
    darkForeground = {
        primary = 0x3F210C,
        secondary = 0x78350F,
        focus = 0x92400E,
        feedback = 0xB45309,
        glow = 0xD97706,
    },
}

local function foregroundPalette()
    local theme = widget.theme()
    if theme and theme.contentTheme == 1 then
        return palettes.darkForeground
    end
    return palettes.lightForeground
end

local function normalizedCount(value)
    return math.max(0, math.min(MAX_COUNT,
        math.floor(tonumber(value) or 0)))
end

local function persistCount(model)
    local persisted = normalizedCount(storage.get("count"))
    if persisted ~= model.count then
        storage.set("count", model.count)
    end
end

local function setup()
    return {
        count = normalizedCount(storage.get("count")),
        feedback = false,
    }
end

local function centeredText(text, y, size, color, width, bold, alpha)
    local metrics = draw.measureText(text, size, width, bold)
    local x = (layout.contentWidth() - metrics.width) / 2
    draw.text(x, y, text, size, color, width, bold, true,
        nil, alpha or 1.0)
end

local function drawInstrument(cx, cy, size, pressed, hovered, focused,
    feedback, colors)
    local bodyDrop = pressed and layout.cu(2.5) or 0
    local bodyY = cy - size / 2 + bodyDrop

    if hovered or feedback then
        draw.circle(cx, cy + bodyDrop, size * 0.48, colors.glow,
            feedback and 0.15 or 0.08)
    end

    draw.shadow(cx - size * 0.38, cy + size * 0.28 + bodyDrop,
        size * 0.76, size * 0.18, 0x241006, layout.cu(9),
        size * 0.09, 0, layout.cu(3), 0.19)
    draw.imageFit(woodenFishImage, cx - size / 2, bodyY, size, size,
        "contain", "center", pressed and 0.97 or 1.0, "linear")

    local malletSize = size * 0.64
    local malletDrop = pressed and layout.cu(7) or 0
    local malletX = cx - size * 0.06
    local malletY = cy - size * 0.61 + malletDrop
    draw.imageFit(malletImage, malletX, malletY,
        malletSize, malletSize, "contain", "center", 1.0, "linear")

    if focused then
        draw.arc(cx, cy + bodyDrop, size * 0.50, 0, 359.5,
            math.max(1, layout.cu(2)), colors.focus, 0.92)
    end

    return cy + size * 0.51 + bodyDrop
end

local function render(_context, model)
    local colors = foregroundPalette()
    local width = math.max(1, layout.contentWidth())
    local height = math.max(1, layout.contentHeight())
    local pressed = interaction.isPressed(STRIKE_KEY)
    local hovered = interaction.isHovered(STRIKE_KEY)
    local focused = interaction.isFocused(STRIKE_KEY)
    local padding = layout.cu(10)
    local counterSize = math.max(layout.fontCu(15),
        math.min(layout.fontCu(24), layout.vmin(8.6)))
    local hintSize = math.max(layout.fontCu(10.5),
        math.min(layout.fontCu(13), layout.vmin(4.6)))
    local countText = l10n.tr("lua_widget.wooden_fish.counter",
        l10n.formatNumber(model.count))
    centeredText(countText, padding, counterSize, colors.primary,
        width - padding * 2, true)

    local instrumentSize = math.max(layout.cu(104),
        math.min(width * 0.72, height * 0.53))
    local instrumentCy = height * 0.56
    local contentBottom = drawInstrument(width / 2, instrumentCy,
        instrumentSize, pressed, hovered, focused, model.feedback, colors)

    if model.feedback then
        local feedbackSize = math.max(layout.fontCu(10.5),
            math.min(layout.fontCu(13.5), layout.vmin(4.8)))
        centeredText(l10n.tr("lua_widget.wooden_fish.feedback"),
            math.min(contentBottom, height - padding - feedbackSize * 1.35),
            feedbackSize, colors.feedback, width - padding * 2,
            true, 0.96)
    else
        centeredText(l10n.tr("lua_widget.wooden_fish.hint"),
            height - padding - hintSize * 1.35, hintSize,
            colors.secondary, width - padding * 2, false, 0.92)
    end

    interaction.region({
        key = STRIKE_KEY,
        shape = {
            type = "circle",
            x = width / 2,
            y = instrumentCy,
            radius = math.max(layout.cu(44), instrumentSize * 0.49),
        },
        cursor = "hand",
        focusable = true,
        tabIndex = 0,
        tooltip = l10n.tr("lua_widget.wooden_fish.tooltip"),
        events = {
            click = { id = "wooden-fish.strike" },
            contextMenu = {
                id = "wooden-fish.menu",
                scope = "component",
            },
        },
        accessibility = {
            role = "button",
            label = l10n.tr("lua_widget.wooden_fish.strike",
                l10n.formatNumber(model.count)),
        },
    })
end

local function event(_context, model, value)
    if value.kind == "action" and value.id == "wooden-fish.strike" then
        if model.count < MAX_COUNT then
            model.count = model.count + 1
        end
        model.feedback = true
        schedule.after("wooden-fish.feedback", 650, {
            whenHidden = "pause",
        })
        schedule.after("wooden-fish.persist", 350, {
            whenHidden = "continue",
        })
        widget.invalidate()
        return
    end

    if value.kind == "action" and value.id == "wooden-fish.reset" then
        model.count = 0
        model.feedback = false
        schedule.cancel("wooden-fish.feedback")
        schedule.cancel("wooden-fish.persist")
        persistCount(model)
        widget.invalidate()
        return
    end

    if value.kind == "schedule" then
        if value.id == "wooden-fish.feedback" then
            model.feedback = false
        elseif value.id == "wooden-fish.persist" then
            persistCount(model)
        end
    end
end

local function menu(_context, _model, request)
    if request.id ~= "wooden-fish.menu" then return nil end
    return ui.menu({
        {
            id = "wooden-fish.reset",
            label = l10n.tr("lua_widget.wooden_fish.reset"),
        },
    })
end

local function dispose(_context, model)
    persistCount(model)
end

return widget.define({
    name = l10n.tr("lua_widget.wooden_fish.name"),
    useCustomStyle = true,
    followPersonalizationDefault = true,
    bg = 0x3A1D12,
    border = 0xF6C98F,
    alpha = 0.44,
    borderAlpha = 0.22,
    gradientEndA = 0.28,
    setup = setup,
    render = render,
    event = event,
    menu = menu,
    dispose = dispose,
})
