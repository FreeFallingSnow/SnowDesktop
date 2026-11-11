-- screen-time/main.lua - today's foreground app usage
local usage
local hourly = false

local function payload()
    if not usage then return nil end
    local snapshot = usage:value()
    if not snapshot then return nil end
    if snapshot.value and snapshot.value.totalSeconds ~= nil then
        return snapshot.value, snapshot.available
    end
    return snapshot, snapshot.available
end

local function duration(seconds)
    seconds = math.max(0, math.floor(tonumber(seconds) or 0))
    local hours = math.floor(seconds / 3600)
    local minutes = math.floor((seconds % 3600) / 60)
    if hours > 0 then
        return l10n.tr("lua_widget.screen_time.duration",
            tostring(hours), tostring(minutes))
    end
    if seconds < 60 then
        return l10n.tr("lua_widget.screen_time.duration_seconds", tostring(seconds))
    end
    return l10n.tr("lua_widget.screen_time.duration_minutes", tostring(minutes))
end

local function setup()
    usage = data.subscribe("system.screenTime", { maxAgeMs = 120000 })
    hourly = storage.get("reportView") == "hours"
end

local function mixColor(from, to, fraction)
    local function channel(shift)
        local start = (from >> shift) & 255
        local finish = (to >> shift) & 255
        return math.floor(start + (finish - start) * fraction + 0.5)
    end
    return (channel(16) << 16) | (channel(8) << 8) | channel(0)
end

local function palette(context)
    local theme = widget.theme()
    local darkText = theme and theme.contentTheme == 1
    local primary = darkText and 0x13212A or 0xFFFFFF
    local system = context and context.theme
    local accent = system and system.accentColor or 0x0078D4
    return {
        primary = primary,
        accent = mixColor(accent, primary, system and system.highContrast and 1 or 0.32),
    }
end

local function textLine(value, x, y, width, size, color, bold, alpha)
    if width <= 0 then return end
    draw.text(x, y, value, size, color, width, bold, true, 0, alpha or 1)
end

