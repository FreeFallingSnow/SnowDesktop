-- quick-launcher/main.lua - API v2 asynchronous desktop and app search
local descriptor
local desktopChanges
local appIndexStatus

local function componentMetrics()
    local row = ui.metrics().layoutRowHeight
    local scale = row / 28
    return {
        layoutRowHeight = row,
        spacingXs = 4 * scale,
        spacingSm = 8 * scale,
        spacingMd = 12 * scale,
        spacingLg = 16 * scale,
        captionFontSize = 10 * scale,
        bodyFontSize = 12 * scale,
        titleFontSize = 14 * scale,
        controlFontSize = 12 * scale,
        iconSize = 16 * scale,
        controlRadius = 8 * scale,
        strokeWidth = scale,
    }
end

local fluent = {
    edit = utf8.char(0xF3DD),
    open = utf8.char(0xF582),
    folderOpen = utf8.char(0xF42E),
    refresh = utf8.char(0xF13D),
}

local settings = {
    fields = {
        { key = "query", label = l10n.tr("lua_widget.quick_launcher.query"),
            type = "text", default = "" },
        { key = "fontScale", label = l10n.tr("lua_widget.common.font_scale"),
            type = "int", default = 100, min = 70, max = 160 },
        { key = "textColor", label = l10n.tr("lua_widget.common.text_color"),
            type = "color", default = 0xFFFFFF },
    },
}

local palettes = {
    dark = {
        text = 0xFFFFFF, muted = 0xCBD5E1, accent = 0xFFFFFF,
        surface = 0xFFFFFF, border = 0xFFFFFF, input = 0xFFFFFF,
    },
    light = {
        text = 0x0F172A, muted = 0x475569, accent = 0x0F172A,
        surface = 0x000000, border = 0x0F172A, input = 0x000000,
    },
}

local function palette(context)
    local result = context.theme and context.theme.mode == "light" and
        palettes.light or palettes.dark
    local follows = storage.get("followPersonalization") == "1" or
        storage.get("followPersonalization") == "true"
    if follows then return result end
    local configured = tonumber(storage.get("textColor"))
    if not configured then return result end
    return {
        text = configured, muted = configured, accent = configured,
        surface = result.surface, border = configured, input = configured,
    }
end

local function fontScale()
    local value = tonumber(storage.get("fontScale"))
    if value then return math.max(70, math.min(160, value)) / 100 end
    local legacy = math.max(10, math.min(24,
        tonumber(storage.get("fontSize")) or 15))
    return legacy / 15
end

local function trim(value)
    return (value or ""):match("^%s*(.-)%s*$")
end

local function cancelSearchTasks(model)
    for id in pairs(model.searchTasks) do
        task.cancel(tonumber(id))
    end
    model.searchTasks = {}
end

local function clearResults(model)
    model.results = { desktop = {}, app = {}, everything = {} }
    model.rows = {}
    model.selectedRef = nil
    model.searchErrors = {}
end

