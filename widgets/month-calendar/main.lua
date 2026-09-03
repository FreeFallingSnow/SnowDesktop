-- month-calendar/main.lua - API v2 local calendar reader and shared selection
local selectedDateSubscription
local eventSubscription
local descriptor

local fluent = {
    today = utf8.char(0xF23C),
    previous = utf8.char(0xF15B),
    next = utf8.char(0xF181),
}

local settings = {
    fields = {
        {
            key = "weekStart",
            label = l10n.tr("lua_widget.month_calendar.week_start"),
            type = "select",
            default = "locale",
            options = { "locale", "monday", "sunday" },
            optionLabels = {
                l10n.tr("lua_widget.month_calendar.week_start_locale"),
                l10n.tr("lua_widget.month_calendar.week_start_monday"),
                l10n.tr("lua_widget.month_calendar.week_start_sunday"),
            },
        },
        {
            key = "showAdjacent",
            label = l10n.tr("lua_widget.month_calendar.show_adjacent"),
            type = "bool",
            default = true,
        },
        {
            key = "fontScale",
            label = l10n.tr("lua_widget.common.font_scale"),
            type = "int",
            default = 100,
            min = 70,
            max = 135,
        },
    },
}

local function loadStyle()
    descriptor.bg = tonumber(storage.get("bg")) or
        tonumber(storage.get("bgColor")) or 0x151A21
    descriptor.border = tonumber(storage.get("border")) or
        tonumber(storage.get("borderColor")) or 0xFFFFFF
    descriptor.alpha = tonumber(storage.get("alpha")) or 0.40
    descriptor.borderAlpha = tonumber(storage.get("borderAlpha")) or 0.18
    descriptor.gradientEndA = tonumber(storage.get("gradientEndA")) or 0.28
    if storage.get("followPersonalization") == "1" then
        local theme = widget.theme()
        if theme and theme.bg then
            descriptor.bg = theme.bg
            descriptor.border = theme.border or descriptor.border
            descriptor.alpha = theme.alpha or descriptor.alpha
            descriptor.borderAlpha = theme.borderAlpha or descriptor.borderAlpha
            descriptor.gradientEndA = theme.gradientEndA or
                descriptor.gradientEndA
        end
    end
end

local function palette()
    local theme = widget.theme()
    if theme and theme.contentTheme == 1 then
        return { text = 0x000000, inverse = 0xFFFFFF }
    end
    return { text = 0xFFFFFF, inverse = 0x000000 }
end

local function todayDate()
    local now = time.parts(time.now())
    return string.format("%04d-%02d-%02d", now.year, now.month, now.day)
end

local function monthDate(year, month)
    return string.format("%04d-%02d-01", year, month)
end

local function selectedDate()
    if selectedDateSubscription then
        local snapshot = selectedDateSubscription:value()
        if snapshot.available and snapshot.value and snapshot.value.date then
            return snapshot.value.date
        end
    end
    return nil
end

local function refreshEventSubscription(model)
    local first = monthDate(model.viewYear, model.viewMonth)
    local fromDate = calendar.addDays(first, -6)
    local toDate = calendar.addDays(first, 47)
    if not fromDate or not toDate then return end
    if eventSubscription then eventSubscription:unsubscribe() end
    eventSubscription = data.subscribe("calendar.events", {
        fromDate = fromDate,
        toDate = toDate,
        maxAgeMs = 1000,
        whenHidden = "throttle",
    })
    model.eventRange = fromDate .. ":" .. toDate
end

local function setViewFromDate(model, date)
    local info = calendar.dateInfo(date)
    if not info then return false end
    local changed = info.year ~= model.viewYear or info.month ~= model.viewMonth
    model.viewYear = info.year
    model.viewMonth = info.month
    if changed then refreshEventSubscription(model) end
    return true
end

local function selectDate(model, date)
    if not setViewFromDate(model, date) then return end
    if calendar.selectDate(date) then model.selectedDate = date end
end

local function returnToToday(model)
    selectDate(model, todayDate())
end

local function shiftMonth(model, delta)
    model.viewMonth = model.viewMonth + delta
    while model.viewMonth < 1 do
        model.viewMonth = model.viewMonth + 12
        model.viewYear = model.viewYear - 1
    end
    while model.viewMonth > 12 do
        model.viewMonth = model.viewMonth - 12
        model.viewYear = model.viewYear + 1
    end
    refreshEventSubscription(model)
end

