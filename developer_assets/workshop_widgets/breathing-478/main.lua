-- breathing-478/main.lua
-- SnowDesktop community widget: guided 4-7-8 breathing (inhale 4 s,
-- hold 7 s, exhale 8 s) with an animated guide circle and round tracking.
local rhythm = module.require("modules/rhythm.lua")

local KEY_NAME = "workshop.breathing_478.name"
local KEY_PHASE_INHALE = "workshop.breathing_478.phase_inhale"
local KEY_PHASE_HOLD = "workshop.breathing_478.phase_hold"
local KEY_PHASE_EXHALE = "workshop.breathing_478.phase_exhale"
local KEY_IDLE_HINT = "workshop.breathing_478.idle_hint"
local KEY_START = "workshop.breathing_478.start"
local KEY_PAUSE = "workshop.breathing_478.pause"
local KEY_RESUME = "workshop.breathing_478.resume"
local KEY_PAUSED_LABEL = "workshop.breathing_478.paused_label"
local KEY_RESTART = "workshop.breathing_478.restart"
local KEY_SKIP = "workshop.breathing_478.skip"
local KEY_STOP = "workshop.breathing_478.stop"
local KEY_START_AGAIN = "workshop.breathing_478.start_again"
local KEY_JOIN_SECTIONS = "workshop.breathing_478.join_sections"
local KEY_ROUND_PROGRESS = "workshop.breathing_478.round_progress"
local KEY_COMPLETE_TITLE = "workshop.breathing_478.complete_title"
local KEY_COMPLETE_SUMMARY = "workshop.breathing_478.complete_summary"
local KEY_SETTINGS_ROUNDS = "workshop.breathing_478.settings_rounds"
local KEY_SETTINGS_ACCENT = "workshop.breathing_478.settings_accent"
local KEY_TITLE_INHALE = "workshop.breathing_478.title_inhale"
local KEY_TITLE_HOLD = "workshop.breathing_478.title_hold"
local KEY_TITLE_EXHALE = "workshop.breathing_478.title_exhale"
local KEY_TITLE_PAUSED = "workshop.breathing_478.title_paused"
local KEY_TITLE_COMPLETE = "workshop.breathing_478.title_complete"

local PHASE_LABEL_KEYS = {
    inhale = KEY_PHASE_INHALE,
    hold = KEY_PHASE_HOLD,
    exhale = KEY_PHASE_EXHALE,
}
local PHASE_TITLE_KEYS = {
    inhale = KEY_TITLE_INHALE,
    hold = KEY_TITLE_HOLD,
    exhale = KEY_TITLE_EXHALE,
}

local FRAME_ID = "breathing-478.frame"
local TIMER_ID = "breathing-478.timer"
local MENU_ID = "breathing-478.menu"
local SURFACE_KEY = "breathing-478.surface"
local ORB_KEY = "breathing-478.orb"
local TOGGLE_ID = "breathing-478.toggle"
local RESET_ID = "breathing-478.reset"
local MENU_START_ID = "breathing-478.start"
local MENU_PAUSE_ID = "breathing-478.pause"
local MENU_RESUME_ID = "breathing-478.resume"
local MENU_SKIP_ID = "breathing-478.skip"
local MENU_RESTART_ID = "breathing-478.restart"
local MENU_STOP_ID = "breathing-478.stop"

local DEFAULT_ACCENT = 0x63C7B8
local MAX_COLOR = 0xFFFFFF
local IDLE_BREATH = 0.42
local COMPLETE_BREATH = 0.56
local SEGMENT_GAP_DEGREES = 15

-- Fluent System Icons glyphs already used by the built-in timer components.
local ICONS = {
    play = utf8.char(0xF605),
    pause = utf8.char(0xF8AE),
    stop = utf8.char(0xF72A),
    next = utf8.char(0xF569),
    restart = utf8.char(0xF19F),
}

local settings = {
    fields = {
        {
            key = "cycles",
            label = l10n.tr(KEY_SETTINGS_ROUNDS),
            type = "int",
            default = rhythm.defaultCycles(),
            min = 1,
            max = 12,
        },
        {
            key = "accent",
            label = l10n.tr(KEY_SETTINGS_ACCENT),
            type = "color",
            default = DEFAULT_ACCENT,
        },
    },
}

