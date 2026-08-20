local history = module.require("modules/history.lua")
local subscriptions = {}

local metricOrder = { "cpu", "memory", "gpu", "disk", "network" }
local metricDefinitions = {
    cpu = {
        topic = "system.cpu",
        titleKey = "workshop.performance_history.cpu",
        color = 0x34D399,
        percent = true,
        read = function(value) return value and value.usagePercent end,
    },
    memory = {
        topic = "system.memory",
        titleKey = "workshop.performance_history.memory",
        color = 0x60A5FA,
        percent = true,
        read = function(value) return value and value.usagePercent end,
    },
    gpu = {
        topic = "system.gpu",
        titleKey = "workshop.performance_history.gpu",
        color = 0xC084FC,
        percent = true,
        read = function(value)
            if not value or not value.adapters or #value.adapters == 0 then
                return nil
            end
            local maximum = 0
            for _, adapter in ipairs(value.adapters) do
                maximum = math.max(maximum, adapter.usagePercent or 0)
            end
            return maximum
        end,
    },
    disk = {
        topic = "system.storage.io",
        permission = "system.storage.read",
        feature = "data.system.storage.io",
        titleKey = "workshop.performance_history.disk",
        color = 0xF59E0B,
        percent = true,
        read = function(value) return value and value.busyPercent end,
    },
    network = {
        topic = "system.network.traffic",
        permission = "system.network.read",
        feature = "data.system.network.traffic",
        titleKey = "workshop.performance_history.network",
        color = 0x22D3EE,
        percent = false,
        read = function(value)
            if not value or not value.connected then return nil end
            return math.max(0, value.downloadBytesPerSecond or 0)
        end,
    },
}

local settings = {
    groups = {
        {
            id = "history",
            label = l10n.tr("workshop.performance_history.settings_group"),
            description = l10n.tr(
                "workshop.performance_history.settings_group_help"),
            collapsible = true,
            defaultExpanded = true,
        },
    },
    fields = {
        {
            key = "history_length",
            label = l10n.tr("workshop.performance_history.history_length"),
            description = l10n.tr(
                "workshop.performance_history.history_length_help"),
            type = "range",
            min = 20,
            max = 120,
            step = 10,
            default = 60,
            group = "history",
        },
        {
            key = "metrics",
            label = l10n.tr("workshop.performance_history.metrics"),
            description = l10n.tr(
                "workshop.performance_history.metrics_help"),
            type = "multiSelect",
            options = metricOrder,
            optionLabels = {
                l10n.tr("workshop.performance_history.cpu"),
                l10n.tr("workshop.performance_history.memory"),
                l10n.tr("workshop.performance_history.gpu"),
                l10n.tr("workshop.performance_history.disk"),
                l10n.tr("workshop.performance_history.network"),
            },
            default = { "cpu", "memory", "gpu" },
            group = "history",
        },
    },
}

