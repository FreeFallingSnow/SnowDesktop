local player = module.require("modules/player.lua")
local localPlayback = module.require("modules/local_playback.lua")
local systemAudio = module.require("modules/system_audio.lua")
local responsive = module.require("modules/responsive.lua")

local mediaCurrent
local mediaArtwork
local audioAnalysis
local launcherBinding
local descriptor
local RECORD_FRAME = "aurora.record.spin"
local PROGRESS_TICK = "aurora.timeline.tick"
local PROGRESS_TICK_MS = 100
local MEDIA_REFRESH_MS = 100

local glyphs = {
    previous = utf8.char(0xF048),
    play = utf8.char(0xF04B),
    pause = utf8.char(0xF04C),
    next = utf8.char(0xF051),
    music = utf8.char(0xF001),
    playlist = utf8.char(0xF03A),
    options = utf8.char(0xF141),
    sound = utf8.char(0xF028),
}

local function settingEnabled(key, defaultValue)
    local value = storage.get(key)
    if value == nil then return defaultValue == true end
    return value == true or value == 1 or value == "1" or value == "true"
end

local settings = {
    fields = {
        {
            key = "launcherReference",
            label = l10n.tr("workshop.aurora_player.select_launcher"),
            type = "appReference",
            binding = "idlePlayer",
            emptyLabel = l10n.tr("workshop.aurora_player.not_set"),
            noResultsLabel =
                l10n.tr("workshop.aurora_player.no_search_results"),
        },
        {
            key = "cover_background",
            label = l10n.tr("workshop.aurora_player.cover_background"),
            description = l10n.tr(
                "workshop.aurora_player.cover_background_description"),
            type = "bool",
            default = true,
        },
        {
            key = "background_blur",
            label = l10n.tr("workshop.aurora_player.background_blur"),
            description = l10n.tr(
                "workshop.aurora_player.background_blur_description"),
            type = "range",
            min = 0,
            max = 24,
            step = 1,
            default = 8,
        },
        {
            key = "show_visualizer",
            label = l10n.tr("workshop.aurora_player.visualizer"),
            description = l10n.tr(
                "workshop.aurora_player.visualizer_description"),
            type = "bool",
            default = true,
        },
    },
}

local function previewSession()
    return {
        id = "aurora-preview",
        sourceName = "Music Controls Preview",
        title = "Night Drive",
        artist = "Mira Vale",
        album = "Glass Horizons",
        playbackStatus = "playing",
        shuffle = false,
        repeatMode = "list",
        playbackRate = 1.25,
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
            playbackRate = 1.25,
        },
    }
end

local function currentSession(model)
    if model and localPlayback.isLocal(model.localPlayback) then
        return localPlayback.session(model.localPlayback), model.localPlayback.updatedAtMs or 0
    end
    if model and model.preview then return previewSession(), 0 end
    local snapshot = mediaCurrent and mediaCurrent:value() or nil
    return player.session(snapshot), tonumber(snapshot and
        snapshot.timestamp) or 0
end

local function currentLauncher()
    if not launcherBinding then return nil end
    return launcherBinding:item()
end

local function chooseLauncher()
    if launcherBinding then
        launcherBinding:pick()
    else
        widget.openSettings()
    end
end

local function currentArtwork(model, session)
    if model and localPlayback.isLocal(model.localPlayback) then
        return model.localPlayback.player.artwork
    end
    if not session or (model and model.preview) then return nil end
    local artwork = player.artwork(
        mediaArtwork and mediaArtwork:value() or nil,
        session.id, model and model.artworkAfterMs or nil)
    if artwork and model then model.artworkAfterMs = nil end
    return artwork
end

local function currentTimeline(model, session)
    if localPlayback.isLocal(model.localPlayback) then return session and session.timeline end
    if not session or (model and model.timelineAfterMs ~= nil) then
        return nil
    end
    return session.timeline
end