local LIGHT_FOREGROUND = {
    primary = 0xFFFFFF,
    secondary = 0xD6DEE8,
    track = 0xFFFFFF,
}
local DARK_FOREGROUND = {
    primary = 0x111827,
    secondary = 0x4B5563,
    track = 0x111827,
}

local function foregroundPalette()
    local theme = widget.theme()
    if theme and theme.contentTheme == 1 then
        return DARK_FOREGROUND
    end
    return LIGHT_FOREGROUND
end

local function readCycles()
    return rhythm.normalizeCycles(storage.get("cycles"))
end

local function readAccent()
    local value = tonumber(storage.get("accent"))
    if value == nil or value ~= value or value == math.huge or
        value == -math.huge then
        return DEFAULT_ACCENT
    end
    value = math.floor(value)
    return math.max(0, math.min(MAX_COLOR, value))
end

-- Ink bounds center the visible glyphs, not the font line box, so mixed
-- labels and numerals stay optically centered in the circle.
local function inkBounds(text, size, bold)
    local metrics = draw.measureText(text, size, 0, bold)
    local ink = metrics.ink
    if ink and ink.width > 0 and ink.height > 0 then
        return {
            left = ink.left,
            top = ink.top,
            width = ink.width,
            height = ink.height,
        }
    end
    return {
        left = 0,
        top = 0,
        width = metrics.width,
        height = metrics.height,
    }
end

local function fitTextSize(text, size, maximumWidth, bold)
    local metrics = draw.measureText(text, size, 0, bold)
    if maximumWidth and maximumWidth > 0 and metrics.width > maximumWidth then
        size = size * maximumWidth * 0.96 / metrics.width
    end
    return size
end

local function drawTextOptical(text, centerX, centerY, size, color,
        maximumWidth, bold, alpha)
    local bounds = inkBounds(text, size, bold)
    draw.text(centerX - bounds.left - bounds.width * 0.5,
        centerY - bounds.top - bounds.height * 0.5, text, size, color,
        maximumWidth, bold, true, nil, alpha or 1)
end

-- draw.arc has flat stroke ends; little end caps make the segmented idle ring
-- and the phase progress read as one crafted guide instead of clipped arcs.
local function drawArcRounded(centerX, centerY, radius, startDegrees,
        sweepDegrees, thickness, color, alpha)
    if sweepDegrees <= 0.01 then return end
    local sweep = math.min(sweepDegrees, 360)
    draw.arc(centerX, centerY, radius, startDegrees, sweep, thickness,
        color, alpha)
    if sweep >= 359.5 then return end
    local half = thickness * 0.5
    for _, angle in ipairs({ startDegrees, startDegrees + sweep }) do
        local radians = math.rad(angle)
        draw.circle(centerX + math.cos(radians) * radius,
            centerY + math.sin(radians) * radius, half, color, alpha)
    end
end

local function drawPlayIcon(centerX, centerY, size, color, alpha)
    draw.path({
        { op = "move", x = centerX - size * 0.22, y = centerY - size * 0.34 },
        { op = "line", x = centerX - size * 0.22, y = centerY + size * 0.34 },
        { op = "line", x = centerX + size * 0.36, y = centerY },
        { op = "close" },
    }, { fillColor = color, alpha = alpha })
end

local function drawPauseIcon(centerX, centerY, size, color, alpha)
    local barWidth = size * 0.17
    local barHeight = size * 0.62
    local gap = size * 0.14
    draw.rect(centerX - gap * 0.5 - barWidth, centerY - barHeight * 0.5,
        barWidth, barHeight, color, barWidth * 0.42, alpha)
    draw.rect(centerX + gap * 0.5, centerY - barHeight * 0.5,
        barWidth, barHeight, color, barWidth * 0.42, alpha)
end

local function drawRestartIcon(centerX, centerY, size, color, alpha)
    local radius = size * 0.34
    local startDegrees = -55
    local sweepDegrees = 285
    local endDegrees = startDegrees + sweepDegrees
    draw.arc(centerX, centerY, radius, startDegrees, sweepDegrees,
        size * 0.14, color, alpha)
    local radians = math.rad(endDegrees)
    local pointX = centerX + math.cos(radians) * radius
    local pointY = centerY + math.sin(radians) * radius
    local tangentX = -math.sin(radians)
    local tangentY = math.cos(radians)
    local normalX = math.cos(radians)
    local normalY = math.sin(radians)
    local head = size * 0.26
    draw.path({
        { op = "move", x = pointX + tangentX * head,
            y = pointY + tangentY * head },
        { op = "line", x = pointX - normalX * head * 0.7,
            y = pointY - normalY * head * 0.7 },
        { op = "line", x = pointX + normalX * head * 0.7,
            y = pointY + normalY * head * 0.7 },
        { op = "close" },
    }, { fillColor = color, alpha = alpha })
