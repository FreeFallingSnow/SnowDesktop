-- media-controls/main.lua - API v2 media session controls and app launcher
local mediaCurrent
local mediaArtwork
local appIndexStatus

local fluent = {
    settings = utf8.char(0xF6A9),
}

local palettes = {
    dark = {
        title = 0xFFFFFF,
        subtitle = 0xF1F5F9,
        btnText = 0xFFFFFF,
        btnDisabled = 0x64748B,
        btnBg = 0xFFFFFF,
    },
    light = {
        title = 0x1E293B,
        subtitle = 0x334155,
        btnText = 0x1E293B,
        btnDisabled = 0x94A3B8,
        btnBg = 0x000000,
    },
}

local settings = {
    fields = {
        {
            key = "showArtwork",
            label = l10n.tr("lua_widget.media_control.show_artwork"),
            type = "bool",
            default = true,
        },
        {
            key = "launcherTitle",
            searchKey = "launcherSearch",
            label = l10n.tr("lua_widget.media_control.select_launcher"),
            type = "appSearch",
            default = "",
            emptyLabel = l10n.tr("lua_widget.media_control.not_set"),
            noResultsLabel =
                l10n.tr("lua_widget.media_control.no_search_results"),
        },
    },
}

local function getPalette()
    local theme = widget.theme()
    if theme and theme.contentTheme == 1 then
        return palettes.light
    end
    return palettes.dark
end

local function currentSession()
    if not mediaCurrent then return nil end
    local snapshot = mediaCurrent:value()
    if not snapshot.available or not snapshot.value then return nil end
    local session = snapshot.value.session
    if not session or session.playbackStatus == "closed" then return nil end
    return session
end

local function currentArtwork(session)
    if not session or not mediaArtwork or
        storage.get("showArtwork") == "0" then
        return nil
    end
    local snapshot = mediaArtwork:value()
    if not snapshot.available or not snapshot.value then return nil end
    local artwork = snapshot.value
    if artwork.sessionId ~= session.id or not artwork.image then return nil end
    return artwork
end

local function clearLauncherResults(model)
    model.launcherResults = {}
    model.launcherRef = nil
    model.launcherDisplayTitle = ""
    model.catalogRevision = nil
    model.searchError = nil
end

local function startLauncherSearch(model)
    if not widget.hasFeature("task.app.search") or
        not widget.hasPermission("app.discovery") then
        clearLauncherResults(model)
        model.searchError = "permissionDenied"
        return
    end
    if model.searchTask then return end

    local query = storage.get("launcherSearch") or ""
    model.launcherQuery = query
    if query == "" then
        clearLauncherResults(model)
        return
    end
    if #query > 256 then
        clearLauncherResults(model)
        model.searchError = "queryTooLong"
        return
    end

    if appIndexStatus then
        local status = appIndexStatus:value()
        if not status.available or not status.value or
            status.value.state ~= "ready" then
            model.searchRetryPending = true
            model.searchError = status.error or "appIndexNotReady"
            return
        end
    end

    local taskId, err = task.start("app.search", {
        query = query,
        limit = 8,
        offset = 0,
    })
    if taskId then
        model.searchTask = taskId
        model.searchRetryPending = false
        model.searchError = nil
    else
        model.searchError = err or "searchRejected"
    end
end

local function startMediaAction(model, taskName, pendingState)
    if not widget.hasPermission("media.action") then return end
    local session = currentSession()
    if not session then return end
    local taskId, err = task.start(taskName, {
        sessionId = session.id,
    })
    if taskId then
        model.mediaTasks[tostring(taskId)] = taskName
        model.pendingState = pendingState
    else
        model.pendingState = nil
        widget.log("warn", taskName .. " rejected: " .. tostring(err))
    end
end

local function startLauncher(model)
    if not model.launcherRef or
        not widget.hasFeature("task.app.launch") or
        not widget.hasPermission("app.launch") then
        widget.openSettings()
        return
    end
    local taskId, err = task.start("app.launch", {
        ref = model.launcherRef,
    })
    if taskId then
        model.launchTask = taskId
    else
        widget.log("warn", "app.launch rejected: " .. tostring(err))
    end
