local player = module.require("modules/player.lua")

local mediaCurrent
local mediaArtwork
local mediaTimeline
local audioAnalysis
local RECORD_FRAME = "aurora.record.spin"
local PROGRESS_TICK = "aurora.timeline.tick"
local PROGRESS_TICK_MS = 100

local glyphs = {
    previous = utf8.char(0xF048),
    play = utf8.char(0xF04B),
    pause = utf8.char(0xF04C),
    next = utf8.char(0xF051),
    music = utf8.char(0xF001),
}

local function settingEnabled(key)
    local value = storage.get(key)
    return value == true or value == 1 or value == "1" or value == "true"
end

local settings = {
    fields = {
        {
            key = "show_visualizer",
            label = l10n.tr("workshop.aurora_player.visualizer"),
            description = l10n.tr(
                "workshop.aurora_player.visualizer_description"),
            type = "bool",
            default = false,
        },
    },
}

local function previewSession()
    return {
        id = "aurora-preview",
        sourceName = "Aurora Player",
        title = "Aurora Nights",
        artist = "Mira Vale",
        album = "Glass Horizons",
        playbackStatus = "playing",
        controls = {
            canPlay = true,
            canPause = true,
            canPlayPause = true,
            canStop = true,
            canNext = true,
            canPrevious = true,
            canSeek = true,
            canChangePlaybackRate = true,
            canToggleShuffle = true,
            canChangeRepeatMode = true,
        },
        timeline = {
            sessionId = "aurora-preview",
            positionMs = 154000,
            durationMs = 238000,
            minimumSeekMs = 0,
            maximumSeekMs = 238000,
            updatedAtMs = 0,
        },
    }
end

local function currentSession(model)
    if model and model.preview then return previewSession() end
    return player.session(mediaCurrent and mediaCurrent:value() or nil)
end

local function currentArtwork(model, session)
    if not session or (model and model.preview) then return nil end
    return player.artwork(mediaArtwork and mediaArtwork:value() or nil,
        session.id)
end

local function currentTimeline(model, session)
    if not session then return nil end
    if model and model.preview then return session.timeline end
    return player.timeline(mediaTimeline and mediaTimeline:value() or nil,
        session)
end

local function setup(context)
    mediaCurrent = data.subscribe("media.current", {
        maxAgeMs = 500,
        whenHidden = "throttle",
    })
    mediaArtwork = data.subscribe("media.artwork", {
        maxAgeMs = 500,
        whenHidden = "throttle",
    })
    mediaTimeline = data.subscribe("media.timeline", {
        maxAgeMs = 250,
        whenHidden = "throttle",
    })

    local analyze = player.shouldAnalyze(
        settingEnabled("show_visualizer"),
        widget.hasFeature("data.audio.output.analysis") and
            widget.hasFeature("view.dataSeries"),
        widget.hasPermission("audio.output.analyze"),
        context.accessibility.reducedMotion)
    if analyze then
        audioAnalysis = data.subscribe("audio.output.analysis", {
            features = { "spectrum" },
            spectrumBins = 48,
            updateHz = 30,
            whenHidden = "pause",
        })
    end

    return {
        preview = context.preview == true,
        visualizer = analyze or context.preview == true,
        tasks = {},
        pendingPlayback = nil,
        seekPreview = nil,
        seekSessionId = nil,
        reducedMotion = context.accessibility.reducedMotion == true,
        visible = true,
        recordRotation = 0,
        recordFramePending = false,
        progressTicking = false,
    }
end

local function updateProgressTicker(model, active)
    if active and not model.progressTicking then
        model.progressTicking = schedule.every(PROGRESS_TICK,
            PROGRESS_TICK_MS, { whenHidden = "pause" }) == true
    elseif not active and model.progressTicking then
        schedule.cancel(PROGRESS_TICK)
        model.progressTicking = false
    end
end