end

local function drawGuideOrb(centerX, centerY, radius, breath, accent)
    local haloRadius = radius * 1.20
    draw.shadow(centerX - haloRadius, centerY - haloRadius,
        haloRadius * 2, haloRadius * 2, accent,
        math.min(radius * 0.42, 64), haloRadius, 0, 0,
        0.22 + 0.10 * breath)
    draw.circle(centerX, centerY, radius, accent, 0.23 + 0.05 * breath)
end

local function drawGuideRing(centerX, centerY, radius, thickness, accent,
        palette, mode, progress)
    if mode == "idle" then
        local available = 360 - SEGMENT_GAP_DEGREES * rhythm.phaseCount()
        local angle = -90
        for index = 1, rhythm.phaseCount() do
            local sweep = available * rhythm.phaseSeconds(index) /
                rhythm.cycleSeconds()
            drawArcRounded(centerX, centerY, radius, angle, sweep,
                thickness, accent, 0.84)
            angle = angle + sweep + SEGMENT_GAP_DEGREES
        end
        return
    end
    if mode == "complete" then
        draw.arc(centerX, centerY, radius, -90, 360, thickness, accent, 0.92)
        return
    end
    draw.arc(centerX, centerY, radius, 0, 360, thickness, palette.track, 0.16)
    if progress > 0.001 then
        drawArcRounded(centerX, centerY, radius, -90, 360 * progress,
            thickness, accent, 0.96)
    end
end

local function phaseTitleKey(model)
    local mode = rhythm.mode(model.state)
    if mode == "complete" then return KEY_TITLE_COMPLETE end
    if mode == "paused" then return KEY_TITLE_PAUSED end
    if mode == "running" then
        return PHASE_TITLE_KEYS[rhythm.phaseId(model.state.phaseIndex)]
    end
    return KEY_NAME
end

local function updateTitle(model)
    widget.setTitle(l10n.tr(phaseTitleKey(model)))
end

local function stopTicker(model)
    if model.framesActive then
        animation.cancelFrame(FRAME_ID)
        model.framesActive = false
    end
    if model.timerActive then
        schedule.cancel(TIMER_ID)
        model.timerActive = false
    end
end

-- Frame callbacks drive the guide at display cadence. If the host cannot
-- schedule frames, a coarse host timer keeps the exercise usable.
local function ensureTicker(model)
    if model.framesActive or model.timerActive then return true end
    if animation.requestFrame(FRAME_ID) then
        model.framesActive = true
        return true
    end
    model.timerActive = schedule.every(TIMER_ID, 50,
        { whenHidden = "pause" }) and true or false
    if not model.timerActive then
        widget.log("warn", "breathing-478: no animation ticker available")
    end
    return model.timerActive
end

local function finishSession(model)
    stopTicker(model)
    updateTitle(model)
    widget.invalidate()
end

local function startSession(model)
    rhythm.start(model.state, time.monotonic(), readCycles())
    updateTitle(model)
    ensureTicker(model)
    widget.invalidate()
end

local function pauseSession(model)
    if rhythm.pause(model.state, time.monotonic()) then
        stopTicker(model)
        updateTitle(model)
        widget.invalidate()
    end
end

local function resumeSession(model)
    if rhythm.resume(model.state, time.monotonic()) then
        updateTitle(model)
        ensureTicker(model)
        widget.invalidate()
    end
end

local function skipPhase(model)
    local result = rhythm.skip(model.state, time.monotonic())
    if result == "complete" then
        finishSession(model)
    elseif result then
        updateTitle(model)
        if rhythm.mode(model.state) == "running" then ensureTicker(model) end
        widget.invalidate()
    end
end

local function stopSession(model)
    rhythm.stop(model.state)
    stopTicker(model)
    updateTitle(model)
    widget.invalidate()
end

local function toggleSession(model)
    local mode = rhythm.mode(model.state)
    if mode == "running" then
        pauseSession(model)
    elseif mode == "paused" then
        resumeSession(model)
    else
        startSession(model)
    end
end

