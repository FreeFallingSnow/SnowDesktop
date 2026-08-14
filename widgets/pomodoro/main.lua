-- pomodoro/main.lua - API v2 Pomodoro timer with background deadlines
local DEFAULT_WORK_COLOR = 0xFF6347
local DEFAULT_BREAK_COLOR = 0x4ECDC4

local fluent = {
    play = utf8.char(0xF605),
    pause = utf8.char(0xF8AE),
    stop = utf8.char(0xF72A),
    next = utf8.char(0xF569),
    reset = utf8.char(0xF19F),
}

local descriptor

local settings = {
    fields = {
        { key = "workMin", label = l10n.tr("lua_widget.pomodoro.work_minutes"), type = "int", default = 25, min = 1, max = 120 },
        { key = "breakMin", label = l10n.tr("lua_widget.pomodoro.short_break_minutes"), type = "int", default = 5, min = 1, max = 60 },
        { key = "longBreakMin", label = l10n.tr("lua_widget.pomodoro.long_break_minutes"), type = "int", default = 15, min = 1, max = 120 },
        { key = "longBreakInterval", label = l10n.tr("lua_widget.pomodoro.long_break_interval"), type = "int", default = 4, min = 1, max = 10 },
        { key = "workColor", label = l10n.tr("lua_widget.pomodoro.work_color"), type = "color", default = DEFAULT_WORK_COLOR },
        { key = "breakColor", label = l10n.tr("lua_widget.pomodoro.break_color"), type = "color", default = DEFAULT_BREAK_COLOR },
    },
}

local function clampInteger(value, minimum, maximum, fallback)
    local number = math.floor(tonumber(value) or fallback)
    return math.max(minimum, math.min(maximum, number))
end

local function loadConfig()
    return {
        workMin = clampInteger(storage.get("workMin"), 1, 120, 25),
        breakMin = clampInteger(storage.get("breakMin"), 1, 60, 5),
        longBreakMin = clampInteger(storage.get("longBreakMin"), 1, 120, 15),
        longBreakInterval = clampInteger(storage.get("longBreakInterval"), 1, 10, 4),
        workColor = tonumber(storage.get("workColor")) or DEFAULT_WORK_COLOR,
        breakColor = tonumber(storage.get("breakColor")) or DEFAULT_BREAK_COLOR,
    }
end

local function configSignature(config)
    return table.concat({
        config.workMin,
        config.breakMin,
        config.longBreakMin,
        config.longBreakInterval,
    }, ":")
end

local function loadStyle()
    descriptor.bg = tonumber(storage.get("bg")) or
        tonumber(storage.get("bgColor")) or 0x151A21
    descriptor.border = tonumber(storage.get("border")) or
        tonumber(storage.get("borderColor")) or 0xFFFFFF
    descriptor.alpha = tonumber(storage.get("alpha")) or 0.42
    descriptor.gradientEndA = tonumber(storage.get("gradientEndA")) or 0.30
    if storage.get("followPersonalization") == "1" then
        local theme = widget.theme()
        if theme and theme.bg then
            descriptor.bg = theme.bg
            descriptor.border = theme.border or descriptor.border
            descriptor.alpha = theme.alpha or descriptor.alpha
            descriptor.gradientEndA = theme.gradientEndA or
                descriptor.gradientEndA
        end
    end
end

local function getPalette()
    local theme = widget.theme()
    if theme and theme.contentTheme == 1 then
        return {
            text = 0x1E293B,
            muted = 0x334155,
            track = 0xE2E8F0,
            button = 0xE2E8F0,
            pause = 0xD97706,
        }
    end
    return {
        text = 0xF1F5F9,
        muted = 0xF1F5F9,
        track = 0x1E293B,
        button = 0x1E293B,
        pause = 0xFFB347,
    }
end

local function nowSeconds()
    return math.floor(time.now() / 1000)
end

local function getState()
    local value = storage.get("state") or "idle"
    if value == "work" or value == "break" or value == "paused" then
        return value
    end
    return "idle"
end

local function getSessions()
    return math.max(0, math.floor(tonumber(storage.get("sessions")) or 0))
end

local function getPausedRemaining()
    return math.max(0,
        math.floor(tonumber(storage.get("pausedRemaining")) or 0))
end

local function targetForState(state, config)
    if state == "work" then return config.workMin * 60 end
    if state == "break" then
        local sessions = getSessions()
        if sessions > 0 and sessions % config.longBreakInterval == 0 then
            return config.longBreakMin * 60
        end
        return config.breakMin * 60
    end
    return config.workMin * 60