local function palette(context)
    if context.accessibility.highContrast then
        local light = context.theme.mode == "light"
        return {
            primary = light and 0x000000 or 0xFFFFFF,
            secondary = light and 0x202020 or 0xE5E7EB,
            subtle = light and 0x404040 or 0xCBD5E1,
            button = light and 0x000000 or 0xFFFFFF,
            buttonText = light and 0xFFFFFF or 0x070A16,
            secondaryButton = light and 0xFFFFFF or 0x111827,
            secondaryButtonText = light and 0x000000 or 0xFFFFFF,
            disabled = light and 0x777777 or 0x6B7280,
            cover = light and 0xE5E5E5 or 0x202020,
            record = light and 0x000000 or 0xFFFFFF,
            groove = light and 0xFFFFFF or 0x000000,
        }
    end
    local theme = widget.theme()
    if theme and theme.contentTheme == 1 then
        return {
            primary = 0x111827,
            secondary = 0x334155,
            subtle = 0x475569,
            button = 0x111827,
            buttonText = 0xFFFFFF,
            secondaryButton = 0xFFFFFF,
            secondaryButtonText = 0x111827,
            disabled = 0x94A3B8,
            cover = 0x4338CA,
            record = 0x111827,
            groove = 0x94A3B8,
        }
    end
    return {
        primary = 0xFFFFFF,
        secondary = 0xE2E8F0,
        subtle = 0xB8C3D8,
        button = 0xFFFFFF,
        buttonText = 0x11142A,
        secondaryButton = 0x111827,
        secondaryButtonText = 0xFFFFFF,
        disabled = 0x718096,
        cover = 0x3730A3,
        record = 0x080A0F,
        groove = 0x64748B,
    }
end

local function backgroundLayer(context, model)
    local width = layout.width()
    local height = layout.height()
    if context.accessibility.highContrast then
        return
    end

    local session = currentSession(model)
    local artwork = currentArtwork(model, session)
    if not artwork then return end
    draw.imageFit(artwork, 0, 0, width, height,
        "cover", "center", 0.92, "linear")
    draw.gradientRect(0, 0, width, height,
        0x070A17, 0x11152A, "horizontal", 0, 0.62)
    draw.gradientRect(0, height * 0.38, width, height * 0.62,
        0x11152A, 0x050713, "vertical", 0, 0.36)
end

local function startTask(model, name, arguments, pendingPlayback)
    if model.preview or not widget.hasPermission("media.action") then
        return false
    end
    local taskId, err = task.start(name, arguments)
    if not taskId then
        widget.log("warn", name .. " rejected: " .. tostring(err))
        return false
    end
    model.tasks[tostring(taskId)] = name
    if pendingPlayback then model.pendingPlayback = pendingPlayback end
    return true
end

local function startSessionTask(model, name, capability, extra)
    local session = currentSession(model)
    if not player.canControl(session,
        widget.hasPermission("media.action"), capability) then
        return
    end
    local arguments = extra or {}
    arguments.sessionId = session.id
    startTask(model, name, arguments)
end

local function controlButton(key, glyph, label, enabled, colors, primary)
    local size = layout.cu(primary and 42 or 36)
    return view.iconButton({
        key = key,
        glyph = glyph,
        iconFont = "fa",
        width = size,
        height = size,
        fontSize = layout.fontCu(primary and 16 or 13),
        enabled = enabled,
        action = { id = key },
        accessibility = { role = "button", label = label },
        style = {
            background = primary and colors.button or colors.secondaryButton,
            foreground = primary and colors.buttonText or
                colors.secondaryButtonText,
            cornerRadius = size * 0.5,
            opacity = primary and 0.96 or 0.78,
        },
        hoverStyle = { opacity = primary and 0.86 or 0.92 },
        pressedStyle = { opacity = primary and 0.72 or 0.64 },
        disabledStyle = {
            background = colors.secondaryButton,
            foreground = colors.disabled,
            opacity = 0.46,
        },
    })
end

local function recordCircle(key, size, style)
    return view.shape({
        key = key,
        shape = "circle",
        width = size,
        height = size,
        alignSelf = "center",
        style = style,
    })
end

