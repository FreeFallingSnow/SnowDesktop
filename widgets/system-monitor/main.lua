-- system-monitor/main.lua - API v2 system data subscriptions
local subscriptions = {}

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
        { key = "show_cpu", label = l10n.tr("lua_widget.system_monitor.show_cpu"), type = "bool", default = true },
        { key = "show_memory", label = l10n.tr("lua_widget.system_monitor.show_memory"), type = "bool", default = true },
        { key = "show_gpu", label = l10n.tr("lua_widget.system_monitor.show_gpu"), type = "bool", default = true },
        { key = "show_vram", label = l10n.tr("lua_widget.system_monitor.show_vram"), type = "bool", default = true },
        { key = "show_network", label = l10n.tr("lua_widget.system_monitor.show_network"), type = "bool", default = true },
        { key = "show_battery", label = l10n.tr("lua_widget.system_monitor.show_battery"), type = "bool", default = true },
    },
}

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

local function formatRate(bytes)
    bytes = math.max(0, bytes or 0)
    if bytes >= 1024 * 1024 then
        return string.format("%.1f MB/s", bytes / 1024 / 1024)
    end
    if bytes >= 1024 then
        return string.format("%.0f KB/s", bytes / 1024)
    end
    return tostring(math.floor(bytes)) .. " B/s"
end

local function showCard(name)
    return storage.get("show_" .. name) ~= "0"
end

local function subscriptionValue(handle)
    if not handle then return nil end
    local snapshot = handle:value()
    if snapshot.available then return snapshot.value end
    return nil
end