local function appendSection(model, source, label)
    local items = model.results[source]
    if not items or #items == 0 then return end
    model.rows[#model.rows + 1] = { kind = "header", title = label }
    for _, item in ipairs(items) do
        item.kind = source == "app" and "app" or "item"
        model.rows[#model.rows + 1] = { kind = "item", item = item }
    end
end

local function rebuildRows(model)
    model.rows = {}
    appendSection(model, "desktop",
        l10n.tr("lua_widget.quick_launcher.desktop_results",
            tostring(#model.results.desktop)))
    appendSection(model, "app",
        l10n.tr("lua_widget.quick_launcher.application_results",
            tostring(#model.results.app)))
    appendSection(model, "everything",
        l10n.tr("lua_widget.quick_launcher.everything_results",
            tostring(#model.results.everything)))
    if model.selectedRef then
        for _, row in ipairs(model.rows) do
            if row.kind == "item" and row.item.ref == model.selectedRef then
                return
            end
        end
    end
    model.selectedRef = nil
end

local function startSearchTask(model, taskName, source, limit)
    local taskId, taskError = task.start(taskName, {
        query = model.query, limit = limit, offset = 0,
    })
    if taskId then
        model.searchTasks[tostring(taskId)] = {
            source = source, query = model.query, task = taskName,
        }
    else
        model.searchErrors[source] = taskError or "searchRejected"
    end
end

local function appIndexReady()
    if not appIndexStatus then return true end
    local snapshot = appIndexStatus:value()
    return snapshot.available and snapshot.value and
        snapshot.value.state == "ready"
end

local function beginSearch(model)
    cancelSearchTasks(model)
    clearResults(model)
    if model.query == "" then return end

    startSearchTask(model, "desktop.search", "desktop", 100)
    if widget.hasFeature("task.app.search") and
        widget.hasPermission("app.discovery") and appIndexReady() then
        startSearchTask(model, "app.search", "app", 40)
    end
    if widget.hasFeature("task.everything.search") and
        widget.hasPermission("everything.search") then
        startSearchTask(model, "everything.search", "everything", 40)
    end
end

local function queueSearch(model, delay)
    cancelSearchTasks(model)
    clearResults(model)
    schedule.cancel("quick-search")
    if model.query ~= "" then
        schedule.after("quick-search", delay or 140,
            { whenHidden = "pause" })
    end
    interaction.setScrollOffset("quick-results", 0)
end

local function setup(context)
    desktopChanges = data.subscribe("desktop.changes", {
        maxAgeMs = 1000, whenHidden = "pause",
    })
    if widget.hasFeature("data.app.indexStatus") and
        widget.hasPermission("app.discovery") then
        appIndexStatus = data.subscribe("app.indexStatus", {
            maxAgeMs = 1000, whenHidden = "throttle",
        })
    end
    widget.setTitle(l10n.tr("lua_widget.quick_launcher.name"))
    local model = {
        query = trim(storage.get("query") or ""),
        results = { desktop = {}, app = {}, everything = {} },
        rows = {}, searchTasks = {}, actionTasks = {},
        searchErrors = {}, selectedRef = nil,
        desktopRevision = nil, appRevision = nil,
    }
    if context.preview and model.query ~= "" then
        model.results.app = {
            {
                ref = "preview-app",
                title = l10n.tr("lua_widget.quick_launcher.preview_result"),
                preview = true,
            },
        }
        rebuildRows(model)
    elseif model.query ~= "" then
        schedule.after("quick-search", 1, { whenHidden = "pause" })
    end
    return model
end

local function syncQueryAndSources(model)
    local query = trim(storage.get("query") or "")
    if query ~= model.query then
        model.query = query
        schedule.cancel("quick-search")
        beginSearch(model)
        interaction.setScrollOffset("quick-results", 0)
    end

    if desktopChanges then
        local snapshot = desktopChanges:value()
        local revision = snapshot.available and snapshot.value and
            snapshot.value.revision or nil
        if revision and model.desktopRevision and
            revision ~= model.desktopRevision and model.query ~= "" then
            queueSearch(model, 60)
        end
        if revision then model.desktopRevision = revision end
    end

    if appIndexStatus then
        local snapshot = appIndexStatus:value()
        local revision = snapshot.available and snapshot.value and
            snapshot.value.revision or nil
        if revision and model.appRevision and
            revision ~= model.appRevision and model.query ~= "" then
            queueSearch(model, 60)
        end
        if revision then model.appRevision = revision end
    end
end

local function registerRegion(key, shape, events, label)
    interaction.region({
        key = key, shape = shape, cursor = "hand", events = events,
        accessibility = { role = "listitem", label = label },
    })
end

local function drawCenteredStatus(text, x, y, width, height, font, color,
    unit, alpha)
    local measured = draw.measureText(text, font, 0, false)
    local measuredWidth = math.min(width, measured.width)
    local drawY = y + math.max(0, (height - measured.height) / 2)
    draw.text(x + math.max(0, (width - measuredWidth) / 2), drawY,
        text, font, color, math.max(unit, measuredWidth + unit),
        false, true, 0, alpha or 1.0)
end

local function render(context, model)
    syncQueryAndSources(model)
    local colors = palette(context)
    local width = layout.contentWidth()
    local height = layout.contentHeight()
    local metrics = componentMetrics()
    local unit = metrics.strokeWidth
    local scale = fontScale()
    local fontSize = metrics.bodyFontSize * scale
    local inputFont = metrics.controlFontSize * scale
    local smallFont = metrics.captionFontSize * scale
    local titleFont = metrics.titleFontSize * scale
    local contentInset = metrics.spacingSm
    local inputHeight = math.min(metrics.layoutRowHeight,
        math.max(unit, height - contentInset * 2))
    local inputTop = contentInset

    control.textInput({
        key = "quick.search", storageKey = "query",
        shape = { type = "rect", x = contentInset, y = inputTop,
            width = width - contentInset * 2, height = inputHeight },
        placeholder = l10n.tr("lua_widget.quick_launcher.search_placeholder"),
        fontSize = inputFont, textColor = colors.input,
        placeholderColor = colors.muted, backgroundColor = colors.surface,
        borderColor = colors.border, focusedBorderColor = colors.accent,
        backgroundAlpha = 0.065, focusedBackgroundAlpha = 0.11,
        borderAlpha = 0.16, focusedBorderAlpha = 0.70,
        radius = metrics.controlRadius, padding = metrics.spacingSm,
        borderThickness = metrics.strokeWidth, selectAll = false,
        liveUpdate = true, maxBytes = 256,
    })

    local listTop = math.min(height,
        inputTop + inputHeight + metrics.spacingXs)
    local listBottom = height - metrics.spacingXs
    local viewportHeight = math.max(unit, listBottom - listTop)
    local viewport = { type = "rect", x = contentInset, y = listTop,
        width = width - contentInset * 2, height = viewportHeight }
    interaction.region({
        key = "quick.surface", shape = viewport,
        events = { contextMenu = {
            id = "quick.menu", scope = "component" } },
        accessibility = { role = "list", label = descriptor.name },
    })

    if model.query == "" then
        drawCenteredStatus(
            l10n.tr("lua_widget.quick_launcher.empty_prompt"),
            contentInset, listTop, width - contentInset * 2, viewportHeight,
            titleFont, colors.text, unit, 0.78)
        return
    end

    if #model.rows == 0 then
        local searching = next(model.searchTasks) ~= nil
        if searching then
            drawCenteredStatus(
                l10n.tr("lua_widget.quick_launcher.searching"),
                contentInset, listTop, width - contentInset * 2, viewportHeight,
                smallFont, colors.muted, unit, 0.62)
        else
            drawCenteredStatus(
                l10n.tr("lua_widget.quick_launcher.no_matches"),
                contentInset, listTop, width - contentInset * 2, viewportHeight,
                titleFont, colors.text, unit, 0.78)
        end
        return
    end

    local headerHeight = smallFont + metrics.spacingMd
    local itemHeight = metrics.layoutRowHeight
    local itemGap = metrics.spacingXs
    local itemStride = itemHeight + itemGap
    local offsets = {}
    local contentHeight = 0
    for index, row in ipairs(model.rows) do
        offsets[index] = contentHeight
        contentHeight = contentHeight +
            (row.kind == "header" and headerHeight or itemStride)
    end
    if model.rows[#model.rows].kind ~= "header" then
        contentHeight = contentHeight - itemGap
    end
    local scroll = interaction.scroll({
        key = "quick-results", shape = viewport,
        contentHeight = math.ceil(contentHeight),
    })
    local iconSize = math.max(fontSize + metrics.spacingXs,
        metrics.spacingLg)
    draw.pushClip(contentInset, listTop,
        width - contentInset * 2, viewportHeight)
    for index, row in ipairs(model.rows) do
        local rowHeight = row.kind == "header" and headerHeight or itemStride
        local y = listTop + offsets[index] - scroll.offset
        if y + rowHeight >= listTop and y <= listTop + viewportHeight then
            if row.kind == "header" then
                draw.text(contentInset + metrics.spacingXs,
                    y + metrics.spacingXs, row.title,
                    smallFont, colors.muted,
                    width - contentInset * 2 - metrics.spacingSm,
                    false, true)
            else
                local item = row.item
                local key = "quick.item." .. tostring(index)
                local selected = item.ref == model.selectedRef
                local hovered = interaction.isHovered(key)
                if selected or hovered then
                    draw.rect(contentInset, y, width - contentInset * 2,
                        itemHeight, colors.surface,
                        metrics.controlRadius, selected and 0.12 or 0.075)
                end
                if selected then
                    draw.strokeRect(contentInset + unit, y + unit,
                        width - contentInset * 2 - unit * 2,
                        itemHeight - unit * 2, colors.accent,
                        math.max(0, metrics.controlRadius - unit),
                        metrics.strokeWidth, 0.42)
                end
                local actionValue = {
                    ref = item.ref, kind = item.kind, title = item.title,
                }
                registerRegion(key, {
                    type = "roundedRect", x = contentInset, y = y,
                    width = width - contentInset * 2,
                    height = itemHeight,
                    radius = metrics.controlRadius,
                }, {
                    click = { id = "quick.select", value = actionValue },
                    doubleClick = { id = "quick.open", value = actionValue },
                    contextMenu = { id = "quick.menu", value = actionValue },
                }, item.title)
                local iconX = contentInset + metrics.spacingSm
                local iconY = y + math.max(0, (itemHeight - iconSize) / 2)
                if item.preview then
                    draw.fluent(fluent.open, iconX, iconY, iconSize,
                        colors.text)
                else
                    draw.icon(item.ref, iconX, iconY, iconSize,
                        (selected or hovered) and 1.0 or 0.86)
                end
                local textX = iconX + iconSize + metrics.spacingSm
                local textMetrics = draw.measureText(
                    item.title, fontSize, 0, false)
                local textY = y + math.max(0,
                    (itemHeight - textMetrics.height) / 2)
                draw.text(textX, textY, item.title,
                    fontSize, colors.text,
                    math.max(unit,
                        width - contentInset - textX - metrics.spacingXs),
                    false, true)
            end
        end
    end
    draw.popClip()
end

local function startAction(model, taskName, reference)
    local taskId, taskError = task.start(taskName,
        reference and { ref = reference } or nil)
    if taskId then
        model.actionTasks[tostring(taskId)] = taskName
    else
        widget.log("warn", taskName .. " rejected: " ..
            tostring(taskError))
    end
end

local function openItem(model, value)
    if not value or not value.ref then return end
    model.selectedRef = value.ref
    if value.kind == "app" then
        if not widget.hasFeature("task.app.launch") then return end
        if not widget.hasPermission("app.launch") then
            widget.openSettings()
            return
        end
        startAction(model, "app.launch", value.ref)
    else
        if not widget.hasFeature("task.shell.item") then return end
        if not widget.hasPermission("desktop.action") then
            widget.openSettings()
            return
        end
        startAction(model, "shell.openItem", value.ref)
    end
end

local function event(_context, model, value)
    if value.kind == "schedule" and value.id == "quick-search" then
        beginSearch(model)
        return
    elseif value.kind == "environment" then
        widget.setTitle(l10n.tr("lua_widget.quick_launcher.name"))
        return
    elseif value.kind == "task.complete" then
        local search = model.searchTasks[tostring(value.taskId)]
        if search then
            model.searchTasks[tostring(value.taskId)] = nil
            if search.query == model.query then
                if value.ok and value.value then
                    model.results[search.source] = value.value.items or {}
                    model.searchErrors[search.source] = nil
                else
                    model.results[search.source] = {}
                    model.searchErrors[search.source] =
                        value.error or "searchFailed"
                end
                rebuildRows(model)
            end
            return
        end
        local actionName = model.actionTasks[tostring(value.taskId)]
        if actionName then
            model.actionTasks[tostring(value.taskId)] = nil
            if not value.ok then
                widget.log("warn", actionName .. " failed: " ..
                    tostring(value.error))
                if value.error == "staleReference" or
                    value.error == "invalidReference" then
                    queueSearch(model, 1)
                end
            end
        end
        return
    elseif value.kind ~= "action" then
        return
    end

    if value.id == "quick.select" and value.value then
        model.selectedRef = value.value.ref
    elseif value.id == "quick.open" then
        openItem(model, value.value)
    elseif value.id == "quick.reveal" and value.value and
        value.value.kind ~= "app" then
        model.selectedRef = value.value.ref
        if widget.hasFeature("task.shell.item") and
            widget.hasPermission("desktop.action") then
            startAction(model, "shell.revealItem", value.value.ref)
        else
            widget.openSettings()
        end
    elseif value.id == "quick.refresh" then
        if widget.hasFeature("task.desktop.refresh") and
            widget.hasPermission("desktop.action") then
            startAction(model, "desktop.refresh")
        else
            widget.openSettings()
        end
    elseif value.id == "quick.edit" then
        control.focus("quick.search")
    end
end

local function menu(_context, model, request)
    if request.id ~= "quick.menu" then return nil end
    local value = request.value
    local canDesktopAction = widget.hasPermission("desktop.action")
    local canOpen = value and value.ref and
        ((value.kind == "app" and widget.hasFeature("task.app.launch") and
            widget.hasPermission("app.launch")) or
            (value.kind ~= "app" and widget.hasFeature("task.shell.item") and
                canDesktopAction))
    local items = {}
    if value and value.ref then
        items[#items + 1] = { id = "quick.open",
            label = l10n.tr("lua_widget.quick_launcher.open_match"),
            icon = fluent.open, iconFont = "fluent", enabled = canOpen }
        if value.kind ~= "app" then
            items[#items + 1] = { id = "quick.reveal",
                label = l10n.tr("lua_widget.quick_launcher.reveal_match"),
                icon = fluent.folderOpen, iconFont = "fluent",
                enabled = canDesktopAction and
                    widget.hasFeature("task.shell.item") }
        end
        return ui.menu(items)
    end
    items[#items + 1] = { id = "quick.edit",
        label = l10n.tr("lua_widget.quick_launcher.edit_query"),
        icon = fluent.edit, iconFont = "fluent" }
    items[#items + 1] = { id = "quick.refresh",
        label = l10n.tr("lua_widget.quick_launcher.refresh_desktop"),
        icon = fluent.refresh, iconFont = "fluent",
        enabled = canDesktopAction and
            widget.hasFeature("task.desktop.refresh") }
    if #model.rows > 0 then
        local count = 0
        for _, row in ipairs(model.rows) do
            if row.kind == "item" then count = count + 1 end
        end
        items[#items + 1] = { type = "separator" }
        items[#items + 1] = { id = "quick.count",
            label = l10n.tr("lua_widget.quick_launcher.match_count",
                tostring(count)), enabled = false }
    end
    return ui.menu(items)
end

local function dispose(_context, model)
    cancelSearchTasks(model)
    for id in pairs(model.actionTasks) do task.cancel(tonumber(id)) end
    schedule.cancel("quick-search")
    if desktopChanges then desktopChanges:unsubscribe() end
    if appIndexStatus then appIndexStatus:unsubscribe() end
end

descriptor = {
    name = l10n.tr("lua_widget.quick_launcher.name"),
    useCustomStyle = true,
    followPersonalizationDefault = true,
    showTitle = true,
    bottomBarHover = false,
    bg = 0x151A21,
    border = 0xFFFFFF,
    alpha = 0.40,
    borderAlpha = 0.22,
    gradientEndA = 0.32,
    settings = settings,
    setup = setup,
    render = render,
    event = event,
    menu = menu,
    dispose = dispose,
}

return widget.define(descriptor)