end

local function remainingSeconds(config, epochSeconds)
    local state = getState()
    if state == "idle" then return config.workMin * 60 end
    if state == "paused" then return getPausedRemaining() end
    local started = tonumber(storage.get("startedAtEpoch")) or epochSeconds
    local elapsed = math.max(0, epochSeconds - started)
    return math.max(0, targetForState(state, config) - elapsed)
end

local function progress(config, remaining)
    local state = getState()
    if state == "idle" then return 0 end
    local target = targetForState(state, config)
    if target <= 0 then return 0 end
    return math.max(0, math.min(1, 1 - remaining / target))
end

local function updateTitle()
    local state = getState()
    if state == "work" then
        widget.setTitle(l10n.tr("lua_widget.pomodoro.title_work"))
    elseif state == "break" then
        widget.setTitle(l10n.tr("lua_widget.pomodoro.title_break"))
    elseif state == "paused" then
        widget.setTitle(l10n.tr("lua_widget.pomodoro.title_paused"))
    else
        widget.setTitle(l10n.tr("lua_widget.pomodoro.name"))
    end
end

local function postNotification(model, messageKey)
    if not widget.hasFeature("task.start") or
        not widget.hasFeature("task.notification.show") or
        not widget.hasPermission("notification.post") then
        return
    end
    local taskId, err = task.start("notification.show", {
        title = l10n.tr("lua_widget.pomodoro.name"),
        message = l10n.tr(messageKey),
    })
    if taskId then
        model.notificationTasks[tostring(taskId)] = true
    else
        widget.log("warn", "notification.show rejected: " .. tostring(err))
    end
end

local function configureSchedules(model, config)
    schedule.cancel("visual")
    schedule.cancel("deadline")
    local state = getState()
    if state ~= "work" and state ~= "break" then return end

    schedule.every("visual", 1000, { whenHidden = "pause" })
    local remaining = remainingSeconds(config, nowSeconds())
    schedule.after("deadline", math.max(1, remaining * 1000), {
        whenHidden = "continue",
    })
    model.configSignature = configSignature(config)
end

local function completePhase(model, config, notify)
    local state = getState()
    if state == "work" then
        storage.set("sessions", tostring(getSessions() + 1))
        storage.set("state", "break")
        storage.set("startedAtEpoch", tostring(nowSeconds()))
        storage.set("pausedRemaining", "0")
        storage.set("prevState", "")
        if notify then
            postNotification(model, "lua_widget.pomodoro.work_complete")
        end
    elseif state == "break" then
        if getSessions() >= config.longBreakInterval then
            storage.set("sessions", "0")
        end
        storage.set("state", "idle")
        storage.set("startedAtEpoch", "0")
        storage.set("pausedRemaining", "0")
        storage.set("prevState", "")
        if notify then
            postNotification(model, "lua_widget.pomodoro.break_complete")
        end
    else
        return false
    end
    updateTitle()
    configureSchedules(model, config)
    return true
end

local function reconcile(model, notify)
    local config = loadConfig()
    local signature = configSignature(config)
    local changed = signature ~= model.configSignature
    model.configSignature = signature
    local state = getState()
    if (state == "work" or state == "break") and
        remainingSeconds(config, nowSeconds()) <= 0 then
        completePhase(model, config, notify)
    elseif changed then
        configureSchedules(model, config)
    end
end

local function startWork(model)
    local config = loadConfig()
    storage.set("state", "work")
    storage.set("startedAtEpoch", tostring(nowSeconds()))
    storage.set("pausedRemaining", "0")
    storage.set("prevState", "")
    updateTitle()
    configureSchedules(model, config)
end

local function pauseTimer(model)
    local state = getState()
    if state ~= "work" and state ~= "break" then return end
    local config = loadConfig()
    storage.set("prevState", state)
    storage.set("pausedRemaining",
        tostring(remainingSeconds(config, nowSeconds())))
    storage.set("state", "paused")
    storage.set("startedAtEpoch", "0")
    updateTitle()
    configureSchedules(model, config)
end

local function resumeTimer(model)
    if getState() ~= "paused" then return end
    local config = loadConfig()
    local previous = storage.get("prevState") or "work"
    if previous ~= "work" and previous ~= "break" then previous = "work" end
    local target = targetForState(previous, config)
    local remaining = math.min(target, getPausedRemaining())
    storage.set("state", previous)
    storage.set("startedAtEpoch",
        tostring(nowSeconds() - (target - remaining)))
    storage.set("pausedRemaining", "0")
    storage.set("prevState", "")
    updateTitle()
    configureSchedules(model, config)
