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

local function setup()
    schedule.every("date", 30000, { whenHidden = "pause" })
    return { date = time.parts() }
end

local function event(_context, model, change)
    if (change.kind == "schedule" and change.id == "date")
        or change.kind == "environment"
        or (change.kind == "visibility" and change.visible) then
        local date = time.parts()
        local previous = model.date
        model.date = date
        if date.day ~= previous.day or date.month ~= previous.month
            or date.year ~= previous.year or change.kind == "environment" then
            widget.invalidate()
        end
    end
end

local center = { x = "center", y = "center" }

local function render(context, model)
    local width, height = context.layoutSize.width, context.layoutSize.height
    local side = math.min(width, height)
    local textWidth = side * 0.84
    local darkForeground = widget.theme().contentTheme == 1
    local primary = darkForeground and 0x172634 or 0xF6FAFF
    local accent = darkForeground and 0x075473 or 0xA8E2FB
    local secondary = darkForeground and 0x44566A or 0xE2EBF4
    local date = model.date

    local monthYear = l10n.tr("lua_widget.today_date.month_year",
        l10n.tr(monthKeys[date.month]), tostring(date.year))
    local lines = {
        { text = l10n.tr(weekdayKeys[date.wday]), size = side * 0.116, color = accent, bold = true },
        { text = tostring(date.day), size = side * 0.50, color = primary, bold = true },
        { text = monthYear, size = side * 0.109, color = secondary, bold = false },
    }
    local gap = side * 0.071
    local blockHeight = gap * (#lines - 1)
    for _, line in ipairs(lines) do
        local metrics = draw.measureText(line.text, line.size, 0, line.bold)
        if metrics.width > textWidth then
            line.size = line.size * textWidth / metrics.width
            metrics = draw.measureText(line.text, line.size, 0, line.bold)
        end
        -- Use the visible glyph height when the host provides ink metrics.
        line.height = metrics.ink and metrics.ink.height or line.size * 0.78
        blockHeight = blockHeight + line.height
    end

    local x, y = width * 0.5, (height - blockHeight) * 0.5
    for _, line in ipairs(lines) do
        draw.text(x, y + line.height * 0.5, line.text, line.size, line.color,
            textWidth + 2, line.bold, true, 0, 1, nil, center)
        y = y + line.height + gap
    end
end

return widget.define({
    name = l10n.tr("lua_widget.today_date.name"),
    showTitle = false,
    followPersonalizationDefault = true,
    setup = setup,
    event = event,
    render = render,
})
