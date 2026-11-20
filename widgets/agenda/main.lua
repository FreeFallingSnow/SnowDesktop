-- agenda/main.lua - API v2 calendar agenda with local editing
local draft = module.require("modules/editor_draft.lua")
local draftKeys = draft.keys
local descriptor

local EDGE_FADE = widget.hasFeature("draw.clip.edgeFade")
local VIEW_EDGE_FADE = widget.hasFeature("view.scroll.edgeFade")

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
    details = utf8.char(0xE24B),
    today = utf8.char(0xF23C),
    previous = utf8.char(0xF15B),
    next = utf8.char(0xF181),
}

local SELECTED_ID = "agenda_selected_id"
local reminderValues = { -1, 0, 5, 15, 30, 60, 1440 }

local function supportsSeries()
    return widget.hasFeature("calendar.series") and
        widget.hasFeature("task.calendar.series") and
        widget.hasFeature("ui.datePicker.multiple") and
        type(calendar.seriesById)=="function"
end

local settings = {
    fields = {
        {
            key = "rangeDays",
            agentDescription = "Selects how many days of events to query, including the starting date. It affects which event groups appear.",
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
            agentDescription = "Scales the agenda's rendered text and its measured row layout relative to the component metrics.",
            label = l10n.tr("lua_widget.common.font_scale"),
            type = "int",
            default = 100,
            min = 70,
            max = 135,
        },
    },
}

local function todayDate()
    local now = time.parts()
    return string.format("%04d-%02d-%02d", now.year, now.month, now.day)
end

local function eventTiming(item)
    return item.allDay and l10n.tr("lua_widget.agenda.all_day") or
        (draft.formatTime(item.startMinutes) .. " – " .. draft.formatTime(item.endMinutes))
end

local function isSubscription(id)
    -- The host uses this reserved prefix for ICS event IDs.
    return type(id) == "string" and id:sub(1, 4) == "ics-"
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

local function clearDraft(model)
    if model.pendingPanelTask then task.cancel(model.pendingPanelTask) end
    draft.clear()
    model.editorError = nil
    model.pendingPanelTask = nil
    model.datePicker = nil
    model.timePicker = nil
    model.selectOpen = nil
    model.rule = nil
    model.seriesDefinition = nil
    model.occurrenceItem = nil
    model.scope = nil
    model.mode = nil
    model.confirmSeriesChange = false
end

local function openEditor(model)
    local title = storage.get(draftKeys.mode) == "edit" and
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