local function currentBackgroundBlur()
    return player.clamp(storage.get("background_blur"), 0, 24, 8)
end

local function syncBackgroundBlur()
    if not descriptor or not descriptor.backgroundLayer then return false end
    local blur = currentBackgroundBlur()
    if descriptor.backgroundLayer.blurRadius == blur then return false end
    descriptor.backgroundLayer.blurRadius = blur
    return true
end

local function shouldAnalyze(context)
    return player.shouldAnalyze(
        settingEnabled("show_visualizer", true),
        widget.hasFeature("data.audio.output.analysis") and
            widget.hasFeature("view.dataSeries"),
        widget.hasPermission("audio.output.analyze"),
        context.accessibility.reducedMotion)
end

local function subscribeAudioAnalysis()
    return data.subscribe("audio.output.analysis", {
        features = { "spectrum" },
        spectrumBins = 48,
        updateHz = 30,
        whenHidden = "pause",
    })
end

local function syncAudioAnalysis(context, model)
    local analyze = not model.preview and shouldAnalyze(context)
    if analyze and not audioAnalysis then
        audioAnalysis = subscribeAudioAnalysis()
    elseif not analyze and audioAnalysis then
        audioAnalysis:unsubscribe()
        audioAnalysis = nil
    end
    model.visualizer = analyze or model.preview
end

local playbackOptions=module.require("modules/playback_options.lua")
local function setup(context)
    syncBackgroundBlur()
    launcherBinding = slots.binding("idlePlayer")
    mediaCurrent = data.subscribe("media.current", {
        maxAgeMs = MEDIA_REFRESH_MS,
        whenHidden = "throttle",
    })
    mediaArtwork = data.subscribe("media.artwork", {
        maxAgeMs = MEDIA_REFRESH_MS,
        whenHidden = "throttle",
    })
    local model = {
        localPlayback = localPlayback.setup(context),
        systemAudio = systemAudio.new(context),
        preview = context.preview == true,
        visualizer = false,
        tasks = {},
        pendingPlayback = nil,
        seekPreview = nil,
        seekSessionId = nil,
        seekTaskId = nil,
        seekTargetMs = nil,
        seekBaselineUpdatedAtMs = nil,
        seekBaselinePositionMs = nil,
        seekCommittedAtMs = nil,
        mediaIdentity = nil,
        artworkAfterMs = nil,
        timelineAfterMs = nil,
        reducedMotion = context.accessibility.reducedMotion == true,
        visible = true,
        recordRotation = 0,
        recordFramePending = false,
        progressTicking = false,
    }
    model.localPlayback.beforeLocal=function()
        if model.preview then return end
        local session=player.session(mediaCurrent and mediaCurrent:value())
        local name,args=playbackOptions.pauseRequest(session,widget.hasPermission("media.action"))
        if not name then return end
        local id,err=task.start(name,args)
        if id then model.tasks[tostring(id)]=name
        else widget.log("warn",name.." rejected: "..tostring(err)) end
    end
    syncAudioAnalysis(context, model)
    return model
end

local function clearSeekPreview(model)
    model.seekPreview = nil
    model.seekSessionId = nil
    model.seekTaskId = nil
    model.seekTargetMs = nil
    model.seekBaselineUpdatedAtMs = nil
    model.seekBaselinePositionMs = nil
    model.seekCommittedAtMs = nil
end

local function syncMediaIdentity(model, session, timestamp)
    local identity = player.mediaIdentity(session)
    if model.mediaIdentity == identity then
        if player.timelineIdentityConfirmed(timestamp,
                model.timelineAfterMs, model.preview) then
            model.timelineAfterMs = nil
        end
        return
    end
    model.mediaIdentity = identity
    clearSeekPreview(model)
    local changedAt = session and math.max(0,
        tonumber(timestamp) or 0) or nil
    model.artworkAfterMs = model.preview and nil or changedAt
    model.timelineAfterMs = model.preview and nil or changedAt
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