end

local function stopTimer(model)
    storage.set("state", "idle")
    storage.set("startedAtEpoch", "0")
    storage.set("pausedRemaining", "0")
    storage.set("prevState", "")
    updateTitle()
    configureSchedules(model, loadConfig())
end

local function skipPhase(model)
    local config = loadConfig()
    local state = getState()
    if state == "work" then
        storage.set("sessions", tostring(getSessions() + 1))
        storage.set("state", "break")
        storage.set("startedAtEpoch", tostring(nowSeconds()))
        postNotification(model, "lua_widget.pomodoro.work_skipped")
    elseif state == "break" then
        if getSessions() >= config.longBreakInterval then
            storage.set("sessions", "0")
        end
        storage.set("state", "work")
        storage.set("startedAtEpoch", tostring(nowSeconds()))
        postNotification(model, "lua_widget.pomodoro.break_skipped")
    else
        return
    end
    storage.set("pausedRemaining", "0")
    storage.set("prevState", "")
    updateTitle()
    configureSchedules(model, config)
end

local function resetTimer(model)
    storage.set("state", "idle")
    storage.set("startedAtEpoch", "0")
    storage.set("pausedRemaining", "0")
    storage.set("sessions", "0")
    storage.set("prevState", "")
    updateTitle()
    configureSchedules(model, loadConfig())
end

local function dispatchAction(model, action)
    if action == "pomodoro.start" then startWork(model)
    elseif action == "pomodoro.resume" then resumeTimer(model)
    elseif action == "pomodoro.pause" then pauseTimer(model)
    elseif action == "pomodoro.stop" then stopTimer(model)
    elseif action == "pomodoro.skip" then skipPhase(model)
    elseif action == "pomodoro.reset" then resetTimer(model)
    end
end

local function setup()
    local model = {
        configSignature = "",
        notificationTasks = {},
    }
    local state = getState()
    if (state == "work" or state == "break") and
        (tonumber(storage.get("startedAtEpoch")) or 0) <= 0 then
        storage.set("startedAtEpoch", tostring(nowSeconds()))
    end
    updateTitle()
    reconcile(model, false)
    return model
end

local function formatTime(seconds)
    local minutes = math.floor(seconds / 60)
    return string.format("%02d:%02d", minutes, math.floor(seconds % 60))
end

local function drawTrackRing(cx, cy, radius, thickness, color, alpha)
    local inner = radius - thickness / 2
    local outer = radius + thickness / 2
    local step = 2 * math.pi /
        math.max(360, math.floor(4 * math.pi * radius))
    local angle = 0
    while angle < 2 * math.pi do
        draw.line(cx + math.cos(angle) * inner,
            cy + math.sin(angle) * inner,
            cx + math.cos(angle) * outer,
            cy + math.sin(angle) * outer,
            thickness, color, alpha)
        angle = angle + step
    end
end

local function drawProgressArc(cx, cy, radius, value, thickness, color)
    if value <= 0 then return end
    local inner = radius - thickness / 2
    local outer = radius + thickness / 2
    local step = 2 * math.pi /
        math.max(360, math.floor(4 * math.pi * radius))
    local first = -math.pi / 2
    local angle = first
    local last = first + value * 2 * math.pi
    while angle < last do
        draw.line(cx + math.cos(angle) * inner,
            cy + math.sin(angle) * inner,
            cx + math.cos(angle) * outer,
            cy + math.sin(angle) * outer,
            thickness, color, 1.0)
        angle = angle + step
    end
end

local function drawDots(cx, cy, filled, total, color, radius, gap)
    local startX = cx - (total - 1) * gap / 2
    for index = 1, total do
        local x = startX + (index - 1) * gap
        if index <= filled then
            draw.circle(x, cy, radius, color, 1.0)
        else
            draw.circle(x, cy, radius, 0xFFFFFF, 0.30)
        end
    end
end

local function drawButton(id, glyph, label, cx, cy, radius,
    background, foreground)
    local hovered = interaction.isHovered(id)
    local pressed = interaction.isPressed(id)
    local alpha = pressed and 1.0 or (hovered and 0.94 or 0.86)
    draw.circle(cx, cy, radius, background, alpha)
    local size = radius * 1.1
    draw.fa(glyph, cx - size / 2, cy - size / 2, size, foreground)
    interaction.region({
        key = id,
        shape = { type = "circle", x = cx, y = cy, radius = radius },
        cursor = "hand",
        events = {
            click = { id = id },
            doubleClick = { id = id },
            contextMenu = { id = "pomodoro.menu" },
        },
        accessibility = { role = "button", label = label },
    })
