local woodenFishImage = resource.image("wooden-fish")
local malletImage = resource.image("mallet")

local STRIKE_KEY = "wooden-fish.strike"
local FEEDBACK_FRAME = "wooden-fish.feedback-frame"
local FEEDBACK_FALLBACK = "wooden-fish.feedback-fallback"
local FEEDBACK_DURATION_MS = 900
local MALLET_STRIKE_DURATION_MS = 240
local MALLET_REST_ANGLE = 90
local MALLET_STRIKE_ANGLE = -22
local MALLET_SOURCE_TIP_X = 0.92
local MALLET_SOURCE_TIP_Y = 0.08
local DAY_CHECK = "wooden-fish.day-check"
local MAX_COUNT = 999999999

local settings = {
    fields = {
        {
            key = "customTextEnabled",
            label = l10n.tr("lua_widget.wooden_fish.custom_text"),
            type = "bool",
            default = false,
        },
        {
            key = "customTerm",
            label = l10n.tr("lua_widget.wooden_fish.custom_term"),
            type = "text",
            default = l10n.tr("lua_widget.wooden_fish.term"),
            maxLength = 12,
            validationMessage = l10n.tr(
                "lua_widget.wooden_fish.custom_term_invalid"),
            showWhen = {
                key = "customTextEnabled",
                operator = "truthy",
            },
        },
        {
            key = "customHint",
            label = l10n.tr("lua_widget.wooden_fish.custom_hint"),
            type = "text",
            default = l10n.tr("lua_widget.wooden_fish.hint"),
            maxLength = 40,
            validationMessage = l10n.tr(
                "lua_widget.wooden_fish.custom_hint_invalid"),
            showWhen = {
                key = "customTextEnabled",
                operator = "truthy",
            },
        },
    },
}

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

local function todayKey()
    local now = time.parts(time.now())
    return string.format("%04d-%02d-%02d", now.year, now.month, now.day)
end

local function settingEnabled(key)
    local value = storage.get(key)
    return value == true or value == 1 or value == "1" or value == "true"
end

local function nonEmptySetting(key, fallback)
    local value = storage.get(key)
    if type(value) == "string" and value:match("%S") then return value end
    return fallback
end

local function resolvedCopy()
    local term = l10n.tr("lua_widget.wooden_fish.term")
    local hint = l10n.tr("lua_widget.wooden_fish.hint")
    if settingEnabled("customTextEnabled") then
        term = nonEmptySetting("customTerm", term)
        hint = nonEmptySetting("customHint", hint)
    end
    return term, hint
end

local function persistCounts(model)
    local persisted = normalizedCount(storage.get("count"))
    if persisted ~= model.count then
        storage.set("count", model.count)
    end
    local persistedToday = normalizedCount(storage.get("todayCount"))
    if persistedToday ~= model.todayCount then
        storage.set("todayCount", model.todayCount)
    end
    if storage.get("todayDate") ~= model.todayDate then
        storage.set("todayDate", model.todayDate)
    end
end

local function rollToday(model)
    local current = todayKey()
    if model.todayDate == current then return false end
    model.todayDate = current
    model.todayCount = 0
    return true
end

local function setup()
    local currentDate = todayKey()
    local storedDate = storage.get("todayDate")
    local model = {
        count = normalizedCount(storage.get("count")),
        todayDate = currentDate,
        todayCount = storedDate == currentDate and
            normalizedCount(storage.get("todayCount")) or 0,
        feedbackActive = false,
        feedbackAnimated = false,
        feedbackElapsedMs = 0,
    }
    if storedDate ~= currentDate then persistCounts(model) end
    schedule.every(DAY_CHECK, 60000, { whenHidden = "continue" })
    return model
end

local function centeredText(text, y, size, color, width, bold, alpha,
        centerX)
    local metrics = draw.measureText(text, size, 0, bold)
    if metrics.width > width then
        size = size * width / metrics.width
        metrics = draw.measureText(text, size, 0, bold)
    end
    local x = (centerX or layout.contentWidth() / 2) - metrics.width / 2
    draw.text(x, y, text, size, color, width, bold, true,
        nil, alpha or 1.0)
end

