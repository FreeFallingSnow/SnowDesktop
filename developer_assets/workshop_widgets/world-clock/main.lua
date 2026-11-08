local zones = {
    { id = "China Standard Time", label = l10n.tr("lua_widget.world_clock.shanghai") },
    { id = "Tokyo Standard Time", label = l10n.tr("lua_widget.world_clock.tokyo") },
    { id = "Korea Standard Time", label = l10n.tr("lua_widget.world_clock.seoul") },
    { id = "Singapore Standard Time", label = l10n.tr("lua_widget.world_clock.singapore") },
    { id = "India Standard Time", label = l10n.tr("lua_widget.world_clock.new_delhi") },
    { id = "Arabian Standard Time", label = l10n.tr("lua_widget.world_clock.dubai") },
    { id = "Romance Standard Time", label = l10n.tr("lua_widget.world_clock.paris") },
    { id = "GMT Standard Time", label = l10n.tr("lua_widget.world_clock.london") },
    { id = "Eastern Standard Time", label = l10n.tr("lua_widget.world_clock.new_york") },
    { id = "Pacific Standard Time", label = l10n.tr("lua_widget.world_clock.los_angeles") },
    { id = "AUS Eastern Standard Time", label = l10n.tr("lua_widget.world_clock.sydney") },
    { id = "New Zealand Standard Time", label = l10n.tr("lua_widget.world_clock.auckland") },
}

local zoneIds, zoneLabels, zonesById = {}, {}, {}
for i, zone in ipairs(zones) do
    zoneIds[i], zoneLabels[i] = zone.id, zone.label
    zonesById[zone.id] = zone
end

local fields = {}
for i, defaultZone in ipairs({ "Tokyo Standard Time", "GMT Standard Time", "Eastern Standard Time" }) do
    fields[i] = {
        key = "city" .. i,
        type = "select",
        label = l10n.tr("lua_widget.world_clock.city_slot", tostring(i)),
        default = defaultZone,
        options = zoneIds,
        optionLabels = zoneLabels,
    }
end

local glassStyle = {
    bg = 0x192742,
    border = 0xCCD6E8,
    alpha = 0.82,
    borderAlpha = 0.38,
    gradientEndA = 0.69,
    glassEnabled = true,
}
local settings = {
    fields = fields,
    presets = {
        {
            id = "navy-glass",
            label = l10n.tr("lua_widget.world_clock.navy_glass"),
            default = true,
            values = {
                bg = glassStyle.bg,
                border = glassStyle.border,
                alpha = glassStyle.alpha,
                borderAlpha = glassStyle.borderAlpha,
                gradientEndA = glassStyle.gradientEndA,
                glassEnabled = glassStyle.glassEnabled,
                followPersonalization = false,
                __contentTheme = 0,
            },
        },
    },
}

local function dateKey(parts)
    return parts.year * 10000 + parts.month * 100 + parts.day
end

local function minuteOfDay(parts)
    return parts.hour * 60 + parts.min
end

local function dayFromUtc(parts, utc)
    -- A Windows zone's UTC offset can move the civil date by at most one day.
    local date, utcDate = dateKey(parts), dateKey(utc)
    if date == utcDate then return 0 end
    return date < utcDate and -1 or 1
end

local function formatClock(parts)
    return string.format("%02d:%02d", parts.hour, parts.min)
end

local function formatDate(parts)
    return l10n.tr("lua_widget.world_clock.date", tostring(parts.month), tostring(parts.day))
end

local function formatDay(dayDifference, parts)
    if dayDifference == -1 then return l10n.tr("lua_widget.world_clock.yesterday") end
    if dayDifference == 0 then return l10n.tr("lua_widget.world_clock.today") end
    if dayDifference == 1 then return l10n.tr("lua_widget.world_clock.tomorrow") end
    return formatDate(parts)
end

