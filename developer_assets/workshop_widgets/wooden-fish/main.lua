local woodenFishImage = resource.image("wooden-fish")
local malletImage = resource.image("mallet")

local STRIKE_KEY = "wooden-fish.strike"
local FEEDBACK_FRAME = "wooden-fish.feedback-frame"
local FEEDBACK_FALLBACK = "wooden-fish.feedback-fallback"
local FEEDBACK_DURATION_MS = 900
local MALLET_STRIKE_DURATION_MS = 240
local MALLET_STRIKE_ANGLE = -22
local MAX_COUNT = 999999999

local palettes = {
    lightForeground = {
        primary = 0xFFF7ED,
        secondary = 0xFED7AA,
        focus = 0xFDE68A,
        feedback = 0xFCD34D,
    },
    darkForeground = {
        primary = 0x3F210C,
        secondary = 0x78350F,
        focus = 0x92400E,
        feedback = 0xB45309,
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
        feedbackActive = false,
        feedbackAnimated = false,
        feedbackElapsedMs = 0,
    }
end

local function centeredText(text, y, size, color, width, bold, alpha)
    local metrics = draw.measureText(text, size, width, bold)
    local x = (layout.contentWidth() - metrics.width) / 2
    draw.text(x, y, text, size, color, width, bold, true,
        nil, alpha or 1.0)
end

local function malletRotation(model, pressed)
    if pressed then return MALLET_STRIKE_ANGLE end
    if not model.feedbackActive or not model.feedbackAnimated then return 0 end

    local progress = math.max(0, math.min(1,
        model.feedbackElapsedMs / MALLET_STRIKE_DURATION_MS))
    local eased = progress * progress * (3 - 2 * progress)
    return MALLET_STRIKE_ANGLE * (1 - eased)
end

local function drawInstrument(cx, cy, size, pressed, model)
    local bodyDrop = pressed and layout.cu(1.5) or 0
    local bodyY = cy - size / 2 + bodyDrop

    draw.shadow(cx - size * 0.38, cy + size * 0.28 + bodyDrop,
        size * 0.76, size * 0.18, 0x241006, layout.cu(9),
        size * 0.09, 0, layout.cu(3), 0.19)
    draw.imageFit(woodenFishImage, cx - size / 2, bodyY, size, size,
        "contain", "center", pressed and 0.97 or 1.0, "linear")

    -- Lift the mallet head above the striking surface at rest, then pivot the
    -- mallet around the far end of its handle so the head follows a strike arc.
    local malletSize = size * 0.56
    local malletX = cx + size * 0.02
    local malletY = cy - size * 0.92
    draw.imageFit(malletImage, malletX, malletY,
        malletSize, malletSize, "contain", "center", 1.0, "linear",
        malletRotation(model, pressed), 0.90, 0.08)
end

local function drawFeedback(model, y, baseSize, colors, width)
    if not model.feedbackActive then return end

    local progress = math.max(0, math.min(1,
        model.feedbackElapsedMs / FEEDBACK_DURATION_MS))
    local popProgress = math.min(1, progress / 0.34)
    local popScale = 1 + math.sin(popProgress * math.pi) * 0.20
    local rise = layout.cu(20) * (1 - (1 - progress) * (1 - progress))
    local alpha = math.max(0, 1 - progress * progress)
    centeredText(l10n.tr("lua_widget.wooden_fish.feedback"),
        y - rise, baseSize * popScale, colors.feedback,
        width, true, alpha)
end

local function startFeedback(model)
    model.feedbackActive = true
    model.feedbackAnimated = false
    model.feedbackElapsedMs = 0
    schedule.cancel(FEEDBACK_FALLBACK)
    local accepted = animation.requestFrame(FEEDBACK_FRAME)
    model.feedbackAnimated = accepted
    if not accepted then
        schedule.after(FEEDBACK_FALLBACK, 700, {
            whenHidden = "pause",
        })
    end
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
    local hintSize = math.max(layout.fontCu(12.5),
        math.min(layout.fontCu(15.5), layout.vmin(5.4)))
    local countText = l10n.tr("lua_widget.wooden_fish.counter",
        l10n.formatNumber(model.count))
    centeredText(countText, padding, counterSize, colors.primary,
        width - padding * 2, true)

    local instrumentSize = math.max(layout.cu(104),
        math.min(width * 0.72, height * 0.53))
    local instrumentCy = height * 0.56
    drawInstrument(width / 2, instrumentCy, instrumentSize, pressed, model)

    local feedbackSize = math.max(layout.fontCu(12),
        math.min(layout.fontCu(15), layout.vmin(5.1)))
    drawFeedback(model, instrumentCy - instrumentSize * 0.62,
        feedbackSize, colors, width - padding * 2)

    centeredText(l10n.tr("lua_widget.wooden_fish.hint"),
        height - padding - hintSize * 1.35, hintSize,
        (hovered or focused) and colors.focus or colors.secondary,
        width - padding * 2, false, 0.96)

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
            doubleClick = { id = "wooden-fish.strike" },
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
        startFeedback(model)
        schedule.after("wooden-fish.persist", 350, {
            whenHidden = "continue",
        })
        widget.invalidate()
        return
    end

    if value.kind == "action" and value.id == "wooden-fish.reset" then
        model.count = 0
        model.feedbackActive = false
        model.feedbackAnimated = false
        model.feedbackElapsedMs = 0
        animation.cancelFrame(FEEDBACK_FRAME)
        schedule.cancel(FEEDBACK_FALLBACK)
        schedule.cancel("wooden-fish.persist")
        persistCount(model)
        widget.invalidate()
        return
    end

    if value.kind == "frame" and value.id == FEEDBACK_FRAME then
        model.feedbackElapsedMs = math.min(FEEDBACK_DURATION_MS,
            model.feedbackElapsedMs + math.max(0, value.deltaMs or 0))
        if model.feedbackElapsedMs < FEEDBACK_DURATION_MS then
            animation.requestFrame(FEEDBACK_FRAME)
        else
            model.feedbackActive = false
            model.feedbackAnimated = false
            -- Reset the host's per-ID frame clock so the next strike starts
            -- from delta 0 instead of inheriting an old capped interval.
            animation.cancelFrame(FEEDBACK_FRAME)
        end
        widget.invalidate()
        return
    end

    if value.kind == "schedule" then
        if value.id == FEEDBACK_FALLBACK then
            model.feedbackActive = false
            model.feedbackAnimated = false
            widget.invalidate()
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
    animation.cancelFrame(FEEDBACK_FRAME)
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
