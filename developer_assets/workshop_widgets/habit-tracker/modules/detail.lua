local history = module.require("modules/history.lua")
local previous = utf8.char(0x2039)
local nextMonth = utf8.char(0x203A)
local previousYear = utf8.char(0x00AB)
local nextYear = utf8.char(0x00BB)
local up = utf8.char(0x2191)
local down = utf8.char(0x2193)

local function text(key, value, m, secondary)
    return view.text({
        key = key, text = value, width = "fill", height = m.row, minWidth = 0,
        fontSize = secondary and m.caption or m.body, overflowText = "ellipsis",
        style = { foreground = secondary and "textSecondary" or "textPrimary" },
    })
end

local function command(key, label, m, width, enabled, id, value, accessible)
    return view.button({
        key = key, label = label, width = width, height = m.row,
        flexShrink = width == "fill" and 1 or 0, minWidth = 0,
        fontSize = m.body, textAlign = "center", enabled = enabled,
        padding = { horizontal = m.gap * 0.45 },
        style = { foreground = "textPrimary", cornerRadius = m.row * 6 / 28 },
        accessibility = accessible and { label = accessible },
        events = { click = { id = id, value = value } },
    })
end

local function row(key, m, children)
    return view.row({
        key = key, width = "fill", height = m.row, flexShrink = 0,
        alignItems = "center", gap = m.gap, children = children,
    })
end