local function openDetails(model, item)
    if model.panelOpen or not item or not isSubscription(item.id) then return end
    model.details = item
    widget.openPanel({
        title = l10n.tr("lua_widget.agenda.details"),
        width = 400,
        height = 300,
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
    draft.startNew(date, todayDate())
    model.rule=supportsSeries() and draft.defaultRule(date) or nil
    model.scope="single"
    model.mode="single"
    openEditor(model)
end

local function startEdit(model, item, wholeSeries)
    if not item or isSubscription(item.id) or not widget.hasPermission("calendar.write") then return end
    if model.panelOpen then
        widget.closePanel()
        return
    end
    draft.startEdit(item)
    model.occurrenceItem=item
    model.seriesDefinition=item.seriesId and supportsSeries() and calendar.seriesById(item.seriesId) or nil
    model.scope=model.seriesDefinition and (wholeSeries and "series" or "occurrence") or "single"
    model.mode=model.scope=="series" and model.seriesDefinition.rule.kind or "single"
    if model.seriesDefinition then
        model.rule=draft.copyRule(model.seriesDefinition.rule)
        if model.scope=="series" then draft.fillFromItem(model.seriesDefinition.event) end
    else model.rule=nil end
    openEditor(model)
end

local function saveDraft(model)
    if model.pendingPanelTask or not widget.hasPermission("calendar.write") then return end
    local request, reason = draft.buildSaveRequest(model)
    if reason == "confirmSeriesChange" then
        model.confirmSeriesChange = true
        widget.invalidate()
        return
    end
    if not request then
        model.editorError = reason
        return
    end
    local taskId, taskError = task.start(request.name, request.arguments)
    if taskId then
        model.pendingPanelTask = taskId
        model.editorError = nil
    else
        model.editorError = taskError == "permissionDenied" and
            "permissionDenied" or "saveFailed"
    end
end

local function deleteEvent(model, item, wholeSeries)
    if not item or isSubscription(item.id) or model.pendingDeleteTask then return end
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

local function registerRegion(key, shape, eventsValue, label, enabled, viewport)
    if viewport then
        local left, top = math.max(shape.x, viewport.x), math.max(shape.y, viewport.y)
        local right = math.min(shape.x + shape.width, viewport.x + viewport.width)
        local bottom = math.min(shape.y + shape.height, viewport.y + viewport.height)
        if right <= left or bottom <= top then return end
        if left ~= shape.x or top ~= shape.y or right ~= shape.x + shape.width or bottom ~= shape.y + shape.height then
            shape = { type = "rect", x = left, y = top, width = right - left, height = bottom - top }
        end
    end
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
    draw.text(x + width / 2, y + height / 2, text, size,
        color, math.max(1, width), bold, true, 0, alpha or 1.0, nil,
        { x = "center", y = "center" })
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
    draw.glyph(glyph, shape.x + shape.width / 2, shape.y + shape.height / 2,
        metrics.iconSize, colors.accent, "fluent", enabled and 1.0 or 0.28)
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
    centeredText(label, shape.x,
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
    local contentHeight = #items * rowHeight - rowGap
    local scrollViewport = { type = "rect", x = pad, y = listTop,
        width = width - pad, height = viewportHeight }
    local scroll = interaction.scroll({
        key = "agenda.scroll",
        shape = scrollViewport,
        contentHeight = contentHeight > viewportHeight and math.ceil(contentHeight) or 0,
    })
    local first = math.max(1, math.floor(scroll.offset / rowHeight) + 1)
    local last = math.min(#items,
        math.ceil((scroll.offset + viewportHeight) / rowHeight))
    local selectedId = storage.get(SELECTED_ID)
    draw.pushClip(pad, listTop, width - pad * 2, viewportHeight,
        EDGE_FADE and { scroll = scroll, fadeLength = metrics.layoutRowHeight * 0.5 } or nil)
    for index = first, last do
        local item = items[index]
        local subscribed = isSubscription(item.id)
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
            click = { id = subscribed and "agenda.details" or "agenda.select", value = item.id },
            doubleClick = not subscribed and { id = "agenda.edit", value = item.id } or nil,
            contextMenu = { id = "agenda.menu", value = item.id },
        }, item.title or l10n.tr("lua_widget.agenda.untitled"), true, viewport)

        local textX = pad + metrics.spacingSm
        local textWidth = width - pad * 2 - metrics.spacingLg
        draw.text(textX, y + metrics.spacingXs,
            item.title ~= "" and item.title or
                l10n.tr("lua_widget.agenda.untitled"),
            mainFont, colors.text, textWidth, true, true)
        draw.text(textX, y + cardHeight - smallFont - metrics.spacingSm,
            formatDate(item.date, true) .. " · " .. eventTiming(item),
            smallFont, colors.secondary, textWidth, false, true, 0, 0.92)
    end
    draw.popClip()
end

local function panel(context, model)
    local row=ui.metrics().layoutRowHeight
    local busy=model.pendingPanelTask~=nil
    local function button(id,label,enabled)
        return view.button({key="agenda."..id,label=label,width="fill",height=row,fontSize=row*0.46,textAlign="center",
            padding={horizontal=row*0.32},
            style={foreground="textPrimary",cornerRadius=row*0.12},
            enabled=enabled~=false,action={id="agenda.panel",value=id},accessibility={label=label}})
    end
    local children={}
    local function selectField(id,label,selected,options)
        children[#children+1]=view.text({key="agenda.select.label."..id,text=label,
            height=row,fontSize=row*0.43,style={foreground="textSecondary"}})
        children[#children+1]=view.select({key="agenda.select."..id,selectedValue=tostring(selected),
            options=options,width="fill",height=row,fontSize=row*0.46,
            style={foreground="textPrimary"},enabled=not busy,expanded=model.selectOpen==id,
            events={click={id="agenda.select.open",value=id},change={id="agenda.select.change",value=id}},
            accessibility={label=label}})
    end
    local details = model.details
    if details then
        local width = math.max(1, layout.contentWidth() - row * 0.8)
        local function detailText(key, value, fontSize, foreground)
            local measured = draw.measureText(value, fontSize, width)
            return view.text({key="agenda.details."..key,text=value,width="fill",
                height=math.ceil(math.max(fontSize * 1.4, measured.height)),
                textWrap="wrap",verticalAlign="start",fontSize=fontSize,
                style={foreground=foreground}})
        end
        children = {
            detailText("title", details.title ~= "" and details.title or l10n.tr("lua_widget.agenda.untitled"),
                row * 0.58, "textPrimary"),
            detailText("time", formatDate(details.date, true) .. " · " .. eventTiming(details),
                row * 0.46, "textSecondary"),
            detailText("readOnly", l10n.tr("lua_widget.agenda.subscription_read_only"),
                row * 0.43, "textSecondary"),
        }
        if details.notes and details.notes ~= "" then
            children[#children+1] = detailText("notes", details.notes, row * 0.46, "textPrimary")
        end
        children[#children+1] = button("cancel", l10n.tr("lua_widget.agenda.close"))
    elseif model.timePicker then
        children={model.timePicker:view({rowHeight=row}),button("picker.back",l10n.tr("lua_widget.agenda.cancel"))}
    elseif model.datePicker then
        children={model.datePicker:view({rowHeight=row}),button("picker.back",l10n.tr("lua_widget.agenda.cancel"))}
    else
        local title=storage.get(draftKeys.title) or ""
        local date=storage.get(draftKeys.date) or ""
        local start=storage.get(draftKeys.startTime) or ""
        local finish=storage.get(draftKeys.endTime) or ""
        local allDay=storage.get(draftKeys.allDay)=="1"
        local titleError=draft.trim(title)=="" and l10n.tr("lua_widget.agenda.invalid_title") or nil
        local dateError=model.mode~="dates" and not calendar.dateInfo(date) and l10n.tr("lua_widget.agenda.invalid_date") or nil
        local startMinutes,endMinutes=draft.parseTime(start),draft.parseTime(finish)
        local timeError=not allDay and (not startMinutes or not endMinutes or endMinutes<startMinutes)
            and l10n.tr("lua_widget.agenda.invalid_time") or nil
        local function field(key,value,label,err,multi)
            children[#children+1]=view.text({key=key..".label",text=label,height=row,fontSize=row*0.43,style={foreground="textSecondary"}})
            local options={key=key,value=value,style={foreground="textPrimary"},height=row,fontSize=row*0.46,
                maxBytes=key==draftKeys.title and 512 or (multi and 8192 or 10),enabled=not busy,
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
        field(draftKeys.title,title,l10n.tr("lua_widget.agenda.title"),titleError)
        if supportsSeries() and (storage.get(draftKeys.mode)=="new" or model.seriesDefinition) then
            if model.seriesDefinition then
                selectField("scope",l10n.tr("lua_widget.agenda.scope"),model.scope,{
                    {key="occurrence",value="occurrence",label=l10n.tr("lua_widget.agenda.scope_occurrence")},
                    {key="series",value="series",label=l10n.tr("lua_widget.agenda.scope_series")}})
            end
            if model.scope~="occurrence" then
                local modes=model.seriesDefinition and {"dates","weekly","monthly"} or
                    {"single","dates","weekly","monthly"}
                local choices={}
                local modeKeys={single="lua_widget.agenda.mode_single",dates="lua_widget.agenda.mode_dates",
                    weekly="lua_widget.agenda.mode_weekly",monthly="lua_widget.agenda.mode_monthly"}
                for _,mode in ipairs(modes) do
                    choices[#choices+1]={key=mode,value=mode,label=l10n.tr(modeKeys[mode])}
                end
                selectField("mode",l10n.tr("lua_widget.agenda.date_mode"),model.mode,choices)
            end
        end
        if model.mode=="dates" and model.rule then
            local dates=model.rule.dates
            local summary=l10n.tr("lua_widget.agenda.choose_dates")
            if #dates>0 then
                local tokens={}
                local includeYear=dates[1]:sub(1,4)~=dates[#dates]:sub(1,4)
                local available=math.max(60,(context.layoutSize and context.layoutSize.width or row*10)-row*1.3-32)
                for index,dateValue in ipairs(dates) do
                    local compact=formatDate(dateValue,false)
                    if includeYear then compact=dateValue:sub(1,4).."/"..compact end
                    local candidate={table.unpack(tokens)}
                    candidate[#candidate+1]=compact
                    local remaining=#dates-index
                    local preview=table.concat(candidate," · ")
                    if remaining>0 then preview=preview.."  +"..tostring(remaining) end
                    if #tokens>0 and draw.measureText(preview,row*0.46,0,false).width>available then break end
                    tokens=candidate
                    summary=preview
                    if index>=6 then break end
                end
            end
            local dateLabel=l10n.tr("lua_widget.agenda.date")
            if #dates>0 then dateLabel=dateLabel.." · "..l10n.tr("lua_widget.agenda.selected_date_count",tostring(#dates)) end
            children[#children+1]=view.text({key="agenda.date.label",text=dateLabel,
                height=row,fontSize=row*0.43,style={foreground="textSecondary"}})
            children[#children+1]=view.button({key="agenda.openDatePicker",label=summary,width="fill",height=row,
                fontSize=row*0.46,textAlign="start",padding={horizontal=row*0.32},
                style={foreground="textPrimary",cornerRadius=row*0.12},
                enabled=not busy,action={id="agenda.panel",value="openDatePicker"},
                accessibility={label=dateLabel..": "..(#dates>0 and table.concat(dates,", ") or summary)}})
        else
            field(draftKeys.date,date,model.mode=="single" and l10n.tr("lua_widget.agenda.date") or
                l10n.tr("lua_widget.agenda.start_date"),dateError)
            children[#children+1]=button("openDatePicker",l10n.tr("lua_widget.agenda.choose_date"),not busy)
        end
        if model.rule and (model.mode=="weekly" or model.mode=="monthly") then
            local function ruleField(id,value,label)
                if id~="monthDay" then
                    children[#children+1]=view.text({key="agenda.rule.label."..id,text=label,height=row,
                        fontSize=row*0.43,style={foreground="textSecondary"}})
                end
                if id=="interval" or id=="monthDay" then
                    children[#children+1]=view.numberInput({key="agenda.rule.number."..id,value=tonumber(value) or 1,
                        min=1,max=id=="interval" and 99 or 31,step=1,height=row,fontSize=row*0.46,
                        enabled=not busy,action={id="agenda.rule",value=id},accessibility={label=label}})
                else
                    children[#children+1]=view.textInput({key="agenda.rule."..id,value=tostring(value),height=row,
                        fontSize=row*0.46,maxBytes=10,enabled=not busy,action={id="agenda.rule",value=id},
                        accessibility={label=label}})
                end
            end
            ruleField("interval",model.rule.interval,l10n.tr(model.mode=="weekly" and
                "lua_widget.agenda.interval_weeks" or "lua_widget.agenda.interval_months"))
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
                selectField("monthDay",l10n.tr("lua_widget.agenda.month_day"),
                    model.rule.monthDay==0 and "last" or "specific",{
                    {key="specific",value="specific",label=l10n.tr("lua_widget.agenda.month_day")},
                    {key="last",value="last",label=l10n.tr("lua_widget.agenda.last_day")}})
                if model.rule.monthDay~=0 then
                    ruleField("monthDay",model.rule.monthDay,l10n.tr("lua_widget.agenda.month_day"))
                end
            end
            selectField("endType",l10n.tr("lua_widget.agenda.end_date"),
                model.rule.endDate=="" and "never" or "date",{
                {key="never",value="never",label=l10n.tr("lua_widget.agenda.never_ends")},
                {key="date",value="date",label=l10n.tr("lua_widget.agenda.end_date")}})
            if model.rule.endDate~="" then ruleField("endDate",model.rule.endDate,l10n.tr("lua_widget.agenda.end_date")) end
        end
        children[#children+1]=view.checkbox({key="agenda.allDay",label=l10n.tr("lua_widget.agenda.all_day"),checked=allDay,
            height=row,fontSize=row*0.46,style={foreground="textPrimary"},enabled=not busy,action={id="agenda.panel",value="toggleAllDay"}})
        if not allDay then
            local function timeField(key,value,label)
                children[#children+1]=view.text({key=key..".label",text=label,height=row,
                    fontSize=row*0.43,style={foreground="textSecondary"}})
                children[#children+1]=view.button({key=key..".picker",label=value~="" and value or l10n.tr("lua_widget.agenda.choose_time"),
                    width="fill",height=row,fontSize=row*0.46,textAlign="start",padding={horizontal=row*0.32},
                    style={foreground="textPrimary",cornerRadius=row*0.12},enabled=not busy,
                    action={id="agenda.panel",value="openTimePicker"},
                    accessibility={label=label..": "..value..". "..l10n.tr("lua_widget.agenda.choose_time")}})
            end
            timeField(draftKeys.startTime,start,l10n.tr("lua_widget.agenda.start"))
            timeField(draftKeys.endTime,finish,l10n.tr("lua_widget.agenda.end"))
            if timeError then children[#children+1]=view.text({key="agenda.time.error",text=l10n.tr("lua_widget.agenda.invalid_time"),
                height=row,fontSize=row*0.42,style={foreground="textSecondary"}}) end
        end
        local reminders={}
        for _,minutes in ipairs(reminderValues) do
            local key=tostring(minutes)
            reminders[#reminders+1]={key=key,value=key,label=reminderLabel(minutes)}
        end
        selectField("reminder",l10n.tr("lua_widget.agenda.reminder"),
            storage.get(draftKeys.reminder) or "15",reminders)
        field(draftKeys.notes,storage.get(draftKeys.notes) or "",l10n.tr("lua_widget.agenda.notes"),nil,true)
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
    local page=details and "details" or model.timePicker and "time"
        or model.datePicker and "date" or "editor"
    return view.scroll({key="agenda.panel.scroll."..page,width="fill",height="fill",
        edgeFade=VIEW_EDGE_FADE and row * 0.5 or nil,children={
        view.column({key="agenda.form",width="fill",height="auto",padding=row*0.2,gap=row*0.25,children=children})}})
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
        storage.set(draftKeys.allDay,
            storage.get(draftKeys.allDay) == "1" and "0" or "1")
        widget.invalidate()
    elseif id == "scope" and model.seriesDefinition then
        model.scope=model.scope=="series" and "occurrence" or "series"
        model.mode=model.scope=="series" and model.rule.kind or "single"
        draft.fillFromItem(model.scope=="series" and model.seriesDefinition.event or model.occurrenceItem)
        model.confirmSeriesChange=false
        widget.invalidate()
    elseif id:sub(1,5)=="mode:" and model.rule and model.scope~="occurrence" then
        local mode=id:sub(6)
        if mode=="single" or mode=="dates" or mode=="weekly" or mode=="monthly" then
            if model.seriesDefinition and mode=="single" then return end
            model.mode=mode;model.rule.kind=mode
            local date=storage.get(draftKeys.date) or todayDate()
            local info=calendar.dateInfo(date)
            if #model.rule.dates==0 then model.rule.dates={date} end
            if #model.rule.weekdays==0 then model.rule.weekdays={info and info.weekday or 1} end
            if model.rule.monthDay==nil then model.rule.monthDay=info and info.day or 1 end
            model.confirmSeriesChange=false
            widget.invalidate()
        end
    elseif id == "endType" and model.rule then
        model.rule.endDate=model.rule.endDate=="" and
            (calendar.addDays(storage.get(draftKeys.date) or todayDate(),30) or todayDate()) or ""
        model.confirmSeriesChange=false;widget.invalidate()
    elseif id == "lastDay" and model.rule then
        local info=calendar.dateInfo(storage.get(draftKeys.date) or todayDate())
        model.rule.monthDay=model.rule.monthDay==0 and (info and info.day or 1) or 0
        model.confirmSeriesChange=false;widget.invalidate()
    elseif id == "openDatePicker" then
        if model.mode=="dates" and model.rule then
            model.datePicker=ui.datePicker({key="agenda.date",mode="multiple",value=model.rule.dates,
                todayDate=todayDate(),allowClear=false})
        else
            model.datePicker=ui.datePicker({key="agenda.date",value=storage.get(draftKeys.date) or "",
                todayDate=todayDate(),allowClear=false})
        end
        widget.invalidate()
    elseif id == "openTimePicker" then
        model.timePicker=ui.timePicker({key="agenda.time",mode="range",allowClear=false,
            value={startTime=storage.get(draftKeys.startTime) or "",endTime=storage.get(draftKeys.endTime) or ""}})
        widget.invalidate()
    elseif id == "picker.back" then
        model.timePicker=nil
        model.datePicker=nil
        widget.invalidate()
    end
end

local function event(_context, model, value)
    if model.details and value.kind == "action" and value.surface == "panel" then
        if value.id == "agenda.panel" and value.value == "cancel" then
            widget.closePanel()
        end
        return
    end
    if model.timePicker then
        local result=model.timePicker:handle(value)
        if result then
            if result.changed then
                storage.transaction(function(tx) tx:set(draftKeys.startTime,result.value.startTime);tx:set(draftKeys.endTime,result.value.endTime) end)
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
                    storage.set(draftKeys.date,result.value[1])
                else storage.set(draftKeys.date,result.value) end
                model.datePicker=nil;model.confirmSeriesChange=false
            end
            widget.invalidate()
            return
        end
    end
    if value.kind=="action" and value.surface=="panel" and value.id=="agenda.select.open" then
        model.selectOpen=value.expanded==true and tostring(value.value) or nil
        widget.invalidate();return
    end
    if value.kind=="action" and value.surface=="panel" and value.id=="agenda.select.change" then
        local field=type(value.value)=="string" and value.value or ""
        local selection=type(value.selection)=="string" and value.selection or ""
        model.selectOpen=nil
        if field=="mode" and (selection=="single" or selection=="dates" or
            selection=="weekly" or selection=="monthly") then
            handlePanelAction(model,"mode:"..selection)
        elseif field=="scope" and (selection=="series" or selection=="occurrence") then
            if model.scope~=selection then handlePanelAction(model,"scope") end
        elseif field=="monthDay" and model.rule and (selection=="specific" or selection=="last") then
            if (model.rule.monthDay==0)~=(selection=="last") then handlePanelAction(model,"lastDay") end
        elseif field=="endType" and model.rule and (selection=="never" or selection=="date") then
            if (model.rule.endDate=="")~=(selection=="never") then handlePanelAction(model,"endType") end
        elseif field=="reminder" then
            for _,minutes in ipairs(reminderValues) do
                if selection==tostring(minutes) then storage.set(draftKeys.reminder,selection);break end
            end
        end
        widget.invalidate();return
    end
    if value.kind=="action" and value.id=="agenda.rule" and value.surface=="panel" then
        if model.rule then
            if value.value=="endDate" and type(value.text)=="string" and #value.text<=10 then
                model.rule.endDate=value.text
            elseif (value.value=="interval" or value.value=="monthDay") and
                value.numberValid and type(value.controlValue)=="number" then
                model.rule[value.value]=value.controlValue
            end
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
        local allowed={ [draftKeys.title]=true,[draftKeys.date]=true,[draftKeys.startTime]=true,[draftKeys.endTime]=true,[draftKeys.notes]=true }
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
            if model.details then
                model.details = nil
            else
                clearDraft(model)
            end
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
                local date = storage.get(draftKeys.date) or selectedDate(model)
                local id = value.value.id
                storage.transaction(function(tx)
                    draft.clearTransaction(tx)
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
    elseif value.id == "agenda.details" and id then
        storage.set(SELECTED_ID, id)
        openDetails(model, model.eventsById[id])
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
    if item and isSubscription(item.id) then
        return ui.menu({
            {id="agenda.details",label=l10n.tr("lua_widget.agenda.details"),
                icon=fluent.details,iconFont="fluent"},
        })
    end
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