local function setup()
    selectedDateSubscription = data.subscribe("calendar.selectedDate", {
        maxAgeMs = 1000,
        whenHidden = "throttle",
    })
    local initial = selectedDate() or todayDate()
    local info = calendar.dateInfo(initial) or calendar.dateInfo(todayDate())
    local model = {
        viewYear = info.year,
        viewMonth = info.month,
        selectedDate = initial,
        eventRange = "",
    }
    refreshEventSubscription(model)
    widget.setTitle(l10n.tr("lua_widget.month_calendar.name"))
    return model
end

local function effectiveWeekStart()
    local mode = storage.get("weekStart") or "locale"
    if mode == "monday" or mode == "1" then return 2 end
    if mode == "sunday" or mode == "2" then return 1 end
    local language = l10n.language()
    if language == "en-US" or language == "zh-TW" or
        language == "ja-JP" or language == "ko-KR" then
        return 1
    end
    return 2
end

local function monthName(month)
    if month == 1 then return l10n.tr("lua_widget.month_calendar.month_1") end
    if month == 2 then return l10n.tr("lua_widget.month_calendar.month_2") end
    if month == 3 then return l10n.tr("lua_widget.month_calendar.month_3") end
    if month == 4 then return l10n.tr("lua_widget.month_calendar.month_4") end
    if month == 5 then return l10n.tr("lua_widget.month_calendar.month_5") end
    if month == 6 then return l10n.tr("lua_widget.month_calendar.month_6") end
    if month == 7 then return l10n.tr("lua_widget.month_calendar.month_7") end
    if month == 8 then return l10n.tr("lua_widget.month_calendar.month_8") end
    if month == 9 then return l10n.tr("lua_widget.month_calendar.month_9") end
    if month == 10 then return l10n.tr("lua_widget.month_calendar.month_10") end
    if month == 11 then return l10n.tr("lua_widget.month_calendar.month_11") end
    return l10n.tr("lua_widget.month_calendar.month_12")
end

local function fontScale()
    local value = tonumber(storage.get("fontScale"))
    if value then return math.max(70, math.min(135, value)) / 100 end
    local legacy = math.max(11, math.min(20,
        tonumber(storage.get("fontSize")) or 15))
    return legacy / 15
end

local function showAdjacent()
    return storage.get("showAdjacent") ~= "0"
end

local function scaled(value)
    return layout.rpx(value)
end

local function scaledFont(value)
    return scaled(value) * fontScale()
end

local function monthCells(model)
    local first = monthDate(model.viewYear, model.viewMonth)
    local firstInfo = calendar.dateInfo(first)
    local leading = (firstInfo.weekday - effectiveWeekStart() + 7) % 7
    local gridStart = calendar.addDays(first, -leading)
    local cells = {}
    for index = 0, 41 do
        local date = calendar.addDays(gridStart, index)
        local info = calendar.dateInfo(date)
        cells[#cells + 1] = {
            date = date,
            info = info,
            currentMonth = info.year == model.viewYear and
                info.month == model.viewMonth,
        }
    end
    return cells
end

local function eventCounts()
    local counts = {}
    if not eventSubscription then return counts end
    local snapshot = eventSubscription:value()
    if not snapshot.available or not snapshot.value then return counts end
    for _, item in ipairs(snapshot.value.events or {}) do
        counts[item.date] = (counts[item.date] or 0) + 1
    end
    return counts
end

local function centeredText(text, x, y, width, height,
    size, color, bold, alpha)
    local measured = draw.measureText(text, size, width, bold)
    draw.text(x + math.max(0, (width - measured.width) / 2),
        y + math.max(0, (height - measured.height) / 2),
        text, size, color, math.max(1, width), bold, true, 0,
        alpha or 1.0)
end

local function submitButton(id, label, shape)
    interaction.region({
        key = id,
        shape = {
            type = "roundedRect",
            x = shape.x,
            y = shape.y,
            width = shape.width,
            height = shape.height,
            radius = scaled(7),
        },
        cursor = "hand",
        events = {
            click = { id = id },
            contextMenu = { id = "calendar.menu", scope = "component" },
        },
        accessibility = { role = "button", label = label },
    })
end

