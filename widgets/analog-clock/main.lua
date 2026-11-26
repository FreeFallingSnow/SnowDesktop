-- analog_clock.lua - API v2 指针时钟
local settings = {
    presets = {
        {
            id = "transparent",
            label = l10n.tr("lua_widget.analog_clock.preset_transparent"),
            default = true,
            values = {
                bg = 0x000000,
                border = 0x000000,
                alpha = 0,
                borderAlpha = 0,
                gradientEndA = 0,
            }
        }
    },
    fields = {
        {
            key = "faceTheme",
            agentDescription = "Light and Dark draw a solid clock face. Theme uses the component's current appearance as the dial background and matches its foreground theme. Choosing Theme switches the Transparent Face appearance preset to Follow Global, including an instance still using that default preset. Other appearance choices stay selected.",
            label = l10n.tr("lua_widget.analog_clock.face_theme"),
            type = "select",
            default = "light",
            options = { "light", "dark", "theme" },
            optionLabels = {
                l10n.tr("lua_widget.analog_clock.theme_light"),
                l10n.tr("lua_widget.analog_clock.theme_dark"),
                l10n.tr("lua_widget.analog_clock.theme_host"),
            },
        },
        { key = "showSecondHand",
            agentDescription = "Draws the red seconds hand and red center accent. The hour and minute hands stay visible.",
            label = l10n.tr("lua_widget.analog_clock.show_second_hand"), type = "bool", default = true
        },
        { key = "showNumbers",
            agentDescription = "Draws the hour numerals around the clock face.",
            label = l10n.tr("lua_widget.analog_clock.show_numbers"), type = "bool", default = true
        },
    }
}

local palettes = {
    dark = {
        shadow = 0x000000,
        outer = 0x334155,
        face = 0x111827,
        majorTick = 0xF8FAFC,
        minorTick = 0x64748B,
        number = 0xF8FAFC,
        handShadow = 0x000000,
        hourHand = 0xF8FAFC,
        minuteHand = 0xCBD5E1,
        capOuter = 0x0F172A,
        capInner = 0xF8FAFC,
    },
    light = {
        shadow = 0x000000,
        outer = 0xD7DEE8,
        face = 0xFFFFFF,
        majorTick = 0x1F2937,
        minorTick = 0xAEB7C5,
        number = 0x1F2937,
        handShadow = 0xFFFFFF,
        hourHand = 0x111827,
        minuteHand = 0x374151,
        capOuter = 0xFFFFFF,
        capInner = 0x111827,
    },
}

local function palette(faceTheme)
    if faceTheme == "theme" then
        return widget.theme().contentTheme == 1 and palettes.light or palettes.dark
    end
    return palettes[faceTheme] or palettes.light
end

local function setup()
    schedule.every("clock", 1000, { whenHidden = "pause" })
end

local function event(_context, _model, change)
    if change.kind ~= "settings.changed" or change.preview or
        storage.get("faceTheme") ~= "theme" then
        return
    end
    for _, key in ipairs(change.keys) do
        if key == "faceTheme" then
            local follow = storage.get("followPersonalization")
            local preset = storage.get("__preset")
            if follow ~= "1" and follow ~= "true" and
                (preset == nil or preset == "transparent") then
                storage.set("followPersonalization", "1")
            end
            return
        end
    end
end