local function render(_context, model)
    local width = layout.contentWidth()
    local height = layout.contentHeight()
    local short = math.min(width, height)
    if short <= 0 then return end

    local palette = foregroundPalette()
    local accent = readAccent()
    local state = model.state
    local now = time.monotonic()
    local mode = rhythm.mode(state)
    local cycles = mode == "idle" and readCycles() or state.cycles

    local padding = short * 0.05
    local headerHeight = short * 0.08
    local bottomHeight = short * 0.08
    local gap = short * 0.035
    local cornerHitRadius = short * 0.08
    local cornerIconSize = short * 0.08
    local orbDiameter = math.max(0, math.min(
        width - padding * 2,
        height - padding * 2 - headerHeight - bottomHeight - gap * 2))
    local compositionHeight = headerHeight + gap + orbDiameter + gap +
        bottomHeight
    local top = (height - compositionHeight) * 0.5
    local centerX = width * 0.5
    local headerCenterY = top + headerHeight * 0.5
    local orbCenterY = top + headerHeight + gap + orbDiameter * 0.5
    local bottomCenterY = top + headerHeight + gap + orbDiameter + gap +
        bottomHeight * 0.5
    local ringRadius = orbDiameter * 0.46
    local primaryCenterX = padding + cornerHitRadius
    local resetCenterX = width - padding - cornerHitRadius
    local restartVisible = mode == "running" or mode == "paused"

    local phaseIndex = state.phaseIndex
    local progress = rhythm.progress(state, now)
    local seconds = rhythm.remainingSeconds(state, now)
    local breath = rhythm.breathFraction(phaseIndex, progress)
    local phaseLabel = l10n.tr(PHASE_LABEL_KEYS[rhythm.phaseId(phaseIndex)])
    local headerText = l10n.tr(KEY_NAME)
    local labelText = ""
    local valueText = ""
    local hintText = ""
    local summaryText = ""

    if mode == "idle" then
        breath = IDLE_BREATH
        progress = 0
        hintText = l10n.tr(KEY_IDLE_HINT)
    elseif mode == "complete" then
        breath = COMPLETE_BREATH
        progress = 1
        labelText = l10n.tr(KEY_COMPLETE_TITLE)
        summaryText = l10n.tr(KEY_COMPLETE_SUMMARY,
            l10n.formatNumber(state.cycles))
    else
        headerText = l10n.tr(KEY_ROUND_PROGRESS,
            l10n.formatNumber(state.cycle), l10n.formatNumber(cycles))
        labelText = mode == "paused" and
            l10n.tr(KEY_JOIN_SECTIONS, phaseLabel,
                l10n.tr(KEY_PAUSED_LABEL)) or phaseLabel
        valueText = l10n.formatNumber(seconds)
    end

    -- Guide circle: a soft accent glow whose radius follows the breath.
    local orbRadius = ringRadius * (0.50 + 0.38 * breath)
    drawGuideOrb(centerX, orbCenterY, orbRadius, breath, accent)
    drawGuideRing(centerX, orbCenterY, ringRadius, ringRadius * 0.078,
        accent, palette, mode, progress)

    -- Centered content: the countdown owns the optical center of the ring;
    -- the pattern or completion title takes the same place when idle/complete.
    local textWidth = ringRadius * 1.55
    if mode == "idle" then
        local pattern = rhythm.patternText()
        local patternSize = fitTextSize(pattern, ringRadius * 0.50,
            textWidth, true)
        drawTextOptical(pattern, centerX, orbCenterY, patternSize,
            palette.primary, textWidth, true)
    elseif mode == "complete" then
        local titleSize = fitTextSize(labelText, ringRadius * 0.40,
            textWidth, true)
        drawTextOptical(labelText, centerX, orbCenterY, titleSize,
            palette.primary, textWidth, true)
    else
        local valueSize = fitTextSize(valueText, ringRadius * 0.82,
            textWidth, true)
        drawTextOptical(valueText, centerX, orbCenterY, valueSize,
            palette.primary, textWidth, true)
    end

    -- The phase hint now sits in its own bottom row, so the number can stay
    -- exactly centered in the circle.
    local bottomText
    local bottomColor = palette.secondary
    local bottomAlpha = 0.95
    local bottomFont = short * 0.058
    if mode == "idle" then
        bottomText = hintText
    elseif mode == "complete" then
        bottomText = summaryText
        bottomFont = short * 0.056
    else
        bottomText = labelText
        bottomColor = palette.primary
        bottomAlpha = 0.92
        bottomFont = short * 0.062
    end
    local bottomMaximumWidth = width - padding * 2
    local bottomSize = fitTextSize(bottomText, bottomFont,
        bottomMaximumWidth, false)
    drawTextOptical(bottomText, centerX, bottomCenterY, bottomSize,
        bottomColor, bottomMaximumWidth, false, bottomAlpha)

    -- Header: round counter while active, component name otherwise; the
    -- play/pause and restart controls share the same optical center line.
    local headerMaximumWidth = width -
        (padding + cornerIconSize * 0.5) * 2 - gap * 2
    local headerSize = fitTextSize(headerText, short * 0.058,
        headerMaximumWidth, false)
    drawTextOptical(headerText, centerX, headerCenterY, headerSize,
        palette.secondary, headerMaximumWidth, false, 0.90)

    local primaryHovered = interaction.isHovered(TOGGLE_ID)
    local primaryPressed = interaction.isPressed(TOGGLE_ID)
    if primaryHovered or primaryPressed then
        draw.circle(primaryCenterX, headerCenterY,
            cornerHitRadius * 0.86, palette.primary,
            primaryPressed and 0.15 or 0.08)
    end
    if mode == "running" then
        drawPauseIcon(primaryCenterX, headerCenterY, cornerIconSize,
            palette.primary, primaryPressed and 0.72 or 0.94)
    else
        drawPlayIcon(primaryCenterX, headerCenterY, cornerIconSize,
            palette.primary, primaryPressed and 0.72 or 0.94)
    end

    if restartVisible then
        local hovered = interaction.isHovered(RESET_ID)
        local pressed = interaction.isPressed(RESET_ID)
        if hovered or pressed then
            draw.circle(resetCenterX, headerCenterY,
                cornerHitRadius * 0.86, palette.primary,
                pressed and 0.15 or 0.08)
        end
        drawRestartIcon(resetCenterX, headerCenterY, cornerIconSize,
            hovered and palette.primary or palette.secondary,
            pressed and 0.72 or 0.92)
    end

    -- Interaction regions. The full surface only contributes the component
    -- context menu; explicit controls are submitted afterwards on top.
    interaction.region({
        key = SURFACE_KEY,
        shape = { type = "rect", x = 0, y = 0,
            width = width, height = height },
        events = {
            contextMenu = { id = MENU_ID, scope = "component" },
        },
    })
    interaction.region({
        key = ORB_KEY,
        shape = { type = "circle", x = centerX, y = orbCenterY,
            radius = ringRadius * 1.02 },
        cursor = "hand",
        events = {
            click = { id = TOGGLE_ID },
            contextMenu = { id = MENU_ID, scope = "component" },
        },
    })
    local actionKey
    if mode == "running" then
        actionKey = KEY_PAUSE
    elseif mode == "paused" then
        actionKey = KEY_RESUME
    elseif mode == "complete" then
        actionKey = KEY_START_AGAIN
    else
        actionKey = KEY_START
    end
    local actionLabel = l10n.tr(actionKey)
    local statusText = ""
    if mode == "running" or mode == "paused" then
        statusText = phaseLabel .. " " .. valueText
    elseif mode == "complete" then
        statusText = l10n.tr(KEY_JOIN_SECTIONS, labelText, summaryText)
    end
    local accessibleLabel = statusText ~= "" and
        l10n.tr(KEY_JOIN_SECTIONS, actionLabel, statusText) or actionLabel
    interaction.region({
        key = TOGGLE_ID,
        shape = { type = "circle", x = primaryCenterX,
            y = headerCenterY, radius = cornerHitRadius },
        cursor = "hand",
        focusable = true,
        tabIndex = 0,
        tooltip = actionLabel,
        events = {
            click = { id = TOGGLE_ID },
            contextMenu = { id = MENU_ID, scope = "component" },
        },
        accessibility = { role = "button", label = accessibleLabel },
    })
    if restartVisible then
        interaction.region({
            key = RESET_ID,
            shape = { type = "circle", x = resetCenterX,
                y = headerCenterY, radius = cornerHitRadius },
            cursor = "hand",
            focusable = true,
            tabIndex = 1,
            tooltip = l10n.tr(KEY_RESTART),
            events = {
                click = { id = RESET_ID },
                contextMenu = { id = MENU_ID, scope = "component" },
            },
            accessibility = {
                role = "button",
                label = l10n.tr(KEY_RESTART),
            },
        })
    end