local function drawHeaderButton(id, label, fontSize, colors, shape,
    iconOnly, iconYOffset)
    local hovered = interaction.isHovered(id)
    local pressed = interaction.isPressed(id)
    draw.strokeRect(shape.x, shape.y, shape.width, shape.height,
        colors.text, scaled(7), scaled(pressed and 2 or 1),
        pressed and 0.72 or (hovered and 0.48 or 0.28))
    if iconOnly then
        draw.fa(label,
            shape.x + (shape.width - fontSize) / 2,
            shape.y + (shape.height - fontSize) / 2 + (iconYOffset or 0),
            fontSize, colors.text)
    else
        centeredText(label, shape.x, shape.y, shape.width, shape.height,
            fontSize, colors.text, true, 1.0)
    end
    submitButton(id, label, shape)
end

local function weekdayLabel(weekday)
    if weekday == 1 then
        return l10n.tr("lua_widget.month_calendar.sun")
    elseif weekday == 2 then
        return l10n.tr("lua_widget.month_calendar.mon")
    elseif weekday == 3 then
        return l10n.tr("lua_widget.month_calendar.tue")
    elseif weekday == 4 then
        return l10n.tr("lua_widget.month_calendar.wed")
    elseif weekday == 5 then
        return l10n.tr("lua_widget.month_calendar.thu")
    elseif weekday == 6 then
        return l10n.tr("lua_widget.month_calendar.fri")
    end
    return l10n.tr("lua_widget.month_calendar.sat")
end

local function render(context, model)
    loadStyle()
    local colors = palette()
    local width = layout.contentWidth()
    local contentHeight = layout.contentHeight()
    local padding = scaled(11)
    local headerHeight = scaled(30)
    local calendarGap = scaled(7)
    local weekdayHeight = scaled(22)
    local weekdayTop = padding + headerHeight + calendarGap
    local gridTop = weekdayTop + weekdayHeight
    local gridHeight = math.max(scaled(90),
        contentHeight - scaled(5) - gridTop)
    local cellWidth = (width - padding * 2) / 7
    local cellHeight = gridHeight / 6
    local textScale = fontScale()
    local fontSize = scaled(15) * textScale
    local smallFont = scaled(12) * textScale

    interaction.region({
        key = "calendar.surface",
        shape = {
            type = "rect", x = 0, y = 0,
            width = width, height = contentHeight,
        },
        events = { contextMenu = {
            id = "calendar.menu", scope = "component" } },
        accessibility = {
            role = "group",
            label = l10n.tr("lua_widget.month_calendar.name"),
        },
    })

    local buttonSize = scaled(26)
    local buttonY = padding + (headerHeight - buttonSize) / 2
    local iconFont = scaledFont(14)
    local previousShape = {
        x = padding,
        y = buttonY,
        width = buttonSize,
        height = buttonSize,
    }
    local nextShape = {
        x = padding + buttonSize + scaled(4),
        y = buttonY,
        width = buttonSize,
        height = buttonSize,
    }
    local todayText = l10n.tr("lua_widget.month_calendar.today")
    local todayMetrics = draw.measureText(todayText, smallFont, 0, true)
    local todayWidth = math.max(scaled(40),
        todayMetrics.width + scaled(12))
    local todayShape = {
        x = width - padding - todayWidth,
        y = buttonY,
        width = todayWidth,
        height = buttonSize,
    }
    drawHeaderButton("calendar.previous", "", iconFont, colors,
        previousShape, true, scaled(1.4))
    drawHeaderButton("calendar.next", "", iconFont, colors,
        nextShape, true, scaled(1.4))
    drawHeaderButton("calendar.today", todayText, smallFont, colors,
        todayShape, false)

    local title = l10n.tr("lua_widget.month_calendar.month_format",
        tostring(model.viewYear), monthName(model.viewMonth))
    local titleX = nextShape.x + nextShape.width + scaled(8)
    local titleWidth = math.max(scaled(1),
        todayShape.x - titleX - scaled(5))
    centeredText(title, titleX, padding, titleWidth, headerHeight,
        fontSize, colors.text, true, 1.0)

    local weekStart = effectiveWeekStart()
    for column = 0, 6 do
        local weekday = ((weekStart - 1 + column) % 7) + 1
        centeredText(weekdayLabel(weekday),
            padding + column * cellWidth, weekdayTop,
            cellWidth, weekdayHeight, smallFont, colors.text, true, 0.60)
    end

    local selected = context.preview and model.selectedDate or
        (selectedDate() or model.selectedDate)
    local today = todayDate()
    local counts = eventCounts()
    for index, cell in ipairs(monthCells(model)) do
        local zero = index - 1
        local column = zero % 7
        local row = math.floor(zero / 7)
        local x = padding + column * cellWidth
        local y = gridTop + row * cellHeight
        local visible = cell.currentMonth or showAdjacent()
        if visible then
            local key = "calendar.date." .. cell.date
            local isSelected = cell.date == selected
            local isToday = cell.date == today
            local circleRatio = math.min(0.90,
                0.72 * math.max(1, textScale))
            local diameter = math.min(cellWidth * circleRatio,
                cellHeight * circleRatio)
            local centerX = x + cellWidth / 2
            local centerY = y + cellHeight * 0.44
            if isSelected then
                draw.circle(centerX, centerY, diameter / 2,
                    colors.text, 0.92)
            elseif isToday then
                draw.strokeRect(centerX - diameter / 2,
                    centerY - diameter / 2, diameter, diameter,
                    colors.text, diameter / 2, scaled(1.3), 0.82)
            elseif interaction.isHovered(key) then
                draw.circle(centerX, centerY, diameter / 2,
                    colors.text, 0.12)
            end
            centeredText(tostring(cell.info.day),
                centerX - cellWidth / 2, centerY - cellHeight / 2,
                cellWidth, cellHeight, fontSize,
                isSelected and colors.inverse or colors.text,
                isToday or isSelected,
                cell.currentMonth and 1.0 or 0.38)
            if counts[cell.date] then
                draw.circle(centerX,
                    y + cellHeight - scaled(4),
                    math.max(scaled(1.4),
                        cellWidth * 0.035),
                    isSelected and colors.inverse or colors.text,
                    cell.currentMonth and 0.86 or 0.34)
            end
            interaction.region({
                key = key,
                shape = {
                    type = "rect", x = x, y = y,
                    width = cellWidth, height = cellHeight,
                },
                cursor = "hand",
                events = {
                    click = {
                        id = "calendar.select",
                        value = { date = cell.date },
                    },
                    contextMenu = {
                        id = "calendar.menu", scope = "component" },
                },
                accessibility = {
                    role = "button",
                    label = cell.date,
                },
            })
        end
    end
