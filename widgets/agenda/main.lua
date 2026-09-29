-- agenda/main.lua - API v2 subscribed calendar with an editable panel
local descriptor

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
    add = utf8.char(0xF211),
    edit = utf8.char(0xE246),
    delete = utf8.char(0xF34C),
    today = utf8.char(0xF23C),
    previous = utf8.char(0xF15B),
    next = utf8.char(0xF181),
}

local DRAFT_TITLE = "agenda_draft_title"
local DRAFT_DATE = "agenda_draft_date"
local DRAFT_ALL_DAY = "agenda_draft_all_day"
local DRAFT_START = "agenda_draft_start"
local DRAFT_END = "agenda_draft_end"
local DRAFT_REMINDER = "agenda_draft_reminder"
local DRAFT_NOTES = "agenda_draft_notes"
local EDITOR_MODE = "agenda_editor_mode"
local EDITOR_ID = "agenda_editor_id"
local EDITOR_REVISION = "agenda_editor_revision"
local SELECTED_ID = "agenda_selected_id"
local reminderValues = { -1, 0, 5, 15, 30, 60, 1440 }

local function supportsSeries()
    return widget.hasFeature("calendar.series") and
        widget.hasFeature("task.calendar.series") and
        widget.hasFeature("ui.datePicker.multiple") and
        type(calendar.seriesById)=="function"
end

