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

local function drawCentered(text, x, y, size, width, color, bold)
    local metrics = draw.measureText(text, size, 0, bold)
    local fittedSize = size * math.min(1, width / math.max(1, metrics.width))
    draw.text(x, y, text, fittedSize, color, width + 2, bold, true, 0, 1, nil, center)
end

local function render(context, model)
    local width, height = context.layoutSize.width, context.layoutSize.height
    local side = math.min(width, height)
    local x, y = width * 0.5, height * 0.5
    local textWidth = side * 0.84
    local darkForeground = widget.theme().contentTheme == 1
    local primary = darkForeground and 0x172634 or 0xF6FAFF
    local accent = darkForeground and 0x075473 or 0xA8E2FB
    local secondary = darkForeground and 0x44566A or 0xE2EBF4
    local date = model.date

    drawCentered(l10n.tr(weekdayKeys[date.wday]), x, y - side * 0.31,
        side * 0.112, textWidth, accent, true)
    drawCentered(tostring(date.day), x, y - side * 0.041,
        side * 0.56, textWidth, primary, true)
    draw.line(x - side * 0.11, y + side * 0.216,
        x + side * 0.11, y + side * 0.216, math.max(1, side * 0.009), accent, 0.72)
    local monthYear = l10n.tr("lua_widget.today_date.month_year",
        l10n.tr(monthKeys[date.month]), tostring(date.year))
    drawCentered(monthYear, x, y + side * 0.324,
        side * 0.097, textWidth, secondary, false)
end

return widget.define({
    name = l10n.tr("lua_widget.today_date.name"),
    showTitle = false,
    followPersonalizationDefault = true,
    setup = setup,
    event = event,
    render = render,
})
