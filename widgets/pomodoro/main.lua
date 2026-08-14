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
    local width = math.max(1, context.logicalSize.width)
    local contentHeight = math.max(1,
        context.logicalSize.height - layout.barHeight())
    local state = getState()
    local remaining = remainingSeconds(config, nowSeconds())
    local accent = state == "break" and config.breakColor or config.workColor
    local sessionsInSet = getSessions() % config.longBreakInterval

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
    local padding = scale(8)

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
    local dotDiameter = dotRadius * 2
    local buttonDiameter = buttonRadius * 2
    local labelHeight = labelFont * 1.4
    local reservedHeight = labelHeight + dotDiameter + buttonDiameter +
        gap * 3 + padding * 2
    local ringDiameter = math.max(scale(48), math.min(
        width - padding * 2, contentHeight - reservedHeight))

    local dots = {}
    for index = 1, config.longBreakInterval do
        dots[#dots + 1] = view.shape({
            key = "pomodoro.round." .. tostring(index),
            shape = "circle",
            width = dotDiameter,
            height = dotDiameter,
            visible = state ~= "paused",
            style = {
                background = index <= sessionsInSet and accent or 0xFFFFFF,
                opacity = index <= sessionsInSet and 1.0 or 0.30,
            },
        })
    end

    local function iconButton(id, glyph, accessibilityLabel,
        background, foreground)
        return view.iconButton({
            key = id,
            glyph = glyph,
            iconFont = "fa",
            width = buttonDiameter,
            height = buttonDiameter,
            fontSize = buttonRadius * 1.1,
            action = { id = id },
            events = {
                doubleClick = { id = id },
                contextMenu = { id = "pomodoro.menu" },
            },
            accessibility = {
                role = "button",
                label = accessibilityLabel,
            },
            style = {
                background = background,
                foreground = foreground,
                cornerRadius = buttonRadius,
                opacity = 0.86,
            },
            hoverStyle = { opacity = 0.94 },
            pressedStyle = { opacity = 1.0 },
        })
    end

    local buttons = {}
    if state == "idle" then
        buttons[1] = iconButton("pomodoro.start", "",
            l10n.tr("lua_widget.pomodoro.start"),
            config.workColor, 0xFFFFFF)
        buttons[2] = iconButton("pomodoro.reset", "",
            l10n.tr("lua_widget.pomodoro.reset_count"),
            palette.button, palette.text)
    elseif state == "paused" then
        buttons[1] = iconButton("pomodoro.resume", "",
            l10n.tr("lua_widget.pomodoro.resume"),
            config.workColor, 0xFFFFFF)
        buttons[2] = iconButton("pomodoro.stop", "",
            l10n.tr("lua_widget.pomodoro.stop"),
            palette.button, palette.text)
    else
        buttons[1] = iconButton("pomodoro.pause", "",
            l10n.tr("lua_widget.pomodoro.pause"),
            palette.pause, 0xFFFFFF)
        buttons[2] = iconButton("pomodoro.skip", "",
            l10n.tr("lua_widget.pomodoro.skip"),
            palette.button, palette.text)
    end

    return view.column({
        key = "pomodoro.surface",
        width = "fill",
        height = "fill",
        padding = padding,
        gap = gap,
        alignItems = "center",
        justifyContent = "center",
        events = {
            doubleClick = { id = "pomodoro.reset" },
            contextMenu = { id = "pomodoro.menu" },
        },
        accessibility = {
            role = "group",
            label = l10n.tr("lua_widget.pomodoro.name"),
        },
        children = {
            view.stack({
                key = "pomodoro.timer",
                width = ringDiameter,
                height = ringDiameter,
                children = {
                    view.progressRing({
                        key = "pomodoro.progress",
                        width = "fill",
                        height = "fill",
                        value = progress(config, remaining),
                        thickness = ringThickness,
                        trackOpacity = 0.5,
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
            }),
            view.text({
                key = "pomodoro.state",
                text = label,
                width = "fill",
                height = labelHeight,
                fontSize = labelFont,
                textAlign = "center",
                style = { foreground = palette.muted },
            }),
            view.row({
                key = "pomodoro.rounds",
                height = dotDiameter,
                gap = math.max(0, dotGap - dotDiameter),
                alignItems = "center",
                justifyContent = "center",
                children = dots,
            }),
            view.row({
                key = "pomodoro.actions",
                height = buttonDiameter,
                gap = buttonGap,
                alignItems = "center",
                justifyContent = "center",
                children = buttons,
            }),
        },
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
