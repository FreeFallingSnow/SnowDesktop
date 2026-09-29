-- system-monitor/main.lua - API v2 system data subscriptions
local subscriptions = {}
local gpuSubscriptionDetails
local gpuSelection = {}
local cardLayout = module.require("modules/card_layout.lua")
local cardPreferences = module.require("modules/card_preferences.lua")
local monitorData = module.require("modules/monitor_data.lua")
local monitorSources = module.require("modules/monitor_sources.lua")

local fluent = {
    refresh = utf8.char(0xF13D),
    style = utf8.char(0xF592),
}

local style = {
    bg = 0x0F172A,
    border = 0xFFFFFF,
    alpha = 0.34,
    borderAlpha = 0.16,
    gradientEndA = 0.30,
}

local settings = {
    fields = {
        { key = "show_cpu", label = l10n.tr("lua_widget.system_monitor.show_cpu"), type = "bool", default = monitorSources.defaults.cpu },
        { key = "show_memory", label = l10n.tr("lua_widget.system_monitor.show_memory"), type = "bool", default = monitorSources.defaults.memory },
        { key = "show_gpu", label = l10n.tr("lua_widget.system_monitor.show_gpu"), type = "bool", default = monitorSources.defaults.gpu },
        { key = "show_vram", label = l10n.tr("lua_widget.system_monitor.show_vram"), type = "bool", default = monitorSources.defaults.vram },
        { key = "show_network", label = l10n.tr("lua_widget.system_monitor.show_network"), type = "bool", default = monitorSources.defaults.network },
        { key = "show_battery", label = l10n.tr("lua_widget.system_monitor.show_battery"), type = "bool", default = monitorSources.defaults.battery },
        { key = "show_storage", label = l10n.tr("lua_widget.system_monitor.show_storage"), type = "bool", default = monitorSources.defaults.storage },
        { key = "show_disk_io", label = l10n.tr("lua_widget.system_monitor.show_disk_io"), type = "bool", default = monitorSources.defaults.disk_io },
        { key = "show_uptime", label = l10n.tr("lua_widget.system_monitor.show_uptime"), type = "bool", default = monitorSources.defaults.uptime },
    },
}

