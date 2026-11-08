local catalog = module.require("modules/cities.lua")
local defaultCities = { "Tokyo Standard Time", "GMT Standard Time", "Eastern Standard Time" }

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

    for i, defaultCity in ipairs(defaultCities) do
        local city = catalog.byId[storage.get("city" .. i)] or catalog.byId[defaultCity]
        local parts = time.parts(now, city.zone)
        local dayDifference = dayFromUtc(parts, utc) - localDay
        local offset = dayDifference * 1440 + minuteOfDay(parts) - localMinute
        model.cities[i] = {
            id = city.storageId,
            label = city.label,
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
    local model = { cities = {}, query = "", results = catalog.search(""), queryRevision = 0 }
    update(model)
    return model
end

local function updateQuery(model, query)
    if query == model.query then return end
    model.query = query
    model.results = catalog.search(query)
    model.queryRevision = model.queryRevision + 1
end

local function selectCity(model, city)
    local slot = model.pickerSlot
    if not slot or not city then return end
    if model.cities[slot].id ~= city.storageId then
        storage.set("city" .. slot, city.storageId)
        refresh(model, time.now())
    end
    widget.closePanel()
end

local function event(context, model, change)
    if (change.kind == "schedule" and change.id == "minute")
        or change.kind == "environment" or (change.kind == "visibility" and change.visible) then
        update(model)
    elseif change.kind == "panel" and change.action == "closed" then
        model.pickerSlot = nil
    elseif change.kind == "action" then
        if change.id == "open.city" and model.cities[change.value] then
            model.pickerSlot = change.value
            updateQuery(model, "")
            model.queryRevision = model.queryRevision + 1
            local row = ui.metrics().layoutRowHeight
            widget.openPanel({ title = l10n.tr("lua_widget.world_clock.choose_city"),
                width = math.ceil(row * 11), height = math.ceil(row * 12) })
        elseif change.id == "query.change" then
            updateQuery(model, change.text or "")
        elseif change.id == "query.submit" and #model.results == 1 then
            selectCity(model, model.results[1])
        elseif change.id == "choose.city" then
            selectCity(model, catalog.byId[change.value])
        end
    end
end

local function panel(context, model)
    local row = ui.metrics().layoutRowHeight
    local inset = math.min(row * 6 / 28, context.layoutSize.width / 4)
    local gap = row * 4 / 28
    local label = l10n.tr("lua_widget.world_clock.search_city")
    local searchBox = view.searchBox({
        key = "city.query", width = "fill", height = row, flexShrink = 0,
        value = model.query, placeholder = label, maxBytes = 256, fontSize = row * 12 / 28,
        padding = { horizontal = row * 9 / 28, vertical = 0 },
        style = { foreground = "textPrimary", borderColor = "border",
            borderWidth = row / 28, cornerRadius = row * 6 / 28 },
        accessibility = { label = label },
        events = { change = { id = "query.change" }, submit = { id = "query.submit" } },
    })

    local current = model.cities[model.pickerSlot or 1].id
    local results = {}
    for _, city in ipairs(model.results) do
        local selected = city.storageId == current
        results[#results + 1] = view.button({
            key = "city.result." .. city.id, label = city.label,
            width = "fill", height = row, flexShrink = 0,
            fontSize = row * 12 / 28, textAlign = "start",
            padding = { horizontal = row * 9 / 28, vertical = 0 },
            style = { foreground = "textPrimary",
                background = selected and "surfaceVariant" or nil,
                borderColor = "systemAccent", borderWidth = selected and row / 28 or 0,
                cornerRadius = row * 6 / 28 },
            events = { click = { id = "choose.city", value = city.storageId } },
        })
    end
    if #results == 0 then
        results[1] = view.text({
            key = "city.empty", text = l10n.tr("lua_widget.world_clock.no_matches"),
            width = "fill", height = row, fontSize = row * 12 / 28,
            padding = { horizontal = row * 9 / 28, vertical = 0 },
            style = { foreground = "textSecondary" },
        })
    end

    local scrollKey = "city.scroll." .. model.queryRevision
    if context.layoutSize.height < 2 * row + 2 * inset + gap then
        table.insert(results, 1, searchBox)
        return view.scroll({ key = scrollKey, width = "fill", height = "fill", children = {
            view.column({ key = "city.all", width = "fill", height = "auto",
                padding = inset, gap = gap, children = results }),
        } })
    end
    return view.column({ key = "city.panel", width = "fill", height = "fill", gap = gap, children = {
        view.column({ key = "city.header", width = "fill", height = row + inset, flexShrink = 0,
            padding = { left = inset, right = inset, top = inset, bottom = 0 },
            children = { searchBox } }),
        view.scroll({ key = scrollKey, width = "fill", height = "fill", children = {
            view.column({ key = "city.results", width = "fill", height = "auto",
                padding = { left = inset, right = inset, top = 0, bottom = inset },
                gap = gap, children = results }),
        } }),
    } })
end