end

local function event(_context, model, value)
    if value.kind == "data.change" and
        value.topic == "calendar.selectedDate" then
        local current = selectedDate()
        if current and current ~= model.selectedDate then
            model.selectedDate = current
            setViewFromDate(model, current)
        end
        return
    end
    if value.kind == "environment" then
        widget.setTitle(l10n.tr("lua_widget.month_calendar.name"))
        return
    end
    if value.kind ~= "action" then return end
    if value.id == "calendar.previous" then
        shiftMonth(model, -1)
    elseif value.id == "calendar.next" then
        shiftMonth(model, 1)
    elseif value.id == "calendar.today" then
        returnToToday(model)
    elseif value.id == "calendar.select" and value.value then
        selectDate(model, value.value.date)
    end
end

local function menu(_context, _model, request)
    if request.id ~= "calendar.menu" then return nil end
    return ui.menu({
        {
            id = "calendar.today",
            label = l10n.tr("lua_widget.month_calendar.today"),
            icon = fluent.today,
            iconFont = "fluent",
        },
        {
            id = "calendar.previous",
            label = l10n.tr("lua_widget.month_calendar.previous_month"),
            icon = fluent.previous,
            iconFont = "fluent",
        },
        {
            id = "calendar.next",
            label = l10n.tr("lua_widget.month_calendar.next_month"),
            icon = fluent.next,
            iconFont = "fluent",
        },
    })
end

local function dispose()
    if selectedDateSubscription then
        selectedDateSubscription:unsubscribe()
        selectedDateSubscription = nil
    end
    if eventSubscription then
        eventSubscription:unsubscribe()
        eventSubscription = nil
    end
end

local function migrateStorage(oldVersion, newVersion)
    if oldVersion >= 2 or newVersion < 2 then return end
    local old = storage.get("weekStart")
    if old == "0" then storage.set("weekStart", "locale")
    elseif old == "1" then storage.set("weekStart", "monday")
    elseif old == "2" then storage.set("weekStart", "sunday")
    end
end

descriptor = {
    name = l10n.tr("lua_widget.month_calendar.name"),
    useCustomStyle = true,
    followPersonalizationDefault = true,
    showTitle = false,
    bottomBarHover = false,
    bg = 0x151A21,
    border = 0xFFFFFF,
    alpha = 0.40,
    borderAlpha = 0.18,
    gradientEndA = 0.28,
    settings = settings,
    setup = setup,
    render = render,
    event = event,
    menu = menu,
    dispose = dispose,
    migrateStorage = migrateStorage,
}

return widget.define(descriptor)