end

local function menu(_context, model, request)
    if request.id ~= MENU_ID then return nil end
    local mode = rhythm.mode(model.state)
    local items = {}
    local function add(id, key, icon)
        items[#items + 1] = {
            id = id,
            label = l10n.tr(key),
            icon = icon,
            iconFont = "fluent",
        }
    end
    if mode == "idle" then
        add(MENU_START_ID, KEY_START, ICONS.play)
    elseif mode == "running" then
        add(MENU_PAUSE_ID, KEY_PAUSE, ICONS.pause)
        add(MENU_SKIP_ID, KEY_SKIP, ICONS.next)
    elseif mode == "paused" then
        add(MENU_RESUME_ID, KEY_RESUME, ICONS.play)
        add(MENU_SKIP_ID, KEY_SKIP, ICONS.next)
    else
        add(MENU_START_ID, KEY_START_AGAIN, ICONS.play)
    end
    if mode == "running" or mode == "paused" then
        items[#items + 1] = { type = "separator" }
        add(MENU_RESTART_ID, KEY_RESTART, ICONS.restart)
        add(MENU_STOP_ID, KEY_STOP, ICONS.stop)
    elseif mode == "complete" then
        items[#items + 1] = { type = "separator" }
        add(MENU_STOP_ID, KEY_STOP, ICONS.stop)
    end
    return ui.menu(items)
end

local function event(_context, model, value)
    if value.kind == "frame" and value.id == FRAME_ID then
        model.framesActive = false
        local result = rhythm.advance(model.state, time.monotonic())
        if result == "complete" then
            finishSession(model)
        else
            if result then updateTitle(model) end
            -- A frame queued before pause/cancel can still arrive. Only a
            -- running session keeps requesting frames.
            if rhythm.mode(model.state) == "running" then
                ensureTicker(model)
            end
            widget.invalidate()
        end
        return
    end
    if value.kind == "schedule" and value.id == TIMER_ID then
        -- Schedule events carry a wall-clock timestamp; the breathing state
        -- uses the monotonic clock so sleep and clock changes cannot corrupt
        -- phase progress.
        local result = rhythm.advance(model.state, time.monotonic())
        if result == "complete" then
            finishSession(model)
        elseif result then
            updateTitle(model)
            widget.invalidate()
        end
        return
    end
    if value.kind == "visibility" then
        if not value.visible then
            -- The guide cannot be followed while its page is hidden, so keep
            -- the current phase instead of silently finishing in the background.
            if rhythm.mode(model.state) == "running" then
                pauseSession(model)
            end
            return
        end
        if rhythm.mode(model.state) == "running" then
            -- Hidden surfaces may drop the queued frame request.
            model.framesActive = false
            local result = rhythm.advance(model.state, time.monotonic())
            if result == "complete" then
                finishSession(model)
            else
                if result then updateTitle(model) end
                ensureTicker(model)
                widget.invalidate()
            end
        end
        return
    end
    if value.kind == "environment" then
        updateTitle(model)
        return
    end
    if value.kind == "settings.changed" then
        -- Accent changes apply immediately; round-count changes apply when the
        -- next session starts, so the current session target stays stable.
        widget.invalidate()
        return
    end
    if value.kind ~= "action" then return end

    local action = value.id
    if action == TOGGLE_ID then
        toggleSession(model)
    elseif action == RESET_ID or action == MENU_RESTART_ID or
        action == MENU_START_ID then
        startSession(model)
    elseif action == MENU_PAUSE_ID then
        pauseSession(model)
    elseif action == MENU_RESUME_ID then
        resumeSession(model)
    elseif action == MENU_SKIP_ID then
        skipPhase(model)
    elseif action == MENU_STOP_ID then
        stopSession(model)
    end
end

local function setup()
    return {
        state = rhythm.new(readCycles()),
        framesActive = false,
        timerActive = false,
    }
end

local function dispose(_context, model)
    if model then stopTicker(model) end
end

return widget.define({
    name = l10n.tr(KEY_NAME),
    useCustomStyle = true,
    followPersonalizationDefault = true,
    bg = 0x151A21,
    border = 0xFFFFFF,
    alpha = 0.42,
    borderAlpha = 0.18,
    gradientEndA = 0.30,
    settings = settings,
    setup = setup,
    render = render,
    event = event,
    menu = menu,
    dispose = dispose,
})