local lightInk = { main = 0xF8FAFF, muted = 0xD8E0EF, day = 0xFFC66D, night = 0xD7BCFF }
local darkInk = { main = 0x152237, muted = 0x47566C, day = 0x875018, night = 0x63419B }

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
    if w <= 0 or h <= 0 then return end
    local stacked = w / h < 1.35
    local scale = math.min(w / (stacked and 260 or 420), h / 224)
    local left, right = 13, w / scale - 13
    -- Anchor the first and last text rows close to the edges at any aspect ratio.
    local verticalScale = (h - 34 * scale) / 190
    local originY = 18 * (scale - verticalScale)
    local theme = widget.theme()
    local ink = theme and theme.contentTheme == 1 and darkInk or lightInk

    local function yAt(y)
        return originY + y * verticalScale
    end

    local function textAt(value, x, y, size, maxWidth, color, bold, rightAligned)
        local fontSize = size * scale
        local budget = maxWidth * scale
        local measured = draw.measureText(value, fontSize, 0, bold)
        if measured.width > budget then
            fontSize = fontSize * budget / measured.width
            measured = draw.measureText(value, fontSize, 0, bold)
        end
        local start = x * scale
        if rightAligned then start = start - measured.width end
        draw.text(start, yAt(y) - measured.height / 2,
            value, fontSize, color, budget * 1.02, bold, true)
        return start, measured.width
    end

    local function rule(x1, y1, x2, y2)
        draw.line(x1 * scale, yAt(y1), x2 * scale, yAt(y2), 0.7 * scale, ink.muted, 0.5)
    end

    local function cityButton(city, index, startX, endX, top, bottom, arrowY)
        local key = "city." .. index
        local label = l10n.tr("lua_widget.world_clock.change_city", city.label)
        local x, y = (startX + 2) * scale, yAt(top)
        local width = (endX - startX - 4) * scale
        local height = math.min(h - 3 * scale, yAt(bottom)) - y
        local radius = 5 * scale
        interaction.region({
            key = key, cursor = "hand", focusable = true, tooltip = label,
            shape = { type = "roundedRect", x = x, y = y, width = width, height = height, radius = radius },
            accessibility = { role = "button", label = label },
            events = { click = { id = "open.city", value = index } },
        })
        if interaction.isHovered(key) then
            draw.rect(x, y, width, height, ink.main, radius, 0.08)
        end
        if interaction.isFocused(key) then
            draw.strokeRect(x, y, width, height, ink.main, radius, scale, 0.8)
        end
        local arrowX, arrowCenter = (endX - 8) * scale, yAt(arrowY)
        draw.line(arrowX - 4 * scale, arrowCenter - 2 * scale, arrowX, arrowCenter + 2 * scale,
            0.8 * scale, ink.muted, 0.66)
        draw.line(arrowX, arrowCenter + 2 * scale, arrowX + 4 * scale, arrowCenter - 2 * scale,
            0.8 * scale, ink.muted, 0.66)
    end

    local function cityName(city, x, y, width)
        local start, measured = textAt(city.label, x, y, 15, width - 18, ink.main, true)
        drawDaylight(start + measured + 10 * scale, yAt(y),
            5.8 * scale, city.daylight, ink, 0.95 * scale)
    end

    textAt(l10n.tr("lua_widget.world_clock.local"), left, 18, 16, 125, ink.main, true)
    textAt(model.localTime, left, stacked and 53 or 63, stacked and 47 or 59, 218, ink.main, true)
    local dateStart = textAt(model.localDate, right, 18, 15, 81, ink.main, false, true)
    drawDaylight(dateStart - 14 * scale, yAt(18), 6.5 * scale, model.localDaylight, ink, scale)

    if stacked then
        rule(left, 84, right, 84)
        local valueRight = right - 21
        local nameWidth = valueRight - left - 77
        local lineGap = 18 * scale / verticalScale
        for i, city in ipairs(model.cities) do
            local y = 122 + (i - 1) * 43
            cityButton(city, i, left, right, y - 35, y + 9, y - lineGap / 2)
            cityName(city, left, y - lineGap, nameWidth)
            textAt(city.detail, left, y, 12, nameWidth, ink.muted, false)
            textAt(city.clock, valueRight, y - lineGap / 2, 23, 69, ink.main, true, true)
            if i < #model.cities then rule(left, y + 11, right, y + 11) end
        end
        return
    end

    rule(left, 105, right, 105)
    local columnWidth = (right - left) / #model.cities
    for i, city in ipairs(model.cities) do
        local columnLeft = left + (i - 1) * columnWidth
        local columnRight = columnLeft + columnWidth
        local x = columnLeft
        if i > 1 then
            rule(columnLeft, 116, columnLeft, 216)
            x = x + 12
        end
        cityButton(city, i, columnLeft, columnRight, 111, 221, 130)
        local width = columnRight - x - 8
        cityName(city, x, 130, width - 12)
        textAt(city.clock, x, 170, 33, width, ink.main, true)
        textAt(city.detail, x, 208, 13.5, width, ink.muted, false)
    end
end
return widget.define({
    useCustomStyle = true,
    followPersonalizationDefault = true,
    showTitle = false,
    setup = setup,
    event = event,
    render = render,
    panel = panel,
})
