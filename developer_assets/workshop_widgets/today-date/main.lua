local weekdayKeys = {
    "lua_widget.today_date.sunday",
    "lua_widget.today_date.monday",
    "lua_widget.today_date.tuesday",
    "lua_widget.today_date.wednesday",
    "lua_widget.today_date.thursday",
    "lua_widget.today_date.friday",
    "lua_widget.today_date.saturday",
}

local monthKeys = {
    "lua_widget.today_date.january",
    "lua_widget.today_date.february",
    "lua_widget.today_date.march",
    "lua_widget.today_date.april",
    "lua_widget.today_date.may",
    "lua_widget.today_date.june",
    "lua_widget.today_date.july",
    "lua_widget.today_date.august",
    "lua_widget.today_date.september",
    "lua_widget.today_date.october",
    "lua_widget.today_date.november",
    "lua_widget.today_date.december",
}

local function localDate()
    local parts = time.parts()
    return calendar.dateInfo(string.format("%04d-%02d-%02d", parts.year, parts.month, parts.day))
end

local function setup()
    schedule.every("date", 30000, { whenHidden = "pause" })
    return { date = localDate() }
end

local function event(_context, model, change)
    if (change.kind == "schedule" and change.id == "date")
        or change.kind == "environment"
        or (change.kind == "visibility" and change.visible) then
        local date = localDate()
        local previous = model.date
        model.date = date
        if date.day ~= previous.day or date.month ~= previous.month
            or date.year ~= previous.year or change.kind == "environment" then
            widget.invalidate()
        end
    end
end

local center = { x = "center", y = "center" }

local function drawMonth(date, x, y, side, accent, today, neutral)
    local radius = side * 0.433
    local tickLength = side * 0.036
    local thickness = side * 0.012
    for day = 1, date.daysInMonth do
        local angle = (day - 1) * 2 * math.pi / date.daysInMonth - math.pi * 0.5
        local dx, dy = math.cos(angle), math.sin(angle)
        if day == date.day then
            local inner = radius - tickLength * 1.55
            draw.line(x + dx * inner, y + dy * inner,
                x + dx * radius, y + dy * radius, thickness * 1.6, today, 1)
            draw.circle(x + dx * radius, y + dy * radius, thickness * 0.95, today, 1)
        else
            local elapsed = day < date.day
            local inner = radius - tickLength
            draw.line(x + dx * inner, y + dy * inner,
                x + dx * radius, y + dy * radius, thickness,
                elapsed and accent or neutral, elapsed and 0.76 or 0.27)
        end
    end
end

local function render(context, model)
    local width, height = context.layoutSize.width, context.layoutSize.height
    local side = math.min(width, height)
    local darkForeground = widget.theme().contentTheme == 1
    local primary = darkForeground and 0x172634 or 0xF6FAFF
    local accent = darkForeground and 0x00665B or 0x8EF4D9
    local today = darkForeground and 0x9B4800 or 0xFFD58C
    local secondary = darkForeground and 0x44566A or 0xE2EBF4
    local date = model.date
    local x, centerY = width * 0.5, height * 0.5

    drawMonth(date, x, centerY, side, accent, today, primary)

    local lines = {
        { text = l10n.tr(weekdayKeys[date.weekday]), size = side * 0.104,
            width = side * 0.52, color = accent, bold = true },
        { text = tostring(date.day), size = side * 0.438,
            width = side * 0.67, color = primary, bold = true },
        { text = l10n.tr(monthKeys[date.month]), size = side * 0.096,
            width = side * 0.52, color = secondary, bold = false },
    }
    local gap = side * 0.043
    local blockHeight = gap * (#lines - 1)
    for _, line in ipairs(lines) do
        local metrics = draw.measureText(line.text, line.size, 0, line.bold)
        if metrics.width > line.width then
            line.size = line.size * line.width / metrics.width
            metrics = draw.measureText(line.text, line.size, 0, line.bold)
        end
        -- Use the visible glyph height when the host provides ink metrics.
        line.height = metrics.ink and metrics.ink.height or line.size * 0.78
        blockHeight = blockHeight + line.height
    end

    local y = centerY - blockHeight * 0.5
    for _, line in ipairs(lines) do
        draw.text(x, y + line.height * 0.5, line.text, line.size, line.color,
            line.width + 2, line.bold, true, 0, 1, nil, center)
        y = y + line.height + gap
    end
end

return widget.define({
    name = l10n.tr("lua_widget.today_date.name"),
    showTitle = false,
    useCustomStyle = true,
    followPersonalizationDefault = true,
    setup = setup,
    event = event,
    render = render,
})
