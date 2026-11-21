local draft = {}

local keys = {
    title = "agenda_draft_title",
    date = "agenda_draft_date",
    allDay = "agenda_draft_all_day",
    startTime = "agenda_draft_start",
    endTime = "agenda_draft_end",
    reminder = "agenda_draft_reminder",
    notes = "agenda_draft_notes",
    mode = "agenda_editor_mode",
    id = "agenda_editor_id",
    revision = "agenda_editor_revision",
}
draft.keys = keys

function draft.trim(value)
    return (value or ""):gsub("^%s+", ""):gsub("%s+$", "")
end

function draft.formatTime(minutes)
    local value = math.max(0, math.min(1439, tonumber(minutes) or 0))
    return string.format("%02d:%02d", math.floor(value / 60), value % 60)
end

function draft.parseTime(value)
    local hour, minute = string.match(draft.trim(value), "^(%d%d?):(%d%d)$")
    hour = tonumber(hour)
    minute = tonumber(minute)
    if not hour or not minute or hour < 0 or hour > 23 or
        minute < 0 or minute > 59 then
        return nil
    end
    return hour * 60 + minute
end

function draft.copyRule(rule)
    local result = {
        kind = rule.kind,
        dates = {},
        startDate = rule.startDate,
        endDate = rule.endDate or "",
        interval = rule.interval or 1,
        weekdays = {},
        monthDay = rule.monthDay or 0,
    }
    for _, date in ipairs(rule.dates or {}) do
        result.dates[#result.dates + 1] = date
    end
    for _, weekday in ipairs(rule.weekdays or {}) do
        result.weekdays[#result.weekdays + 1] = weekday
    end
    return result
end

function draft.defaultRule(date)
    local info = calendar.dateInfo(date)
    return {
        kind = "dates", dates = { date }, startDate = date, endDate = "",
        interval = 1, weekdays = { info and info.weekday or 1 },
        monthDay = info and info.day or 1,
    }
end

local function dayNumber(info)
    local year = info.year - 1
    local days = year * 365 + math.floor(year / 4) - math.floor(year / 100) +
        math.floor(year / 400) + info.day
    for month = 1, info.month - 1 do
        local first = calendar.dateInfo(string.format("%04d-%02d-01", info.year, month))
        days = days + first.daysInMonth
    end
    return days
end

local function ruleMatches(rule, date)
    if not calendar.dateInfo(date) then return false end
    if rule.kind == "dates" then
        for _, item in ipairs(rule.dates) do
            if item == date then return true end
        end
        return false
    end
    if date < rule.startDate or (rule.endDate ~= "" and date > rule.endDate) then
        return false
    end
    local current, anchor = calendar.dateInfo(date), calendar.dateInfo(rule.startDate)
    if not current or not anchor then return false end
    if rule.kind == "monthly" then
        local months = (current.year - anchor.year) * 12 + current.month - anchor.month
        if months < 0 or months % rule.interval ~= 0 then return false end
        return current.day == (rule.monthDay == 0 and current.daysInMonth or rule.monthDay)
    end
    if rule.kind == "weekly" then
        local weeks = math.floor((dayNumber(current) - current.weekday + 1 -
            (dayNumber(anchor) - anchor.weekday + 1)) / 7)
        if weeks < 0 or weeks % rule.interval ~= 0 then return false end
        for _, weekday in ipairs(rule.weekdays) do
            if weekday == current.weekday then return true end
        end
    end
    return false
end

function draft.clearTransaction(tx)
    tx:remove(keys.title)
    tx:remove(keys.date)
    tx:remove(keys.allDay)
    tx:remove(keys.startTime)
    tx:remove(keys.endTime)
    tx:remove(keys.reminder)
    tx:remove(keys.notes)
    tx:remove(keys.mode)
    tx:remove(keys.id)
    tx:remove(keys.revision)
end

function draft.clear()
    storage.transaction(draft.clearTransaction)
end

function draft.startNew(date, today)
    local startMinutes = 540
    if date == today then
        local now = time.parts()
        startMinutes = math.min(1410,
            math.floor((now.hour * 60 + now.min + 29) / 30) * 30)
    end
    storage.transaction(function(tx)
        draft.clearTransaction(tx)
        tx:set(keys.title, "")
        tx:set(keys.date, date)
        tx:set(keys.allDay, "0")
        tx:set(keys.startTime, draft.formatTime(startMinutes))
        tx:set(keys.endTime, draft.formatTime(math.min(1439, startMinutes + 60)))
        tx:set(keys.reminder, "15")
        tx:set(keys.mode, "new")
    end)
end

function draft.fillFromItem(item)
    storage.transaction(function(tx)
        tx:set(keys.title, item.title or "")
        tx:set(keys.date, item.date)
        tx:set(keys.allDay, item.allDay and "1" or "0")
        tx:set(keys.startTime, draft.formatTime(item.startMinutes))
        tx:set(keys.endTime, draft.formatTime(item.endMinutes))
        tx:set(keys.reminder, tostring(item.reminderMinutes or -1))
        if item.notes and item.notes ~= "" then
            tx:set(keys.notes, item.notes)
        else
            tx:remove(keys.notes)
        end
    end)
end

function draft.startEdit(item)
    storage.transaction(function(tx)
        draft.clearTransaction(tx)
        tx:set(keys.mode, "edit")
        tx:set(keys.id, item.id)
        tx:set(keys.revision, tostring(item.revision))
    end)
    draft.fillFromItem(item)
end

local function normalizedRule(mode, sourceRule, date)
    local rule = draft.copyRule(sourceRule)
    rule.kind = mode
    rule.interval = tonumber(rule.interval)
    rule.monthDay = tonumber(rule.monthDay)
    rule.startDate = date
    rule.endDate = draft.trim(rule.endDate or "")
    if mode == "dates" then
        table.sort(rule.dates)
        rule.startDate = rule.dates[1] or ""
        rule.endDate = rule.dates[#rule.dates] or ""
        rule.interval = 1
        rule.weekdays = {}
        rule.monthDay = 0
    elseif mode == "weekly" then
        rule.dates = {}
        rule.monthDay = 0
    elseif mode == "monthly" then
        rule.dates = {}
        rule.weekdays = {}
    end
    if not calendar.dateInfo(rule.startDate) or
        (rule.endDate ~= "" and (not calendar.dateInfo(rule.endDate) or rule.endDate < rule.startDate)) or
        not rule.interval or rule.interval < 1 or rule.interval > 99 or rule.interval % 1 ~= 0 or
        (mode == "dates" and (#rule.dates < 1 or #rule.dates > 366)) or
        (mode == "weekly" and #rule.weekdays == 0) or
        (mode == "monthly" and (not rule.monthDay or rule.monthDay < 0 or
            rule.monthDay > 31 or rule.monthDay % 1 ~= 0)) then
        return nil
    end
    return rule
end

function draft.buildSaveRequest(editor)
    local title = draft.trim(storage.get(keys.title) or "")
    if title == "" then return nil, "invalidTitle" end
    local date = draft.trim(storage.get(keys.date) or "")
    local mode = editor.mode or "single"
    local rule
    if mode ~= "single" then
        rule = normalizedRule(mode, editor.rule, date)
        if not rule then return nil, "invalidDate" end
        date = rule.startDate
    end
    if not calendar.dateInfo(date) then return nil, "invalidDate" end

    local allDay = storage.get(keys.allDay) == "1"
    local startMinutes = draft.parseTime(storage.get(keys.startTime) or "")
    local endMinutes = draft.parseTime(storage.get(keys.endTime) or "")
    if not allDay and (not startMinutes or not endMinutes or endMinutes < startMinutes) then
        return nil, "invalidTime"
    end
    local arguments = {
        title = title,
        date = date,
        allDay = allDay,
        startMinutes = allDay and 0 or startMinutes,
        endMinutes = allDay and 0 or endMinutes,
        notes = storage.get(keys.notes) or "",
        reminderMinutes = tonumber(storage.get(keys.reminder)) or 15,
    }
    local taskName = rule and "calendar.series.create" or "calendar.create"
    if storage.get(keys.mode) == "edit" then
        local series = editor.seriesDefinition
        if editor.scope == "series" and series then
            taskName = "calendar.series.update"
            arguments.id = series.id
            arguments.expectedRevision = series.revision
            if not editor.confirmSeriesChange then
                for _, exception in ipairs(series.exceptions) do
                    if not ruleMatches(rule, exception.occurrenceDate) then
                        return nil, "confirmSeriesChange"
                    end
                end
            end
        else
            taskName = "calendar.update"
            arguments.id = storage.get(keys.id) or ""
            arguments.expectedRevision = tonumber(storage.get(keys.revision)) or 0
        end
    end
    if rule then
        arguments.kind = rule.kind
        arguments.dates = rule.dates
        arguments.startDate = rule.startDate
        arguments.endDate = rule.endDate
        arguments.interval = rule.interval
        arguments.weekdays = rule.weekdays
        arguments.monthDay = rule.monthDay
    end
    return { name = taskName, arguments = arguments }
end

return draft
