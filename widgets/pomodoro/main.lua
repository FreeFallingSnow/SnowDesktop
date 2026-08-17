-- pomodoro/main.lua - API v2 Pomodoro timer with background deadlines
local DEFAULT_WORK_COLOR = 0xFF6347
local DEFAULT_BREAK_COLOR = 0x4ECDC4

local fa = {
    play = utf8.char(0xF04B),
    pause = utf8.char(0xF04C),
    stop = utf8.char(0xF04D),
    next = utf8.char(0xF050),
    reset = utf8.char(0xF0E2),
}

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
    local background = descriptor and descriptor.bg or 0x151A21
    local red = math.floor(background / 0x10000) % 0x100
    local green = math.floor(background / 0x100) % 0x100
    local blue = background % 0x100
    local isLight = red * 299 + green * 587 + blue * 114 >= 160000
    if isLight then
        return {
            text = 0x162033,
            track = 0xD8DEE8,
            timerSurface = 0xFFFFFF,
            timerBorder = 0xE2E8F0,
            controlSurface = 0xEEF2F7,
            controlBorder = 0xCBD5E1,
            button = 0xDCE4EE,
            dot = 0xA8B3C4,
            pause = 0xD97706,
        }
    end
    return {
        text = 0xF8FAFC,
        track = 0x263244,
        timerSurface = 0x111927,
        timerBorder = 0x263244,
        controlSurface = 0x202B3B,
        controlBorder = 0x34445A,
        button = 0x2A374B,
        dot = 0x66758A,
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
    local phase = state
    if state == "paused" then
        phase = storage.get("prevState") or "work"
        if phase ~= "work" and phase ~= "break" then phase = "work" end
    end
    local target = targetForState(phase, config)
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

local function buildView(context)
    loadStyle()
    local config = loadConfig()
    local palette = getPalette()
    local width = math.max(1, context.layoutSize.width)
    local contentHeight = math.max(1, context.layoutSize.height)
    local state = getState()
    local remaining = remainingSeconds(config, nowSeconds())
    local activePhase = state
    if state == "paused" then
        activePhase = storage.get("prevState") or "work"
    end
    if activePhase ~= "break" then activePhase = "work" end
    local accent = activePhase == "break" and
        config.breakColor or config.workColor
    local sessionsInSet = getSessions() % config.longBreakInterval

    local padding = math.max(layout.cu(4), math.min(
        layout.cu(10), layout.vmin(2.5)))
    local availableWidth = math.max(1, width - padding * 2)
    local availableHeight = math.max(1, contentHeight - padding * 2)
    local isWide = availableWidth >= availableHeight * 1.35
    local verticalGap = math.max(layout.cu(4), math.min(
        layout.cu(7), layout.vh(1.8)))
    local statusGap = math.max(layout.cu(1.5), math.min(
        layout.cu(3), layout.vmin(0.8)))
    local majorGap = math.max(layout.cu(8), math.min(
        layout.cu(20), layout.vmin(5)))
    local buttonDiameter = math.max(layout.cu(30), math.min(
        layout.cu(52), layout.vmin(16)))
    local buttonRadius = buttonDiameter / 2
    local buttonGap = math.max(layout.cu(5), buttonDiameter * 0.16)
    local dockPadding = math.max(layout.cu(3), buttonDiameter * 0.08)
    local actionHeight = buttonDiameter + dockPadding * 2
    local actionsWidth = buttonDiameter * 2 + buttonGap + dockPadding * 2
    local dotDiameter = math.max(layout.cu(4), math.min(
        layout.cu(8), buttonDiameter * 0.14))
    local dotGap = math.max(layout.cu(3), dotDiameter * 0.65)
    local labelFont = math.max(layout.fontCu(10), math.min(
        layout.fontCu(18), layout.vmin(5.2)))
    local labelHeight = labelFont * 1.25
    local statusHeight = labelHeight + statusGap + dotDiameter

    local ringDiameter
    if isWide then
        ringDiameter = math.max(layout.cu(64), math.min(
            availableHeight * 0.94,
            availableWidth * 0.52))
    else
        local reservedHeight = statusHeight + actionHeight + verticalGap * 2
        ringDiameter = math.max(layout.cu(52), math.min(
            availableWidth * 0.90,
            availableHeight - reservedHeight))
    end

    local subline = ""
    if state == "work" then
        subline = l10n.tr("lua_widget.pomodoro.round_current",
            sessionsInSet + 1, config.longBreakInterval)
    elseif state == "break" then
        subline = l10n.tr("lua_widget.pomodoro.round_completed",
            sessionsInSet, config.longBreakInterval)
    end
    local label = stateLabel(state) .. subline
    local timeText = formatTime(remaining)
    local ringThickness = math.max(layout.cu(4), math.min(
        layout.cu(12), ringDiameter * 0.055))
    local timeFont = math.max(layout.fontCu(20), math.min(
        layout.fontCu(64), ringDiameter * 0.27))

    local dots = {}
    for index = 1, config.longBreakInterval do
        dots[#dots + 1] = view.shape({
            key = "pomodoro.round." .. tostring(index),
            shape = "circle",
            width = dotDiameter,
            height = dotDiameter,
            style = {
                background = index <= sessionsInSet and accent or palette.dot,
            },
        })
    end

    local function iconButton(id, glyph, accessibilityLabel,
        background, foreground, borderColor)
        return view.iconButton({
            key = id,
            glyph = glyph,
            iconFont = "fa",
            width = buttonDiameter,
            height = buttonDiameter,
            fontSize = buttonDiameter * 0.44,
            action = { id = id },
            events = {
                contextMenu = { id = "pomodoro.menu", scope = "component" },
            },
            accessibility = {
                role = "button",
                label = accessibilityLabel,
            },
            style = {
                background = background,
                foreground = foreground,
                cornerRadius = buttonRadius,
                borderColor = borderColor,
                borderWidth = borderColor and 1 or 0,
            },
            hoverStyle = { opacity = 0.90 },
            pressedStyle = { opacity = 0.78 },
        })
    end

    local buttons = {}
    if state == "idle" then
        buttons[1] = iconButton("pomodoro.start", fa.play,
            l10n.tr("lua_widget.pomodoro.start"),
            config.workColor, 0xFFFFFF)
        buttons[2] = iconButton("pomodoro.reset", fa.reset,
            l10n.tr("lua_widget.pomodoro.reset_count"),
            palette.button, palette.text, palette.controlBorder)
    elseif state == "paused" then
        buttons[1] = iconButton("pomodoro.resume", fa.play,
            l10n.tr("lua_widget.pomodoro.resume"),
            accent, 0xFFFFFF)
        buttons[2] = iconButton("pomodoro.stop", fa.stop,
            l10n.tr("lua_widget.pomodoro.stop"),
            palette.button, palette.text, palette.controlBorder)
    else
        buttons[1] = iconButton("pomodoro.pause", fa.pause,
            l10n.tr("lua_widget.pomodoro.pause"),
            palette.pause, 0xFFFFFF)
        buttons[2] = iconButton("pomodoro.skip", fa.next,
            l10n.tr("lua_widget.pomodoro.skip"),
            palette.button, palette.text, palette.controlBorder)
    end

    local timer = view.stack({
        key = "pomodoro.timer",
        width = ringDiameter,
        height = ringDiameter,
        style = {
            background = palette.timerSurface,
            borderColor = palette.timerBorder,
            borderWidth = 1,
            cornerRadius = ringDiameter / 2,
        },
        children = {
            view.progressRing({
                key = "pomodoro.progress",
                width = "fill",
                height = "fill",
                value = progress(config, remaining),
                thickness = ringThickness,
                trackOpacity = 0.82,
                style = {
                    background = palette.track,
                    foreground = accent,
                },
                accessibility = {
                    role = "progressbar",
                    label = label,
                },
            }),
            view.text({
                key = "pomodoro.time",
                text = timeText,
                width = "fill",
                height = "fill",
                fontSize = timeFont,
                bold = true,
                textAlign = "center",
                style = { foreground = palette.text },
            }),
        },
    })
    local status = view.text({
        key = "pomodoro.state",
        text = stateLabel(state),
        width = "fill",
        height = labelHeight,
        fontSize = labelFont,
        bold = true,
        textAlign = "center",
        style = { foreground = accent },
    })
    local rounds = view.row({
        key = "pomodoro.rounds",
        width = "fill",
        height = dotDiameter,
        gap = dotGap,
        alignItems = "center",
        justifyContent = "center",
        children = dots,
    })
    local statusBlock = view.column({
        key = "pomodoro.status",
        width = "fill",
        height = statusHeight,
        gap = statusGap,
        alignItems = "center",
        justifyContent = "center",
        children = { status, rounds },
    })
    local actions = view.row({
        key = "pomodoro.actions",
        width = actionsWidth,
        height = actionHeight,
        padding = dockPadding,
        gap = buttonGap,
        alignItems = "center",
        justifyContent = "center",
        style = {
            background = palette.controlSurface,
            borderColor = palette.controlBorder,
            borderWidth = 1,
            cornerRadius = actionHeight / 2,
        },
        children = buttons,
    })

    local content
    local rootGap
    if isWide then
        local sideWidth = math.max(actionsWidth,
            availableWidth - ringDiameter - majorGap)
        content = {
            timer,
            view.column({
                key = "pomodoro.details",
                width = sideWidth,
                height = ringDiameter,
                gap = verticalGap,
                alignItems = "center",
                justifyContent = "center",
                children = { statusBlock, actions },
            }),
        }
        rootGap = majorGap
    else
        content = { timer, statusBlock, actions }
        rootGap = verticalGap
    end

    local root = isWide and view.row or view.column
    return root({
        key = "pomodoro.surface",
        width = "fill",
        height = "fill",
        padding = padding,
        gap = rootGap,
        alignItems = "center",
        justifyContent = "center",
        events = {
            doubleClick = { id = "pomodoro.reset" },
            contextMenu = { id = "pomodoro.menu", scope = "component" },
        },
        accessibility = {
            role = "group",
            label = l10n.tr("lua_widget.pomodoro.name"),
        },
        children = content,
    })
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
    view = buildView,
    event = event,
    menu = menu,
    migrateStorage = migrateStorage,
}

return widget.define(descriptor)