end

local function setup()
    mediaCurrent = data.subscribe("media.current", {
        maxAgeMs = 500,
        whenHidden = "throttle",
    })
    mediaArtwork = data.subscribe("media.artwork", {
        maxAgeMs = 500,
        whenHidden = "throttle",
    })

    if widget.hasFeature("data.app.indexStatus") and
        widget.hasPermission("app.discovery") then
        appIndexStatus = data.subscribe("app.indexStatus", {
            maxAgeMs = 1000,
            whenHidden = "throttle",
        })
        schedule.every("launcher-sync", 1000, {
            whenHidden = "throttle",
        })
    end

    local model = {
        pendingState = nil,
        mediaTasks = {},
        searchTask = nil,
        launchTask = nil,
        searchRetryPending = false,
        searchError = nil,
        launcherQuery = storage.get("launcherSearch") or "",
        launcherResults = {},
        launcherRef = nil,
        launcherDisplayTitle = "",
        selectedLauncherTitle = storage.get("launcherTitle") or "",
        catalogRevision = nil,
    }
    startLauncherSearch(model)
    return model
end

local function drawButton(model, id, taskName, glyph, label,
    x, y, size, enabled, palette, pendingState)
    local hovered = enabled and interaction.isHovered(id)
    local pressed = enabled and interaction.isPressed(id)
    local alpha = enabled and (pressed and 0.22 or (hovered and 0.16 or 0.10)) or 0.04
    local foreground = enabled and palette.btnText or palette.btnDisabled
    draw.rect(x, y, size, size, palette.btnBg, size * 0.22, alpha)
    draw.fa(glyph, x + size * 0.12, y + size * 0.12,
        size * 0.76, foreground)
    if not enabled then return end

    interaction.region({
        key = id,
        shape = {
            type = "roundedRect",
            x = x,
            y = y,
            width = size,
            height = size,
            radius = size * 0.22,
        },
        cursor = "hand",
        events = {
            click = {
                id = taskName,
                value = { pendingState = pendingState or "" },
            },
        },
        accessibility = {
            role = "button",
            label = label,
        },
    })
end

local function render(_context, model)
    local width = layout.width()
    local height = layout.height()
    local palette = getPalette()
    local session = currentSession()
    local available = session ~= nil
    local controls = available and session.controls or {}
    local canControl = widget.hasPermission("media.action")
    local artwork = currentArtwork(session)

    local isPlaying = available and session.playbackStatus == "playing"
    if model.pendingState == "playing" then
        if isPlaying then model.pendingState = nil end
        isPlaying = true
    elseif model.pendingState == "paused" then
        if not isPlaying then model.pendingState = nil end
        isPlaying = false
    end

    local title = available and session.title ~= "" and session.title or
        l10n.tr("lua_widget.media_control.not_playing")
    local subtitle = ""
    if available then
        subtitle = session.artist ~= "" and session.artist or session.sourceName
    elseif model.launcherDisplayTitle ~= "" then
        subtitle = model.launcherDisplayTitle
    elseif model.launcherQuery == "" or model.searchError == "permissionDenied" then
        subtitle = l10n.tr("lua_widget.media_control.configure_launcher")
    elseif model.searchError and not model.searchRetryPending then
        subtitle = l10n.tr("lua_widget.media_control.no_search_results")
    else
        subtitle = l10n.tr("lua_widget.media_control.double_click_player")
    end

    local interactiveHeight = math.max(1, height)
    interaction.region({
        key = "media.surface",
        shape = {
            type = "rect",
            x = 0,
            y = 0,
            width = width,
            height = interactiveHeight,
        },
        events = {
            doubleClick = { id = "launcher.open" },
            contextMenu = { id = "media.menu", scope = "component" },
        },
        accessibility = {
            role = "group",
            label = l10n.tr("lua_widget.media_control.name"),
        },
    })

    local buttonSize = layout.cu(40)
    local buttonGap = layout.cu(12)
    local total = buttonSize * 3 + buttonGap * 2
    local buttonY = height - buttonSize - layout.cu(8)
    local buttonX = (width - total) / 2

    local textX = layout.cu(18)
    if artwork then
        local artworkSize = math.min(layout.cu(52),
            buttonY - layout.cu(16))
        if artworkSize >= layout.cu(24) then
            local artworkY = math.max(layout.cu(8),
                (buttonY - artworkSize) / 2)
            draw.imageFit(artwork.image, textX, artworkY,
                artworkSize, artworkSize, "cover", "center", 1.0, "linear")
            draw.strokeRect(textX, artworkY, artworkSize, artworkSize,
                palette.btnText, layout.cu(5), layout.cu(1), 0.16)
            textX = textX + artworkSize + layout.cu(12)
        end
    end

    local titleY = subtitle ~= "" and height * 0.14 or height * 0.25
    local textWidth = math.max(layout.cu(24),
        width - textX - layout.cu(18))
    draw.text(textX, titleY, title, layout.fontCu(15),
        palette.title, textWidth, true, true)
    if subtitle ~= "" then
        draw.text(textX, titleY + layout.cu(22), subtitle,
            layout.fontCu(12), palette.subtitle,
            textWidth, true, true)
    end

    drawButton(model, "media.previous", "media.previous", "",
        l10n.tr("lua_widget.media_control.previous"),
        buttonX, buttonY, buttonSize,
        available and canControl and controls.canPrevious,
        palette)
    drawButton(model, "media.toggle", "media.toggle",
        isPlaying and "" or "",
        l10n.tr(isPlaying and "lua_widget.media_control.pause" or
            "lua_widget.media_control.play"),
        buttonX + buttonSize + buttonGap, buttonY, buttonSize,
        available and canControl and controls.canPlayPause,
        palette, isPlaying and "paused" or "playing")
    drawButton(model, "media.next", "media.next", "",
        l10n.tr("lua_widget.media_control.next"),
        buttonX + (buttonSize + buttonGap) * 2, buttonY, buttonSize,
        available and canControl and controls.canNext,
        palette)