local function formatOffset(minutes)
    local magnitude = math.abs(minutes)
    local hours = (minutes < 0 and "−" or "+") .. tostring(magnitude // 60)
    if magnitude % 60 == 0 then
        return l10n.tr("lua_widget.world_clock.offset_hours", hours)
    end
    return l10n.tr("lua_widget.world_clock.offset_minutes", hours, string.format("%02d", magnitude % 60))
end

local function isDay(parts)
    return parts.hour >= 6 and parts.hour < 18
end

local function refresh(model, now)
    local localParts = time.parts(now, "local")
    local utc = time.parts(now, "utc")
    local localDay = dayFromUtc(localParts, utc)
    local localMinute = minuteOfDay(localParts)
    model.localTime = formatClock(localParts)
    model.localDate = formatDate(localParts)
    model.localDaylight = isDay(localParts)

    for i, field in ipairs(fields) do
        local zone = zonesById[storage.get(field.key)] or zonesById[field.default]
        local parts = time.parts(now, zone.id)
        local dayDifference = dayFromUtc(parts, utc) - localDay
        local offset = dayDifference * 1440 + minuteOfDay(parts) - localMinute
        model.cities[i] = {
            label = zone.label,
            clock = formatClock(parts),
            detail = formatDay(dayDifference, parts) .. "  " .. formatOffset(offset),
            daylight = isDay(parts),
        }
    end
end

local function update(model)
    local now = time.now()
    refresh(model, now)
    schedule.at("minute", (now // 60000 + 1) * 60000, { whenHidden = "pause" })
end

local function setup()
    local model = { cities = {} }
    update(model)
    return model
end

local function event(context, model, change)
    if (change.kind == "schedule" and change.id == "minute")
        or change.kind == "environment" or (change.kind == "visibility" and change.visible) then
        update(model)
    elseif change.kind == "settings.changed" then
        refresh(model, time.now())
    else
        return
    end
    widget.invalidate()
end

local lightInk = { main = 0xF8FAFF, muted = 0xD8E0EF, day = 0xFFC66D, night = 0xD7BCFF }
local darkInk = { main = 0x152237, muted = 0x47566C, day = 0x875018, night = 0x63419B }

local function drawGlobe(x, y, radius, ink, line)
    draw.arc(x, y, radius, 0, 360, line, ink)
    draw.arc(x - radius * 0.63, y, radius * 1.18, -58, 116, line, ink)
    draw.arc(x + radius * 0.63, y, radius * 1.18, 122, 116, line, ink)
    draw.line(x - radius, y, x + radius, y, line, ink)
end

local function drawDaylight(x, y, radius, daylight, palette, line)
    local color = daylight and palette.day or palette.night
    if daylight then
        draw.arc(x, y, radius * 0.62, 0, 360, line, color)
        for i = 0, 7 do
            local angle = i * math.pi / 4
            local dx, dy = math.cos(angle) * radius, math.sin(angle) * radius
            draw.line(x + dx * 0.86, y + dy * 0.86, x + dx * 1.18, y + dy * 1.18, line, color)
        end
    else
        draw.arc(x, y, radius, -90, -270, line, color)
        draw.arc(x + radius * 0.5, y - radius * 0.5, radius / math.sqrt(2), -135, -180, line, color)
    end
end

local function render(context, model)
    local w, h = layout.contentWidth(), layout.contentHeight()
    local scale = math.min(w / 420, h / 224)
    local verticalScale = math.min(h / 224, scale * 1.28)
    local left, top = (w - 420 * scale) / 2, (h - 224 * verticalScale) / 2
    local theme = widget.theme()
    local ink = theme and theme.contentTheme == 1 and darkInk or lightInk

    local function textAt(value, x, y, size, maxWidth, color, bold, rightAligned)
        local fontSize = size * scale
        local budget = maxWidth * scale
        local measured = draw.measureText(value, fontSize, 0, bold)
        if measured.width > budget then
            fontSize = fontSize * budget / measured.width
            measured = draw.measureText(value, fontSize, 0, bold)
        end
        local start = left + x * scale
        if rightAligned then start = start - measured.width end
        draw.text(start, top + y * verticalScale - measured.height / 2,
            value, fontSize, color, budget * 1.02, bold, true)
        return start, measured.width
    end

    local function rule(x1, y1, x2, y2)
        draw.line(left + x1 * scale, top + y1 * verticalScale,
            left + x2 * scale, top + y2 * verticalScale, 0.7 * scale, ink.muted, 0.42)
    end

    drawGlobe(left + 27 * scale, top + 22 * verticalScale, 7 * scale, ink.muted, 0.9 * scale)
    textAt(l10n.tr("lua_widget.world_clock.name"), 43, 22, 14, 350, ink.muted, false)
    textAt(l10n.tr("lua_widget.world_clock.local"), 20, 53, 16, 225, ink.main, true)
    textAt(model.localTime, 20, 87, 55, 265, ink.main, true)
    local dateStart = textAt(model.localDate, 400, 53, 15, 113, ink.main, false, true)
    drawDaylight(dateStart - 14 * scale, top + 53 * verticalScale, 6.5 * scale,
        model.localDaylight, ink, 1 * scale)
    rule(20, 123, 400, 123)

    local columnWidth = 380 / #model.cities
    for i, city in ipairs(model.cities) do
        local columnLeft = 20 + (i - 1) * columnWidth
        local x = columnLeft
        if i > 1 then
            rule(columnLeft, 135, columnLeft, 207)
            x = x + 14
        end
        local width = columnLeft + columnWidth - x - 8
        local labelStart, labelWidth = textAt(city.label, x, 145, 15, width - 18, ink.main, true)
        drawDaylight(labelStart + labelWidth + 10 * scale, top + 145 * verticalScale,
            5.8 * scale, city.daylight, ink, 0.95 * scale)
        textAt(city.clock, x, 172, 30, width, ink.main, true)
        textAt(city.detail, x, 199, 13.5, width, ink.muted, false)
    end
end

return widget.define({
    useCustomStyle = true,
    followPersonalizationDefault = false,
    showTitle = false,
    bg = glassStyle.bg,
    border = glassStyle.border,
    alpha = glassStyle.alpha,
    borderAlpha = glassStyle.borderAlpha,
    gradientEndA = glassStyle.gradientEndA,
    glassEnabled = glassStyle.glassEnabled,
    settings = settings,
    setup = setup,
    event = event,
    render = render,
})