local function summarizeGpu(value)
    if not value or not value.adapters or #value.adapters == 0 then
        return nil
    end
    local summary = {
        usagePercent = 0,
        dedicatedMemoryBytes = 0,
        dedicatedUsedBytes = 0,
        names = {},
    }
    for _, adapter in ipairs(value.adapters) do
        summary.usagePercent = math.max(summary.usagePercent,
            adapter.usagePercent or 0)
        summary.dedicatedMemoryBytes = summary.dedicatedMemoryBytes +
            (adapter.dedicatedMemoryBytes or 0)
        summary.dedicatedUsedBytes = summary.dedicatedUsedBytes +
            (adapter.dedicatedUsedBytes or 0)
        if adapter.name and adapter.name ~= "" then
            summary.names[#summary.names + 1] = adapter.name
        end
    end
    summary.name = table.concat(summary.names, " · ")
    return summary
end

local function splitWrap(model, text, fontSize, maxWidth)
    local cacheKey = text .. "\n" .. tostring(maxWidth) ..
        "\n" .. tostring(fontSize)
    local cached = model.wrappedLineCache[cacheKey]
    if cached then return cached end

    local lines = {}
    local line = ""
    for _, codepoint in utf8.codes(text) do
        local char = utf8.char(codepoint)
        local candidate = line .. char
        local metrics = draw.measureText(candidate, fontSize, 0, false)
        if line ~= "" and metrics.width > maxWidth then
            lines[#lines + 1] = line
            line = char
        else
            line = candidate
        end
    end
    if line ~= "" then lines[#lines + 1] = line end
    model.wrappedLineCache[cacheKey] = lines
    return lines
end

local function drawCard(model, x, y, width, height, info, palette)
    draw.rect(x, y, width, height, palette.cardBg,
        layout.cu(10), palette.cardBgA)
    draw.strokeRect(x, y, width, height, palette.cardBd,
        layout.cu(10), layout.cu(1.0), palette.cardBdA)

    local inset = layout.cu(8)
    local subFont = layout.fontCu(12)
    draw.text(x + inset, y + layout.cu(6), info.title, subFont,
        palette.cardSub, width - inset * 2, true, true)

    if info.lines then
        local lineY = y + height * 0.32
        local lineHeight = math.max(layout.cu(12),
            math.floor(height * 0.11))
        for _, line in ipairs(info.lines) do
            draw.text(x + inset, lineY, line.text, lineHeight,
                line.color or palette.cardText,
                width - inset * 2, false, true)
            lineY = lineY + lineHeight + layout.cu(2)
        end
    else
        local valueFont = math.max(layout.fontCu(15),
            math.min(layout.fontCu(24), math.floor(height * 0.18)))
        local metrics = draw.measureText(info.value, valueFont, 0, true)
        draw.text(x + (width - metrics.width) / 2,
            y + height * 0.42 - metrics.height / 2,
            info.value, valueFont, palette.cardText, 0, true)
    end

    local barY = nil
    if info.progress ~= nil then
        local barInset = layout.cu(8)
        local barHeight = layout.cu(4)
        barY = y + height - layout.cu(16)
        draw.rect(x + barInset, barY, width - barInset * 2,
            barHeight, palette.trackBg, layout.cu(2), 1.0)
        draw.rect(x + barInset, barY,
            (width - barInset * 2) * info.progress,
            barHeight, info.color, layout.cu(2), 1.0)
    end

    if info.sub then
        local subWidth = width - layout.cu(16)
        local subText = info.sub
        if info.rotateLines then
            local lines = splitWrap(model, info.sub, subFont, subWidth)
            if #lines > 1 then
                subText = lines[(model.subLineIndex % #lines) + 1]
            end
        end
        local metrics = draw.measureText(subText, subFont, subWidth, false)
        local subBottom = barY and (barY - layout.cu(4)) or
            (y + height - layout.cu(6))
        draw.text(x + layout.cu(8), subBottom - metrics.height,
            subText, subFont, palette.cardSub, subWidth,
            false, info.rotateLines == true)
    end
end

local function setup()
    subscriptions.cpu = data.subscribe("system.cpu", {
        maxAgeMs = 1000,
        whenHidden = "throttle",
    })
    subscriptions.memory = data.subscribe("system.memory", {
        maxAgeMs = 1000,
        whenHidden = "throttle",
    })
    subscriptions.gpu = data.subscribe("system.gpu", {
        maxAgeMs = 1000,
        whenHidden = "pause",
    })
    if widget.hasFeature("data.system.power") and
        widget.hasPermission("system.power.read") then
        subscriptions.power = data.subscribe("system.power", {
            maxAgeMs = 2000,
            whenHidden = "throttle",
        })
    end
    if widget.hasFeature("data.system.network.traffic") and
        widget.hasPermission("system.network.read") then
        subscriptions.network = data.subscribe("system.network.traffic", {
            maxAgeMs = 1000,
            whenHidden = "throttle",
        })
    end
    schedule.every("sub-line", 3000, { whenHidden = "pause" })
    return {
        previousColumns = 0,
        previousRows = 0,
        subLineIndex = 0,
        wrappedLineCache = {},
    }
end

local function buildCards()
    local palette = getPalette()
    local cpu = subscriptionValue(subscriptions.cpu)
    local memory = subscriptionValue(subscriptions.memory)
    local gpu = summarizeGpu(subscriptionValue(subscriptions.gpu))
    local power = subscriptionValue(subscriptions.power)
    local network = subscriptionValue(subscriptions.network)
    local cards = {}

    if showCard("cpu") then
        local percent = cpu and clamp(cpu.usagePercent) or nil
        cards[#cards + 1] = {
            title = "CPU",
            value = percent and string.format("%.0f%%", percent) or "—",
            progress = percent and percent / 100 or nil,
            color = usageColor(percent or 0, palette),
            sub = cpu and cpu.name ~= "" and cpu.name or
                (cpu and cpu.logicalProcessors and cpu.logicalProcessors > 0 and
                    l10n.tr("lua_widget.system_monitor.threads",
                        cpu.logicalProcessors) or nil),
            rotateLines = true,
        }
    end

    if showCard("memory") then
        local percent = memory and clamp(memory.usagePercent) or nil
        cards[#cards + 1] = {
            title = l10n.tr("lua_widget.system_monitor.memory"),
            value = percent and string.format("%.0f%%", percent) or "—",
            progress = percent and percent / 100 or nil,
            color = usageColor(percent or 0, palette),
            sub = memory and memory.totalBytes and memory.totalBytes > 0 and
                string.format("%.1f / %.1f GB",
                    memory.usedBytes / 1024 / 1024 / 1024,
                    memory.totalBytes / 1024 / 1024 / 1024) or nil,
        }
    end

    if showCard("gpu") and gpu then
        local percent = clamp(gpu.usagePercent)
        cards[#cards + 1] = {
            title = "GPU",
            value = string.format("%.0f%%", percent),
            progress = percent / 100,
            color = usageColor(percent, palette),
            sub = gpu.name ~= "" and gpu.name or nil,
            rotateLines = true,
        }
    end

    if showCard("vram") and gpu then
        local total = gpu.dedicatedMemoryBytes / 1024 / 1024 / 1024
        local used = gpu.dedicatedUsedBytes / 1024 / 1024 / 1024
        local percent = total > 0 and clamp(used / total * 100) or 0
        cards[#cards + 1] = {
            title = l10n.tr("lua_widget.system_monitor.vram"),
            value = string.format("%.0f%%", percent),
            progress = percent / 100,
            color = usageColor(percent, palette),
            sub = string.format("%.1f / %.1f GB", used, total),
        }
    end

    if showCard("network") and subscriptions.network then
        cards[#cards + 1] = {
            title = l10n.tr("lua_widget.system_monitor.network"),
            lines = {
                {
                    text = "↓ " .. (network and network.connected and
                        formatRate(network.downloadBytesPerSecond) or "—"),
                    color = palette.netDown,
                },
                {
                    text = "↑ " .. (network and network.connected and
                        formatRate(network.uploadBytesPerSecond) or "—"),
                    color = palette.netUp,
                },
            },
        }
    end

    if showCard("battery") and power then
        local percent = clamp(power.batteryPercent)
        local status = nil
        if power.charging then
            status = l10n.tr("lua_widget.system_monitor.charging")
        elseif power.acPower then
            status = l10n.tr("lua_widget.system_monitor.plugged_in")
        elseif percent <= 20 then
            status = l10n.tr("lua_widget.system_monitor.low_battery")
        end
        cards[#cards + 1] = {
            title = l10n.tr("lua_widget.system_monitor.battery"),
            value = string.format("%.0f%%", percent),
            progress = percent / 100,
            color = usageColor(100 - percent, palette),
            sub = status,
        }
    end
    return cards, palette
end

local function render(_context, model)
    local width = layout.width()
    local height = layout.height()
    local viewportHeight = math.max(1, height - layout.barHeight())
    local cards, palette = buildCards()
    local columns = math.max(1, layout.columns())
    local rows = #cards > 0 and math.ceil(#cards / columns) or 0

    local inset = layout.cu(4)
    local horizontalGap = layout.cu(4)
    local verticalGap = layout.cu(4)
    local availableWidth = width - inset * 2
    local cardWidth = math.floor((availableWidth -
        horizontalGap * (columns - 1)) / columns)
    local cardHeight = layout.cellHeight()
    if rows > 0 then
        local fillHeight = math.floor((viewportHeight - inset * 2 -
            verticalGap * (rows - 1)) / rows)
        if fillHeight > cardHeight then
            cardHeight = math.min(fillHeight,
                cardHeight + math.max(1, math.floor(cardHeight * 0.10)))
        end
    end
    local contentHeight = rows > 0 and math.ceil(
        inset + rows * cardHeight + (rows - 1) * verticalGap + inset) or
        viewportHeight

    local resetScroll = columns ~= model.previousColumns or
        rows ~= model.previousRows
    model.previousColumns = columns
    model.previousRows = rows
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

    draw.pushClip(0, 0, width, viewportHeight)
    if rows == 0 then
        draw.text(layout.cu(10), layout.cu(10),
            l10n.tr("lua_widget.system_monitor.no_visible_cards"),
            layout.fontCu(12), palette.cardSub)
    else
        for index, card in ipairs(cards) do
            local column = (index - 1) % columns
            local row = math.floor((index - 1) / columns)
            local x = inset + column * (cardWidth + horizontalGap)
            local y = inset + row * (cardHeight + verticalGap) -
                scroll.offset
            if y + cardHeight > 0 and y < viewportHeight then
                drawCard(model, x, y, cardWidth, cardHeight,
                    card, palette)
            end
        end
    end
    draw.popClip()

    interaction.region({
        key = "system.surface",
        shape = {
            type = "rect",
            x = 0,
            y = 0,
            width = width,
            height = viewportHeight,
        },
        events = {
            contextMenu = { id = "system.menu" },
        },
        accessibility = {
            role = "group",
            label = l10n.tr("lua_widget.system_monitor.name"),
        },
    })
end

local function event(_context, model, value)
    if value.kind == "schedule" and value.id == "sub-line" then
        model.subLineIndex = model.subLineIndex + 1
        return
    end
    if value.kind ~= "action" then return end
    if value.id == "system.refresh" then
        widget.invalidate()
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
    return ui.menu({
        {
            id = "system.refresh",
            label = l10n.tr("lua_widget.system_monitor.refresh"),
            icon = fluent.refresh,
            iconFont = "fluent",
        },
        { type = "separator" },
        {
            id = "system.resetStyle",
            label = l10n.tr("lua_widget.common.reset_style"),
            icon = fluent.style,
            iconFont = "fluent",
        },
    })
end

local function dispose()
    for _, handle in pairs(subscriptions) do
        handle:unsubscribe()
    end
    subscriptions = {}
end

return widget.define({
    name = l10n.tr("lua_widget.system_monitor.name"),
    useCustomStyle = true,
    followPersonalizationDefault = true,
    showTitle = true,
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