local function ordering(model, m)
    local detail = model.detail
    local title = text("sort.title", l10n.tr("lua_widget.habit_tracker.order"), m, false)
    title.bold = true
    local children = {
        row("sort.header", m, {
            title,
            command("sort.back", l10n.tr("lua_widget.habit_tracker.back"), m,
                math.min(m.row * 3.1, m.width * 0.46), true, "habit.history"),
        }),
    }
    for index, item in ipairs(model.habits) do
        local key = "sort." .. item.slot
        local name = text(key .. ".name", item.name, m, false)
        children[#children + 1] = row(key, m, {
            name,
            command(key .. ".up", up, m, m.row, index > 1, "habit.up", item.slot,
                l10n.tr("lua_widget.habit_tracker.move_up", item.name)),
            command(key .. ".down", down, m, m.row, index < #model.habits, "habit.down", item.slot,
                l10n.tr("lua_widget.habit_tracker.move_down", item.name)),
        })
    end
    if detail.orderFailed then
        local failure = text("sort.failure", l10n.tr("lua_widget.habit_tracker.order_failed"), m, false)
        failure.style.foreground = "error"
        children[#children + 1] = failure
    end
    return children
end

local function calendarView(context, model, m, weekdays)
    local detail = model.detail
    local item = detail.item
    if not item then
        local empty = text("detail.empty", l10n.tr("lua_widget.habit_tracker.empty"), m, true)
        empty.textWrap, empty.maxLines, empty.height = "wrap", 2, m.row * 2
        return { empty }
    end

    local title = text("detail.name", item.name, m, false)
    title.bold, title.fontSize = true, m.row * 18 / 28
    local monthTitle = l10n.tr("lua_widget.habit_tracker.month", detail.month.year, detail.month.month)
    local monthLabel = text("detail.month.label", monthTitle, m, false)
    monthLabel.bold, monthLabel.textAlign, monthLabel.fontSize = true, "center", m.row * 16 / 28
    monthLabel.gridColumn, monthLabel.columnSpan = 3, 3
    local function nav(key, column, glyph, delta, accessible)
        local button = command(key, glyph, m, m.navWidth, history.shiftMonth(detail.date, delta, model.today) ~= nil,
            "habit.month", delta, accessible)
        button.fontSize = math.min(m.row * 17 / 28, m.navWidth * 0.55)
        local cell = row(key .. ".cell", m, { button })
        cell.gridColumn, cell.justifyContent = column, "center"
        return cell
    end
    local toolbar = view.grid({
        key = "detail.month.toolbar", width = "fill", height = m.row, flexShrink = 0,
        columns = 7,
        children = {
            nav("detail.year.prev", 1, previousYear, -12, l10n.tr("lua_widget.habit_tracker.prev_year")),
            nav("detail.month.prev", 2, previous, -1, l10n.tr("lua_widget.habit_tracker.prev_month")),
            monthLabel,
            nav("detail.month.next", 6, nextMonth, 1, l10n.tr("lua_widget.habit_tracker.next_month")),
            nav("detail.year.next", 7, nextYear, 12, l10n.tr("lua_widget.habit_tracker.next_year")),
        },
    })

    local future = detail.date > model.today
    local state = future and l10n.tr("lua_widget.habit_tracker.future")
        or detail.checked and l10n.tr("lua_widget.habit_tracker.done")
        or l10n.tr("lua_widget.habit_tracker.missed")
    local action = detail.checked and l10n.tr("lua_widget.habit_tracker.undo_entry")
        or detail.date == model.today and l10n.tr("lua_widget.habit_tracker.check_in")
        or l10n.tr("lua_widget.habit_tracker.make_up")
    local summary = text("detail.summary", l10n.tr("lua_widget.habit_tracker.summary",
        #detail.completed, detail.total, item.streak), m, true)
    summary.textWrap, summary.maxLines, summary.height = "wrap", 2, m.row * 1.3
    local inline = m.width >= m.row * 13
    local commandWidth = inline and math.min(m.row * 5.6, m.width * 0.30) or "fill"
    local selection = text("detail.selection", l10n.tr("lua_widget.habit_tracker.selected_date",
        detail.date, state), m, true)
    local actions = {
        command("detail.today", l10n.tr("lua_widget.habit_tracker.jump_today"), m,
            commandWidth, detail.date ~= model.today, "habit.jumpToday"),
        command("detail.edit", action, m, commandWidth, not future, "habit.editDate"),
    }
    local actionHeight = m.row * 1.45
    for _, button in ipairs(actions) do
        button.height, button.textWrap, button.maxLines = actionHeight, "wrap", 2
    end
    if inline then table.insert(actions, 1, selection) end
    local actionRow = row("detail.actions", m, actions)
    actionRow.height = actionHeight
    local fixedHeight = m.row * (inline and 4.3 or 5.3)
        + m.gap * (inline and 4 or 5) + m.inset * 2 + actionHeight - m.row
    if model.saveFailed then fixedHeight = fixedHeight + m.row + m.gap end
    local calendarHeight = math.max(m.row * 6.6,
        math.min(m.row * 9, layout.contentHeight() - fixedHeight))
    local children = {
        row("detail.header", m, {
            title,
            command("detail.order", l10n.tr("lua_widget.habit_tracker.order"), m,
                math.min(m.row * 6.2, m.width * 0.50), true, "habit.order"),
        }),
        summary,
        toolbar,
        view.monthCalendar({
            key = "detail.calendar", width = "fill", height = calendarHeight, flexShrink = 0,
            year = detail.month.year, month = detail.month.month, selectedDate = detail.date,
            todayDate = model.today, firstDayOfWeek = 2, showAdjacentDates = false,
            weekdayLabels = { weekdays[7], table.unpack(weekdays, 1, 6) },
            eventDates = detail.completed, fontSize = math.min(m.row * 16 / 28, m.width * 0.54 / 7),
            style = { foreground = "textPrimary" },
            eventStyle = { foreground = context.accessibility.highContrast and "systemAccent" or "success" },
            action = { id = "habit.select" },
            accessibility = { label = monthTitle },
        }),
    }
    if not inline then children[#children + 1] = selection end
    children[#children + 1] = actionRow
    if model.saveFailed then
        local failure = text("detail.failure", l10n.tr("lua_widget.habit_tracker.save_failed"), m, false)
        failure.style.foreground = "error"
        children[#children + 1] = failure
    end
    return children
end

return function(context, model, weekdays)
    if not model.detail then return nil end
    local rowHeight = ui.metrics().layoutRowHeight
    local inset = math.min(rowHeight * 6 / 28, layout.contentWidth() * 0.12)
    local width = layout.contentWidth() - 2 * inset
    local m = {
        row = rowHeight, width = width, inset = inset,
        body = rowHeight * 15 / 28, caption = rowHeight * 13 / 28,
        gap = math.min(rowHeight * 8 / 28, width * 0.035),
        navWidth = math.min(rowHeight * 1.24, width * 0.13),
    }
    local children = model.detail.sorting and ordering(model, m) or calendarView(context, model, m, weekdays)
    return view.scroll({
        key = "history.viewport", width = "fill", height = "fill",
        children = { view.column({
            key = "history.content", width = "fill", height = "auto",
            padding = inset, gap = m.gap, children = children,
        }) },
    })
end