local function drawChart(hours, x, y, width, height, unit, colors)
    local peak = 0
    for index = 1, 24 do
        peak = math.max(peak, tonumber(hours[index]) or 0)
    end
    -- A repeated local hour can contain more than 60 minutes on a clock change.
    local ceiling = math.max(1, math.ceil(peak / 3600)) * 3600
    local font = 11 * unit
    local topLabel = l10n.tr("lua_widget.screen_time.duration_minutes", tostring(ceiling // 60))
    local midLabel = l10n.tr("lua_widget.screen_time.duration_minutes", tostring(ceiling // 120))
    local labelWidth = math.max(draw.measureText(topLabel, font).width, draw.measureText(midLabel, font).width)
    local plotWidth = width - labelWidth - 6 * unit
    local plotHeight = height - 17 * unit
    local baseline = y + plotHeight
    local slot = plotWidth / 24
    local barWidth = slot * 0.72
    local scaleX = x + width - labelWidth
    for index, label in ipairs({topLabel, midLabel, tostring(0)}) do
        local lineY = y + plotHeight * (index - 1) / 2
        draw.line(x, lineY, x + plotWidth, lineY, unit, colors.primary, 0.24)
        local labelY = math.max(y, lineY - 7 * unit)
        local textWidth = draw.measureText(label, font).width
        textLine(label, scaleX + labelWidth - textWidth, labelY, textWidth + unit,
            font, colors.primary, false, 0.82)
    end
    for index = 1, 24 do
        local amount = tonumber(hours[index]) or 0
        if amount > 0 then
            local barHeight = math.max(1, plotHeight * amount / ceiling)
            draw.rect(x + (index - 0.5) * slot - barWidth / 2, baseline - barHeight,
                barWidth, barHeight, colors.accent, math.min(2 * unit, barWidth / 2))
        end
    end
    local tickWidth = draw.measureText("00:00", font).width + unit
    local step = plotWidth >= 7 * tickWidth and 6 or 12
    for hour = 0, 24, step do
        local position = math.max(0, math.min(plotWidth - tickWidth, plotWidth * hour / 24 - tickWidth / 2))
        textLine(string.format("%02d:00", hour), x + position, baseline + 4 * unit,
            tickWidth, font, colors.primary, false, 0.82)
    end
end

local function drawApp(app, peak, x, y, width, unit, trackOffset, colors)
    local font = 12 * unit
    local iconSize = 24 * unit
    if app.icon then
        draw.image(app.icon, x, y + 2 * unit, iconSize, iconSize)
    else
        draw.rect(x, y + 2 * unit, iconSize, iconSize, colors.primary, 5 * unit, 0.12)
        draw.strokeRect(x + 6 * unit, y + 8 * unit, 12 * unit, 10 * unit,
            colors.primary, 2 * unit, unit, 0.70)
        draw.line(x + 6 * unit, y + 11 * unit, x + 18 * unit, y + 11 * unit,
            unit, colors.primary, 0.70)
    end
    local amount = duration(app.seconds)
    local textX = x + iconSize + 8 * unit
    local trackWidth = width - (textX - x)
    local amountWidth = math.min(trackWidth * 0.64, draw.measureText(amount, font, 0, true).width + 3 * unit)
    textLine(tostring(app.name or ""), textX, y, trackWidth - amountWidth - 4 * unit,
        font, colors.primary, false)
    textLine(amount, x + width - amountWidth, y, amountWidth, font, colors.primary, true)
    local trackY = y + trackOffset
    draw.rect(textX, trackY, trackWidth, 4 * unit, colors.primary, 2 * unit, 0.14)
    local fill = trackWidth * math.min(1, (tonumber(app.seconds) or 0) / peak)
    if fill > 0 then
        draw.rect(textX, trackY, fill, 4 * unit, colors.accent, math.min(2 * unit, fill / 2))
    end
end

local function drawViewSwitch(x, y, hitSize, unit, colors)
    local key = "screenTime.switch"
    local focused = widget.hasFeature("interaction.keyboard") and interaction.isFocused(key)
    local size = math.min(28 * unit, 0.75 * hitSize)
    local buttonX, buttonY = x + hitSize - size, y + (hitSize - size) / 2
    unit = math.min(0.9 * unit, size / 25)
    draw.rect(buttonX, buttonY, size, size, colors.primary, 6 * unit, focused and 0.25 or 0.12)
    local left = buttonX + (size - 14 * unit) / 2
    local top = buttonY + (size - 14 * unit) / 2
    if hourly then
        for i = 0, 2 do
            local lineY = top + (2 + 5 * i) * unit
            draw.line(left, lineY, left + (14 - 3 * i) * unit, lineY, 2 * unit, colors.primary)
        end
    else
        for i = 0, 2 do
            local height = (6 + 4 * ((i + 1) % 3)) * unit
            draw.rect(left + 5 * i * unit, top + 14 * unit - height, 3 * unit, height, colors.primary, unit)
        end
    end
    local region = {
        key = key, cursor = "hand",
        shape = { type = "rect", x = x, y = y, width = hitSize, height = hitSize },
        events = {
            click = { id = key },
            contextMenu = { id = "screenTime.menu", scope = "component" },
        },
    }
    local target = hourly and l10n.tr("lua_widget.screen_time.top_apps")
        or l10n.tr("lua_widget.screen_time.hourly")
    if widget.hasFeature("interaction.tooltip") then region.tooltip = target end
    if widget.hasFeature("interaction.accessibility.metadata") then
        region.accessibility = { role = "button", label = target }
    end
    interaction.region(region)
end

local function drawReport(data, width, height, colors)
    local unit = math.max(0.72, math.min(1.5, width / 216, height / 156))
    local pad = math.min(14 * unit, width / 10)
    local topPad = math.min(12 * unit, height / 8)
    local bottomPad = math.min(8 * unit, height / 8)
    local innerWidth = width - 2 * pad
    local usableHeight = height - topPad - bottomPad
    local gap = 8 * unit
    local contentRowHeight = widget.hasFeature("ui.semanticMetrics.rowUnit")
        and ui.metrics().layoutRowHeight or 28
    local headerHeight = math.min(2 * contentRowHeight, usableHeight)
    local switchSize = math.min(headerHeight, innerWidth / 3)
    local valueWidth = innerWidth - switchSize - gap
    local status
    if not data then
        status = l10n.tr("lua_widget.screen_time.waiting")
    elseif data.enabled == false then
        status = l10n.tr("lua_widget.screen_time.paused")
    elseif (tonumber(data.totalSeconds) or 0) <= 0 then
        status = l10n.tr("lua_widget.screen_time.empty")
    end
    local total = data and duration(data.totalSeconds)
    local value, bold = total or status, total ~= nil
    local statusHeight = total and status and draw.measureText(status, 12 * unit).height + 3 * unit or 0
    local apps = data and data.apps or {}
    local trackOffset = math.max(19 * unit, draw.measureText(duration(60), 12 * unit, 0, true).height + 2 * unit)
    local rowContentHeight = math.max(26 * unit, trackOffset + 4 * unit)
    local rowMinimum = rowContentHeight + 3 * unit

    local desired = bold and headerHeight * 0.75 or math.min(12 * unit, headerHeight * 0.75)
    local fullLine = draw.measureText(value, desired, 0, bold)
    local measuredWidth = math.max(1, valueWidth - 2 * unit)
    local font = desired * math.min(1, measuredWidth / math.max(1, fullLine.width),
        headerHeight / math.max(1, fullLine.height))
    local textHeight = draw.measureText(value, font, 0, bold).height
    if headerHeight + statusHeight > usableHeight then statusHeight = 0 end
    local available = usableHeight - headerHeight - statusHeight - gap
    local columnGap = 20 * unit
    local columns = 1
    if width / height >= 1.45 then
        local fitting = math.floor((innerWidth + columnGap) / (150 * unit + columnGap))
        columns = math.max(1, math.min(3, fitting, #apps))
    end
    local fittingRows = math.max(0, math.floor(available / rowMinimum))
    local appCount = not hourly and math.min(#apps, columns * fittingRows) or 0
    local hasChart = hourly and data and (tonumber(data.totalSeconds) or 0) > 0
        and available >= 57 * unit and innerWidth >= 145 * unit
    local y = topPad
    if switchSize > 0 then
        drawViewSwitch(width - pad - switchSize, y + (headerHeight - switchSize) / 2,
            switchSize, unit, colors)
    end

    local textY = y + math.max(0, (headerHeight - textHeight) / 2)
    textLine(value, pad, textY, valueWidth, font, colors.primary, bold)
    y = y + headerHeight
    if statusHeight > 0 then
        textLine(status, pad, y, innerWidth, 12 * unit, colors.primary, false, 0.84)
        y = y + statusHeight
    end
    y = y + gap
    if hasChart then
        drawChart(data.hours or {}, pad, y, innerWidth, available, unit, colors)
    end
    if appCount > 0 then
        local columnWidth = (innerWidth - (columns - 1) * columnGap) / columns
        local rows = math.ceil(appCount / columns)
        local rowHeight = math.min(48 * unit, available / rows)
        local rowInset = (rowHeight - rowContentHeight) / 2
        local peak = math.max(1, tonumber(apps[1].seconds) or 0)
        for i = 0, appCount - 1 do
            drawApp(apps[i + 1], peak, pad + (i % columns) * (columnWidth + columnGap),
                y + (i // columns) * rowHeight + rowInset, columnWidth, unit, trackOffset, colors)
        end
    end
end

local function render()
    local data, available = payload()
    local width, height = layout.width(), layout.height()
    local context = widget.hasFeature("widget.context") and widget.context() or nil
    local content = context and context.layoutSize
    interaction.region({
        key = "screenTime.open",
        shape = { type = "rect", x = 0, y = 0, width = width, height = height },
        events = {
            doubleClick = { id = "screenTime.open" },
            contextMenu = { id = "screenTime.menu", scope = "component" },
        },
    })
    drawReport(available and data or nil, content and content.width or width,
        content and content.height or height, palette(context))
end

local function menu(_, _, request)
    if request.id ~= "screenTime.menu" then return nil end
    return ui.menu({
        {
            id = "screenTime.open",
            label = l10n.tr("lua_widget.screen_time.open_report"),
            icon = utf8.char(0xF582), iconFont = "fluent",
        },
    })
end

local function event(_, _, value)
    if value.kind ~= "action" then return end
    if value.id == "screenTime.switch" then
        hourly = not hourly
        storage.set("reportView", hourly and "hours" or "apps")
        widget.invalidate()
    elseif value.id == "screenTime.open" and widget.hasFeature("task.system.openSettings") then
        task.start("system.openSettings", { page = "screenTime" })
    end
end

return widget.define({
    name = l10n.tr("lua_widget.screen_time.name"),
    useCustomStyle = true,
    followPersonalizationDefault = true,
    bg = 0x10211F,
    border = 0xFFFFFF,
    alpha = 0.42,
    gradientEndA = 0.28,
    setup = setup,
    render = render,
    event = event,
    menu = menu,
})
