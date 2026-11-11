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
    return l10n.tr("lua_widget.screen_time.duration_minutes", tostring(minutes))
end

local function setup()
    usage = data.subscribe("system.screenTime", { maxAgeMs = 120000 })
end

local function render()
    local data, available = payload()
    local width, height = layout.width(), layout.height()
    local text = 0xFFFFFF
    local theme = widget.theme()
    if theme and theme.contentTheme == 1 then text = 0x10211F end
    if not data or not available then
        draw.text(12, 12, l10n.tr("lua_widget.screen_time.waiting"), 13, text)
        return
    end
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
    local barHeight = math.max(18, math.min(42, height * 0.22))
    local gap = 2
    local barWidth = math.max(2, (width - 24 - gap * 23) / 24)
    for index = 1, 24 do
        local amount = tonumber(hours[index]) or 0
        local h = math.max(2, barHeight * amount / peak)
        draw.rect(12 + (index - 1) * (barWidth + gap), barTop + barHeight - h,
            barWidth, h, 0x168C88, 1)
    end
    y = barTop + barHeight + 8
    local apps = data.apps or {}
    if #apps == 0 then
        draw.text(12, y, l10n.tr("lua_widget.screen_time.empty"), 12, text, width - 24, true, false, 0, 0.72)
    else
        local limit = math.min(#apps, 5)
        for index = 1, limit do
            if y > height - 16 then break end
            local app = apps[index]
            draw.text(12, y, tostring(app.name or ""), 13, text, width * 0.62, true)
            draw.text(width * 0.64, y, duration(app.seconds), 13, text, width * 0.32, true)
            y = y + 18
        end
    end
    interaction.region({
        key = "screenTime.open",
        shape = { type = "rect", x = 0, y = 0, width = width, height = height },
        cursor = "hand",
        events = { click = { id = "screenTime.open" } },
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
})