local cardTitles = {
    cpu = "CPU", memory = l10n.tr("lua_widget.system_monitor.memory"),
    gpu = "GPU", vram = l10n.tr("lua_widget.system_monitor.vram"),
    network = l10n.tr("lua_widget.system_monitor.network"),
    battery = l10n.tr("lua_widget.system_monitor.battery"),
    storage = l10n.tr("lua_widget.system_monitor.storage"),
    disk_io = l10n.tr("lua_widget.system_monitor.disk_io"),
    uptime = l10n.tr("lua_widget.system_monitor.uptime"),
}
local sizeFields = {}
for _, field in ipairs(settings.fields) do
    local id = field.key:sub(6)
    sizeFields[#sizeFields + 1] = field
    sizeFields[#sizeFields + 1] = {
        key = "size_" .. id,
        label = l10n.tr("lua_widget.system_monitor.card_size", cardTitles[id]),
        type = "select", default = "1x1", options = cardLayout.sizes,
    }
end
settings.fields = sizeFields
table.insert(settings.fields, 1, {
    key = "main_font_scale", label = l10n.tr("lua_widget.system_monitor.main_font_scale"),
    type = "int", default = 100, min = 70, max = 150,
})

local mainScale = 1
local function mainFont(size)
    return layout.fontCu(size) * mainScale
end

local function getPalette()
    local theme = widget.theme()
    if theme and theme.contentTheme == 1 then
        return {
            cardBg = 0xFFFFFF,
            cardBgA = 0.14,
            cardBd = 0x334155,
            cardBdA = 0.12,
            cardText = 0x1E293B,
            cardSub = 0x334155,
            trackBg = 0xE2E8F0,
            netDown = 0x0D9488,
            netUp = 0xEA580C,
            usageHigh = 0xDC2626,
            usageMed = 0xD97706,
            usageLow = 0x059669,
        }
    end
    return {
        cardBg = 0x000000,
        cardBgA = 0.08,
        cardBd = 0xFFFFFF,
        cardBdA = 0.10,
        cardText = 0xFFFFFF,
        cardSub = 0xF1F5F9,
        trackBg = 0x1E293B,
        netDown = 0x67D5B5,
        netUp = 0xFFB56B,
        usageHigh = 0xFF6B6B,
        usageMed = 0xFFD166,
        usageLow = 0x4ECB71,
    }
end

local function clamp(value)
    return math.max(0, math.min(100, value or 0))
end

local function usageColor(percent, palette)
    if percent >= 90 then return palette.usageHigh end
    if percent >= 70 then return palette.usageMed end
    return palette.usageLow
end

local function formatBytes(bytes)
    return l10n.formatBytes(math.max(0, bytes or 0), {
        base = 1024,
        maximumFractionDigits = 1,
    })
end

local function formatRate(bytes)
    return formatBytes(bytes) .. "/s"
end

local function rateLine(symbol, bytes, color)
    local value = bytes ~= nil and formatRate(bytes) or "—"
    local amount, unit = value:match("^(.-)" .. utf8.char(0x202F) .. "(.+)$")
    return { text = symbol .. " " .. value, symbol = symbol, value = value,
        amount = amount or value, unit = unit, color = color }
end

local function formatPercent(value)
    return l10n.formatNumber(clamp(value), {
        maximumFractionDigits = 0,
    }) .. "%"
end

local function formatUptime(milliseconds)
    local dayMs = 24 * 60 * 60 * 1000
    if milliseconds >= dayMs then
        local days = math.floor(milliseconds / dayMs)
        local hours = math.floor((milliseconds % dayMs) / (60 * 60 * 1000))
        return l10n.tr("lua_widget.system_monitor.days_hours",
            days, hours)
    end
    return l10n.formatDuration(milliseconds, { style = "short" })
end

local function uptimeParts(milliseconds)
    local parts, remaining = {}, math.max(0, milliseconds)
    for _, unit in ipairs({ 86400000, 3600000, 60000, 1000 }) do
        local count = math.floor(remaining / unit)
        remaining = remaining % unit
        if count > 0 then
            local text = unit == 86400000 and l10n.tr("lua_widget.system_monitor.days", count) or
                l10n.formatDuration(count * unit, { style = "short" })
            parts[#parts + 1] = { text = text }
        end
        if #parts == 3 then break end
    end
    if #parts == 0 then parts[1] = { text = l10n.formatDuration(0, { style = "short" }) } end
    return parts
end

local function showCard(name)
    return monitorSources.cardShown(name, storage.get("show_" .. name))
end

local function subscriptionValue(handle, permissionGranted)
    if permissionGranted == false then return nil, "permission" end
    if not handle then return nil, "unavailable" end
    local snapshot = handle:value()
    if snapshot.available then
        return snapshot.value, snapshot.stale and "stale" or nil
    end
    if snapshot.warmingUp then return nil, "waiting" end
    if snapshot.error == "permissionDenied" then
        return nil, "permission"
    end
    if snapshot.error == "notPresent" then return nil, "notPresent" end
    return nil, "unavailable"
end

local function statusText(status)
    if not status then return nil end
    local keys = {
        waiting = "lua_widget.system_monitor.waiting",
        stale = "lua_widget.system_monitor.stale",
        unavailable = "lua_widget.system_monitor.unavailable",
        permission = "lua_widget.system_monitor.permission_required",
        notPresent = "lua_widget.system_monitor.not_present",
    }
    return l10n.tr(keys[status] or keys.unavailable)
end

local function detailsWithStatus(details, status)
    local values = {}
    if details and details ~= "" then values[#values + 1] = details end
    local statusValue = statusText(status)
    if statusValue then values[#values + 1] = statusValue end
    if #values == 0 then return nil end
    return l10n.formatList(values)
end

local function summarizeStorage(value)
    if not value or not value.volumes then return nil end
    local result = { totalBytes = 0, usedBytes = 0, names = {} }
    for _, volume in ipairs(value.volumes) do
        if volume.capacityAvailable and (volume.capacityBytes or 0) > 0 then
            local total = math.max(0, volume.capacityBytes)
            local free = math.max(0, math.min(total,
                volume.freeBytes or 0))
            result.totalBytes = result.totalBytes + total
            result.usedBytes = result.usedBytes + total - free
            if volume.displayName and volume.displayName ~= "" then
                result.names[#result.names + 1] = volume.displayName
            end
        end
    end
    if result.totalBytes <= 0 then return nil end
    return result
end

local function fitFontSize(text, fontSize, minimum, maxWidth, bold, maxHeight)
    local fitted = fontSize
    local metrics = draw.measureText(text, fitted, 0, bold == true)
    while fitted > minimum and (metrics.width > maxWidth or
            (maxHeight and metrics.height > maxHeight)) do
        fitted = fitted - 1
        metrics = draw.measureText(text, fitted, 0, bold == true)
    end
    return fitted, metrics
end

local function drawMarqueeText(key, x, y, text, fontSize, color,
        viewportWidth)
    local metrics = draw.measureText(text, fontSize, 0, false)
    return draw.marqueeText({
        key = key,
        x = x,
        y = y,
        width = viewportWidth,
        height = metrics.height,
        text = text,
        size = fontSize,
        color = color,
        speed = 24,
        gap = layout.cu(24),
    })
end

local function centeredText(x, y, width, height, text, size, color, bold)
    local font, metrics = fitFontSize(text, size, layout.fontCu(9), width, bold, height)
    draw.text(x + math.max(0, (width - metrics.width) / 2),
        y + math.max(0, (height - metrics.height) / 2), text, font,
        color, width, bold, true, height)
end

local function gauge(x, y, width, height, info, palette)
    local side = math.max(1, math.min(width, height, layout.cu(120)))
    local cx, cy = x + width / 2, y + height / 2
    local color = info.color or palette.netDown
    if info.id == "battery" then
        local w, h = side * 0.80, side * 0.34
        local left, top = cx - w / 2, cy - h * 0.9
        draw.strokeRect(left, top, w, h, palette.cardSub,
            layout.cu(4), layout.cu(1.5), 0.70)
        draw.rect(left + w, top + h * 0.3, side * 0.04, h * 0.4,
            palette.cardSub, layout.cu(1), 0.70)
        local pad = layout.cu(3)
        if info.progress ~= nil and info.progress > 0 then
            draw.rect(left + pad, top + pad, (w - pad * 2) * info.progress,
                h - pad * 2, color, layout.cu(2), 0.9)
        end
        centeredText(x, top + h + layout.cu(5), width, side * 0.32,
            info.value, math.min(mainFont(32), side * 0.30 * mainScale), palette.cardText, true)
    elseif widget.hasFeature("draw.advanced") then
        local radius = side * 0.43
        local thickness = math.max(layout.cu(3), side * 0.065)
        draw.arc(cx, cy, radius, 135, 270, thickness, palette.cardBd, 0.14)
        if info.progress ~= nil and info.progress > 0 then
            draw.arc(cx, cy, radius, 135, 270 * info.progress, thickness, color, 1)
        end
        centeredText(cx - side * 0.34, cy - side * 0.18, side * 0.68, side * 0.36,
            info.value, math.min(mainFont(32), side * 0.30 * mainScale), palette.cardText, true)
    else
        -- The existing immediate API still gives older hosts a useful layout.
        centeredText(x, y, width, height * 0.75, info.value,
            mainFont(24), palette.cardText, true)
        draw.rect(x, y + height * 0.82, width, layout.cu(5), palette.trackBg, layout.cu(2))
        if info.progress ~= nil and info.progress > 0 then
            draw.rect(x, y + height * 0.82, width * info.progress,
                layout.cu(5), color, layout.cu(2))
        end
    end
end

local function wrappedText(x, y, width, height, text, palette, key)
    if not text or text == "" or height <= 0 then return end
    local size = layout.fontCu(12)
    local metrics = draw.measureText(text, size, width, false)
    if metrics.height > height + 0.5 then
        local lineHeight = draw.measureText(text, size, 0, false).height
        drawMarqueeText(key .. ".detail", x, y + math.max(0, (height - lineHeight) / 2),
            text, size, palette.cardSub, width)
        return
    end
    draw.text(x, y + math.max(0, (height - metrics.height) / 2),
        text, size, palette.cardSub, width, false, false, height)
end

local function capacityText(x, y, width, height, info, palette, horizontal)
    local gap = layout.cu(8)
    local cellWidth = horizontal and (width - gap) / 2 or width
    local cellHeight = horizontal and height or (height - gap) / 2
    local values = {
        { l10n.tr("lua_widget.system_monitor.used"), info.capacity.used },
        { l10n.tr("lua_widget.system_monitor.total"), info.capacity.total },
    }
    for index, item in ipairs(values) do
        local left = x + (horizontal and (index - 1) * (cellWidth + gap) or 0)
        local top = y + (horizontal and 0 or (index - 1) * (cellHeight + gap))
        local labelHeight = math.min(layout.cu(16), cellHeight * 0.45)
        draw.text(left, top, item[1], layout.fontCu(10), palette.cardSub,
            cellWidth, false, true, labelHeight)
        local font = fitFontSize(item[2], layout.fontCu(16), layout.fontCu(10), cellWidth, true)
        draw.text(left, top + labelHeight, item[2], font, palette.cardText,
            cellWidth, true, true, cellHeight - labelHeight)
    end
end

local function drawExpanded(x, y, width, height, info, palette, columns, rows)
    local gap = layout.cu(8)
    local large = columns > 1 and rows > 1
    if info.lines or info.timeParts then
        local values = info.lines or info.timeParts
        local horizontal = columns > 1 and (not info.timeParts or rows == 1)
        local footer = info.sub and math.min(layout.cu(rows > 1 and 36 or 18), height * 0.23) or 0
        local progressHeight = info.progress ~= nil and layout.cu(8) or 0
        local bodyHeight = height - footer - progressHeight - gap
        local cellWidth = horizontal and (width - gap * (#values - 1)) / #values or width
        local cellHeight = horizontal and bodyHeight or (bodyHeight - gap * (#values - 1)) / #values
        for index, line in ipairs(values) do
            local left = x + (horizontal and (index - 1) * (cellWidth + gap) or 0)
            local top = y + (horizontal and 0 or (index - 1) * (cellHeight + gap))
            if index > 1 then
                if horizontal then
                    draw.line(left - gap / 2, y, left - gap / 2, y + bodyHeight,
                        layout.cu(1), palette.cardBd, 0.12)
                else
                    draw.line(x, top - gap / 2, x + width, top - gap / 2,
                        layout.cu(1), palette.cardBd, 0.12)
                end
            end
            if line.symbol then
                local groupHeight = math.min(cellHeight, layout.cu(66))
                local groupTop = top + (cellHeight - groupHeight) / 2
                local headingHeight = groupHeight * 0.32
                local heading = line.symbol .. (line.unit and (" " .. line.unit) or "")
                centeredText(left, groupTop, cellWidth, headingHeight, heading,
                    layout.fontCu(12), line.color, false)
                centeredText(left, groupTop + headingHeight, cellWidth, groupHeight - headingHeight,
                    line.amount, mainFont(large and 32 or (rows > 1 and 24 or 22)), line.color, true)
            else
                centeredText(left, top, cellWidth, cellHeight, line.text,
                    mainFont(large and 26 or 20), palette.cardText, true)
            end
        end
        wrappedText(x, y + bodyHeight + gap, width, footer, info.sub, palette, info.id)
        if info.progress ~= nil then
            draw.rect(x, y + height - layout.cu(4), width, layout.cu(4),
                palette.trackBg, layout.cu(2))
            if info.progress > 0 then
                draw.rect(x, y + height - layout.cu(4), width * info.progress,
                    layout.cu(4), info.color, layout.cu(2))
            end
        end
        return
    end

    if rows == 1 then
        local gaugeWidth = width * 0.43
        centeredText(x, y, gaugeWidth, height * 0.72, info.value,
            mainFont(24), palette.cardText, true)
        draw.rect(x, y + height * 0.82, gaugeWidth, layout.cu(4), palette.trackBg, layout.cu(2))
        if info.progress ~= nil and info.progress > 0 then
            draw.rect(x, y + height * 0.82, gaugeWidth * info.progress, layout.cu(4),
                info.color, layout.cu(2))
        end
        local textX, textWidth = x + gaugeWidth + gap, width - gaugeWidth - gap
        if info.capacity then
            local statusHeight = info.expandedStatus and layout.cu(18) or 0
            capacityText(textX, y, textWidth, height - statusHeight, info, palette, false)
            wrappedText(textX, y + height - statusHeight, textWidth, statusHeight,
                info.expandedStatus, palette, info.id)
        else
            wrappedText(textX, y, textWidth, height, info.sub, palette, info.id)
        end
    else
        local detail = info.sub
        if info.capacity then detail = info.expandedSub end
        local detailHeight = detail and math.min(height * 0.30,
            draw.measureText(detail, layout.fontCu(12), width, false).height) or 0
        local capacityHeight = info.capacity and layout.cu(columns > 1 and 40 or 76) or 0
        local gaugeHeight = math.min(layout.cu(info.capacity and 76 or 120),
            math.max(layout.cu(32), height - detailHeight - capacityHeight - gap * 2))
        local groupHeight = gaugeHeight + capacityHeight + detailHeight + gap * (detailHeight > 0 and 2 or 1)
        local groupTop = y + math.max(0, (height - groupHeight) / 2)
        if info.capacity or columns == 1 then
            centeredText(x, groupTop, width, gaugeHeight * 0.74, info.value,
                mainFont(large and 32 or 24), palette.cardText, true)
            draw.rect(x, groupTop + gaugeHeight * 0.85, width, layout.cu(6), palette.trackBg, layout.cu(3))
            if info.progress ~= nil and info.progress > 0 then
                draw.rect(x, groupTop + gaugeHeight * 0.85, width * info.progress,
                    layout.cu(6), info.color, layout.cu(3))
            end
        else
            gauge(x, groupTop, width, gaugeHeight, info, palette)
        end
        if info.capacity then
            capacityText(x, groupTop + gaugeHeight + gap, width, capacityHeight,
                info, palette, columns > 1)
        end
        wrappedText(x, groupTop + gaugeHeight + capacityHeight + gap * 2,
            width, detailHeight, detail, palette, info.id)
    end
end

local function drawCard(x, y, width, height, info, palette, columns, rows)
    draw.rect(x, y, width, height, palette.cardBg,
        layout.cu(10), palette.cardBgA)
    draw.strokeRect(x, y, width, height, palette.cardBd,
        layout.cu(10), layout.cu(1.0), palette.cardBdA)

    local inset = layout.cu(8)
    local subFont = layout.fontCu(12)
    local contentWidth = math.max(1, width - inset * 2)
    local titleTop = y + layout.cu(6)
    local titleHeight = draw.measureText(info.title, subFont, 0, true).height
    draw.pushClip(x + inset, titleTop, contentWidth,
        math.max(1, height - layout.cu(12)))
    draw.text(x + inset, titleTop, info.title, subFont,
        palette.cardSub, contentWidth, true, true)

    if columns > 1 or rows > 1 then
        local top = titleTop + titleHeight + layout.cu(6)
        drawExpanded(x + inset, top, contentWidth,
            y + height - layout.cu(8) - top, info, palette, columns, rows)
        draw.popClip()
        return
    end

    local barY = info.progress ~= nil and (y + height - layout.cu(16)) or nil
    local subMetrics = info.sub and draw.measureText(info.sub, subFont, 0, false)
    local subBottom = barY and (barY - layout.cu(4)) or
        (y + height - layout.cu(6))
    local bodyTop = titleTop + titleHeight + layout.cu(4)
    local bodyBottom = (subMetrics and (subBottom - subMetrics.height) or
        barY or (y + height - layout.cu(6))) - layout.cu(4)
    local bodyHeight = math.max(1, bodyBottom - bodyTop)

    if info.lines then
        local sideBySide = width > height * 1.5
        local lineGap = layout.cu(2)
        local slotHeight = sideBySide and bodyHeight or
            (bodyHeight - lineGap * (#info.lines - 1)) / #info.lines
        local slotWidth = sideBySide and
            (contentWidth - lineGap * (#info.lines - 1)) / #info.lines or contentWidth
        local fitted, groupHeight = {}, lineGap * (#info.lines - 1)
        for index, line in ipairs(info.lines) do
            local font, metrics = fitFontSize(line.text,
                math.min(mainFont(24), slotHeight * 0.70 * mainScale),
                layout.fontCu(9), slotWidth, false, slotHeight)
            fitted[index] = { font = font, metrics = metrics }
            groupHeight = groupHeight + metrics.height
        end
        local nextY = bodyTop + math.max(0, (bodyHeight - groupHeight) / 2)
        for index, line in ipairs(info.lines) do
            local font, metrics = fitted[index].font, fitted[index].metrics
            local lineX = x + inset + (sideBySide and (index - 1) * (slotWidth + lineGap) or 0)
            local lineY = sideBySide and (bodyTop + math.max(0, (bodyHeight - metrics.height) / 2)) or nextY
            draw.text(lineX, lineY,
                line.text, font, line.color or palette.cardText,
                slotWidth, false, true, metrics.height)
            nextY = nextY + metrics.height + lineGap
        end
    else
        local valueFont = math.min(mainFont(20), bodyHeight * 0.48 * mainScale)
        if info.wrapValue then
            -- Keep localized duration units intact instead of wrapping a
            -- final CJK character onto a line of its own.
            local count = math.min(2, #info.timeParts)
            local lineHeight = bodyHeight / count
            for index = 1, count do
                centeredText(x + inset, bodyTop + (index - 1) * lineHeight,
                    contentWidth, lineHeight, info.timeParts[index].text,
                    mainFont(15), palette.cardText, true)
            end
        else
            local metrics = nil
            valueFont, metrics = fitFontSize(info.value, valueFont,
                layout.fontCu(10), contentWidth, true, bodyHeight)
            draw.text(x + math.max(inset, (width - metrics.width) / 2),
                bodyTop + math.max(0, (bodyHeight - metrics.height) / 2),
                info.value, valueFont, palette.cardText, contentWidth, true,
                true, bodyHeight)
        end
    end

    if barY then
        local barInset = layout.cu(8)
        local barHeight = layout.cu(4)
        draw.rect(x + barInset, barY, width - barInset * 2,
            barHeight, palette.trackBg, layout.cu(2), 1.0)
        draw.rect(x + barInset, barY,
            (width - barInset * 2) * info.progress,
            barHeight, info.color, layout.cu(2), 1.0)
    end

    if info.sub then
        drawMarqueeText(info.id, x + layout.cu(8),
            subBottom - subMetrics.height, info.sub, subFont,
            palette.cardSub, contentWidth)
    end
    draw.popClip()
end

local function reconcileSubscriptions()
    gpuSubscriptionDetails = monitorSources.reconcile(subscriptions, showCard,
        widget.hasFeature, widget.hasPermission, data.subscribe,
        gpuSubscriptionDetails)
end

local function selectedGpu(value)
    -- Migrate legacy "all" lazily without writing storage during rendering.
    local savedId = storage.get("gpu_scope") ~= "all" and
        storage.get("gpu_adapter_id") or nil
    local snapshot = subscriptions.gpu and subscriptions.gpu:value()
    local choice = monitorData.rememberGpuChoice(gpuSelection, value, savedId,
        storage.get("gpu_adapter_name"), snapshot and snapshot.timestamp)
    return choice and choice.id or savedId, choice and choice.name or
        storage.get("gpu_adapter_name")
end

local function persistGpuSelection()
    local value = subscriptionValue(subscriptions.gpu)
    local id = selectedGpu(value)
    local choice = monitorData.resolveGpuChoice(value, id)
    if not choice then return end
    if storage.get("gpu_adapter_id") ~= choice.id then
        storage.set("gpu_adapter_id", choice.id)
    end
    if storage.get("gpu_adapter_name") ~= choice.name then
        storage.set("gpu_adapter_name", choice.name)
    end
    if storage.get("gpu_scope") ~= "selected" then
        storage.set("gpu_scope", "selected")
    end
    gpuSelection.sourceId = choice.id
end

local function setup()
    reconcileSubscriptions()
    persistGpuSelection()
    return { previousLayout = "" }
end

local function buildCards()
    local palette = getPalette()
    local cpu, cpuState = subscriptionValue(subscriptions.cpu)
    local memory, memoryState = subscriptionValue(subscriptions.memory)
    local gpuValue, gpuState = subscriptionValue(subscriptions.gpu)
    local gpuId, gpuIdentity = selectedGpu(gpuValue)
    local gpuDetails = widget.hasFeature("data.system.gpu.details")
    local gpu = monitorData.summarizeGpu(gpuValue, gpuId, gpuDetails)
    if not gpu and not gpuState then gpuState = "notPresent" end
    if gpu then
        gpuIdentity = gpu.name
    elseif gpuId and gpuId ~= "" then
        gpuIdentity = l10n.formatList({
            gpuIdentity or l10n.tr("lua_widget.system_monitor.gpu_selected"),
            l10n.tr("lua_widget.system_monitor.gpu_choose_hint"),
        })
    end
    local powerPermission = widget.hasPermission("system.power.read")
    local networkPermission = widget.hasPermission("system.network.read")
    local storagePermission = widget.hasPermission("system.storage.read")
    local power, powerState = subscriptionValue(subscriptions.power,
        powerPermission)
    local network, networkState = subscriptionValue(subscriptions.network,
        networkPermission)
    local networkStatus, networkStatusState = subscriptionValue(
        subscriptions.networkStatus, networkPermission)
    local storageValue, storageState = subscriptionValue(
        subscriptions.storage, storagePermission)
    local storageSummary = summarizeStorage(storageValue)
    if not storageSummary and not storageState then
        storageState = "notPresent"
    end
    local diskIo, diskIoState = subscriptionValue(subscriptions.diskIo,
        storagePermission)
    local cards = {}

    if showCard("cpu") then
        local percent = cpu and clamp(cpu.usagePercent) or nil
        cards[#cards + 1] = {
            id = "cpu",
            title = "CPU",
            value = percent and formatPercent(percent) or "—",
            progress = percent and percent / 100 or nil,
            color = usageColor(percent or 0, palette),
            sub = detailsWithStatus(
                cpu and cpu.name ~= "" and cpu.name or
                (cpu and cpu.logicalProcessors and cpu.logicalProcessors > 0 and
                    l10n.tr("lua_widget.system_monitor.threads",
                        cpu.logicalProcessors) or nil), cpuState),
        }
    end

    if showCard("memory") then
        local percent = memory and clamp(memory.usagePercent) or nil
        cards[#cards + 1] = {
            id = "memory",
            title = l10n.tr("lua_widget.system_monitor.memory"),
            value = percent and formatPercent(percent) or "—",
            progress = percent and percent / 100 or nil,
            color = usageColor(percent or 0, palette),
            capacity = memory and memory.totalBytes and memory.totalBytes > 0 and {
                used = formatBytes(memory.usedBytes), total = formatBytes(memory.totalBytes),
            } or nil,
            expandedSub = statusText(memoryState),
            expandedStatus = statusText(memoryState),
            sub = detailsWithStatus(memory and memory.totalBytes and
                memory.totalBytes > 0 and
                    (formatBytes(memory.usedBytes) .. " / " ..
                        formatBytes(memory.totalBytes)) or nil,
                memoryState),
        }
    end

    if showCard("gpu") then
        local percent = gpu and gpu.usagePercent and clamp(gpu.usagePercent) or nil
        cards[#cards + 1] = {
            id = "gpu",
            title = "GPU",
            value = percent and formatPercent(percent) or "—",
            progress = percent and percent / 100 or nil,
            color = usageColor(percent or 0, palette),
            sub = detailsWithStatus(gpuIdentity,
                gpuState or (gpu and gpuDetails and not percent and "unavailable")),
        }
    end

    if showCard("vram") then
        local total = gpu and gpu.dedicatedMemoryBytes or 0
        local used = gpu and gpu.dedicatedUsedBytes or nil
        local percent = total > 0 and used and clamp(used / total * 100) or nil
        local details = total > 0 and ((used and formatBytes(used) or "—") ..
            " / " .. formatBytes(total)) or nil
        cards[#cards + 1] = {
            id = "vram",
            title = l10n.tr("lua_widget.system_monitor.vram"),
            value = percent and formatPercent(percent) or "—",
            progress = percent and percent / 100 or nil,
            color = usageColor(percent or 0, palette),
            capacity = total > 0 and {
                used = used and formatBytes(used) or "—", total = formatBytes(total),
            } or nil,
            expandedSub = statusText(gpuState or (gpu and gpuDetails and not percent and "unavailable")),
            expandedStatus = statusText(gpuState or (gpu and gpuDetails and not percent and "unavailable")),
            sub = detailsWithStatus(details,
                gpuState or (gpu and gpuDetails and not percent and "unavailable")),
        }
    end

    if showCard("network") then
        local connectivity = networkStatus and
            networkStatus.connectivity or nil
        local networkDetails = nil
        if connectivity == "internet" then
            networkDetails = l10n.tr("lua_widget.system_monitor.online")
        elseif connectivity == "local" then
            networkDetails = l10n.tr(
                "lua_widget.system_monitor.local_only")
        elseif connectivity == "none" then
            networkDetails = l10n.tr("lua_widget.system_monitor.offline")
        end
        if networkStatus and networkStatus.costKnown and
            networkStatus.metered then
            networkDetails = networkDetails and l10n.formatList({
                networkDetails,
                l10n.tr("lua_widget.system_monitor.metered"),
            }) or l10n.tr("lua_widget.system_monitor.metered")
        end
        local effectiveNetworkState = networkState or networkStatusState
        cards[#cards + 1] = {
            id = "network",
            title = l10n.tr("lua_widget.system_monitor.network"),
            lines = {
                rateLine("↓", network and network.connected and network.downloadBytesPerSecond or nil, palette.netDown),
                rateLine("↑", network and network.connected and network.uploadBytesPerSecond or nil, palette.netUp),
            },
            sub = detailsWithStatus(networkDetails,
                effectiveNetworkState),
        }
    end

    if showCard("battery") then
        local percent = power and clamp(power.batteryPercent) or nil
        local status = nil
        if power and power.charging then
            status = l10n.tr("lua_widget.system_monitor.charging")
        elseif power and power.acPower then
            status = l10n.tr("lua_widget.system_monitor.plugged_in")
        elseif percent and percent <= 20 then
            status = l10n.tr("lua_widget.system_monitor.low_battery")
        end
        if power and not power.acPower and
            (power.estimatedRemainingSeconds or 0) > 0 then
            local remaining = l10n.tr(
                "lua_widget.system_monitor.remaining",
                l10n.formatDuration(
                    power.estimatedRemainingSeconds * 1000,
                    { style = "short" }))
            status = status and l10n.formatList({ status, remaining }) or
                remaining
        end
        cards[#cards + 1] = {
            id = "battery",
            title = l10n.tr("lua_widget.system_monitor.battery"),
            value = percent and formatPercent(percent) or "—",
            progress = percent and percent / 100 or nil,
            color = usageColor(100 - (percent or 100), palette),
            sub = detailsWithStatus(status, powerState),
        }
    end

    if showCard("storage") then
        local percent = storageSummary and clamp(
            storageSummary.usedBytes / storageSummary.totalBytes * 100) or
            nil
        local details = storageSummary and
            (formatBytes(storageSummary.usedBytes) .. " / " ..
                formatBytes(storageSummary.totalBytes)) or nil
        if storageSummary and #storageSummary.names > 0 then
            details = l10n.formatList({
                details,
                l10n.formatList(storageSummary.names),
            })
        end
        cards[#cards + 1] = {
            id = "storage",
            title = l10n.tr("lua_widget.system_monitor.storage"),
            value = percent and formatPercent(percent) or "—",
            progress = percent and percent / 100 or nil,
            color = usageColor(percent or 0, palette),
            capacity = storageSummary and {
                used = formatBytes(storageSummary.usedBytes), total = formatBytes(storageSummary.totalBytes),
            } or nil,
            expandedSub = detailsWithStatus(storageSummary and l10n.formatList(storageSummary.names), storageState),
            expandedStatus = statusText(storageState),
            sub = detailsWithStatus(details, storageState),
        }
    end

    if showCard("disk_io") then
        local percent = diskIo and clamp(diskIo.busyPercent) or nil
        cards[#cards + 1] = {
            id = "disk_io",
            title = l10n.tr("lua_widget.system_monitor.disk_io"),
            lines = {
                rateLine("↓", diskIo and diskIo.readBytesPerSecond, palette.netDown),
                rateLine("↑", diskIo and diskIo.writeBytesPerSecond, palette.netUp),
            },
            progress = percent and percent / 100 or nil,
            color = usageColor(percent or 0, palette),
            sub = detailsWithStatus(percent and formatPercent(percent) or
                nil, diskIoState),
        }
    end

    if showCard("uptime") then
        local uptime = system.uptime()
        cards[#cards + 1] = {
            id = "uptime",
            title = l10n.tr("lua_widget.system_monitor.uptime"),
            value = formatUptime(uptime.milliseconds),
            wrapValue = true,
            timeParts = uptimeParts(uptime.milliseconds),
            color = palette.usageLow,
        }
    end
    return cards, palette
end

local function render(_context, model)
    mainScale = cardPreferences.fontScale(storage.get("main_font_scale"))
    local width = layout.contentWidth()
    local height = layout.contentHeight()
    local viewportHeight = math.max(1, height)
    local cards, palette = buildCards()
    local savedOrder = storage.get("card_order")
    cards = cardPreferences.arrange(cards, savedOrder)
    local columns = math.max(1, layout.columns())
    local visibleRows = math.max(1, layout.rows())
    local cells, rows, signature = cardLayout.pack(cards, columns,
        function(id) return storage.get("size_" .. id) end,
        cardPreferences.customOrder(savedOrder))
    -- A tall card crossing the viewport edge needs a continuous grid;
    -- otherwise preserve the existing clean break before an overflow row.
    local continuous = cardLayout.crossesViewport(cells, visibleRows)
    local inset = layout.cu(4)
    local horizontalGap = layout.cu(4)
    local verticalGap = layout.cu(4)
    local availableWidth = width - inset * 2
    local cardWidth = math.floor((availableWidth -
        horizontalGap * (columns - 1)) / columns)
    local cardHeight = cardLayout.cardHeight(rows, visibleRows,
        layout.cellHeight(), viewportHeight, verticalGap, inset)
    -- The trailing inset makes the native maximum scroll offset place the
    -- final row at the same inset from the viewport bottom.
    local contentHeight = cardLayout.contentHeight(
        rows, visibleRows, cardHeight, verticalGap, inset,
        viewportHeight, continuous)

    local layoutKey = signature .. ":" .. columns .. ":" .. visibleRows ..
        ":" .. width .. ":" .. height
    local resetScroll = layoutKey ~= model.previousLayout
    model.previousLayout = layoutKey
    local scroll = interaction.scroll({
        key = "system.cards",
        shape = {
            type = "rect",
            x = 0,
            y = 0,
            width = width,
            height = viewportHeight,
        },
        contentHeight = contentHeight,
    })
    if resetScroll then
        scroll.offset = interaction.setScrollOffset("system.cards", 0)
    end
    interaction.region({
        key = "system.surface",
        shape = { type = "rect", x = 0, y = 0,
            width = width, height = viewportHeight },
        events = { contextMenu = { id = "system.menu", scope = "component" } },
        accessibility = { role = "group",
            label = l10n.tr("lua_widget.system_monitor.name") },
    })
    draw.pushClip(0, 0, width, viewportHeight)
    if rows == 0 then
        local emptyText = l10n.tr(
            "lua_widget.system_monitor.no_visible_cards")
        local emptyFont = layout.fontCu(14)
        local emptyMetrics = draw.measureText(
            emptyText, emptyFont, 0, true)
        local emptyWidth = math.min(width, emptyMetrics.width)
        draw.text(math.max(0, (width - emptyWidth) / 2),
            math.max(0, (viewportHeight - emptyMetrics.height) / 2),
            emptyText, emptyFont, palette.cardSub,
            math.max(1, emptyWidth + layout.cu(1)), true, true,
            0, 0.78)
    else
        for index, card in ipairs(cards) do
            local cell = cells[index]
            local x = inset + cell.column * (cardWidth + horizontalGap)
            local y = cardLayout.rowTop(cell.row, visibleRows, cardHeight,
                verticalGap, inset, viewportHeight, continuous) - scroll.offset
            local w = cell.columns * cardWidth + (cell.columns - 1) * horizontalGap
            local h = cell.rows * cardHeight + (cell.rows - 1) * verticalGap
            if y + h > 0 and y < viewportHeight then
                drawCard(x, y, w, h,
                    card, palette, cell.columns, cell.rows)
                local top = math.max(0, y)
                interaction.region({
                    key = "system.card." .. card.id,
                    shape = { type = "rect", x = x, y = top, width = w,
                        height = math.min(viewportHeight, y + h) - top },
                    events = { contextMenu = { id = "system.menu",
                        scope = "component", value = { cardId = card.id } } },
                    accessibility = { role = "group", label = card.title },
                })
            end
        end
    end
    draw.popClip()

end

local function event(_context, _model, value)
    persistGpuSelection()
    if value.kind == "settings.changed" or value.kind == "environment" then
        reconcileSubscriptions()
        widget.invalidate()
        return
    end
    if value.kind ~= "action" then return end
    if value.id == "system.refresh" then
        widget.invalidate()
    elseif value.id == "system.order.reset" then
        storage.remove("card_order")
        widget.invalidate()
    elseif value.id and value.id:sub(1, 13) == "system.order." then
        local id, action = value.id:match("^system%.order%.([a-z_]+)%.([a-z]+)$")
        if id and cardTitles[id] and showCard(id) then
            local order = cardPreferences.move(storage.get("card_order"), id, action, showCard)
            if order then storage.set("card_order", order); widget.invalidate() end
        end
    elseif value.id and value.id:sub(1, 12) == "system.size." then
        local id, size = value.id:match("^system%.size%.([a-z_]+)%.([12]x[12])$")
        if id and cardTitles[id] and showCard(id) then
            storage.set("size_" .. id, size)
            widget.invalidate()
        end
    elseif value.id and value.id:sub(1, 18) == "system.gpu.select." then
        local id = value.id:sub(19)
        local gpuValue = subscriptionValue(subscriptions.gpu)
        local choice = monitorData.resolveGpuChoice(gpuValue, id)
        if choice then
            storage.set("gpu_adapter_id", choice.id)
            storage.set("gpu_adapter_name", choice.name)
            storage.set("gpu_scope", "selected")
            gpuSelection = { sourceId = choice.id, choice = choice }
            widget.invalidate()
        end
    elseif value.id == "system.resetStyle" then
        storage.set("bg", tostring(style.bg))
        storage.set("border", tostring(style.border))
        storage.set("alpha", tostring(style.alpha))
        storage.set("borderAlpha", tostring(style.borderAlpha))
        storage.set("gradientEndA", tostring(style.gradientEndA))
        storage.set("followPersonalization", "1")
    end
end

local function menu(_context, _model, request)
    if request.id ~= "system.menu" then return nil end
    persistGpuSelection()
    local items = {}
    local cardId = request.value and request.value.cardId
    if cardId and cardTitles[cardId] and showCard(cardId) then
        local selectedSize = cardLayout.size(storage.get("size_" .. cardId))
        local sizes = {}
        for _, size in ipairs(cardLayout.sizes) do
            sizes[#sizes + 1] = { id = "system.size." .. cardId .. "." .. size,
                label = size:gsub("x", " × "), checked = size == selectedSize }
        end
        local heading = l10n.tr("lua_widget.system_monitor.card_size", cardTitles[cardId])
        if widget.hasFeature("interaction.contextMenu.submenu") then
            items[#items + 1] = { label = heading, children = sizes }
        else
            items[#items + 1] = { id = "system.size.heading", label = heading, enabled = false }
            for _, item in ipairs(sizes) do items[#items + 1] = item end
        end
        items[#items + 1] = { type = "separator" }
        local saved = storage.get("card_order")
        local moves = {
            { "first", l10n.tr("lua_widget.system_monitor.order_first") },
            { "previous", l10n.tr("lua_widget.system_monitor.order_previous") },
            { "next", l10n.tr("lua_widget.system_monitor.order_next") },
            { "last", l10n.tr("lua_widget.system_monitor.order_last") },
        }
        for _, move in ipairs(moves) do
            items[#items + 1] = {
                id = "system.order." .. cardId .. "." .. move[1], label = move[2],
                enabled = cardPreferences.move(saved, cardId, move[1], showCard) ~= nil,
            }
        end
        items[#items + 1] = { type = "separator" }
    end
    items[#items + 1] = { id = "system.order.reset",
        label = l10n.tr("lua_widget.system_monitor.order_reset"),
        enabled = cardPreferences.customOrder(storage.get("card_order")) }
    items[#items + 1] = { type = "separator" }
    if showCard("gpu") or showCard("vram") then
        local gpuValue, gpuState = subscriptionValue(subscriptions.gpu)
        local selectedId, selectedName = selectedGpu(gpuValue)
        local choices = monitorData.gpuChoices(gpuValue)
        local gpuItems = {}
        local found = false
        for _, choice in ipairs(choices) do
            found = found or choice.id == selectedId
            gpuItems[#gpuItems + 1] = {
                id = "system.gpu.select." .. choice.id,
                label = choice.label,
                checked = choice.id == selectedId,
            }
        end
        if selectedId and selectedId ~= "" and not found then
            gpuItems[#gpuItems + 1] = {
                id = "system.gpu.unavailable",
                label = detailsWithStatus(selectedName or
                    l10n.tr("lua_widget.system_monitor.gpu_selected"),
                    gpuState or "notPresent"),
                enabled = false,
            }
        elseif #choices == 0 then
            gpuItems[#gpuItems + 1] = {
                id = "system.gpu.unavailable",
                label = statusText(gpuState or "notPresent"), enabled = false,
            }
        end
        if widget.hasFeature("interaction.contextMenu.submenu") then
            items[#items + 1] = {
                label = l10n.tr("lua_widget.system_monitor.gpu_choose"),
                children = gpuItems,
            }
        else
            -- Older hosts still expose selection as ordinary menu items.
            items[#items + 1] = {
                id = "system.gpu.heading",
                label = l10n.tr("lua_widget.system_monitor.gpu_choose"),
                enabled = false,
            }
            for _, item in ipairs(gpuItems) do items[#items + 1] = item end
        end
        items[#items + 1] = { type = "separator" }
    end
    items[#items + 1] =
        {
            id = "system.refresh",
            label = l10n.tr("lua_widget.system_monitor.refresh"),
            icon = fluent.refresh,
            iconFont = "fluent",
        }
    items[#items + 1] = { type = "separator" }
    items[#items + 1] =
        {
            id = "system.resetStyle",
            label = l10n.tr("lua_widget.common.reset_style"),
            icon = fluent.style,
            iconFont = "fluent",
        }
    return ui.menu(items)
end

local function dispose(_context, _model)
    persistGpuSelection()
    for _, handle in pairs(subscriptions) do
        handle:unsubscribe()
    end
    subscriptions = {}
    gpuSubscriptionDetails = nil
    gpuSelection = {}
end

return widget.define({
    name = l10n.tr("lua_widget.system_monitor.name"),
    useCustomStyle = true,
    followPersonalizationDefault = true,
    showTitle = false,
    bottomBarHover = true,
    bg = style.bg,
    border = style.border,
    alpha = style.alpha,
    borderAlpha = style.borderAlpha,
    gradientEndA = style.gradientEndA,
    settings = settings,
    setup = setup,
    render = render,
    event = event,
    menu = menu,
    dispose = dispose,
})