local function lightForegroundPalette()
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

local function palette(context, artworkBackgroundActive)
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
    if artworkBackgroundActive then return lightForegroundPalette() end
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
    return lightForegroundPalette()
end

local function backgroundLayer(context, model)
    local width = layout.width()
    local height = layout.height()
    if context.accessibility.highContrast then
        return
    end
    if not settingEnabled("cover_background", true) then return end

    local session, timestamp = currentSession(model)
    syncMediaIdentity(model, session, timestamp)
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
    if localPlayback.isLocal(model.localPlayback) and name:sub(1,6)=="media." then
        return localPlayback.control(model.localPlayback,name,arguments)
    end
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
    return taskId
end

local function startLauncher(model)
    if model.preview then return end
    local launcher = currentLauncher()
    if not launcher then
        chooseLauncher()
        return
    end
    if launcher.availability ~= "available" then
        widget.log("warn", "bound launcher is unavailable")
        return
    end
    if not widget.hasFeature("task.app.launch") or
        not widget.hasPermission("app.launch") then
        widget.openSettings()
        return
    end
    local taskId, err = task.start("app.launch", {
        ref = launcher.reference,
    })
    if taskId then
        model.tasks[tostring(taskId)] = "app.launch"
    else
        widget.log("warn", "app.launch rejected: " .. tostring(err))
    end
end

local function controlAllowed(model)
    if localPlayback.isLocal(model.localPlayback) then return localPlayback.allowed() end
    return widget.hasPermission("media.action")
end

local function startSessionTask(model, name, capability, extra)
    local session = currentSession(model)
    if not player.canControl(session,
        controlAllowed(model), capability) then
        return
    end
    local arguments = extra or {}
    arguments.sessionId = session.id
    startTask(model, name, arguments)
end

