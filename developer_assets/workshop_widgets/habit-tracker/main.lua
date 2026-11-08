local history = module.require("modules/history.lua")
local detailView = module.require("modules/detail.lua")
local dayCheck = "habit.day"
local check = utf8.char(0x2713)
local blank = utf8.char(0x00A0)
local demoToday = "2026-11-08"
local demoDays = { "1234567", "2456", "3567" }

local title = l10n.tr("lua_widget.habit_tracker.name")
local weekdays = {
    l10n.tr("lua_widget.habit_tracker.mon"), l10n.tr("lua_widget.habit_tracker.tue"),
    l10n.tr("lua_widget.habit_tracker.wed"), l10n.tr("lua_widget.habit_tracker.thu"),
    l10n.tr("lua_widget.habit_tracker.fri"), l10n.tr("lua_widget.habit_tracker.sat"),
    l10n.tr("lua_widget.habit_tracker.sun"),
}
local defaultNames = {
    l10n.tr("lua_widget.habit_tracker.read"), l10n.tr("lua_widget.habit_tracker.exercise"),
    l10n.tr("lua_widget.habit_tracker.journal"), "", "",
}
local fields = {}
for slot, name in ipairs(defaultNames) do
    fields[slot] = {
        key = "habit" .. slot, type = "text", default = name, maxLength = 60,
        label = l10n.tr("lua_widget.habit_tracker.habit", slot),
        description = l10n.tr("lua_widget.habit_tracker.habit_hint"),
        validationMessage = l10n.tr("lua_widget.habit_tracker.name_limit"),
    }
end

local function habitName(slot)
    local name = storage.get(fields[slot].key)
    return (name == nil and defaultNames[slot] or name):match("^%s*(.-)%s*$")
end

local function recordFor(model, date)
    local year = date:sub(1, 4)
    if model.records[year] == nil then
        local record = storage.get("history." .. year)
        model.records[year] = type(record) == "string" and record or ""
    end
    return model.records[year]
end

local function checked(model, date, slot)
    return history.has(recordFor(model, date), date, slot)
end

local function localDate(model)
    if model.demo then return demoToday end
    local parts = time.parts(time.now(), "local")
    return string.format("%04d-%02d-%02d", parts.year, parts.month, parts.day)
end

