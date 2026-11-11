-- screen-time/main.lua - today's foreground app usage
local usage

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

local function drawApps(apps, count, x, y, width, unit, colors)
    local font = 12 * unit
    local heading = l10n.tr("lua_widget.screen_time.top_apps")
    textLine(heading, x, y, width, font, colors.primary, true)
    y = y + 21 * unit
    for index = 1, count do
        local app = apps[index]
        local amount = duration(app.seconds)
        local amountWidth = draw.measureText(amount, font).width + unit
        if index > 1 then
            draw.line(x, y - 3 * unit, x + width, y - 3 * unit, unit, colors.primary, 0.18)
        end
        textLine(tostring(app.name or ""), x, y, width - amountWidth - 8 * unit, font, colors.primary, false)
        textLine(amount, x + width - amountWidth, y, amountWidth, font, colors.primary, false, 0.85)
        y = y + 22 * unit
    end
end

local function drawReport(data, width, height, colors)
    local unit = math.max(0.72, math.min(1.5, width / 216, height / 156))
    local pad = math.min(14 * unit, width / 10, height / 8)
    local innerWidth = width - 2 * pad
    local gap = 10 * unit
    local label = l10n.tr("lua_widget.screen_time.today")
    local status
    if not data then
        status = l10n.tr("lua_widget.screen_time.waiting")
    elseif data.enabled == false then
        status = l10n.tr("lua_widget.screen_time.paused")
    elseif (tonumber(data.totalSeconds) or 0) <= 0 then
        status = l10n.tr("lua_widget.screen_time.empty")
    end
    local statusHeight = status and 19 * unit or 0
    local total = data and duration(data.totalSeconds)
    local headerHeight = (total and 55 or 18) * unit + statusHeight
    if height - 2 * pad < headerHeight then
        textLine(total or status, pad, pad, innerWidth, math.min(24 * unit, height / 2), colors.primary, true)
        return
    end

    local available = height - 2 * pad - headerHeight - gap
    local apps = data and data.apps or {}
    local chartMin = 57 * unit
    local rowHeight = 22 * unit
    local listHeading = 21 * unit
    local hasChart = data and (tonumber(data.totalSeconds) or 0) > 0
        and available >= chartMin and innerWidth >= 145 * unit
    local chartWidth = innerWidth
    local chartHeight = 0
    local appCount = 0
    local detailHeight = 0
    local sideBySide = false
    if hasChart then
        sideBySide = #apps > 0 and width / height >= 1.6 and innerWidth >= 346 * unit
        if sideBySide then
            chartWidth = (innerWidth - gap) * 0.55
            appCount = math.min(3, #apps, math.floor((available - listHeading) / rowHeight))
            chartHeight = math.min(available, 148 * unit)
            detailHeight = math.max(chartHeight, listHeading + appCount * rowHeight)
        else
            appCount = math.min(3, #apps, math.max(0,
                math.floor((available - chartMin - gap - listHeading) / rowHeight)))
            local listHeight = appCount > 0 and gap + listHeading + appCount * rowHeight or 0
            chartHeight = math.min(available - listHeight, math.max(118 * unit, height * 0.50))
            detailHeight = chartHeight + listHeight
        end
    end

    local y = (height - headerHeight - detailHeight - (hasChart and gap or 0)) / 2
    textLine(label, pad, y, innerWidth, 12 * unit, colors.primary, false, 0.84)
    y = y + 18 * unit
    if total then
        local desired = 30 * unit
        local measured = draw.measureText(total, desired, 0, true).width
        local font = measured > 0 and desired * math.min(1, (innerWidth - 4 * unit) / measured) or desired
        textLine(total, pad, y, innerWidth, font, colors.primary, true)
        y = y + 37 * unit
    end
    if status then
        textLine(status, pad, y, innerWidth, 12 * unit, colors.primary, false, 0.84)
        y = y + statusHeight
    end
    if hasChart then
        y = y + gap
        drawChart(data.hours or {}, pad, y, chartWidth, chartHeight, unit, colors)
    end
    if appCount > 0 then
        local listX = sideBySide and pad + chartWidth + gap or pad
        local listY = sideBySide and y or y + chartHeight + gap
        drawApps(apps, appCount, listX, listY, width - pad - listX, unit, colors)
    end
end

local function render()
    local data, available = payload()
    local width, height = layout.width(), layout.height()
    local context = widget.hasFeature("widget.context") and widget.context() or nil
    local content = context and context.layoutSize
    drawReport(available and data or nil, content and content.width or width,
        content and content.height or height, palette(context))
    interaction.region({
        key = "screenTime.open",
        shape = { type = "rect", x = 0, y = 0, width = width, height = height },
        events = {
            doubleClick = { id = "screenTime.open" },
            contextMenu = { id = "screenTime.menu", scope = "component" },
        },
    })
end

local function menu(_, _, request)
    if request.id ~= "screenTime.menu" then return nil end
    return ui.menu({
        {
            id = "screenTime.open",
            label = l10n.tr("lua_widget.screen_time.open_report"),
        },
    })
end

local function event(_, _, value)
    if value.kind ~= "action" or value.id ~= "screenTime.open" then return end
    if widget.hasFeature("task.system.openSettings") then
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