local function controlButton(key, glyph, label, enabled, colors, primary,
    plan, tooltip)
    local size = primary and plan.primaryButton or plan.secondaryButton
    local events={pointerEnter={id="media.hover"},pointerLeave={id="media.hover"}}
    if key=="system.audio.open" and widget.hasFeature("view.pointer.events") then
        events.wheel={id="system.audio.volume.wheel"}
    end
    return view.iconButton({
        key = key,
        glyph = glyph,
        iconFont = "fa",
        width = size,
        height = size,
        fontSize = size * (primary and 0.38 or 0.36),
        enabled = enabled,
        action = { id = key },
        events = events,
        tooltip = widget.hasFeature("view.tooltip") and tooltip or nil,
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
    local borderWidth = size * 0.007
    local children = {
        recordCircle("aurora.record.disc", size, {
            background = colors.record,
            borderColor = colors.groove,
            borderWidth = borderWidth,
            opacity = 0.98,
        }),
        recordCircle("aurora.record.groove.outer", size * 0.84, {
            borderColor = colors.groove,
            borderWidth = borderWidth,
            opacity = 0.28,
        }),
        recordCircle("aurora.record.groove.inner", size * 0.74, {
            borderColor = colors.groove,
            borderWidth = borderWidth,
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
                borderWidth = borderWidth,
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
                    fontSize = size * 0.18,
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

local function visualizerNode(model, colors, plan)
    if not model.visualizer or not widget.hasFeature("view.dataSeries") then
        return nil
    end
    local snapshot = audioAnalysis and audioAnalysis:value() or nil
    return view.spectrum({
        key = "aurora.visualizer",
        values = player.spectrum(snapshot, 36, model.preview),
        width = "fill",
        height = "fill",
        padding = { left = plan.spectrumSidePadding,
            right = plan.spectrumSidePadding,
            top = plan.spectrumTop, bottom = 0 },
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
    local session, timestamp = currentSession(model)
    local launcher = currentLauncher()
    syncMediaIdentity(model, session, timestamp)
    local artwork = currentArtwork(model, session)
    local artworkBackgroundActive = player.shouldUseArtworkBackground(
        settingEnabled("cover_background", true), artwork ~= nil,
        context.accessibility.highContrast)
    local colors = palette(context, artworkBackgroundActive)
    local timeline = currentTimeline(model, session)
    local canAct = model.preview or controlAllowed(model)
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

    local localActive = localPlayback.isLocal(model.localPlayback)
    local notice = localActive and localPlayback.notice(model.localPlayback) or nil
    local title = session and session.title ~= "" and session.title or
        (localActive and l10n.tr("music.empty") or l10n.tr("workshop.aurora_player.empty"))
    local artist = session and session.artist ~= "" and session.artist or
        (session and session.sourceName or (launcher and launcher.title or
            l10n.tr("workshop.aurora_player.configure_launcher")))
    if notice then artist=notice end
    local album = session and session.album or ""
    local width = layout.width()
    local height = layout.height()
    local plan = responsive.plan(width, height)
    local padding = plan.padding
    local contentGap = plan.contentGap
    local availableHeight = height - padding * 2
    if not session or model.seekSessionId ~= session.id then
        clearSeekPreview(model)
    end
    local hasTimeline = player.hasTimeline(timeline)
    if not hasTimeline then
        clearSeekPreview(model)
    end
    updateProgressTicker(model, model.visible and not model.preview and not model.recordFramePending and
        hasTimeline and playing)
    local now = hasTimeline and time.now() or nil
    local position = player.position(timeline, playing, now)
    if model.seekPreview ~= nil then
        if model.seekCommittedAtMs ~= nil then
            local optimisticPosition = player.optimisticSeekPosition(timeline,
                model.seekTargetMs, playing, model.seekCommittedAtMs, now)
            if player.seekTimelineCaughtUp(timeline,
                    model.seekBaselineUpdatedAtMs,
                    model.seekBaselinePositionMs, optimisticPosition,
                    playing, now) then
                clearSeekPreview(model)
            else
                position = optimisticPosition
            end
        else
            position = player.seekPosition(timeline, model.seekPreview)
        end
    end
    local progress = player.progress(timeline, position)
    local duration = player.duration(timeline)
    local seekEnabled = session ~= nil and hasTimeline and canAct and
        controls.canSeek == true

    local controlHeight = plan.controlHeight
    local controlRow = view.row({
        key = "aurora.controls",
        width = "fill",
        height = controlHeight,
        gap = plan.controlGap,
        alignItems = "center",
        justifyContent = "center",
        children = {
            controlButton("media.previous", glyphs.previous,
                l10n.tr("workshop.aurora_player.previous"),
                session ~= nil and canAct and controls.canPrevious == true,
                colors, false, plan),
            controlButton("media.toggle", playing and glyphs.pause or
                glyphs.play, l10n.tr(playing and
                    "workshop.aurora_player.pause" or
                    "workshop.aurora_player.play"),
                session ~= nil and canAct and controls.canPlayPause == true,
                colors, true, plan),
            controlButton("media.next", glyphs.next,
                l10n.tr("workshop.aurora_player.next"),
                session ~= nil and canAct and controls.canNext == true,
                colors, false, plan),
        },
    })

    local progressHeight = plan.progressHeight
    local progressNode = hasTimeline and view.column({
        key = "aurora.progress.group",
        width = "fill",
        height = progressHeight,
        gap = 0,
        children = {
            view.slider({
                key = "aurora.progress",
                value = progress,
                min = 0,
                max = 1,
                step = 0.001,
                width = "fill",
                height = plan.sliderHeight,
                enabled = seekEnabled,
                events = {
                    pointerEnter = { id = "media.hover" },
                    pointerLeave = { id = "media.hover" },
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
            view.row({
                key = "aurora.progress.times",
                width = "fill",
                height = progressHeight - plan.sliderHeight,
                justifyContent = "spaceBetween",
                children = {
                    view.text({
                        key = "aurora.progress.current",
                        text = player.formatTime(position),
                        width = plan.detailWidth * 0.30,
                        height = "fill",
                        fontSize = plan.timeFont,
                        style = { foreground = colors.subtle },
                    }),
                    view.text({
                        key = "aurora.progress.duration",
                        text = player.formatTime(duration),
                        width = plan.detailWidth * 0.30,
                        height = "fill",
                        fontSize = plan.timeFont,
                        textAlign = "end",
                        style = { foreground = colors.subtle },
                    }),
                },
            }),
        },
    }) or nil

    local textAlignment = plan.vertical and "center" or "start"
    local metadataPadding = {
        left = plan.metadataInset,
        right = plan.metadataInset,
    }
    local titleHeight = plan.titleHeight
    local artistHeight = session and plan.artistHeight or
        plan.emptyArtistHeight
    local albumHeight = plan.albumHeight
    local detailGap = session and plan.detailGap or plan.emptyDetailGap
    local showAlbum = session ~= nil
    local detailCount = 2 + (showAlbum and 1 or 0) +
        (session and 1 or 0) + (progressNode and 1 or 0)
    local detailHeight = titleHeight + artistHeight +
        (showAlbum and albumHeight or 0) +
        (session and controlHeight or 0) +
        (progressNode and progressHeight or 0) +
        detailGap * math.max(0, detailCount - 1)
    local detailChildren = {
        view.text({
            key = "aurora.title",
            text = title,
            width = "fill",
            height = titleHeight,
            padding = metadataPadding,
            fontSize = plan.titleFont,
            fontWeight = 700,
            textAlign = textAlignment,
            textWrap = "noWrap",
            overflowText = "ellipsis",
            style = { foreground = colors.primary },
            accessibility = { headingLevel = 2 },
        }),
        view.text({
            key = "aurora.artist",
            text = artist,
            width = "fill",
            height = artistHeight,
            padding = metadataPadding,
            fontSize = session and plan.artistFont or
                plan.artistFont * 0.92,
            textAlign = textAlignment,
            textWrap = session and "noWrap" or "wrap",
            maxLines = session and 1 or 2,
            overflowText = "ellipsis",
            style = { foreground = colors.secondary },
        }),
    }
    if showAlbum then
        detailChildren[#detailChildren + 1] = view.text({
            key = "aurora.album",
            text = album,
            width = "fill",
            height = albumHeight,
            padding = metadataPadding,
            fontSize = plan.albumFont,
            textAlign = textAlignment,
            textWrap = "noWrap",
            overflowText = "ellipsis",
            style = { foreground = colors.subtle },
        })
    end
    if session then
        detailChildren[#detailChildren + 1] = controlRow
        if progressNode then
            detailChildren[#detailChildren + 1] = progressNode
        end
    end

    local details = view.column({
        key = "aurora.details",
        width = "fill",
        height = plan.vertical and detailHeight or "fill",
        gap = detailGap,
        justifyContent = "center",
        children = detailChildren,
    })

    local coverSize = plan.coverSize
    if plan.vertical then
        coverSize = math.min(coverSize,
            availableHeight - detailHeight - contentGap)
    end

    local contentDefinition = {
        key = plan.vertical and "aurora.content.vertical" or
            "aurora.content.horizontal",
        width = "fill",
        height = "fill",
        padding = padding,
        gap = contentGap,
        alignItems = "center",
        justifyContent = plan.vertical and "center" or nil,
        children = {
            coverNode(artwork, coverSize, colors, model.recordRotation),
            details,
        },
    }
    local content = plan.vertical and view.column(contentDefinition) or
        view.row(contentDefinition)

    local children = {}
    local visualizer = visualizerNode(model, colors, plan)
    if visualizer then children[#children + 1] = visualizer end
    children[#children + 1] = content
    local utilitySize=plan.secondaryButton*0.68
    local utilities={controlButton("system.audio.open",glyphs.sound,l10n.tr("shared.audio"),
        widget.hasFeature("widget.panel"),colors,false,{secondaryButton=utilitySize},systemAudio.tooltip(model.systemAudio)),controlButton("local.options",glyphs.options,
        l10n.tr("music.options"),widget.hasFeature("widget.panel"),colors,false,
        {secondaryButton=utilitySize})}
    if localActive then table.insert(utilities,1,controlButton("local.playlist",glyphs.playlist,
        l10n.tr("music.playlist"),true,colors,false,{secondaryButton=utilitySize})) end
    local showUtilities=model.systemAudio.surface~=nil or localActive and
        (playing or model.localPlayback.player.status=="loading")
    if widget.hasFeature("interaction.pointerActions") then
        for _,key in ipairs({"aurora.surface","media.previous","media.toggle","media.next",
            "aurora.progress","local.options","local.playlist","system.audio.open"}) do
            if interaction.isHovered(key) then showUtilities=true;break end
        end
    end
    if showUtilities then
        children[#children+1]=view.column({key="aurora.utilities",width="fill",height="fill",
            padding=plan.padding*0.45,justifyContent="start",alignItems="end",children={
                view.row({key="aurora.utilities.row",width="auto",height=utilitySize,gap=utilitySize*0.25,children=utilities})}})
    end
    return view.stack({
        key = "aurora.surface",
        width = "fill",
        height = "fill",
        events = {
            pointerEnter = { id = "media.hover" },
            pointerLeave = { id = "media.hover" },
            fileDrop = { id = "local.import" },
            doubleClick = { id = "launcher.open" },
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
        controlAllowed(model), "canSeek") then return end
    local timeline = currentTimeline(model, session)
    if not player.hasTimeline(timeline) then return end
    local playing = session.playbackStatus == "playing"
    if model.pendingPlayback == "playing" then playing = true end
    if model.pendingPlayback == "paused" then playing = false end
    local position = player.relativeSeekPosition(
        timeline, delta, playing, time.now())
    startTask(model, "media.seek", {
        sessionId = session.id,
        positionMs = position,
    })
end

local function commitSeek(model, session, timeline, fraction)
    if not player.canControl(session,
        controlAllowed(model), "canSeek") or
        not player.hasTimeline(timeline) then
        clearSeekPreview(model)
        widget.invalidate()
        return
    end
    local normalized = player.clamp(fraction, 0, 1, 0)
    model.seekPreview = normalized
    model.seekSessionId = session.id
    model.seekTargetMs = player.seekPosition(timeline, normalized)
    model.seekBaselineUpdatedAtMs = tonumber(timeline.updatedAtMs) or 0
    model.seekBaselinePositionMs = tonumber(timeline.positionMs) or 0
    model.seekCommittedAtMs = time.now()
    local taskId = startTask(model, "media.seek", {
        sessionId = session.id,
        positionMs = model.seekTargetMs,
    })
    if not taskId then
        clearSeekPreview(model)
    else
        model.seekTaskId = tostring(taskId)
    end
    widget.invalidate()
end

local function event(context, model, value)
    if systemAudio.event(model.systemAudio,value,model.localPlayback) then return end
    local previousSource=model.localPlayback.source
    if localPlayback.event(context,model.localPlayback,value) then
        if previousSource~=model.localPlayback.source then
            model.pendingPlayback=nil
            clearSeekPreview(model)
        end
        if value.kind=="task.complete" and model.seekTaskId==tostring(value.taskId) then
            model.seekTaskId=nil
            if value.ok~=true then clearSeekPreview(model) end
        elseif value.kind=="audio.state" and
            (value.status=="stopped" or value.status=="error" or value.status=="ended") then
            clearSeekPreview(model)
        end
        return
    end
    if value.kind == "settings.changed" then
        local refreshBlur = false
        local refreshSpectrum = false
        for _, key in ipairs(value.keys or {}) do
            if key == "background_blur" then refreshBlur = true end
            if key == "show_visualizer" then refreshSpectrum = true end
        end
        if refreshBlur then syncBackgroundBlur() end
        if refreshSpectrum then syncAudioAnalysis(context, model) end
        return
    end
    if value.kind == "environment" then
        model.reducedMotion = context.accessibility.reducedMotion == true
        syncAudioAnalysis(context, model)
        return
    end
    if value.kind == "visibility" then
        model.visible = value.visible == true
        if not model.visible then
            updateProgressTicker(model,false)
            animation.cancelFrame(RECORD_FRAME)
            model.recordFramePending = false
        else
            widget.invalidate()
        end
        return
    end
    if value.kind == "frame" and value.id == RECORD_FRAME then
        model.recordFramePending = false
        local session, timestamp = currentSession(model)
        syncMediaIdentity(model, session, timestamp)
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
        if name and name ~= "app.launch" then model.pendingPlayback = nil end
        if name == "media.seek" then
            local currentSeek = model.seekTaskId == tostring(value.taskId)
            if currentSeek then
                model.seekTaskId = nil
                if failed then clearSeekPreview(model) end
            end
            widget.invalidate()
        end
        if failed then
            widget.log("warn", name .. " failed: " ..
                tostring(value.error))
        end
        return
    end
    if value.kind ~= "action" then return end
    if value.id=="media.hover" then widget.invalidate();return end

    local session = currentSession(model)
    local modeAction=value.id:match("^media%.rate%.") or value.id:match("^media%.shuffle%.") or
        value.id:match("^media%.repeat%.")
    if modeAction and (model.menuSessionId~=(session and session.id) or
        model.menuSource~=model.localPlayback.source) then return end
    if value.id:sub(1,7)=="system." then
        if localPlayback.isLocal(model.localPlayback) then return end
        local name,args=playbackOptions.command(session,controlAllowed(model),value)
        if name then startTask(model,name,args) end
    elseif value.id == "media.previous" then
        startSessionTask(model, "media.previous", "canPrevious")
    elseif value.id == "media.next" then
        startSessionTask(model, "media.next", "canNext")
    elseif value.id == "media.toggle" then
        if not player.canControl(session,
            controlAllowed(model), "canPlayPause") then return end
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
            controlAllowed(model), "canSeek") then return end
        local timeline = currentTimeline(model, session)
        if not player.hasTimeline(timeline) then return end
        local fraction = player.clamp(value.controlValue, 0, 1, 0)
        model.seekPreview = fraction
        model.seekSessionId = session.id
        model.seekTaskId = nil
        model.seekTargetMs = nil
        model.seekBaselineUpdatedAtMs = nil
        model.seekBaselinePositionMs = nil
        model.seekCommittedAtMs = nil
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
            controlAllowed(model),
            "canChangePlaybackRate") then return end
        local rateText = value.id:sub(12):gsub("_", ".")
        local rate = tonumber(rateText)
        if rate then startTask(model, "media.setRate", {
            sessionId = session.id, rate = rate,
        }) end
    elseif value.id == "media.shuffle.on" or
        value.id == "media.shuffle.off" then
        if not player.canControl(session,
            controlAllowed(model),
            "canToggleShuffle") then return end
        startTask(model, "media.setShuffle", {
            sessionId = session.id,
            shuffle = value.id == "media.shuffle.on",
        })
    elseif value.id:sub(1, 13) == "media.repeat." then
        if not player.canControl(session,
            controlAllowed(model),
            "canChangeRepeatMode") then return end
        startTask(model, "media.setRepeat", {
            sessionId = session.id,
            mode = value.id:sub(14),
        })
    elseif value.id == "launcher.open" then
        if localPlayback.isLocal(model.localPlayback) and not session then
            localPlayback.event(context,model.localPlayback,{kind="action",id="local.files"})
        elseif not localPlayback.isLocal(model.localPlayback) and not session then startLauncher(model) end
    elseif value.id == "launcher.configure" then
        chooseLauncher()
    elseif value.id == "launcher.clear" then
        if currentLauncher() then launcherBinding:clear() end
    end
end

local function menu(_context, model, request)
    if request.id=="local.track.menu" then return localPlayback.menu(_context,model.localPlayback,request) end
    if request.id ~= "media.menu" then return nil end
    local session = currentSession(model)
    local timeline = currentTimeline(model, session)
    local permission = controlAllowed(model)
    local modes=playbackOptions.state(session,permission)
    model.menuSessionId=session and session.id
    model.menuSource=model.localPlayback.source
    local function allowed(capability)
        if capability == "canSeek" and
            not player.hasTimeline(timeline) then return false end
        return player.canControl(session, permission, capability)
    end
    local launcher = currentLauncher()
    local rateItems = {}
    for _, rate in ipairs({ "0_5", "0_75", "1", "1_25", "1_5", "2" }) do
        rateItems[#rateItems + 1] = {
            id = "media.rate." .. rate,
            label = rate:gsub("_", ".") .. "×",
            enabled = allowed("canChangePlaybackRate"),
            checked = modes.rateValue==tonumber((rate:gsub("_", "."))),
        }
    end
    local items = {
        {
            id = "launcher.configure",
            label = l10n.tr("workshop.aurora_player.configure_launcher"),
        },
        { type = "separator" },
        {
            id = "launcher.current",
            label = launcher and launcher.title or
                l10n.tr("workshop.aurora_player.not_set"),
            checked = true,
            enabled = false,
        },
    }
    local sourceItems=localPlayback.menuItems(model.localPlayback)
    sourceItems[#sourceItems+1]={type="separator"}
    for _, item in ipairs(items) do sourceItems[#sourceItems+1]=item end
    items=sourceItems
    if launcher then
        items[#items + 1] = { type = "separator" }
        items[#items + 1] = {
            id = "launcher.clear",
            label = l10n.tr("workshop.aurora_player.clear_launcher"),
        }
    end
    items[#items + 1] = { type = "separator" }
    local mediaItems = {
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
    }
    for _, item in ipairs(mediaItems) do
        local first=item.children and item.children[1] and item.children[1].id or ""
        local supported=true
        if first:sub(1,11)=="media.rate." then supported=modes.rate
        elseif first:sub(1,14)=="media.shuffle." then
            supported=modes.shuffle
            for _,choice in ipairs(item.children) do
                choice.checked=modes.shuffleValue~=nil and
                    modes.shuffleValue==(choice.id=="media.shuffle.on")
            end
        elseif first:sub(1,13)=="media.repeat." then
            supported=modes.repeatMode
            for _,choice in ipairs(item.children) do choice.checked=modes.repeatValue==choice.id:sub(14) end
        end
        if supported then items[#items+1]=item end
    end
    return ui.menu(items)
end

local function dispose(_context,model)
    if model and model.systemAudio then systemAudio.dispose(model.systemAudio) end
    if model and model.localPlayback then model.localPlayback.player:stop() end
    animation.cancelFrame(RECORD_FRAME)
    schedule.cancel(PROGRESS_TICK)
    if mediaCurrent then mediaCurrent:unsubscribe() end
    if mediaArtwork then mediaArtwork:unsubscribe() end
    if audioAnalysis then audioAnalysis:unsubscribe() end
    mediaCurrent = nil
    mediaArtwork = nil
    audioAnalysis = nil
    launcherBinding = nil
end

descriptor = {
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
    panel = function(context,model)
        if model.systemAudio.surface=="panel" then return systemAudio.view(model.systemAudio,model.localPlayback) end
        return localPlayback.panel(context,model.localPlayback,currentSession(model),controlAllowed(model))
    end,
    popover = function(_context,model) return systemAudio.view(model.systemAudio,model.localPlayback) end,
    event = event,
    menu = menu,
    dispose = dispose,
}

return widget.define(descriptor)