local function refresh(model)
    local habits = {}
    local completed = 0
    local bestName, bestStreak = nil, 0
    for _, slot in ipairs(model.order) do
        local name = habitName(slot)
        if name ~= "" then
            local done = checked(model, model.today, slot)
            completed = completed + (done and 1 or 0)
            local week = {}
            for i, day in ipairs(model.week) do
                week[i] = checked(model, day, slot)
            end
            -- An unfinished day leaves yesterday's streak active until midnight.
            local date = done and model.today or calendar.addDays(model.today, -1)
            local streak = 0
            while date and checked(model, date, slot) do
                streak = streak + 1
                date = calendar.addDays(date, -1)
            end
            habits[#habits + 1] = { slot = slot, name = name, done = done, week = week, streak = streak }
            if streak > bestStreak then bestName, bestStreak = name, streak end
        end
    end
    model.habits = habits
    model.completed = completed
    model.streak = bestName and l10n.tr("lua_widget.habit_tracker.streak", bestName, bestStreak)
end

local function updateDay(model)
    local today = localDate(model)
    if today == model.today then return false end
    model.today = today
    local index = (calendar.dateInfo(today).weekday + 5) % 7 + 1
    model.week = {}
    for i = 1, 7 do model.week[i] = calendar.addDays(today, i - index) end
    return true
end

local function setup(context)
    local model = {
        demo = context.preview and storage.get("demo") == "1", records = {},
        order = history.readOrder(storage.get("habitOrder"), #fields),
    }
    updateDay(model)
    if model.demo then
        local record = ""
        for slot, days in ipairs(demoDays) do
            for day in days:gmatch("%d") do
                record = history.toggle(record, model.week[tonumber(day)], slot)
            end
        end
        model.records[demoToday:sub(1, 4)] = record
    else
        schedule.every(dayCheck, 30000, { whenHidden = "pause" })
    end
    refresh(model)
    return model
end

local function refreshDetail(model)
    local detail = model.detail
    if not detail or detail.sorting then return end
    local item
    for _, habit in ipairs(model.habits) do
        if habit.slot == detail.slot then
            item = habit
            break
        end
    end
    item = item or model.habits[1]
    detail.item = item
    if not item then return end

    if detail.date:sub(1, 7) > model.today:sub(1, 7) then detail.date = model.today end
    detail.slot = item.slot
    detail.month = calendar.dateInfo(detail.date)
    local prefix = detail.date:sub(1, 8)
    local record = recordFor(model, detail.date)
    local completed = {}
    for day = 1, detail.month.daysInMonth do
        local date = string.format("%s%02d", prefix, day)
        if date <= model.today and history.has(record, date, item.slot) then
            completed[#completed + 1] = date
        end
    end
    detail.completed = completed
    detail.checked = detail.date <= model.today and history.has(record, detail.date, item.slot)
    if detail.totalSlot ~= item.slot or detail.totalThrough ~= model.today
        or detail.totalRevision ~= model.historyRevision then
        detail.total = history.total(model.records, model.today, item.slot)
        detail.totalSlot, detail.totalThrough, detail.totalRevision = item.slot, model.today, model.historyRevision
    end
end

local function toggleDate(model, slot, date)
    if not fields[slot] or habitName(slot) == "" or date > model.today then return false end
    local year = date:sub(1, 4)
    local record = history.toggle(recordFor(model, date), date, slot)
    local ok = model.demo or pcall(storage.set, "history." .. year, record)
    if ok then
        model.records[year] = record
        model.historyRevision = (model.historyRevision or 0) + 1
    end
    model.saveFailed = not ok
    return true
end

local function moveHabit(model, slot, offset)
    for index, item in ipairs(model.habits) do
        if item.slot == slot then
            local neighbor = model.habits[index + offset]
            if not neighbor then return false end
            local order = history.swap(model.order, slot, neighbor.slot)
            local ok = model.demo or pcall(storage.set, "habitOrder", table.concat(order))
            if ok then model.order = order end
            model.detail.orderFailed = not ok
            return true
        end
    end
    return false
end

local function handleAction(model, value)
    local id = value.id
    if id == "habit.toggle" then return toggleDate(model, value.value, model.today) end
    if id == "habit.open" and fields[value.value] and habitName(value.value) ~= "" then
        for _, key in ipairs(storage.keys()) do
            local year = key:match("^history%.(%d%d%d%d)$")
            if year then recordFor(model, year) end
        end
        model.detail = { slot = value.value, date = model.today }
        widget.openPanel({
            title = l10n.tr("lua_widget.habit_tracker.history"),
            width = 488, height = 484,
        })
        return true
    end

    local detail = model.detail
    if not detail or value.surface ~= "panel" then return false end
    if id == "habit.order" then
        detail.sorting = true
    elseif id == "habit.history" then
        detail.sorting = false
    elseif detail.sorting then
        if id == "habit.up" then return moveHabit(model, value.value, -1) end
        if id == "habit.down" then return moveHabit(model, value.value, 1) end
        return false
    elseif id == "habit.select" then
        local date = value.selection
        if type(date) ~= "string" or not date:match("^%d%d%d%d%-%d%d%-%d%d$")
            or date:sub(1, 7) ~= detail.date:sub(1, 7) or not calendar.dateInfo(date) then
            return false
        end
        detail.date = date
        model.saveFailed = false
    elseif id == "habit.month" then
        local date = history.shiftMonth(detail.date, value.value, model.today)
        if not date then return false end
        detail.date = date
        model.saveFailed = false
    elseif id == "habit.jumpToday" then
        detail.date = model.today
        model.saveFailed = false
    elseif id == "habit.editDate" then
        return toggleDate(model, detail.slot, detail.date)
    else
        return false
    end
    return true
end

local function event(_, model, value)
    if value.kind == "action" then
        local changedDay = updateDay(model)
        if not handleAction(model, value) and not changedDay then return end
    elseif value.kind == "settings.changed" then
        -- Read current settings, including the host's live-preview overlay.
    elseif (value.kind == "schedule" and value.id == dayCheck)
        or (value.kind == "visibility" and value.visible) then
        if not updateDay(model) then return end
    else
        return
    end
    refresh(model)
    refreshDetail(model)
    widget.invalidate()
end

local function label(key, content, height, size, secondary)
    return view.text({
        key = key, text = content, width = "fill", height = height, minWidth = 0,
        flexShrink = 0, fontSize = size, overflowText = "ellipsis",
        style = { foreground = secondary and "textSecondary" or "textPrimary" },
    })
end

local function markStyle(done, size, round, colors)
    return {
        background = done and colors.fill or nil,
        foreground = done and colors.check or "textSecondary",
        borderColor = "textSecondary",
        borderWidth = done and 0 or math.max(1, size * 0.045),
        cornerRadius = size * (round and 0.5 or 0.25),
    }
end

local function toggle(item, size, round, colors)
    local actionLabel = item.done and l10n.tr("lua_widget.habit_tracker.undo", item.name)
        or l10n.tr("lua_widget.habit_tracker.complete", item.name)
    -- The accessible label names the action. The visible pending mark is a hollow shape.
    return view.button({
        key = "toggle." .. item.slot, label = item.done and check or blank,
        width = size, height = size, flexShrink = 0, fontSize = size * 0.70,
        bold = true, textAlign = "center", padding = 0, style = markStyle(item.done, size, round, colors),
        hoverStyle = { borderColor = "textPrimary", borderWidth = 1.5 },
        accessibility = { label = actionLabel },
        events = { click = { id = "habit.toggle", value = item.slot } },
    })
end

local function divider(key)
    return view.divider({
        key = key, width = "fill", height = 1, flexShrink = 0,
        style = { foreground = "borderStrong", opacity = 0.33 },
    })
end

local function habitLabel(item, m, height)
    local name = label("name." .. item.slot, item.name, height, m.body)
    name.focusable = true
    name.cursor = "hand"
    name.accessibility = { role = "button", label = l10n.tr("lua_widget.habit_tracker.open_history", item.name) }
    name.hoverStyle = { foreground = "systemAccent" }
    name.events = { click = { id = "habit.open", value = item.slot } }
    return name
end

local function habitRow(item, m, width, stacked, rowHeight)
    local key = "habit." .. item.slot
    local name = habitLabel(item, m, rowHeight)
    name.flexShrink = 1
    local mark = toggle(item, math.min(m.row * 0.72, width), true, m.colors)
    if stacked then
        name.height, name.fontSize = m.row * 1.1, math.min(m.body, width * 0.22)
        name.textWrap, name.maxLines, name.textAlign = "wrap", 2, "center"
        name.flexShrink = 0
        mark.alignSelf = "center"
        local height = name.height + mark.height + m.smallGap
        return view.column({
            key = key, width = "fill", height = height, flexShrink = 0,
            children = { name, mark, view.spacer({ key = "habit.space." .. item.slot, height = m.smallGap }) },
        }), height
    end
    return view.row({
        key = key, width = "fill", height = rowHeight, flexShrink = 0,
        alignItems = "center", gap = m.smallGap, children = { name, mark },
    }), rowHeight
end

local function heading(text, m, width)
    local name = label("title", title, m.row, m.row * 0.56)
    name.bold, name.flexShrink = true, 1
    local children = { name }
    if text and width >= m.row * 3.7 then
        local status = label("status", text, m.row, m.caption, true)
        status.width = math.min(m.row * 2.8, width * 0.39)
        status.textAlign = "end"
        children[2] = status
    end
    return view.row({
        key = "heading", width = "fill", height = m.row, flexShrink = 0,
        gap = m.smallGap, children = children,
    })
end

local function spreadRows(row, available, count)
    -- Use spare height for rows while keeping font and control sizes tied to the base row.
    return math.max(row, math.min(row * 1.85, available / count))
end

local function weekly(model, m, width, height)
    local nameWidth = math.min(m.base * 6, width * 0.28)
    local colGap = m.base * 0.14
    local lane = (width - nameWidth - colGap * 7) / 7
    local cellSize = math.min(lane * 0.76, m.row * 0.63)
    local dayHeight = m.row * 0.72
    local count = #model.habits
    local rowHeight = spreadRows(m.row, height - dayHeight - count, count)
    local header = { view.spacer({ key = "week.names.spacer", width = nameWidth, flexShrink = 0 }) }
    local todayIndex
    for i, date in ipairs(model.week) do
        local today = date == model.today
        if today then todayIndex = i end
        local weekday = label("weekday." .. i, weekdays[i], dayHeight, m.caption, not today)
        weekday.width, weekday.textAlign, weekday.bold = lane, "center", today
        header[#header + 1] = weekday
    end
    local rows = {
        view.row({ key = "week.days", width = "fill", height = dayHeight, flexShrink = 0,
            gap = colGap, children = header }),
        divider("week.line.header"),
    }
    for index, item in ipairs(model.habits) do
        local name = habitLabel(item, m, rowHeight)
        name.width = nameWidth
        local cells = { name }
        for i, date in ipairs(model.week) do
            local cell
            if i == todayIndex then
                cell = toggle(item, cellSize, false, m.colors)
            else
                local done = date < model.today and item.week[i]
                local state = date > model.today and l10n.tr("lua_widget.habit_tracker.future")
                    or done and l10n.tr("lua_widget.habit_tracker.done")
                    or l10n.tr("lua_widget.habit_tracker.missed")
                cell = view.text({
                    key = "mark." .. item.slot .. "." .. i, text = done and check or "",
                    width = cellSize, height = cellSize, fontSize = cellSize * 0.70,
                    textAlign = "center", bold = true, flexShrink = 0,
                    style = date <= model.today and markStyle(done, cellSize, false, m.colors) or nil,
                    accessibility = { label = l10n.tr("lua_widget.habit_tracker.day_state",
                        item.name, weekdays[i], state) },
                })
            end
            cells[#cells + 1] = view.row({
                key = "week.cell." .. item.slot .. "." .. i, width = lane, height = rowHeight,
                flexShrink = 0, alignItems = "center", justifyContent = "center", children = { cell },
            })
        end
        rows[#rows + 1] = view.row({
            key = "week.habit." .. item.slot, width = "fill", height = rowHeight, flexShrink = 0,
            gap = colGap, children = cells,
        })
        if index < #model.habits then rows[#rows + 1] = divider("week.line." .. item.slot) end
    end
    local matrixHeight = dayHeight + count * (rowHeight + 1)
    return view.stack({
        key = "week", width = "fill", height = matrixHeight, flexShrink = 0,
        children = {
            view.row({
                key = "week.highlight", width = "fill", height = "fill",
                children = {
                    view.spacer({ key = "week.before.today",
                        width = nameWidth + colGap + (todayIndex - 1) * (lane + colGap), flexShrink = 0 }),
                    view.column({ key = "week.today", width = lane, height = "fill", flexShrink = 0,
                        style = { background = "textPrimary", opacity = 0.11, cornerRadius = m.row * 0.26 } }),
                },
            }),
            view.column({ key = "week.rows", width = "fill", height = "fill", children = rows }),
        },
    }), matrixHeight
end

local function desktop(context, model)
    local w, h = layout.contentWidth(), layout.contentHeight()
    local base = math.min(ui.metrics().layoutRowHeight, math.max(h * 0.75, 1))
    local padX = math.min(math.max(base * 0.56, (w - base * 25) / 2), w * 0.085)
    local padY = math.min(base * 0.56, h * 0.075)
    local width, height = w - 2 * padX, h - 2 * padY
    local count = #model.habits
    local weekFits = count > 0 and width >= base * 11.9 and height >= base * (count + 1.9)
    local stripFits = count > 0 and height < base * (count + 1.45) and width >= count * base * 3.25
    local row = math.max(base, math.min(base * 1.52, (height - count) / (count + 1.94)))
    local highContrast = context.accessibility.highContrast
    local m = {
        base = base, row = row, body = row * 0.44, caption = row * 0.31, smallGap = row * 0.14,
        colors = {
            fill = highContrast and "systemAccent" or "success",
            check = highContrast and "accentText" or (context.theme.mode == "light" and 0xFFFFFF or 0x102017),
        },
    }
    local summary = count > 0 and l10n.tr("lua_widget.habit_tracker.today", model.completed, count)
    local children, usedHeight = {}, 0
    local footer = model.saveFailed and l10n.tr("lua_widget.habit_tracker.save_failed") or model.streak
    local footerHeight = row * 0.62
    local footerRule = not stripFits and 1 or 0
    local footerSpace = footerRule + m.smallGap + footerHeight
    local function append(node, nodeHeight)
        usedHeight = usedHeight + nodeHeight
        children[#children + 1] = node
    end
    local function space(key, size)
        append(view.spacer({ key = key, height = size, flexShrink = 0 }), size)
    end

    local headingFits = (count == 0 or width >= base * 3.4 or height >= base * (count + 2.4))
        and width >= base * 2
        and (not stripFits or height >= row * 2 + m.smallGap)
    if headingFits then
        append(heading(weekFits and l10n.tr("lua_widget.habit_tracker.week") or summary, m, width), row)
        space("heading.gap", m.smallGap)
    end
    local spread = false
    if count == 0 then
        local hint = label("empty", l10n.tr("lua_widget.habit_tracker.empty"), row * 2, m.body, true)
        hint.textWrap, hint.maxLines = "wrap", 3
        append(hint, row * 2)
    elseif weekFits then
        spread = true
        local matrix, matrixHeight = weekly(model, m, width, height - usedHeight
            - (model.saveFailed and footerSpace or 0))
        append(matrix, matrixHeight)
    elseif stripFits then
        local gap = base * 0.48
        local items, itemWidth = {}, (width - gap * (count - 1)) / count
        for i, item in ipairs(model.habits) do
            local inset = base * 0.23
            local itemRow = habitRow(item, m, itemWidth - inset * 2, false, row)
            itemRow.padding = { horizontal = inset }
            items[i] = view.stack({
                key = "strip.item." .. item.slot, width = itemWidth, height = row, flexShrink = 0,
                children = {
                    view.column({ key = "strip.backdrop." .. item.slot, width = "fill", height = "fill",
                        style = { background = "textPrimary", opacity = 0.09, cornerRadius = base * 0.24 } }),
                    itemRow,
                },
            })
        end
        append(view.row({
            key = "strip", width = "fill", height = row, gap = gap, flexShrink = 0, children = items,
        }), row)
    else
        local stacked = width < row * 2.45
        spread = not stacked
        local listHeight = height - usedHeight - (count - 1)
        local reserveFooter = model.saveFailed or (footer and width >= row * 2.75
            and listHeight >= count * row + footerSpace)
        local rowHeight = stacked and row
            or spreadRows(row, listHeight - (reserveFooter and footerSpace or 0), count)
        for index, item in ipairs(model.habits) do
            local node, itemHeight = habitRow(item, m, width, stacked, rowHeight)
            append(node, itemHeight)
            if index < count then append(divider("line." .. item.slot), 1) end
        end
    end
    if footer and (model.saveFailed or (not weekFits and width >= row * 2.75
            and usedHeight + footerSpace <= height + 0.01)) then
        if footerRule > 0 then append(divider("footer.line"), footerRule) end
        space("footer.gap", m.smallGap)
        local note = label("footer", footer, footerHeight, m.caption, true)
        if model.saveFailed then note.style.foreground = "error" end
        append(note, footerHeight)
    end
    local content = view.column({
        key = "content", width = "fill", height = "auto",
        padding = { horizontal = padX, vertical = padY }, children = children,
    })
    if usedHeight > height + 0.01 then
        return view.scroll({ key = "viewport", width = "fill", height = "fill", children = { content } })
    end
    return view.column({
        key = "card", width = "fill", height = "fill",
        justifyContent = spread and "start" or "center", children = { content },
    })
end
return widget.define({
    name = title,
    showTitle = false,
    useCustomStyle = true,
    followPersonalizationDefault = true,
    bg = 0x142948, border = 0xACC8F4, alpha = 0.48, borderAlpha = 0.20, glassEnabled = true,
    settings = { fields = fields },
    setup = setup,
    view = desktop,
    panel = function(context, model) return detailView(context, model, weekdays) end,
    event = event,
})
