-- screen-time/main.lua - today's foreground app usage
local usage

local function payload()
    if not usage then return nil end
    local snapshot = usage:value()
    if not snapshot then return nil end
    if snapshot.value and snapshot.value.totalSeconds ~= nil then
        return snapshot.value, snapshot.available
    end
    return snapshot, snapshot.available
end

local function duration(seconds)
    seconds = math.max(0, math.floor(tonumber(seconds) or 0))
    local hours = math.floor(seconds / 3600)
    local minutes = math.floor((seconds % 3600) / 60)
    if hours > 0 then
        return l10n.tr("lua_widget.screen_time.duration",
            tostring(hours), tostring(minutes))
    end
    if seconds < 60 then
        return l10n.tr("lua_widget.screen_time.duration_seconds", tostring(seconds))
    end
    return l10n.tr("lua_widget.screen_time.duration_minutes", tostring(minutes))
end

local function setup()
    usage = data.subscribe("system.screenTime", { maxAgeMs = 120000 })
end

local function drawReport(data, width, height, text, theme)
    draw.text(12, 8, duration(data.totalSeconds), 22, text)
    local y = 36
    if data.enabled == false then
        draw.text(12, y, l10n.tr("lua_widget.screen_time.paused"), 12, text, width - 24, true, false, 0, 0.72)
        y = y + 18
    end
    local hours = data.hours or {}
    local peak = 1
    for index = 1, 24 do
        peak = math.max(peak, tonumber(hours[index]) or 0)
    end
    local barTop = y
    local barHeight = math.max(16, math.min(36, height * 0.18))
    local gap = 3
    local barWidth = math.max(2, (width - 24 - gap * 23) / 24)
    local accent = theme and theme.contentTheme == 1 and 0x0E6E6A or 0x7ED0CC
    for index = 1, 24 do
        local amount = tonumber(hours[index]) or 0
        if amount > 0 then
            local h = math.max(3, barHeight * amount / peak)
            draw.rect(12 + (index - 1) * (barWidth + gap), barTop + barHeight - h,
                barWidth, h, accent, 2)
        end
    end
    y = barTop + barHeight + 10
    local apps = data.apps or {}
    if #apps == 0 then
        draw.text(12, y, l10n.tr("lua_widget.screen_time.empty"), 12, text, width - 24, true, false, 0, 0.72)
    else
        local appPeak = 1
        local limit = math.min(#apps, 4)
        for index = 1, limit do
            appPeak = math.max(appPeak, tonumber(apps[index].seconds) or 0)
        end
        for index = 1, limit do
            if y > height - 18 then break end
            local app = apps[index]
            local seconds = tonumber(app.seconds) or 0
            local nameWidth = width * 0.46
            draw.text(12, y, tostring(app.name or ""), 13, text, nameWidth, true)
            local barX = 16 + nameWidth
            local barMax = math.max(24, width - barX - width * 0.28)
            if seconds > 0 then
                draw.rect(barX, y + 6, math.max(4, barMax * seconds / appPeak), 4, accent, 2)
            end
            draw.text(width * 0.74, y, duration(seconds), 13, text, width * 0.24, true)
            y = y + 20
        end
    end
end

local function render()
    local data, available = payload()
    local width, height = layout.width(), layout.height()
    local text = 0xFFFFFF
    local theme = widget.theme()
    if theme and theme.contentTheme == 1 then text = 0x10211F end
    if not data or not available then
        draw.text(12, 12, l10n.tr("lua_widget.screen_time.waiting"), 13, text)
    else
        drawReport(data, width, height, text, theme)
    end
    interaction.region({
        key = "screenTime.open",
        shape = { type = "rect", x = 0, y = 0, width = width, height = height },
        events = {
            doubleClick = { id = "screenTime.open" },
            contextMenu = { id = "screenTime.menu", scope = "component" },
        },
    })
end

local function menu(_, _, request)
    if request.id ~= "screenTime.menu" then return nil end
    return ui.menu({
        {
            id = "screenTime.open",
            label = l10n.tr("lua_widget.screen_time.open_report"),
        },
    })
end

local function event(_, _, value)
    if value.kind ~= "action" or value.id ~= "screenTime.open" then return end
    if widget.hasFeature("task.system.openSettings") then
        task.start("system.openSettings", { page = "screenTime" })
    end
end

return widget.define({
    name = l10n.tr("lua_widget.screen_time.name"),
    useCustomStyle = true,
    followPersonalizationDefault = true,
    bg = 0x10211F,
    border = 0xFFFFFF,
    alpha = 0.42,
    gradientEndA = 0.28,
    setup = setup,
    render = render,
    event = event,
    menu = menu,
})