local function copyRule(rule)
    local result={kind=rule.kind,dates={},startDate=rule.startDate,endDate=rule.endDate or "",
        interval=rule.interval or 1,weekdays={},monthDay=rule.monthDay or 0}
    for _,date in ipairs(rule.dates or {}) do result.dates[#result.dates+1]=date end
    for _,weekday in ipairs(rule.weekdays or {}) do result.weekdays[#result.weekdays+1]=weekday end
    return result
end

local function defaultRule(date)
    local info=calendar.dateInfo(date)
    return {kind="dates",dates={date},startDate=date,endDate="",interval=1,
        weekdays={info and info.weekday or 1},monthDay=info and info.day or 1}
end

local function ruleMatches(rule,date)
    if not calendar.dateInfo(date) then return false end
    if rule.kind=="dates" then
        for _,item in ipairs(rule.dates) do if item==date then return true end end
        return false
    end
    if date<rule.startDate or (rule.endDate~="" and date>rule.endDate) then return false end
    local current,anchor=calendar.dateInfo(date),calendar.dateInfo(rule.startDate)
    if not current or not anchor then return false end
    if rule.kind=="monthly" then
        local months=(current.year-anchor.year)*12+current.month-anchor.month
        if months<0 or months%rule.interval~=0 then return false end
        return current.day==(rule.monthDay==0 and current.daysInMonth or rule.monthDay)
    end
    if rule.kind=="weekly" then
        local function serial(info)
            local y=info.year-1
            local days=y*365+math.floor(y/4)-math.floor(y/100)+math.floor(y/400)+info.day
            for month=1,info.month-1 do
                local first=calendar.dateInfo(string.format("%04d-%02d-01",info.year,month))
                days=days+first.daysInMonth
            end
            return days
        end
        local weeks=math.floor((serial(current)-current.weekday+1-
            (serial(anchor)-anchor.weekday+1))/7)
        if weeks<0 or weeks%rule.interval~=0 then return false end
        for _,weekday in ipairs(rule.weekdays) do if weekday==current.weekday then return true end end
    end
    return false
end

local settings = {
    fields = {
        {
            key = "rangeDays",
            label = l10n.tr("lua_widget.agenda.range"),
            type = "select",
            default = "7",
            options = { "1", "3", "7", "30" },
            optionLabels = {
                l10n.tr("lua_widget.agenda.range_1"),
                l10n.tr("lua_widget.agenda.range_3"),
                l10n.tr("lua_widget.agenda.range_7"),
                l10n.tr("lua_widget.agenda.range_30"),
            },
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

local function trim(value)
    return (value or ""):gsub("^%s+", ""):gsub("%s+$", "")
end

local function todayDate()
    local now = time.parts()
    return string.format("%04d-%02d-%02d", now.year, now.month, now.day)
end

local function formatTime(minutes)
    local value = math.max(0, math.min(1439, tonumber(minutes) or 0))
    return string.format("%02d:%02d", math.floor(value / 60), value % 60)
end

local function parseTime(value)
    local hour, minute = string.match(trim(value), "^(%d%d?):(%d%d)$")
    hour = tonumber(hour)
    minute = tonumber(minute)
    if not hour or not minute or hour < 0 or hour > 23 or
        minute < 0 or minute > 59 then
        return nil
    end
    return hour * 60 + minute
end

local function rangeDays()
    local value = tonumber(storage.get("rangeDays")) or 7
    if value == 1 or value == 3 or value == 7 or value == 30 then
        return value
    end
    return 7
end

local function fontScale()
    local value = tonumber(storage.get("fontScale"))
    if value then return math.max(70, math.min(135, value)) / 100 end
    local legacy = math.max(11, math.min(20,
        tonumber(storage.get("fontSize")) or 15))
    return legacy / 15
end

local function palette(context)
    local light = context.theme and context.theme.mode == "light"
    return light and {
        text = 0x000000, muted = 0x4F4F4F, secondary = 0x303030,
        card = 0x000000,
        accent = 0x000000, inverse = 0xFFFFFF, danger = 0x8A1C1C,
    } or {
        text = 0xFFFFFF, muted = 0xB7B7B7, secondary = 0xE0E0E0,
        card = 0xFFFFFF,
        accent = 0xFFFFFF, inverse = 0x000000, danger = 0xFFB5B5,
    }
end

local function formatDate(date, includeWeekday)
    local info = calendar.dateInfo(date)
    if not info then return date end
    local base = l10n.tr("lua_widget.agenda.date_format",
        tostring(info.month), tostring(info.day))
    if not includeWeekday then return base end
    local keys = {
        "lua_widget.agenda.weekday_sun",
        "lua_widget.agenda.weekday_mon",
        "lua_widget.agenda.weekday_tue",
        "lua_widget.agenda.weekday_wed",
        "lua_widget.agenda.weekday_thu",
        "lua_widget.agenda.weekday_fri",
        "lua_widget.agenda.weekday_sat",
    }
    return base .. " · " .. l10n.tr(keys[info.weekday])
end

local function reminderLabel(value)
    local keys = {
        [-1] = "lua_widget.agenda.reminder_none",
        [0] = "lua_widget.agenda.reminder_now",
        [5] = "lua_widget.agenda.reminder_5",
        [15] = "lua_widget.agenda.reminder_15",
        [30] = "lua_widget.agenda.reminder_30",
        [60] = "lua_widget.agenda.reminder_60",
        [1440] = "lua_widget.agenda.reminder_1440",
    }
    return l10n.tr(keys[tonumber(value) or 15] or keys[15])
end

local function selectedDate(model)
    if model.selectedSubscription then
        local snapshot = model.selectedSubscription:value()
        if snapshot.available and snapshot.value and snapshot.value.date then
            model.selectedDate = snapshot.value.date
        end
    end
    return model.selectedDate or todayDate()
end

local function rebuildEventSubscription(model)
    local first = selectedDate(model)
    if model.eventSubscription then
        model.eventSubscription:unsubscribe()
    end
    model.eventSubscription = data.subscribe("calendar.events", {
        fromDate = first,
        toDate = calendar.addDays(first, 29),
        whenHidden = "pause",
        maxAgeMs = 86400000,
    })
    model.subscriptionStart = first
end

local function events(model)
    local result = {}
    model.eventsById = {}
    if not model.eventSubscription then return result end
    local snapshot = model.eventSubscription:value()
    if not snapshot.available or not snapshot.value then return result end
    local first = selectedDate(model)
    local last = calendar.addDays(first, rangeDays() - 1)
    for _, item in ipairs(snapshot.value.events or {}) do
        if item.date >= first and item.date <= last then
            result[#result + 1] = item
            model.eventsById[item.id] = item
        end
    end
    return result
end

local function clearDraftTransaction(tx)
    tx:remove(DRAFT_TITLE)
    tx:remove(DRAFT_DATE)
    tx:remove(DRAFT_ALL_DAY)
    tx:remove(DRAFT_START)
    tx:remove(DRAFT_END)
    tx:remove(DRAFT_REMINDER)
    tx:remove(DRAFT_NOTES)
    tx:remove(EDITOR_MODE)
    tx:remove(EDITOR_ID)
    tx:remove(EDITOR_REVISION)
end

local function clearDraft(model)
    if model.pendingPanelTask then task.cancel(model.pendingPanelTask) end
    storage.transaction(clearDraftTransaction)
    model.editorError = nil
    model.pendingPanelTask = nil
    model.datePicker = nil
    model.timePicker = nil
    model.reminderPicker = nil
    model.rule = nil
    model.seriesDefinition = nil
    model.occurrenceItem = nil
    model.scope = nil
    model.mode = nil
    model.confirmSeriesChange = false
end

local function openEditor(model)
    local title = storage.get(EDITOR_MODE) == "edit" and
        l10n.tr("lua_widget.agenda.edit") or
        l10n.tr("lua_widget.agenda.add")
    model.editorError = nil
    widget.openPanel({
        title = title,
        width = math.min(720, math.max(460, layout.cu(460))),
        height = math.min(820, math.max(520, layout.cu(540))),
    })
    model.panelOpen = true
end

local function startNew(model)
    if not widget.hasPermission("calendar.write") then return end
    if model.panelOpen then
        widget.closePanel()
        return
    end
    local date = selectedDate(model)
    local startMinutes = 540
    if date == todayDate() then
        local now = time.parts()
        startMinutes = math.min(1410,
            math.floor((now.hour * 60 + now.min + 29) / 30) * 30)
    end
    storage.transaction(function(tx)
        clearDraftTransaction(tx)
        tx:set(DRAFT_TITLE, "")
        tx:set(DRAFT_DATE, date)
        tx:set(DRAFT_ALL_DAY, "0")
        tx:set(DRAFT_START, formatTime(startMinutes))
        tx:set(DRAFT_END, formatTime(math.min(1439, startMinutes + 60)))
        tx:set(DRAFT_REMINDER, "15")
        tx:set(EDITOR_MODE, "new")
    end)
    model.rule=supportsSeries() and defaultRule(date) or nil
    model.scope="single"
    model.mode="single"
    openEditor(model)
end

local function fillDraftFromItem(item)
    storage.transaction(function(tx)
        tx:set(DRAFT_TITLE,item.title or "")
        tx:set(DRAFT_DATE,item.date)
        tx:set(DRAFT_ALL_DAY,item.allDay and "1" or "0")
        tx:set(DRAFT_START,formatTime(item.startMinutes))
        tx:set(DRAFT_END,formatTime(item.endMinutes))
        tx:set(DRAFT_REMINDER,tostring(item.reminderMinutes or -1))
        if item.notes and item.notes~="" then tx:set(DRAFT_NOTES,item.notes)
        else tx:remove(DRAFT_NOTES) end
    end)
end

local function startEdit(model, item, wholeSeries)
    if not item or not widget.hasPermission("calendar.write") then return end
    if model.panelOpen then
        widget.closePanel()
        return
    end
    storage.transaction(function(tx)
        clearDraftTransaction(tx)
        tx:set(EDITOR_MODE, "edit")
        tx:set(EDITOR_ID, item.id)
        tx:set(EDITOR_REVISION, tostring(item.revision))
    end)
    fillDraftFromItem(item)
    model.occurrenceItem=item
    model.seriesDefinition=item.seriesId and supportsSeries() and calendar.seriesById(item.seriesId) or nil
    model.scope=model.seriesDefinition and (wholeSeries and "series" or "occurrence") or "single"
    model.mode=model.scope=="series" and model.seriesDefinition.rule.kind or "single"
    if model.seriesDefinition then
        model.rule=copyRule(model.seriesDefinition.rule)
        if model.scope=="series" then fillDraftFromItem(model.seriesDefinition.event) end
    else model.rule=nil end
    openEditor(model)
end

local function saveDraft(model)
    if model.pendingPanelTask or not widget.hasPermission("calendar.write") then return end
    local title = trim(storage.get(DRAFT_TITLE) or "")
    if title == "" then model.editorError = "invalidTitle" return end
    local date = trim(storage.get(DRAFT_DATE) or "")
    local mode = model.mode or "single"
    local seriesMode=mode~="single"
    local rule
    if seriesMode then
        rule=copyRule(model.rule)
        rule.kind=mode
        rule.interval=tonumber(rule.interval)
        rule.monthDay=tonumber(rule.monthDay)
        rule.startDate=date
        rule.endDate=trim(rule.endDate or "")
        if mode=="dates" then
            table.sort(rule.dates)
            date=rule.dates[1] or ""
            rule.startDate=date
            rule.endDate=rule.dates[#rule.dates] or ""
            rule.interval=1;rule.weekdays={};rule.monthDay=0
        elseif mode=="weekly" then rule.dates={};rule.monthDay=0
        elseif mode=="monthly" then rule.dates={};rule.weekdays={} end
        if not calendar.dateInfo(rule.startDate) or
            (rule.endDate~="" and (not calendar.dateInfo(rule.endDate) or rule.endDate<rule.startDate)) or
            not rule.interval or rule.interval<1 or rule.interval>99 or rule.interval%1~=0 or
            (mode=="dates" and (#rule.dates<1 or #rule.dates>366)) or
            (mode=="weekly" and #rule.weekdays==0) or
            (mode=="monthly" and (not rule.monthDay or rule.monthDay<0 or rule.monthDay>31 or rule.monthDay%1~=0)) then
            model.editorError="invalidDate";return
        end
    end
    if not calendar.dateInfo(date) then
        model.editorError = "invalidDate"
        return
    end
    local allDay = storage.get(DRAFT_ALL_DAY) == "1"
    local startMinutes = parseTime(storage.get(DRAFT_START) or "")
    local endMinutes = parseTime(storage.get(DRAFT_END) or "")
    if not allDay and (not startMinutes or not endMinutes or
        endMinutes < startMinutes) then
        model.editorError = "invalidTime"
        return
    end
    local arguments = {
        title = title,
        date = date,
        allDay = allDay,
        startMinutes = allDay and 0 or startMinutes,
        endMinutes = allDay and 0 or endMinutes,
        notes = storage.get(DRAFT_NOTES) or "",
        reminderMinutes = tonumber(storage.get(DRAFT_REMINDER)) or 15,
    }
    local taskName = seriesMode and "calendar.series.create" or "calendar.create"
    if storage.get(EDITOR_MODE) == "edit" then
        if model.scope=="series" and model.seriesDefinition then
            taskName="calendar.series.update"
            arguments.id=model.seriesDefinition.id
            arguments.expectedRevision=model.seriesDefinition.revision
            if #model.seriesDefinition.exceptions>0 and not model.confirmSeriesChange then
                for _,exception in ipairs(model.seriesDefinition.exceptions) do
                    if not ruleMatches(rule,exception.occurrenceDate) then
                        model.confirmSeriesChange=true
                        widget.invalidate()
                        return
                    end
                end
            end
        else
            taskName="calendar.update"
            arguments.id=storage.get(EDITOR_ID) or ""
            arguments.expectedRevision=tonumber(storage.get(EDITOR_REVISION)) or 0
        end
    end
    if seriesMode then
        arguments.kind=rule.kind
        arguments.dates=rule.dates
        arguments.startDate=rule.startDate
        arguments.endDate=rule.endDate
        arguments.interval=rule.interval
        arguments.weekdays=rule.weekdays
        arguments.monthDay=rule.monthDay
    end
    local taskId, taskError = task.start(taskName, arguments)
    if taskId then
        model.pendingPanelTask = taskId
        model.editorError = nil
    else
        model.editorError = taskError == "permissionDenied" and
            "permissionDenied" or "saveFailed"
    end
end

local function deleteEvent(model, item, wholeSeries)
    if not item or model.pendingDeleteTask then return end
    local name="calendar.remove"
    local arguments={id=item.id}
    if wholeSeries and item.seriesId and supportsSeries() then
        local series=calendar.seriesById(item.seriesId)
        if not series then return end
        name="calendar.series.remove"
        arguments={id=series.id,expectedRevision=series.revision}
    end
    local taskId, taskError = task.start(name, arguments)
    if taskId then
        model.pendingDeleteTask = taskId
    else
        widget.log("warn", "calendar.remove rejected: " .. tostring(taskError))
    end
end

local function editorErrorText(error)
    if error == "invalidTitle" then
        return l10n.tr("lua_widget.agenda.invalid_title")
    elseif error == "invalidDate" then
        return l10n.tr("lua_widget.agenda.invalid_date")
    elseif error == "invalidTime" then
        return l10n.tr("lua_widget.agenda.invalid_time")
    elseif error == "conflict" then
        return l10n.tr("lua_widget.agenda.conflict")
    elseif error then
        return l10n.tr("lua_widget.agenda.save_failed")
    end
    return nil
end

local function registerRegion(key, shape, eventsValue, label, enabled)
    interaction.region({
        key = key,
        shape = shape,
        cursor = enabled == false and "default" or "hand",
        enabled = enabled ~= false,
        events = eventsValue,
        accessibility = { role = "button", label = label },
    })
end

local function centeredText(text, x, y, width, height, size, color, bold, alpha)
    local measured = draw.measureText(text, size, 0, bold)
    draw.text(x + math.max(0, (width - measured.width) / 2),
        y + math.max(0, (height - measured.height) / 2), text, size,
        color, math.max(1, width), bold, true, 0, alpha or 1.0)
end

local function drawHeaderButton(key, glyph, label, shape, colors, enabled,
    metrics)
    local hovered = enabled and interaction.isHovered(key)
    local pressed = enabled and interaction.isPressed(key)
    if hovered then
        draw.rect(shape.x, shape.y, shape.width, shape.height,
            colors.accent, metrics.controlRadius,
            pressed and 0.16 or 0.10)
    end
    draw.fluent(glyph,
        shape.x + (shape.width - metrics.iconSize) / 2,
        shape.y + (shape.height - metrics.iconSize) / 2 +
            metrics.spacingXs * 0.5,
        metrics.iconSize, colors.accent, enabled and 1.0 or 0.28)
    registerRegion(key, { type = "roundedRect", x = shape.x, y = shape.y,
        width = shape.width, height = shape.height,
        radius = metrics.controlRadius },
        { click = { id = key }, contextMenu = {
            id = "agenda.menu", scope = "component" } },
        label, enabled)
end

local function drawHeaderTextButton(key, label, shape, colors, enabled,
    metrics, fontSize)
    local hovered = enabled and interaction.isHovered(key)
    local pressed = enabled and interaction.isPressed(key)
    draw.rect(shape.x, shape.y, shape.width, shape.height,
        colors.accent, metrics.controlRadius,
        pressed and 0.16 or (hovered and 0.10 or 0.055))
    centeredText(label, shape.x - metrics.strokeWidth * 0.5,
        shape.y, shape.width, shape.height,
        fontSize, colors.accent, true,
        enabled and 0.92 or 0.28)
    registerRegion(key, { type = "roundedRect", x = shape.x, y = shape.y,
        width = shape.width, height = shape.height,
        radius = metrics.controlRadius },
        { click = { id = key }, contextMenu = {
            id = "agenda.menu", scope = "component" } },
        label, enabled)
end

local function render(context, model)
    local width = layout.contentWidth()
    local height = layout.contentHeight()
    local metrics = componentMetrics()
    local unit = metrics.strokeWidth
    local pad = metrics.spacingMd
    local colors = palette(context)
    local scale = fontScale()
    local titleFont = metrics.titleFontSize * scale
    local headerSmallFont = metrics.captionFontSize * scale
    local mainFont = metrics.bodyFontSize * scale
    local smallFont = metrics.captionFontSize * scale
    local selected = selectedDate(model)
    local canWrite = widget.hasPermission("calendar.write") and not context.preview

    local headerTop = math.min(metrics.spacingSm,
        math.max(0, height - unit))
    local headerHeight = math.min(metrics.layoutRowHeight,
        math.max(unit, height - headerTop))
    local button = headerHeight
    local gap = metrics.spacingXs
    local headerY = headerTop
    local listTop = math.min(height,
        headerY + button + metrics.spacingXs)
    local titleText = formatDate(selected, false)
    local titleMetrics = draw.measureText(titleText, titleFont, 0, true)
    local todayLabel = l10n.tr("lua_widget.agenda.today")
    local todayMetrics = draw.measureText(todayLabel,
        headerSmallFont, 0, true)
    local todayWidth = math.max(button,
        todayMetrics.width + button * 0.30)
    local wideHeaderWidth = pad * 2 + button * 3 + todayWidth +
        gap * 5 + math.max(button, titleMetrics.width)
    local narrowHeader = width < wideHeaderWidth
    if narrowHeader then
        drawHeaderButton("agenda.previous", fluent.previous,
            l10n.tr("lua_widget.agenda.previous_day"),
            { x = pad, y = headerY, width = button, height = button },
            colors, true, metrics)
        drawHeaderButton("agenda.next", fluent.next,
            l10n.tr("lua_widget.agenda.next_day"),
            { x = width - pad - button, y = headerY,
                width = button, height = button }, colors, true, metrics)
        centeredText(titleText,
            pad + button + gap, headerY,
            math.max(unit, width - pad * 2 - button * 2 - gap * 2),
            button, titleFont, colors.text, true)
    else
        drawHeaderButton("agenda.previous", fluent.previous,
            l10n.tr("lua_widget.agenda.previous_day"),
            { x = pad, y = headerY, width = button, height = button },
            colors, true, metrics)
        drawHeaderButton("agenda.next", fluent.next,
            l10n.tr("lua_widget.agenda.next_day"),
            { x = pad + button + gap, y = headerY,
                width = button, height = button }, colors, true, metrics)
        local addX = width - pad - button
        drawHeaderButton("agenda.add", fluent.add,
            l10n.tr("lua_widget.agenda.add"),
            { x = addX, y = headerY,
                width = button, height = button }, colors, canWrite, metrics)
        local todayX = addX - gap - todayWidth
        drawHeaderTextButton("agenda.today", todayLabel,
            { x = todayX, y = headerY,
                width = todayWidth, height = button }, colors, true,
            metrics, headerSmallFont)
        local titleX = pad + button * 2 + gap * 2
        local titleRight = todayX - gap
        centeredText(titleText, titleX, headerY,
            math.max(unit, titleRight - titleX), button,
            titleFont, colors.text, true)
    end

    local listBottom = height - metrics.spacingXs
    local viewportHeight = math.max(unit, listBottom - listTop)
    local viewport = { type = "rect", x = pad, y = listTop,
        width = width - pad * 2, height = viewportHeight }
    interaction.region({
        key = "agenda.surface",
        shape = viewport,
        events = {
            click = { id = "agenda.clearSelection" },
            contextMenu = { id = "agenda.menu", scope = "component" },
        },
        accessibility = { role = "list", label = descriptor.name },
    })

    local items = events(model)
    if #items == 0 then
        local emptyTitle = l10n.tr("lua_widget.agenda.empty")
        local emptyHint = l10n.tr(narrowHeader and
            "lua_widget.agenda.empty_hint_compact" or
            "lua_widget.agenda.empty_hint")
        local emptyTitleMetrics = draw.measureText(
            emptyTitle, titleFont, 0, true)
        local emptyHintMetrics = draw.measureText(
            emptyHint, smallFont, 0, false)
        local emptyGap = metrics.spacingXs
        local emptyBlockHeight = emptyTitleMetrics.height + emptyGap +
            emptyHintMetrics.height
        local emptyTop = listTop + math.max(0,
            (viewportHeight - emptyBlockHeight) / 2)
        centeredText(emptyTitle, pad, emptyTop,
            width - pad * 2, emptyTitleMetrics.height, titleFont,
            colors.text, true, 0.78)
        centeredText(emptyHint, pad,
            emptyTop + emptyTitleMetrics.height + emptyGap,
            width - pad * 2, emptyHintMetrics.height, smallFont,
            colors.muted, false, canWrite and 0.62 or 0.38)
        return
    end

    local cardHeight = math.max(metrics.layoutRowHeight * 1.35,
        mainFont + smallFont + metrics.spacingSm)
    local rowGap = metrics.spacingXs
    local rowHeight = cardHeight + rowGap
    local scroll = interaction.scroll({
        key = "agenda.scroll",
        shape = viewport,
        contentHeight = math.ceil(#items * rowHeight - rowGap),
    })
    local first = math.max(1, math.floor(scroll.offset / rowHeight) + 1)
    local last = math.min(#items,
        math.ceil((scroll.offset + viewportHeight) / rowHeight))
    local selectedId = storage.get(SELECTED_ID)
    draw.pushClip(pad, listTop, width - pad * 2, viewportHeight)
    for index = first, last do
        local item = items[index]
        local y = listTop + (index - 1) * rowHeight - scroll.offset
        local key = "agenda.event." .. item.id
        local highlighted = context.selected and selectedId == item.id
        local hovered = interaction.isHovered(key)
        draw.rect(pad, y, width - pad * 2, cardHeight,
            colors.card, metrics.controlRadius,
            highlighted and 0.12 or (hovered and 0.085 or 0.05))
        if highlighted then
            draw.strokeRect(pad + unit, y + unit,
                width - pad * 2 - unit * 2, cardHeight - unit * 2,
                colors.accent, math.max(0, metrics.controlRadius - unit),
                metrics.strokeWidth, 0.40)
        end
        registerRegion(key, { type = "roundedRect", x = pad, y = y,
            width = width - pad * 2, height = cardHeight,
            radius = metrics.controlRadius }, {
            click = { id = "agenda.select", value = item.id },
            doubleClick = { id = "agenda.edit", value = item.id },
            contextMenu = { id = "agenda.menu", value = item.id },
        }, item.title or l10n.tr("lua_widget.agenda.untitled"), true)

        local textX = pad + metrics.spacingSm
        local textWidth = width - pad * 2 - metrics.spacingLg
        draw.text(textX, y + metrics.spacingXs,
            item.title ~= "" and item.title or
                l10n.tr("lua_widget.agenda.untitled"),
            mainFont, colors.text, textWidth, true, true)
        local timing = item.allDay and l10n.tr("lua_widget.agenda.all_day") or
            (formatTime(item.startMinutes) .. " – " .. formatTime(item.endMinutes))
        draw.text(textX, y + cardHeight - smallFont - metrics.spacingSm,
            formatDate(item.date, true) .. " · " .. timing,
            smallFont, colors.secondary, textWidth, false, true, 0, 0.92)
    end
    draw.popClip()
end

local function panel(context, model)
    local row=ui.metrics().layoutRowHeight
    local busy=model.pendingPanelTask~=nil
    local function button(id,label,enabled)
        return view.button({key="agenda."..id,label=label,width="fill",height=row,fontSize=row*0.46,textAlign="center",
            style={foreground="textPrimary",cornerRadius=row*0.12},
            enabled=enabled~=false,action={id="agenda.panel",value=id},accessibility={label=label}})
    end
    local children={}
    if model.reminderPicker then
        children={view.text({key="agenda.reminder.heading",text=l10n.tr("lua_widget.agenda.reminder"),
            height=row,fontSize=row*0.46,style={foreground="textPrimary"}})}
        local selected=tonumber(storage.get(DRAFT_REMINDER)) or 15
        for _,minutes in ipairs(reminderValues) do
            local option=button("reminder:"..tostring(minutes),reminderLabel(minutes),not busy)
            if minutes==selected then option.style={foreground=0xFFFFFF,background=0x175CD3,cornerRadius=row*0.12} end
            children[#children+1]=option
        end
        children[#children+1]=button("picker.back",l10n.tr("lua_widget.agenda.cancel"),not busy)
    elseif model.timePicker then
        children={model.timePicker:view({rowHeight=row}),button("picker.back",l10n.tr("lua_widget.agenda.cancel"))}
    elseif model.datePicker then
        children={model.datePicker:view({rowHeight=row}),button("picker.back",l10n.tr("lua_widget.agenda.cancel"))}
    else
        local title=storage.get(DRAFT_TITLE) or ""
        local date=storage.get(DRAFT_DATE) or ""
        local start=storage.get(DRAFT_START) or ""
        local finish=storage.get(DRAFT_END) or ""
        local allDay=storage.get(DRAFT_ALL_DAY)=="1"
        local titleError=trim(title)=="" and l10n.tr("lua_widget.agenda.invalid_title") or nil
        local dateError=model.mode~="dates" and not calendar.dateInfo(date) and l10n.tr("lua_widget.agenda.invalid_date") or nil
        local startMinutes,endMinutes=parseTime(start),parseTime(finish)
        local timeError=not allDay and (not startMinutes or not endMinutes or endMinutes<startMinutes)
            and l10n.tr("lua_widget.agenda.invalid_time") or nil
        local function field(key,value,label,err,multi)
            children[#children+1]=view.text({key=key..".label",text=label,height=row,fontSize=row*0.43,style={foreground="textSecondary"}})
            local options={key=key,value=value,style={foreground="textPrimary"},height=row,fontSize=row*0.46,
                maxBytes=key==DRAFT_TITLE and 512 or (multi and 8192 or 10),enabled=not busy,
                validationState=err and "error" or "none",validationMessage=err or "",
                action={id="agenda.field",value=key},accessibility={label=label}}
            if multi then
                local available=math.max(1,(context.layoutSize and context.layoutSize.width or row*10)-row*1.3-40)
                local measured=draw.measureText(value,options.fontSize,available,false)
                options.height=math.max(row*2,measured.height+row*1.5)
            end
            children[#children+1]=multi and view.textArea(options) or view.textInput(options)
            if err then children[#children+1]=view.text({key=key..".error",text=err,height=row,fontSize=row*0.42,style={foreground="textSecondary"}}) end
        end
        field(DRAFT_TITLE,title,l10n.tr("lua_widget.agenda.title"),titleError)
        if supportsSeries() and (storage.get(EDITOR_MODE)=="new" or model.seriesDefinition) then
            if model.seriesDefinition then
                children[#children+1]=button("scope",l10n.tr("lua_widget.agenda.scope")..": "..
                    l10n.tr(model.scope=="series" and "lua_widget.agenda.scope_series" or
                        "lua_widget.agenda.scope_occurrence"),not busy)
            end
            if model.scope~="occurrence" then
                children[#children+1]=view.text({key="agenda.mode.label",text=l10n.tr("lua_widget.agenda.date_mode"),
                    height=row,fontSize=row*0.43,style={foreground="textSecondary"}})
                local modes=model.seriesDefinition and {"dates","weekly","monthly"} or
                    {"single","dates","weekly","monthly"}
                local choices={}
                local modeKeys={single="lua_widget.agenda.mode_single",dates="lua_widget.agenda.mode_dates",
                    weekly="lua_widget.agenda.mode_weekly",monthly="lua_widget.agenda.mode_monthly"}
                for _,mode in ipairs(modes) do
                    local choice=button("mode:"..mode,l10n.tr(modeKeys[mode]),not busy)
                    if model.mode==mode then choice.style={foreground=0xFFFFFF,background=0x175CD3,cornerRadius=row*0.12} end
                    choices[#choices+1]=choice
                end
                children[#children+1]=view.grid({key="agenda.modes",columns=2,height=row*2+row*0.1,
                    gap=row*0.1,children=choices})
            end
        end
        if model.mode=="dates" and model.rule then
            local dates=model.rule.dates
            local summary=#dates==0 and l10n.tr("lua_widget.agenda.choose_dates") or dates[1]
            if #dates>1 then summary=summary.." · "..dates[2] end
            if #dates>2 then summary=summary.." +"..tostring(#dates-2) end
            children[#children+1]=view.text({key="agenda.date.label",text=l10n.tr("lua_widget.agenda.date"),
                height=row,fontSize=row*0.43,style={foreground="textSecondary"}})
            children[#children+1]=view.button({key="agenda.openDatePicker",label=summary,width="fill",height=row,
                fontSize=row*0.46,textAlign="left",style={foreground="textPrimary",cornerRadius=row*0.12},
                enabled=not busy,action={id="agenda.panel",value="openDatePicker"},
                accessibility={label=l10n.tr("lua_widget.agenda.date")..": "..summary}})
        else
            field(DRAFT_DATE,date,model.mode=="single" and l10n.tr("lua_widget.agenda.date") or
                l10n.tr("lua_widget.agenda.start_date"),dateError)
            children[#children+1]=button("openDatePicker",l10n.tr("lua_widget.agenda.choose_date"),not busy)
        end
        if model.rule and (model.mode=="weekly" or model.mode=="monthly") then
            local function ruleField(id,value,label)
                children[#children+1]=view.text({key="agenda.rule.label."..id,text=label,height=row,
                    fontSize=row*0.43,style={foreground="textSecondary"}})
                children[#children+1]=view.textInput({key="agenda.rule."..id,value=tostring(value),height=row,
                    fontSize=row*0.46,maxBytes=10,enabled=not busy,action={id="agenda.rule",value=id},
                    accessibility={label=label}})
            end
            ruleField("interval",model.rule.interval,l10n.tr("lua_widget.agenda.interval"))
            children[#children+1]=button("endType",l10n.tr(model.rule.endDate=="" and
                "lua_widget.agenda.never_ends" or "lua_widget.agenda.ends_on"),not busy)
            if model.rule.endDate~="" then ruleField("endDate",model.rule.endDate,l10n.tr("lua_widget.agenda.end_date")) end
            if model.mode=="weekly" then
                local days={}
                for index=1,7 do
                    local checked=false
                    for _,weekday in ipairs(model.rule.weekdays) do if weekday==index then checked=true end end
                    days[#days+1]=view.checkbox({key="agenda.weekday."..index,
                        label=l10n.tr(({"lua_widget.agenda.weekday_sun","lua_widget.agenda.weekday_mon",
                            "lua_widget.agenda.weekday_tue","lua_widget.agenda.weekday_wed",
                            "lua_widget.agenda.weekday_thu","lua_widget.agenda.weekday_fri",
                            "lua_widget.agenda.weekday_sat"})[index]),checked=checked,height=row,
                        fontSize=row*0.42,enabled=not busy,action={id="agenda.weekday",value=index}})
                end
                children[#children+1]=view.grid({key="agenda.weekdays",columns=4,height=row*2,gap=row*0.12,children=days})
            else
                children[#children+1]=button("lastDay",l10n.tr("lua_widget.agenda.last_day")..": "..
                    (model.rule.monthDay==0 and "✓" or "—"),not busy)
                if model.rule.monthDay~=0 then
                    ruleField("monthDay",model.rule.monthDay,l10n.tr("lua_widget.agenda.month_day"))
                end
            end
        end
        children[#children+1]=view.checkbox({key="agenda.allDay",label=l10n.tr("lua_widget.agenda.all_day"),checked=allDay,
            height=row,fontSize=row*0.46,style={foreground="textPrimary"},enabled=not busy,action={id="agenda.panel",value="toggleAllDay"}})
        if not allDay then
            children[#children+1]=button("openTimePicker",l10n.tr("lua_widget.agenda.choose_time"),not busy)
            field(DRAFT_START,start,l10n.tr("lua_widget.agenda.start"),not startMinutes and timeError or nil)
            field(DRAFT_END,finish,l10n.tr("lua_widget.agenda.end"),timeError)
        end
        children[#children+1]=button("openReminderPicker",l10n.tr("lua_widget.agenda.reminder")..": "..
            reminderLabel(storage.get(DRAFT_REMINDER)),not busy)
        field(DRAFT_NOTES,storage.get(DRAFT_NOTES) or "",l10n.tr("lua_widget.agenda.notes"),nil,true)
        local err=editorErrorText(model.editorError)
        if err then children[#children+1]=view.text({key="agenda.save.error",text=err,height=row*2,fontSize=row*0.43,textWrap="wrap"}) end
        if model.confirmSeriesChange then
            children[#children+1]=view.text({key="agenda.series.confirm",text=l10n.tr("lua_widget.agenda.confirm_exceptions"),
                height=row*2,fontSize=row*0.43,textWrap="wrap"})
        end
        children[#children+1]=view.row({key="agenda.actions",height=row,gap=row*0.3,children={
            button("cancel",l10n.tr("lua_widget.agenda.cancel"),not busy),
            button("save",l10n.tr("lua_widget.agenda.save"),not busy and not titleError and not dateError and not timeError and widget.hasPermission("calendar.write"))}})
    end
    local page=model.reminderPicker and "reminder" or model.timePicker and "time"
        or model.datePicker and "date" or "editor"
    return view.scroll({key="agenda.panel.scroll."..page,width="fill",height="fill",children={
        view.column({key="agenda.form",width="fill",height="auto",padding=row*0.65,gap=row*0.25,children=children})}})
end

local function setup()
    widget.setTitle(l10n.tr("lua_widget.agenda.name"))
    local model = {
        selectedDate = todayDate(), eventsById = {},
        pendingPanelTask = nil, pendingDeleteTask = nil,
        editorError = nil, datePicker = nil, panelOpen = false,
    }
    model.selectedSubscription = data.subscribe("calendar.selectedDate", {
        whenHidden = "pause", maxAgeMs = 86400000,
    })
    selectedDate(model)
    rebuildEventSubscription(model)
    return model
end

local function shiftSelectedDate(model, offset)
    local date = calendar.addDays(selectedDate(model), offset)
    if date and calendar.selectDate(date) then
        model.selectedDate = date
        rebuildEventSubscription(model)
        interaction.setScrollOffset("agenda.scroll", 0)
        storage.remove(SELECTED_ID)
    end
end

local function handlePanelAction(model, id)
    if id == "cancel" then
        widget.closePanel()
    elseif id == "save" then
        saveDraft(model)
    elseif id == "toggleAllDay" then
        storage.set(DRAFT_ALL_DAY,
            storage.get(DRAFT_ALL_DAY) == "1" and "0" or "1")
        widget.invalidate()
    elseif id == "scope" and model.seriesDefinition then
        model.scope=model.scope=="series" and "occurrence" or "series"
        model.mode=model.scope=="series" and model.rule.kind or "single"
        fillDraftFromItem(model.scope=="series" and model.seriesDefinition.event or model.occurrenceItem)
        model.confirmSeriesChange=false
        widget.invalidate()
    elseif id:sub(1,5)=="mode:" and model.rule and model.scope~="occurrence" then
        local mode=id:sub(6)
        if mode=="single" or mode=="dates" or mode=="weekly" or mode=="monthly" then
            if model.seriesDefinition and mode=="single" then return end
            model.mode=mode;model.rule.kind=mode
            local date=storage.get(DRAFT_DATE) or todayDate()
            local info=calendar.dateInfo(date)
            if #model.rule.dates==0 then model.rule.dates={date} end
            if #model.rule.weekdays==0 then model.rule.weekdays={info and info.weekday or 1} end
            if model.rule.monthDay==nil then model.rule.monthDay=info and info.day or 1 end
            model.confirmSeriesChange=false
            widget.invalidate()
        end
    elseif id == "endType" and model.rule then
        model.rule.endDate=model.rule.endDate=="" and
            (calendar.addDays(storage.get(DRAFT_DATE) or todayDate(),30) or todayDate()) or ""
        model.confirmSeriesChange=false;widget.invalidate()
    elseif id == "lastDay" and model.rule then
        local info=calendar.dateInfo(storage.get(DRAFT_DATE) or todayDate())
        model.rule.monthDay=model.rule.monthDay==0 and (info and info.day or 1) or 0
        model.confirmSeriesChange=false;widget.invalidate()
    elseif id == "openDatePicker" then
        if model.mode=="dates" and model.rule then
            model.datePicker=ui.datePicker({key="agenda.date",mode="multiple",value=model.rule.dates,
                todayDate=todayDate(),allowClear=false})
        else
            model.datePicker=ui.datePicker({key="agenda.date",value=storage.get(DRAFT_DATE) or "",
                todayDate=todayDate(),allowClear=false})
        end
        widget.invalidate()
    elseif id == "openTimePicker" then
        model.timePicker=ui.timePicker({key="agenda.time",mode="range",allowClear=false,
            value={startTime=storage.get(DRAFT_START) or "",endTime=storage.get(DRAFT_END) or ""}})
        widget.invalidate()
    elseif id == "picker.back" then
        model.reminderPicker=nil
        model.timePicker=nil
        model.datePicker=nil
        widget.invalidate()
    elseif id == "openReminderPicker" and not model.pendingPanelTask then
        model.reminderPicker=true
        widget.invalidate()
    elseif model.reminderPicker and not model.pendingPanelTask and id:sub(1,9)=="reminder:" then
        local selected=tonumber(id:sub(10))
        for _,minutes in ipairs(reminderValues) do
            if selected==minutes then
                storage.set(DRAFT_REMINDER,tostring(minutes));model.reminderPicker=nil
                widget.invalidate();break
            end
        end
    end
end

local function event(_context, model, value)
    if model.timePicker then
        local result=model.timePicker:handle(value)
        if result then
            if result.changed then
                storage.transaction(function(tx) tx:set(DRAFT_START,result.value.startTime);tx:set(DRAFT_END,result.value.endTime) end)
                model.timePicker=nil
            end
            widget.invalidate();return
        end
    end
    if model.datePicker then
        local result=model.datePicker:handle(value)
        if result then
            if result.changed then
                if model.mode=="dates" and model.rule then
                    model.rule.dates=result.value
                    storage.set(DRAFT_DATE,result.value[1])
                else storage.set(DRAFT_DATE,result.value) end
                model.datePicker=nil;model.confirmSeriesChange=false
            end
            widget.invalidate()
            return
        end
    end
    if value.kind=="action" and value.id=="agenda.rule" and value.surface=="panel" then
        if model.rule and type(value.text)=="string" and #value.text<=10 then
            if value.value=="endDate" then model.rule.endDate=value.text
            elseif value.value=="interval" then model.rule.interval=value.text
            elseif value.value=="monthDay" then model.rule.monthDay=value.text end
            model.confirmSeriesChange=false;widget.invalidate()
        end
        return
    end
    if value.kind=="action" and value.id=="agenda.weekday" and value.surface=="panel" then
        local day=tonumber(value.value)
        if model.rule and day and day>=1 and day<=7 then
            local found
            for index,weekday in ipairs(model.rule.weekdays) do if weekday==day then found=index end end
            if found then table.remove(model.rule.weekdays,found)
            else model.rule.weekdays[#model.rule.weekdays+1]=day;table.sort(model.rule.weekdays) end
            model.confirmSeriesChange=false;widget.invalidate()
        end
        return
    end
    if value.kind=="action" and value.id=="agenda.field" and value.surface=="panel" then
        local allowed={ [DRAFT_TITLE]=true,[DRAFT_DATE]=true,[DRAFT_START]=true,[DRAFT_END]=true,[DRAFT_NOTES]=true }
        if not model.pendingPanelTask and allowed[value.value] and type(value.text)=="string" then
            storage.set(value.value,value.text)
            model.editorError=nil
            model.confirmSeriesChange=false
            widget.invalidate()
        end
        return
    end
    if value.kind == "environment" then
        widget.setTitle(l10n.tr("lua_widget.agenda.name"))
        return
    elseif value.kind == "data.change" then
        if value.topic == "calendar.selectedDate" then
            selectedDate(model)
            rebuildEventSubscription(model)
            interaction.setScrollOffset("agenda.scroll", 0)
            storage.remove(SELECTED_ID)
        end
        return
    elseif value.kind == "panel" then
        if value.action == "opened" then
            model.panelOpen = true
        elseif value.action == "closed" then
            model.panelOpen = false
            clearDraft(model)
        end
        return
    elseif value.kind == "action" and value.surface == "panel" and
        value.id == "agenda.panel" then
        handlePanelAction(model, tostring(value.value or ""))
        return
    elseif value.kind == "task.complete" then
        if value.taskId == model.pendingPanelTask then
            model.pendingPanelTask = nil
            if value.ok and value.value then
                local date = storage.get(DRAFT_DATE) or selectedDate(model)
                local id = value.value.id
                storage.transaction(function(tx)
                    clearDraftTransaction(tx)
                    if id and id ~= "" then tx:set(SELECTED_ID, id) end
                end)
                calendar.selectDate(date)
                model.selectedDate = date
                widget.closePanel()
            else
                model.editorError = value.error == "conflict" and
                    "conflict" or "saveFailed"
            end
        elseif value.taskId == model.pendingDeleteTask then
            model.pendingDeleteTask = nil
            if value.ok then storage.remove(SELECTED_ID) end
        end
        return
    elseif value.kind ~= "action" then
        return
    end

    local id = value.value and tostring(value.value) or nil
    if value.id == "agenda.previous" then
        shiftSelectedDate(model, -1)
    elseif value.id == "agenda.next" then
        shiftSelectedDate(model, 1)
    elseif value.id == "agenda.today" then
        local today = todayDate()
        calendar.selectDate(today)
        model.selectedDate = today
        rebuildEventSubscription(model)
        interaction.setScrollOffset("agenda.scroll", 0)
        storage.remove(SELECTED_ID)
    elseif value.id == "agenda.add" then
        startNew(model)
    elseif value.id == "agenda.clearSelection" then
        storage.remove(SELECTED_ID)
    elseif value.id == "agenda.select" and id then
        storage.set(SELECTED_ID, id)
    elseif value.id == "agenda.edit" and id then
        storage.set(SELECTED_ID, id)
        startEdit(model, model.eventsById[id])
    elseif value.id == "agenda.editSeries" and id then
        storage.set(SELECTED_ID,id)
        startEdit(model,model.eventsById[id],true)
    elseif value.id == "agenda.delete" and id then
        deleteEvent(model, model.eventsById[id])
    elseif value.id == "agenda.deleteSeries" and id then
        deleteEvent(model,model.eventsById[id],true)
    end
end

local function menu(_context, model, request)
    if request.id ~= "agenda.menu" then return nil end
    local id = request.value and tostring(request.value) or
        storage.get(SELECTED_ID)
    local item = id and model.eventsById[id] or nil
    local canWrite = widget.hasPermission("calendar.write")
    if item then
        local actions={
            {
                id = "agenda.edit",
                label = l10n.tr(item.seriesId and "lua_widget.agenda.edit_occurrence" or "lua_widget.agenda.edit"),
                icon = fluent.edit, iconFont = "fluent",
                enabled = canWrite,
            },
            {
                id = "agenda.delete",
                label = l10n.tr(item.seriesId and "lua_widget.agenda.delete_occurrence" or "lua_widget.agenda.delete"),
                icon = fluent.delete, iconFont = "fluent",
                enabled = canWrite,
            },
        }
        if item.seriesId and supportsSeries() then
            actions[#actions+1]={type="separator"}
            actions[#actions+1]={id="agenda.editSeries",label=l10n.tr("lua_widget.agenda.edit_series"),
                icon=fluent.edit,iconFont="fluent",enabled=canWrite}
            actions[#actions+1]={id="agenda.deleteSeries",label=l10n.tr("lua_widget.agenda.delete_series"),
                icon=fluent.delete,iconFont="fluent",enabled=canWrite}
        end
        return ui.menu(actions)
    end
    return ui.menu({
        {
            id = "agenda.add", label = l10n.tr("lua_widget.agenda.add"),
            icon = fluent.add, iconFont = "fluent", enabled = canWrite,
        },
        { type = "separator" },
        {
            id = "agenda.today", label = l10n.tr("lua_widget.agenda.today"),
            icon = fluent.today, iconFont = "fluent",
        },
        {
            id = "agenda.previous",
            label = l10n.tr("lua_widget.agenda.previous_day"),
            icon = fluent.previous, iconFont = "fluent",
        },
        {
            id = "agenda.next", label = l10n.tr("lua_widget.agenda.next_day"),
            icon = fluent.next, iconFont = "fluent",
        },
    })
end

local function dispose(_context, model)
    if model.selectedSubscription then
        model.selectedSubscription:unsubscribe()
    end
    if model.eventSubscription then model.eventSubscription:unsubscribe() end
end

descriptor = {
    name = l10n.tr("lua_widget.agenda.name"),
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
    panel = panel,
    event = event,
    menu = menu,
    dispose = dispose,
}

return widget.define(descriptor)