local function coverNode(artwork, size, colors, rotation)
    local labelSize = size * 0.64
    local children = {
        recordCircle("aurora.record.disc", size, {
            background = colors.record,
            borderColor = colors.groove,
            borderWidth = layout.cu(1),
            opacity = 0.98,
        }),
        recordCircle("aurora.record.groove.outer", size * 0.84, {
            borderColor = colors.groove,
            borderWidth = layout.cu(1),
            opacity = 0.28,
        }),
        recordCircle("aurora.record.groove.inner", size * 0.74, {
            borderColor = colors.groove,
            borderWidth = layout.cu(1),
            opacity = 0.16,
        }),
    }
    if artwork then
        children[#children + 1] = view.image({
            key = "aurora.record.artwork",
            source = artwork,
            alt = "",
            fit = "cover",
            width = labelSize,
            height = labelSize,
            alignSelf = "center",
            transform = {
                rotate = rotation,
                originX = 0.5,
                originY = 0.5,
            },
            style = {
                cornerRadius = labelSize * 0.5,
            },
        })
    else
        children[#children + 1] = view.column({
            key = "aurora.record.placeholder",
            width = labelSize,
            height = labelSize,
            alignSelf = "center",
            alignItems = "center",
            justifyContent = "center",
            style = {
                background = colors.cover,
                borderColor = colors.groove,
                borderWidth = layout.cu(1),
                cornerRadius = labelSize * 0.5,
                opacity = 0.92,
            },
            children = {
                view.icon({
                    key = "aurora.record.placeholder.icon",
                    glyph = glyphs.music,
                    iconFont = "fa",
                    width = "fill",
                    height = "fill",
                    fontSize = layout.fontCu(27),
                    textAlign = "center",
                    verticalAlign = "center",
                    style = { foreground = 0xFFFFFF, opacity = 0.86 },
                    accessibility = { hidden = true },
                }),
            },
        })
    end
    return view.stack({
        key = "aurora.record",
        width = size,
        height = size,
        children = children,
    })
end

local function visualizerNode(model, colors)
    if not model.visualizer or not widget.hasFeature("view.dataSeries") then
        return nil
    end
    local snapshot = audioAnalysis and audioAnalysis:value() or nil
    return view.spectrum({
        key = "aurora.visualizer",
        values = player.spectrum(snapshot, 36, model.preview),
        width = "fill",
        height = "fill",
        padding = { left = layout.cu(10), right = layout.cu(10),
            top = layout.cu(36), bottom = layout.cu(8) },
        min = 0,
        max = 1,
        fillOpacity = 0.26,
        style = { foreground = colors.primary, opacity = 0.68 },
        accessibility = {
            label = l10n.tr("workshop.aurora_player.visualizer"),
            hidden = true,
        },
    })
end