local function compactTextPair(left, right, y, size, color, width)
    local gap = size * 0.35
    local leftMetrics = draw.measureText(left, size, 0, true)
    local rightMetrics = draw.measureText(right, size, 0, true)
    local pairWidth = leftMetrics.width + gap + rightMetrics.width

    if pairWidth > width then
        size = size * width / pairWidth
        gap = size * 0.35
        leftMetrics = draw.measureText(left, size, 0, true)
        rightMetrics = draw.measureText(right, size, 0, true)
        pairWidth = leftMetrics.width + gap + rightMetrics.width
    end

    local x = (layout.contentWidth() - pairWidth) / 2
    draw.text(x, y, left, size, color, leftMetrics.width,
        true, true)
    draw.text(x + leftMetrics.width + gap, y,
        right, size, color, rightMetrics.width, true, true)
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
    local bodyDrop = pressed and size * 0.012 or 0
    local bodyY = cy - size / 2 + bodyDrop

    draw.shadow(cx - size * 0.38, cy + size * 0.28 + bodyDrop,
        size * 0.76, size * 0.18, 0x241006, size * 0.07,
        size * 0.09, 0, size * 0.024, 0.19)
    draw.imageFit(woodenFishImage, cx - size / 2, bodyY, size, size,
        "contain", "center", pressed and 0.97 or 1.0, "linear")

    -- Turn the source sprite into the requested head-upper-left pose. Compensate
    -- the draw position as it swings so the rotated lower-right handle tip is
    -- the fixed pivot throughout the strike.
    local malletSize = size * 0.56
    local malletX = cx + size * 0.12
    local malletY = cy - size * 0.60
    local rotation = MALLET_REST_ANGLE + malletRotation(model, pressed)
    local radians = math.rad(rotation)
    local cosine = math.cos(radians)
    local sine = math.sin(radians)
    local sourceTipX = MALLET_SOURCE_TIP_X - 0.5
    local sourceTipY = MALLET_SOURCE_TIP_Y - 0.5
    local currentTipX = sourceTipX * cosine - sourceTipY * sine
    local currentTipY = sourceTipX * sine + sourceTipY * cosine
    local restTipX = 0.5 - MALLET_SOURCE_TIP_Y
    local restTipY = MALLET_SOURCE_TIP_X - 0.5
    local pivotCompensationX = (restTipX - currentTipX) * malletSize
    local pivotCompensationY = (restTipY - currentTipY) * malletSize
    draw.imageFit(malletImage,
        malletX + pivotCompensationX, malletY + pivotCompensationY,
        malletSize, malletSize, "contain", "center", 1.0, "linear",
        rotation, 0.5, 0.5)
end

local function drawFeedback(model, y, baseSize, colors, width, centerX, term)
    if not model.feedbackActive then return end

    local progress = math.max(0, math.min(1,
        model.feedbackElapsedMs / FEEDBACK_DURATION_MS))
    local popProgress = math.min(1, progress / 0.34)
    local popScale = 1 + math.sin(popProgress * math.pi) * 0.20
    local rise = baseSize * 1.18 *
        (1 - (1 - progress) * (1 - progress))
    local alpha = math.max(0, 1 - progress * progress)
    centeredText(l10n.tr("lua_widget.wooden_fish.feedback", term),
        y - rise, baseSize * popScale, colors.feedback,
        width, true, alpha, centerX)
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
    local width = layout.contentWidth()
    local height = layout.contentHeight()
    local pressed = interaction.isPressed(STRIKE_KEY)
    local short = math.min(width, height)
    local padding = short * 0.052
    local textSize = short * 0.09
    local term, hint = resolvedCopy()
    local textWidth = width - padding * 2
    local todayText = l10n.tr("lua_widget.wooden_fish.today_counter",
        l10n.formatNumber(model.todayCount))
    local totalText = l10n.tr("lua_widget.wooden_fish.total_counter",
        l10n.formatNumber(model.count))
    local instrumentSize = math.min(width * 0.82, height * 0.53)
    local instrumentCx = width * 0.50
    local textCenterX = width * 0.50
    local textHeight = textSize * 1.35
    local groupGap = textSize * 0.62
    local groupHeight = textHeight * 2 + groupGap * 2 + instrumentSize
    local groupY = (height - groupHeight) * 0.5
    local instrumentCy = groupY + textHeight + groupGap +
        instrumentSize * 0.5
    local hintY = instrumentCy + instrumentSize * 0.5 + groupGap
    compactTextPair(todayText, totalText, groupY, textSize,
        colors.feedback, textWidth)
    drawInstrument(instrumentCx, instrumentCy, instrumentSize, pressed, model)

    drawFeedback(model, instrumentCy - instrumentSize * 0.62,
        textSize, colors, textWidth, instrumentCx, term)

    centeredText(hint, hintY, textSize, colors.feedback,
        textWidth, true, 0.96, textCenterX)

    interaction.region({
        key = STRIKE_KEY,
        shape = {
            type = "circle",
            x = instrumentCx,
            y = instrumentCy,
            radius = instrumentSize * 0.49,
        },
        cursor = "hand",
        focusable = true,
        tabIndex = 0,
        tooltip = l10n.tr("lua_widget.wooden_fish.tooltip", term),
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
                term, l10n.formatNumber(model.count)),
        },
    })
end

local function event(_context, model, value)
    if value.kind == "action" and value.id == "wooden-fish.strike" then
        rollToday(model)
        if model.count < MAX_COUNT then
            model.count = model.count + 1
            model.todayCount = math.min(MAX_COUNT, model.todayCount + 1)
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
        model.todayDate = todayKey()
        model.todayCount = 0
        model.feedbackActive = false
        model.feedbackAnimated = false
        model.feedbackElapsedMs = 0
        animation.cancelFrame(FEEDBACK_FRAME)
        schedule.cancel(FEEDBACK_FALLBACK)
        schedule.cancel("wooden-fish.persist")
        persistCounts(model)
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
            persistCounts(model)
        elseif value.id == DAY_CHECK and rollToday(model) then
            persistCounts(model)
            widget.invalidate()
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
    schedule.cancel(DAY_CHECK)
    persistCounts(model)
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
    settings = settings,
    setup = setup,
    render = render,
    event = event,
    menu = menu,
    dispose = dispose,
})