end

local function stateLabel(state)
    if state == "work" then
        return l10n.tr("lua_widget.pomodoro.state_work")
    elseif state == "break" then
        return l10n.tr("lua_widget.pomodoro.state_break")
    elseif state == "paused" then
        return l10n.tr("lua_widget.pomodoro.state_paused")
    end
    return l10n.tr("lua_widget.pomodoro.state_idle")
end

local function render()
    loadStyle()
    local config = loadConfig()
    local palette = getPalette()
    local width = layout.width()
    local height = layout.height()
    local contentHeight = math.max(1, height - layout.barHeight())
    local centerX = width / 2
    local state = getState()
    local remaining = remainingSeconds(config, nowSeconds())
    local accent = state == "break" and config.breakColor or config.workColor
    local sessionsInSet = getSessions() % config.longBreakInterval

    interaction.region({
        key = "pomodoro.surface",
        shape = {
            type = "rect", x = 0, y = 0,
            width = width, height = contentHeight,
        },
        events = {
            doubleClick = { id = "pomodoro.reset" },
            contextMenu = { id = "pomodoro.menu" },
        },
        accessibility = {
            role = "group",
            label = l10n.tr("lua_widget.pomodoro.name"),
        },
    })

    local rows = math.max(1, layout.rows())
    local function scale(value, minimum)
        return math.max(minimum or 0, layout.cu(value * rows))
    end
    local function font(value)
        return layout.fontCu(value * rows)
    end

    local ringThickness = scale(5, layout.cu(7))
    local labelFont = font(6)
    local timeFont = font(18)
    local dotRadius = scale(2.5)
    local dotGap = scale(8)
    local buttonRadius = scale(9)
    local buttonGap = scale(12)
    local gap = scale(5)
    local ringPadding = scale(18)

    local subline = ""
    if state == "work" then
        subline = l10n.tr("lua_widget.pomodoro.round_current",
            sessionsInSet + 1, config.longBreakInterval)
    elseif state == "break" then
        subline = l10n.tr("lua_widget.pomodoro.round_completed",
            sessionsInSet, config.longBreakInterval)
    end
    local label = stateLabel(state) .. subline
    local labelMetrics = draw.measureText(label, labelFont, 0, true)
    local timeText = formatTime(remaining)
    local timeMetrics = draw.measureText(timeText, timeFont, 0, true)
    local dotsHeight = dotRadius * 2 + gap
    local buttonsHeight = buttonRadius * 2 + gap
    local belowHeight = labelMetrics.height + gap + dotsHeight + gap +
        buttonsHeight
    local ringRadius = math.min(width, contentHeight - belowHeight) / 2 -
        ringPadding
    if ringRadius < scale(28) then
        ringRadius = math.min(width, contentHeight - belowHeight) / 2 -
            ringPadding / 2
    end
    if ringRadius <= 0 then return end

    local totalHeight = ringRadius * 2 + ringThickness * 2 + belowHeight
    local y = math.max(0, (contentHeight - totalHeight) / 2)
    local ringCenterY = y + ringRadius + ringThickness
    drawTrackRing(centerX, ringCenterY, ringRadius, ringThickness,
        palette.track, 0.5)
    drawProgressArc(centerX, ringCenterY, ringRadius,
        progress(config, remaining), ringThickness, accent)
    draw.text(centerX - timeMetrics.width / 2,
        ringCenterY - timeMetrics.height / 2, timeText, timeFont,
        palette.text, 0, true, true)

    y = ringCenterY + ringRadius + ringThickness + gap
    draw.text(centerX - labelMetrics.width / 2, y, label, labelFont,
        palette.muted)
    y = y + labelMetrics.height + gap
    if state ~= "paused" then
        drawDots(centerX, y + dotRadius, sessionsInSet,
            config.longBreakInterval, accent, dotRadius, dotGap)
    end

    local buttonY = y + dotsHeight + gap + buttonRadius
    local firstX = centerX - buttonRadius - buttonGap / 2
    local secondX = centerX + buttonRadius + buttonGap / 2
    if state == "idle" then
        drawButton("pomodoro.start", "",
            l10n.tr("lua_widget.pomodoro.start"), firstX, buttonY,
            buttonRadius, config.workColor, 0xFFFFFF)
        drawButton("pomodoro.reset", "",
            l10n.tr("lua_widget.pomodoro.reset_count"), secondX, buttonY,
            buttonRadius, palette.button, palette.text)
    elseif state == "paused" then
        drawButton("pomodoro.resume", "",
            l10n.tr("lua_widget.pomodoro.resume"), firstX, buttonY,
            buttonRadius, config.workColor, 0xFFFFFF)
        drawButton("pomodoro.stop", "",
            l10n.tr("lua_widget.pomodoro.stop"), secondX, buttonY,
            buttonRadius, palette.button, palette.text)
    else
        drawButton("pomodoro.pause", "",
            l10n.tr("lua_widget.pomodoro.pause"), firstX, buttonY,
            buttonRadius, palette.pause, 0xFFFFFF)
        drawButton("pomodoro.skip", "",
            l10n.tr("lua_widget.pomodoro.skip"), secondX, buttonY,
            buttonRadius, palette.button, palette.text)
    end