local function render()
    local t = time.parts(time.now())
    local faceTheme = storage.get("faceTheme")
    local themedDial = faceTheme == "theme"
    local colors = palette(faceTheme)
    local showSecondHand = storage.get("showSecondHand") ~= "0"
    local showNumbers = storage.get("showNumbers") ~= "0"
    local w = layout.width()
    local h = layout.height()
    local cx = w / 2
    local cy = h / 2
    local size = math.min(w, h)
    local unit = size / 96
    local r = size / 2 - unit * (themedDial and 7 or 10)
    if r <= 0 then return end

    local function su(value)
        return unit * value
    end

    local outerStroke = su(1.4)
    local hourTickLen = su(7)
    local minuteTickLen = su(3.2)
    local hourTickWidth = su(1.35)
    local quarterTickWidth = su(1.8)
    local minuteTickWidth = su(0.55)
    local hourHandWidth = su(2.0)
    local minuteHandWidth = su(1.25)
    local secondHandWidth = su(0.45)

    local function point(angle, radius)
        return cx + math.cos(angle) * radius, cy + math.sin(angle) * radius
    end

    local function hand(angle, front, back, width, color, alpha)
        local x1, y1 = point(angle + math.pi, back)
        local x2, y2 = point(angle, front)
        draw.line(x1, y1, x2, y2, width, color, alpha or 1.0)
    end

    if not themedDial then
        draw.circle(cx, cy + su(1.5), r + outerStroke + su(1.2),
            colors.shadow, 0.10)
        draw.circle(cx, cy, r + outerStroke, colors.outer, 0.95)
        draw.circle(cx, cy, r, colors.face, 1.0)
    end

    -- 刻度：主刻度更稳，副刻度更轻。
    for i = 0, 59 do
        local a = i * math.pi / 30 - math.pi / 2
        local major = i % 5 == 0
        local quarter = i % 15 == 0
        local len = major and hourTickLen or minuteTickLen
        local thick = quarter and quarterTickWidth or (major and hourTickWidth or minuteTickWidth)
        local col = major and colors.majorTick or colors.minorTick
        local alphaTick = major and 0.86 or 0.46
        local x1, y1 = point(a, r - len)
        local x2, y2 = point(a, r - su(2))
        draw.line(x1, y1, x2, y2, thick, col, alphaTick)
    end

    if showNumbers then
        local numberFont = su(6.8)
        local numberGap = su(0.65)
        local preferredRadius = r - su(10.5)
        for hour = 1, 12 do
            local a = hour * math.pi / 6 - math.pi / 2
            local label = tostring(hour)
            local metrics = draw.measureText(label, numberFont, 0, true)
            local ink = metrics.ink or metrics
            local radialExtent = math.abs(math.cos(a)) * ink.width / 2 +
                math.abs(math.sin(a)) * ink.height / 2
            local safeRadius = r - hourTickLen - radialExtent - numberGap
            local numberRadius = math.min(preferredRadius, safeRadius)
            local tx, ty = point(a, numberRadius)
            draw.text(tx, ty, label, numberFont, colors.number, 0, true, true,
                nil, nil, nil, { x = "center", y = "center" })
        end
    end

    local ha = ((t.hour % 12) + t.min / 60) * math.pi / 6 - math.pi / 2
    local ma = (t.min + t.sec / 60) * math.pi / 30 - math.pi / 2

    -- 指针和中心帽随表盘短边线性缩放。
    hand(ha, r * 0.45, su(7), hourHandWidth + su(0.5),
        colors.handShadow, 0.45)
    hand(ma, r * 0.65, su(8), minuteHandWidth + su(0.4),
        colors.handShadow, 0.35)
    hand(ha, r * 0.43, su(6), hourHandWidth, colors.hourHand, 0.96)
    hand(ma, r * 0.63, su(7), minuteHandWidth, colors.minuteHand, 0.96)
    if showSecondHand then
        local sa = t.sec * math.pi / 30 - math.pi / 2
        hand(sa, r * 0.76, su(13), secondHandWidth, 0xEF4444, 0.96)
    end

    draw.circle(cx, cy, su(4.6), colors.capOuter, 1.0)
    draw.circle(cx, cy, su(3.3), colors.capInner, 0.98)
    if showSecondHand then
        draw.circle(cx, cy, su(1.4), 0xEF4444, 1.0)
    end
end

local function circularOutline()
    local center, radius = 0.5, 0.46
    local low, high = center - radius, center + radius
    local arm = radius * 0.5522847498
    local near, far = center - arm, center + arm
    return {
        fit = "contain",
        when = { setting = "faceTheme", value = "theme" },
        path = {
            { op = "move", x = center, y = low },
            { op = "cubic", x1 = far, y1 = low, x2 = high, y2 = near, x = high, y = center },
            { op = "cubic", x1 = high, y1 = far, x2 = far, y2 = high, x = center, y = high },
            { op = "cubic", x1 = near, y1 = high, x2 = low, y2 = far, x = low, y = center },
            { op = "cubic", x1 = low, y1 = near, x2 = near, y2 = low, x = center, y = low },
            { op = "close" },
        },
    }
end

return widget.define({
    useCustomStyle = true,
    outline = widget.hasFeature("widget.outline.condition") and circularOutline() or nil,
    bg = 0x000000,
    border = 0x000000,
    alpha = 0,
    gradientEndA = 0,
    settings = settings,
    setup = setup,
    event = event,
    render = render,
})