end

local function handleSearchCompletion(model, event)
    if event.taskId ~= model.searchTask then return false end
    model.searchTask = nil
    clearLauncherResults(model)
    if not event.ok or not event.value then
        model.searchError = event.error or "searchFailed"
        model.searchRetryPending = event.error == "appIndexNotReady"
        return true
    end

    model.launcherResults = event.value.items or {}
    model.catalogRevision = event.value.catalogRevision
    local chosen = nil
    if model.selectedLauncherTitle ~= "" then
        for _, item in ipairs(model.launcherResults) do
            if item.title == model.selectedLauncherTitle then
                chosen = item
                break
            end
        end
    end
    chosen = chosen or model.launcherResults[1]
    if chosen then
        model.launcherRef = chosen.ref
        model.launcherDisplayTitle = chosen.title or ""
    else
        model.searchError = "notFound"
    end
    return true
end

local function syncLauncher(model)
    local query = storage.get("launcherSearch") or ""
    if query ~= model.launcherQuery then
        if model.searchTask then
            task.cancel(model.searchTask)
            model.searchTask = nil
        end
        model.launcherQuery = query
        model.selectedLauncherTitle = ""
        clearLauncherResults(model)
        model.searchRetryPending = true
    end

    local selectedTitle = storage.get("launcherTitle") or ""
    if selectedTitle ~= model.selectedLauncherTitle then
        model.selectedLauncherTitle = selectedTitle
        model.launcherRef = nil
        model.launcherDisplayTitle = ""
        for _, item in ipairs(model.launcherResults) do
            if item.title == selectedTitle then
                model.launcherRef = item.ref
                model.launcherDisplayTitle = item.title or ""
                break
            end
        end
        if selectedTitle ~= "" and not model.launcherRef then
            model.searchRetryPending = true
        end
    end

    if appIndexStatus then
        local status = appIndexStatus:value()
        if status.available and status.value and
            status.value.state == "ready" then
            if model.catalogRevision and
                model.catalogRevision ~= status.value.revision then
                clearLauncherResults(model)
                model.searchRetryPending = true
            end
            if model.searchRetryPending or
                (model.launcherQuery ~= "" and
                    not model.searchTask and #model.launcherResults == 0) then
                startLauncherSearch(model)
            end
        end
    end
end

local function event(_context, model, value)
    if value.kind == "schedule" and value.id == "launcher-sync" then
        syncLauncher(model)
        return
    end

    if value.kind == "task.complete" then
        if handleSearchCompletion(model, value) then return end
        if value.taskId == model.launchTask then
            model.launchTask = nil
            if not value.ok then
                widget.log("warn", "app.launch failed: " ..
                    tostring(value.error))
                if value.error == "staleReference" or
                    value.error == "invalidReference" then
                    clearLauncherResults(model)
                    model.searchRetryPending = true
                end
            end
            return
        end
        local mediaTask = model.mediaTasks[tostring(value.taskId)]
        if mediaTask then
            model.mediaTasks[tostring(value.taskId)] = nil
            if not value.ok then
                model.pendingState = nil
                widget.log("warn", mediaTask .. " failed: " ..
                    tostring(value.error))
            end
        end
        return
    end

    if value.kind ~= "action" then return end
    if value.id == "media.previous" then
        startMediaAction(model, "media.previous")
    elseif value.id == "media.toggle" then
        local pendingState = value.value and value.value.pendingState or nil
        startMediaAction(model, "media.toggle", pendingState)
    elseif value.id == "media.next" then
        startMediaAction(model, "media.next")
    elseif value.id == "launcher.open" then
        if not currentSession() then startLauncher(model) end
    elseif value.id == "launcher.configure" then
        widget.openSettings()
    elseif value.id == "launcher.clear" then
        model.selectedLauncherTitle = ""
        model.launcherRef = nil
        model.launcherDisplayTitle = ""
        storage.remove("launcherTitle")
    else
        local index = tonumber(string.match(value.id or "",
            "^launcher%.select%.(%d+)$"))
        local item = index and model.launcherResults[index] or nil
        if item then
            model.selectedLauncherTitle = item.title or ""
            model.launcherRef = item.ref
            model.launcherDisplayTitle = item.title or ""
            if storage.get("launcherTitle") ~= model.selectedLauncherTitle then
                storage.set("launcherTitle", model.selectedLauncherTitle)
            end
        end
    end
end

local function menu(_context, model, request)
    if request.id ~= "media.menu" then return nil end
    local items = {
        {
            id = "launcher.configure",
            label = l10n.tr("lua_widget.media_control.configure_launcher"),
            icon = fluent.settings,
            iconFont = "fluent",
        },
    }
    if #model.launcherResults > 0 then
        items[#items + 1] = { type = "separator" }
        for index = 1, math.min(5, #model.launcherResults) do
            local item = model.launcherResults[index]
            items[#items + 1] = {
                id = "launcher.select." .. tostring(index),
                label = item.title or "",
                checked = item.ref == model.launcherRef,
            }
        end
    end
    items[#items + 1] = { type = "separator" }
    items[#items + 1] = {
        id = "launcher.clear",
        label = l10n.tr("lua_widget.media_control.not_set"),
        checked = model.launcherRef == nil,
    }
    return ui.menu(items)
end

local function migrateStorage(oldVersion, newVersion)
    if oldVersion >= 2 or newVersion < 2 then return end
    local query = storage.get("launcherSearch") or ""
    if query ~= "" then return end

    local oldLauncher = storage.get("launcher") or ""
    if oldLauncher == "" then return end
    local displayName = string.match(oldLauncher, "([^/\\]+)$") or oldLauncher
    displayName = string.gsub(displayName, "%.lnk$", "")
    displayName = string.gsub(displayName, "%.exe$", "")
    if displayName ~= "" then storage.set("launcherSearch", displayName) end
end

return widget.define({
    name = l10n.tr("lua_widget.media_control.name"),
    useCustomStyle = true,
    followPersonalizationDefault = true,
    bottomBarHover = true,
    bg = 0x0F172A,
    border = 0xFFFFFF,
    alpha = 0.42,
    borderAlpha = 0.16,
    gradientEndA = 0.30,
    settings = settings,
    setup = setup,
    render = render,
    event = event,
    menu = menu,
    migrateStorage = migrateStorage,
})