end

local function event(_context, model, value)
    if value.kind == "schedule" then
        if value.id == "visual" or value.id == "deadline" then
            reconcile(model, true)
        end
        return
    end
    if value.kind == "task.complete" then
        local key = tostring(value.taskId or "")
        if model.notificationTasks[key] then
            model.notificationTasks[key] = nil
            if not value.ok then
                widget.log("warn", "notification.show failed: " ..
                    tostring(value.error))
            end
        end
        return
    end
    if value.kind == "environment" then
        updateTitle()
        return
    end
    if value.kind == "action" then
        dispatchAction(model, value.id)
    end
end

local function menu(_context, _model, request)
    if request.id ~= "pomodoro.menu" then return nil end
    local state = getState()
    local items = {}
    if state == "idle" then
        items[#items + 1] = {
            id = "pomodoro.start",
            label = l10n.tr("lua_widget.pomodoro.start"),
            icon = fluent.play,
            iconFont = "fluent",
        }
    elseif state == "paused" then
        items[#items + 1] = {
            id = "pomodoro.resume",
            label = l10n.tr("lua_widget.pomodoro.resume"),
            icon = fluent.play,
            iconFont = "fluent",
        }
        items[#items + 1] = {
            id = "pomodoro.stop",
            label = l10n.tr("lua_widget.pomodoro.stop"),
            icon = fluent.stop,
            iconFont = "fluent",
        }
    else
        items[#items + 1] = {
            id = "pomodoro.pause",
            label = l10n.tr("lua_widget.pomodoro.pause"),
            icon = fluent.pause,
            iconFont = "fluent",
        }
        items[#items + 1] = {
            id = "pomodoro.skip",
            label = l10n.tr("lua_widget.pomodoro.skip"),
            icon = fluent.next,
            iconFont = "fluent",
        }
        items[#items + 1] = {
            id = "pomodoro.stop",
            label = l10n.tr("lua_widget.pomodoro.stop"),
            icon = fluent.stop,
            iconFont = "fluent",
        }
    end
    items[#items + 1] = { type = "separator" }
    items[#items + 1] = {
        id = "pomodoro.reset",
        label = l10n.tr("lua_widget.pomodoro.reset_count"),
        icon = fluent.reset,
        iconFont = "fluent",
    }
    return ui.menu(items)
end

local function migrateStorage(oldVersion, newVersion)
    if oldVersion >= 2 or newVersion < 2 then return end
    local state = getState()
    if state ~= "work" and state ~= "break" then
        storage.set("startedAtEpoch", "0")
        return
    end

    local oldStart = tonumber(storage.get("startTime"))
    local currentMilliseconds = time.now()
    local currentSeconds = math.floor(currentMilliseconds / 1000)
    if not oldStart or oldStart < 0 or oldStart >= 86400 then
        storage.set("startedAtEpoch", tostring(currentSeconds))
        return
    end
    local parts = time.parts(currentMilliseconds)
    local secondsSinceMidnight = parts.hour * 3600 + parts.min * 60 + parts.sec
    local elapsed = secondsSinceMidnight - oldStart
    if elapsed < 0 then elapsed = elapsed + 86400 end
    storage.set("startedAtEpoch", tostring(currentSeconds - elapsed))
end

descriptor = {
    name = l10n.tr("lua_widget.pomodoro.name"),
    useCustomStyle = true,
    followPersonalizationDefault = true,
    bottomBarHover = false,
    bg = 0x151A21,
    border = 0xFFFFFF,
    alpha = 0.42,
    gradientEndA = 0.30,
    settings = settings,
    setup = setup,
    render = render,
    event = event,
    menu = menu,
    migrateStorage = migrateStorage,
}

return widget.define(descriptor)