local function selectedMetrics()
    local stored = storage.get("metrics")
    if type(stored) == "string" then
        local parsed = {}
        for id in string.gmatch(stored, "[^,%s]+") do
            parsed[#parsed + 1] = id
        end
        stored = parsed
    elseif type(stored) ~= "table" then
        return { "cpu", "memory", "gpu" }
    end
    local result = {}
    local seen = {}
    for _, id in ipairs(stored) do
        if metricDefinitions[id] and not seen[id] then
            seen[id] = true
            result[#result + 1] = id
        end
    end
    return result
end

local function historyLength()
    return math.floor(history.clamp(storage.get("history_length") or 60,
        20, 120))
end

local function seriesKey(id) return "series." .. id end
local function statusKey(id) return "status." .. id end
local function latestKey(id) return "latest." .. id end

local function statusFromSnapshot(snapshot)
    if snapshot.available then
        return snapshot.stale and "stale" or "ready"
    end
    if snapshot.warmingUp then return "waiting" end
    if snapshot.error == "permissionDenied" then return "permission" end
    if snapshot.error == "notPresent" then return "notPresent" end
    return "unavailable"
end

local function captureMetric(id)
    local definition = metricDefinitions[id]
    local handle = subscriptions[id]
    if not handle then
        local status = definition.permission and
            not widget.hasPermission(definition.permission) and
            "permission" or "unavailable"
        state.set(statusKey(id), status)
        return
    end

    local snapshot = handle:value()
    local status = statusFromSnapshot(snapshot)
    state.set(statusKey(id), status)
    if not snapshot.available then return end

    local value = definition.read(snapshot.value)
    if value == nil then
        state.set(statusKey(id), "notPresent")
        return
    end
    if definition.percent then value = history.clamp(value, 0, 100) end
    local values = state.get(seriesKey(id), {})
    state.set(seriesKey(id), history.append(values, value, historyLength()))
    state.set(latestKey(id), value)
end

local function pruneState(selected)
    local keep = {}
    for _, id in ipairs(selected) do keep[id] = true end
    for _, key in ipairs(state.keys()) do
        local id = string.match(key, "^[^.]+%.(.+)$")
        if id and metricDefinitions[id] and not keep[id] then
            state.remove(key)
        end
    end
end

local function initializeState(context, selected)
    local seeds = {
        cpu = { 42, 18, 0 },
        memory = { 58, 7, 5 },
        gpu = { 35, 25, 10 },
        disk = { 26, 22, 16 },
        network = { 28, 20, 21 },
    }
    local length = math.min(historyLength(), 36)
    for _, id in ipairs(metricOrder) do
        if not state.has(seriesKey(id)) then
            local seed = seeds[id]
            local values = context.preview and history.seed(length,
                seed[1], seed[2], seed[3]) or { 0 }
            if id == "network" then
                for index = 1, #values do
                    values[index] = values[index] * 1024 * 64
                end
            end
            state.set(seriesKey(id), values)
            state.set(latestKey(id), history.latest(values, 0))
            state.set(statusKey(id), context.preview and "ready" or
                "waiting")
        end
    end
    pruneState(selected)
end

local function subscribeMetric(id)
    local definition = metricDefinitions[id]
    if definition.feature and not widget.hasFeature(definition.feature) then
        return
    end
    if definition.permission and
        not widget.hasPermission(definition.permission) then
        return
    end
    subscriptions[id] = data.subscribe(definition.topic, {
        maxAgeMs = id == "cpu" and 500 or 1000,
        whenHidden = (id == "gpu" or id == "disk") and "pause" or
            "throttle",
    })
end

local function setup(context)
    local selected = selectedMetrics()
    for _, id in ipairs(selected) do subscribeMetric(id) end
    state.clear()
    initializeState(context, selected)
    for _, id in ipairs(selected) do captureMetric(id) end
    schedule.every("performance.sample", 1000, {
        whenHidden = "pause",
    })
    return { selected = selected }
end

local function displayValue(id, value)
    if value == nil then return "—" end
    if metricDefinitions[id].percent then
        return l10n.formatNumber(value, {
            maximumFractionDigits = 0,
        }) .. "%"
    end
    return l10n.formatBytes(value, {
        base = 1024,
        maximumFractionDigits = 1,
    }) .. "/s"
end

local function statusText(status)
    local keys = {
        waiting = "workshop.performance_history.waiting",
        stale = "workshop.performance_history.stale",
        permission = "workshop.performance_history.permission",
        notPresent = "workshop.performance_history.not_present",
        unavailable = "workshop.performance_history.unavailable",
    }
    return l10n.tr(keys[status] or keys.unavailable)
end

local function metricCard(context, id)
    local definition = metricDefinitions[id]
    local values = state.get(seriesKey(id), { 0 })
    if #values == 0 then values = { 0 } end
    local status = state.get(statusKey(id), "waiting")
    local latest = state.get(latestKey(id), history.latest(values, 0))
    local badgeText = status == "ready" and displayValue(id, latest) or
        status == "stale" and l10n.tr(
            "workshop.performance_history.stale_value",
            displayValue(id, latest)) or statusText(status)
    local chartHeight = context.sizeClass == "small" and
        layout.cu(28) or layout.cu(34)
    local titleWidth = math.max(layout.cu(72),
        context.layoutSize.width * 0.46)
    local badgeWidth = math.max(layout.cu(58),
        context.layoutSize.width * 0.34)
    local chartOptions = {
        key = "metric.chart." .. id,
        values = values,
        height = chartHeight,
        thickness = layout.cu(1.5),
        fillOpacity = 0.16,
        accessibility = {
            role = "img",
            label = l10n.tr("workshop.performance_history.chart_label",
                l10n.tr(definition.titleKey)),
        },
        style = { foreground = definition.color },
    }
    if definition.percent then
        chartOptions.min = 0
        chartOptions.max = 100
    end
    if context.sizeClass ~= "small" then chartOptions.trackOpacity = 0.12 end
    local chart = context.sizeClass == "small" and
        view.sparkline(chartOptions) or view.lineChart(chartOptions)

    local header = view.row({
        key = "metric.header." .. id,
        width = "fill",
        height = layout.cu(20),
        alignItems = "center",
        justifyContent = "spaceBetween",
        children = {
            view.text({
                key = "metric.title." .. id,
                text = l10n.tr(definition.titleKey),
                width = titleWidth,
                height = layout.cu(20),
                fontSize = layout.fontCu(12),
                bold = true,
                style = { foreground = "textPrimary" },
            }),
            view.badge({
                key = "metric.value." .. id,
                text = badgeText,
                width = badgeWidth,
                height = layout.cu(18),
                padding = { horizontal = layout.cu(5) },
                fontSize = layout.fontCu(10),
                bold = true,
                textAlign = "center",
                style = {
                    background = "surfaceVariant",
                    foreground = status == "permission" and "warning" or
                        status == "unavailable" and "error" or
                        definition.color,
                    cornerRadius = layout.cu(9),
                },
            }),
        },
    })

    return view.column({
        key = "metric.card." .. id,
        width = "fill",
        height = context.sizeClass == "small" and layout.cu(58) or
            layout.cu(66),
        padding = { horizontal = layout.cu(7), vertical = layout.cu(4) },
        gap = layout.cu(2),
        events = {
            contextMenu = { id = "performance.menu", scope = "component" },
        },
        accessibility = {
            role = "group",
            label = l10n.tr(definition.titleKey),
        },
        style = {
            background = "surface",
            borderColor = "border",
            borderWidth = layout.cu(1),
            cornerRadius = layout.cu(8),
        },
        children = { header, chart },
    })
end

local function viewTree(context, model)
    local selected = model.selected
    if #selected == 0 then
        return view.box({
            key = "performance.empty",
            width = "fill",
            height = "fill",
            padding = layout.cu(12),
            events = {
                contextMenu = {
                    id = "performance.menu",
                    scope = "component",
                },
            },
            children = { view.text({
                key = "performance.empty.text",
                text = l10n.tr("workshop.performance_history.empty"),
                width = "fill",
                height = "fill",
                fontSize = layout.fontCu(12),
                textAlign = "center",
                textWrap = "wrap",
                style = { foreground = "textSecondary" },
            }) },
        })
    end

    local cards = {}
    for _, id in ipairs(selected) do
        cards[#cards + 1] = metricCard(context, id)
    end
    return view.scroll({
        key = "performance.scroll",
        width = "fill",
        height = "fill",
        showScrollbar = #cards > 3,
        children = { view.column({
            key = "performance.cards",
            width = "fill",
            padding = layout.cu(4),
            gap = layout.cu(4),
            children = cards,
        }) },
    })
end

local function event(context, model, value)
    if value.kind == "schedule" and value.id == "performance.sample" then
        for _, id in ipairs(model.selected) do captureMetric(id) end
        return
    end
    if value.kind == "data.change" then
        for _, id in ipairs(model.selected) do
            if metricDefinitions[id].topic == value.topic then
                captureMetric(id)
                return
            end
        end
        return
    end
    if value.kind == "environment" then
        widget.setTitle(l10n.tr("workshop.performance_history.name"))
        return
    end
    if value.kind == "action" and value.id == "performance.reset" then
        state.clear()
        initializeState(context, model.selected)
        for _, id in ipairs(model.selected) do captureMetric(id) end
    end
end

local function menu(_context, _model, request)
    if request.id ~= "performance.menu" then return nil end
    return ui.menu({
        {
            id = "performance.reset",
            label = l10n.tr("workshop.performance_history.reset"),
        },
    })
end

local function dispose()
    for _, handle in pairs(subscriptions) do handle:unsubscribe() end
    subscriptions = {}
end

return widget.define({
    name = l10n.tr("workshop.performance_history.name"),
    showTitle = true,
    bottomBarHover = true,
    settings = settings,
    setup = setup,
    view = viewTree,
    event = event,
    menu = menu,
    dispose = dispose,
})