local function viewTree(context, model)
    model.reducedMotion = context.accessibility.reducedMotion == true
    local colors = palette(context)
    local session = currentSession(model)
    local artwork = currentArtwork(model, session)
    local timeline = currentTimeline(model, session)
    local canAct = model.preview or widget.hasPermission("media.action")
    local controls = session and session.controls or {}
    local playing = session and session.playbackStatus == "playing"
    if model.pendingPlayback == "playing" then playing = true end
    if model.pendingPlayback == "paused" then playing = false end
    local spinRecord = player.shouldSpinRecord(artwork ~= nil,
        playing and "playing" or "paused", model.reducedMotion,
        model.visible, model.preview)
    if spinRecord and not model.recordFramePending then
        local accepted = animation.requestFrame(RECORD_FRAME)
        model.recordFramePending = accepted == true
    elseif not spinRecord and model.recordFramePending then
        animation.cancelFrame(RECORD_FRAME)
        model.recordFramePending = false
    end

    local title = session and session.title ~= "" and session.title or
        l10n.tr("workshop.aurora_player.empty")
    local artist = session and session.artist ~= "" and session.artist or
        (session and session.sourceName or
            l10n.tr("workshop.aurora_player.empty_hint"))
    local album = session and session.album or ""
    local coverSize = math.max(layout.cu(112), math.min(
        layout.cu(148), layout.height() - layout.cu(36)))
    if not session or model.seekSessionId ~= session.id then
        model.seekPreview = nil
        model.seekSessionId = nil
    end
    local hasTimeline = player.hasTimeline(timeline)
    if not hasTimeline then
        model.seekPreview = nil
        model.seekSessionId = nil
    end
    updateProgressTicker(model, hasTimeline and playing and model.visible and
        not model.preview and not model.recordFramePending)
    local position = model.seekPreview and
        player.seekPosition(timeline, model.seekPreview) or
        player.position(timeline, playing, hasTimeline and time.now() or nil)
    local progress = model.seekPreview or player.progress(timeline, position)
    local duration = player.duration(timeline)
    local seekEnabled = session ~= nil and hasTimeline and canAct and
        controls.canSeek == true

    local controlRow = view.row({
        key = "aurora.controls",
        width = "fill",
        height = layout.cu(46),
        gap = layout.cu(12),
        alignItems = "center",
        justifyContent = "center",
        children = {
            controlButton("media.previous", glyphs.previous,
                l10n.tr("workshop.aurora_player.previous"),
                session ~= nil and canAct and controls.canPrevious == true,
                colors, false),
            controlButton("media.toggle", playing and glyphs.pause or
                glyphs.play, l10n.tr(playing and
                    "workshop.aurora_player.pause" or
                    "workshop.aurora_player.play"),
                session ~= nil and canAct and controls.canPlayPause == true,
                colors, true),
            controlButton("media.next", glyphs.next,
                l10n.tr("workshop.aurora_player.next"),
                session ~= nil and canAct and controls.canNext == true,
                colors, false),
        },
    })

    local progressRow = hasTimeline and view.row({
        key = "aurora.progress.row",
        width = "fill",
        height = layout.cu(28),
        gap = layout.cu(8),
        alignItems = "center",
        children = {
            view.text({
                key = "aurora.progress.current",
                text = player.formatTime(position),
                width = layout.cu(42),
                height = "fill",
                fontSize = layout.fontCu(10),
                textAlign = "end",
                style = { foreground = colors.subtle },
            }),
            view.slider({
                key = "aurora.progress",
                value = progress,
                min = 0,
                max = 1,
                step = 0.001,
                width = "fill",
                height = layout.cu(24),
                enabled = seekEnabled,
                events = {
                    change = { id = "media.seek.change" },
                    pointerUp = { id = "media.seek.commit" },
                },
                accessibility = {
                    label = l10n.tr(
                        "workshop.aurora_player.progress"),
                    value = player.formatTime(position) .. " / " ..
                        player.formatTime(duration),
                },
                style = { foreground = colors.primary },
                disabledStyle = { opacity = 0.40 },
            }),
            view.text({
                key = "aurora.progress.duration",
                text = player.formatTime(duration),
                width = layout.cu(42),
                height = "fill",
                fontSize = layout.fontCu(10),
                style = { foreground = colors.subtle },
            }),
        },
    }) or nil

    local detailChildren = {
        view.text({
            key = "aurora.title",
            text = title,
            width = "fill",
            height = layout.cu(31),
            fontSize = layout.fontCu(19),
            fontWeight = 700,
            textWrap = "noWrap",
            overflowText = "ellipsis",
            style = { foreground = colors.primary },
            accessibility = { headingLevel = 2 },
        }),
        view.text({
            key = "aurora.artist",
            text = artist,
            width = "fill",
            height = layout.cu(session and 22 or 38),
            fontSize = layout.fontCu(session and 12 or 11),
            textWrap = session and "noWrap" or "wrap",
            maxLines = session and 1 or 2,
            overflowText = "ellipsis",
            style = { foreground = colors.secondary },
        }),
    }
    if session then
        detailChildren[#detailChildren + 1] = view.text({
            key = "aurora.album",
            text = album,
            width = "fill",
            height = layout.cu(20),
            fontSize = layout.fontCu(10),
            textWrap = "noWrap",
            overflowText = "ellipsis",
            style = { foreground = colors.subtle },
        })
        detailChildren[#detailChildren + 1] = controlRow
        if progressRow then
            detailChildren[#detailChildren + 1] = progressRow
        end
    end

    local details = view.column({
        key = "aurora.details",
        width = "fill",
        height = coverSize,
        gap = session and layout.cu(2) or layout.cu(6),
        justifyContent = "center",
        children = detailChildren,
    })

    local content = view.row({
        key = "aurora.content",
        width = "fill",
        height = "fill",
        padding = layout.cu(18),
        gap = layout.cu(20),
        alignItems = "center",
        children = {
            coverNode(artwork, coverSize, colors, model.recordRotation),
            details,
        },
    })

    local children = {}
    local visualizer = visualizerNode(model, colors)
    if visualizer then children[#children + 1] = visualizer end
    children[#children + 1] = content
    return view.stack({
        key = "aurora.surface",
        width = "fill",
        height = "fill",
        events = {
            contextMenu = { id = "media.menu", scope = "component" },
        },
        accessibility = {
            role = "group",
            label = l10n.tr("workshop.aurora_player.name"),
        },
        children = children,
    })
end

local function seekRelative(model, delta)
    local session = currentSession(model)
    if not player.canControl(session,
        widget.hasPermission("media.action"), "canSeek") then return end
    local timeline = currentTimeline(model, session)
    if not player.hasTimeline(timeline) then return end
    local minimum = tonumber(timeline.minimumSeekMs) or 0
    local maximum = tonumber(timeline.maximumSeekMs) or
        tonumber(timeline.durationMs) or minimum
    local position = player.clamp(
        (tonumber(timeline.positionMs) or minimum) + delta,
        minimum, maximum, minimum)
    startTask(model, "media.seek", {
        sessionId = session.id,
        positionMs = math.floor(position + 0.5),
    })
end

local function commitSeek(model, session, timeline, fraction)
    if not player.canControl(session,
        widget.hasPermission("media.action"), "canSeek") or
        not player.hasTimeline(timeline) then
        model.seekPreview = nil
        model.seekSessionId = nil
        widget.invalidate()
        return
    end
    local normalized = player.clamp(fraction, 0, 1, 0)
    model.seekPreview = normalized
    model.seekSessionId = session.id
    local started = startTask(model, "media.seek", {
        sessionId = session.id,
        positionMs = player.seekPosition(timeline, normalized),
    })
    if not started then
        model.seekPreview = nil
        model.seekSessionId = nil
    end
    widget.invalidate()
end

local function event(_context, model, value)
    if value.kind == "visibility" then
        model.visible = value.visible == true
        if not model.visible then
            animation.cancelFrame(RECORD_FRAME)
            model.recordFramePending = false
        else
            widget.invalidate()
        end
        return
    end
    if value.kind == "frame" and value.id == RECORD_FRAME then
        model.recordFramePending = false
        local session = currentSession(model)
        local artwork = currentArtwork(model, session)
        local playing = session and session.playbackStatus == "playing"
        if model.pendingPlayback == "playing" then playing = true end
        if model.pendingPlayback == "paused" then playing = false end
        if player.shouldSpinRecord(artwork ~= nil,
                playing and "playing" or "paused", model.reducedMotion,
                model.visible, model.preview) then
            model.recordRotation = player.advanceRecordRotation(
                model.recordRotation, value.deltaMs)
            local accepted = animation.requestFrame(RECORD_FRAME)
            model.recordFramePending = accepted == true
        end
        widget.invalidate()
        return
    end
    if value.kind == "schedule" and value.id == PROGRESS_TICK then
        if model.visible then widget.invalidate() end
        return
    end
    if value.kind == "task.complete" then
        local name, failed = player.finishTask(
            model.tasks, value.taskId, value.ok)
        if name then model.pendingPlayback = nil end
        if name == "media.seek" then
            model.seekPreview = nil
            model.seekSessionId = nil
            widget.invalidate()
        end
        if failed then
            widget.log("warn", name .. " failed: " ..
                tostring(value.error))
        end
        return
    end
    if value.kind ~= "action" then return end

    local session = currentSession(model)
    if value.id == "media.previous" then
        startSessionTask(model, "media.previous", "canPrevious")
    elseif value.id == "media.next" then
        startSessionTask(model, "media.next", "canNext")
    elseif value.id == "media.toggle" then
        if not player.canControl(session,
            widget.hasPermission("media.action"), "canPlayPause") then return end
        local playing = session.playbackStatus == "playing"
        startTask(model, "media.toggle", { sessionId = session.id },
            playing and "paused" or "playing")
    elseif value.id == "media.stop" then
        startSessionTask(model, "media.stop", "canStop")
    elseif value.id == "media.back10" then
        seekRelative(model, -10000)
    elseif value.id == "media.forward10" then
        seekRelative(model, 10000)
    elseif value.id == "media.seek.change" then
        if not player.canControl(session,
            widget.hasPermission("media.action"), "canSeek") then return end
        local timeline = currentTimeline(model, session)
        if not player.hasTimeline(timeline) then return end
        local fraction = player.clamp(value.controlValue, 0, 1, 0)
        model.seekPreview = fraction
        model.seekSessionId = session.id
        widget.invalidate()
        if value.source ~= "pointer" then
            commitSeek(model, session, timeline, fraction)
        end
    elseif value.id == "media.seek.commit" then
        if model.seekPreview == nil or model.seekSessionId ~=
            (session and session.id or nil) then return end
        commitSeek(model, session, currentTimeline(model, session),
            model.seekPreview)
    elseif value.id:sub(1, 11) == "media.rate." then
        if not player.canControl(session,
            widget.hasPermission("media.action"),
            "canChangePlaybackRate") then return end
        local rate = tonumber(value.id:sub(12):gsub("_", "."))
        if rate then startTask(model, "media.setRate", {
            sessionId = session.id, rate = rate,
        }) end
    elseif value.id == "media.shuffle.on" or
        value.id == "media.shuffle.off" then
        if not player.canControl(session,
            widget.hasPermission("media.action"),
            "canToggleShuffle") then return end
        startTask(model, "media.setShuffle", {
            sessionId = session.id,
            shuffle = value.id == "media.shuffle.on",
        })
    elseif value.id:sub(1, 13) == "media.repeat." then
        if not player.canControl(session,
            widget.hasPermission("media.action"),
            "canChangeRepeatMode") then return end
        startTask(model, "media.setRepeat", {
            sessionId = session.id,
            mode = value.id:sub(14),
        })
    end
end

local function menu(_context, model, request)
    if request.id ~= "media.menu" then return nil end
    local session = currentSession(model)
    local timeline = currentTimeline(model, session)
    local permission = widget.hasPermission("media.action")
    local function allowed(capability)
        if capability == "canSeek" and
            not player.hasTimeline(timeline) then return false end
        return player.canControl(session, permission, capability)
    end
    local rateItems = {}
    for _, rate in ipairs({ "0_5", "0_75", "1", "1_25", "1_5", "2" }) do
        rateItems[#rateItems + 1] = {
            id = "media.rate." .. rate,
            label = rate:gsub("_", ".") .. "×",
            enabled = allowed("canChangePlaybackRate"),
        }
    end
    return ui.menu({
        {
            id = "media.stop",
            label = l10n.tr("workshop.aurora_player.stop"),
            enabled = allowed("canStop"),
        },
        { type = "separator" },
        {
            id = "media.back10",
            label = l10n.tr("workshop.aurora_player.back10"),
            enabled = allowed("canSeek"),
        },
        {
            id = "media.forward10",
            label = l10n.tr("workshop.aurora_player.forward10"),
            enabled = allowed("canSeek"),
        },
        { type = "separator" },
        {
            label = l10n.tr("workshop.aurora_player.speed"),
            children = rateItems,
        },
        {
            label = l10n.tr("workshop.aurora_player.shuffle"),
            children = {
                {
                    id = "media.shuffle.on",
                    label = l10n.tr(
                        "workshop.aurora_player.shuffle_on"),
                    enabled = allowed("canToggleShuffle"),
                },
                {
                    id = "media.shuffle.off",
                    label = l10n.tr(
                        "workshop.aurora_player.shuffle_off"),
                    enabled = allowed("canToggleShuffle"),
                },
            },
        },
        {
            label = l10n.tr("workshop.aurora_player.repeat"),
            children = {
                {
                    id = "media.repeat.none",
                    label = l10n.tr(
                        "workshop.aurora_player.repeat_none"),
                    enabled = allowed("canChangeRepeatMode"),
                },
                {
                    id = "media.repeat.track",
                    label = l10n.tr(
                        "workshop.aurora_player.repeat_track"),
                    enabled = allowed("canChangeRepeatMode"),
                },
                {
                    id = "media.repeat.list",
                    label = l10n.tr(
                        "workshop.aurora_player.repeat_list"),
                    enabled = allowed("canChangeRepeatMode"),
                },
            },
        },
    })
end

local function dispose()
    animation.cancelFrame(RECORD_FRAME)
    schedule.cancel(PROGRESS_TICK)
    if mediaCurrent then mediaCurrent:unsubscribe() end
    if mediaArtwork then mediaArtwork:unsubscribe() end
    if mediaTimeline then mediaTimeline:unsubscribe() end
    if audioAnalysis then audioAnalysis:unsubscribe() end
    mediaCurrent = nil
    mediaArtwork = nil
    mediaTimeline = nil
    audioAnalysis = nil
end

return widget.define({
    name = l10n.tr("workshop.aurora_player.name"),
    useCustomStyle = true,
    followPersonalizationDefault = true,
    showTitle = false,
    bg = 0x090B18,
    border = 0xFFFFFF,
    alpha = 0.18,
    borderAlpha = 0.18,
    gradientEndA = 0.10,
    glassEnabled = true,
    settings = settings,
    setup = setup,
    backgroundLayer = {
        render = backgroundLayer,
        opacity = 1,
        blurRadius = 8,
    },
    view = viewTree,
    event = event,
    menu = menu,
    dispose = dispose,
})
