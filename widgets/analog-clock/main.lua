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
            label = l10n.tr("lua_widget.analog_clock.face_theme"),
            type = "select",
            default = "light",
            options = { "light", "dark" },
            optionLabels = {
                l10n.tr("lua_widget.analog_clock.theme_light"),
                l10n.tr("lua_widget.analog_clock.theme_dark"),
            },
        },
        { key = "showSecondHand", label = l10n.tr("lua_widget.analog_clock.show_second_hand"), type = "bool", default = true },
        { key = "showNumbers", label = l10n.tr("lua_widget.analog_clock.show_numbers"), type = "bool", default = true },
    }
}

local palettes = {
    dark = {
        shadow = 0x000000,
        outer = 0x334155,
        face = 0x111827,
        innerStroke = 0x1F2937,
        innerFace = 0x0F172A,
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
        innerStroke = 0xF6F8FB,
        innerFace = 0xFFFFFF,
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

local function palette()
    local faceTheme = storage.get("faceTheme")
    return faceTheme == "dark" and palettes.dark or palettes.light
end

local function setup()
    schedule.every("clock", 1000, { whenHidden = "pause" })
end

local function render()
    local t = time.parts(time.now())
    local colors = palette()
    local showSecondHand = storage.get("showSecondHand") ~= "0"
    local showNumbers = storage.get("showNumbers") ~= "0"
    local w = layout.width()
    local h = layout.height()
    local cx = w / 2
    local cy = h / 2
    local size = math.min(w, h)
    local unit = size / 96
    local r = size / 2 - unit * 10
    if r <= 0 then return end

    local function su(value)
        return unit * value
    end

    local outerStroke = su(1.4)
    local innerStroke = su(0.8)
    local hourTickLen = su(9)
    local minuteTickLen = su(4)
    local hourTickWidth = su(1.6)
    local quarterTickWidth = su(2.2)
    local minuteTickWidth = su(0.75)
    local hourHandWidth = su(4.0)
    local minuteHandWidth = su(2.8)
    local secondHandWidth = su(1.1)

    local function point(angle, radius)
        return cx + math.cos(angle) * radius, cy + math.sin(angle) * radius
    end

    local function hand(angle, front, back, width, color, alpha)
        local x1, y1 = point(angle + math.pi, back)
        local x2, y2 = point(angle, front)
        draw.line(x1, y1, x2, y2, width, color, alpha or 1.0)
    end

    -- 多层表盘，用填充圆模拟描边，避免依赖额外 stroke API。
    draw.circle(cx, cy + su(1.5), r + outerStroke + su(1.2),
        colors.shadow, 0.10)
    draw.circle(cx, cy, r + outerStroke, colors.outer, 0.95)
    draw.circle(cx, cy, r, colors.face, 1.0)
    local innerR = r - su(5) - innerStroke
    draw.circle(cx, cy, innerR + innerStroke, colors.innerStroke, 0.72)
    draw.circle(cx, cy, innerR, colors.innerFace, 1.0)

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
        local numberFont = su(10.5)
        local numberRadius = r - su(16)
        for hour = 1, 12 do
            local a = hour * math.pi / 6 - math.pi / 2
            local label = tostring(hour)
            local metrics = draw.measureText(label, numberFont, 0, true)
            local tx, ty = point(a, numberRadius)
            draw.text(tx - metrics.width / 2, ty - metrics.height / 2,
                label, numberFont, colors.number, 0, true, true)
        end
    end

    local ha = ((t.hour % 12) + t.min / 60) * math.pi / 6 - math.pi / 2
    local ma = (t.min + t.sec / 60) * math.pi / 30 - math.pi / 2

    -- 指针和中心帽随表盘短边线性缩放。
    hand(ha, r * 0.45, su(7), hourHandWidth + su(1.0),
        colors.handShadow, 0.45)
    hand(ma, r * 0.65, su(8), minuteHandWidth + su(0.8),
        colors.handShadow, 0.35)
    hand(ha, r * 0.43, su(6), hourHandWidth, colors.hourHand, 0.96)
    hand(ma, r * 0.63, su(7), minuteHandWidth, colors.minuteHand, 0.96)
    if showSecondHand then
        local sa = t.sec * math.pi / 30 - math.pi / 2
        hand(sa, r * 0.76, su(13), secondHandWidth, 0xEF4444, 0.96)
    end

    draw.circle(cx, cy, su(6.4), colors.capOuter, 1.0)
    draw.circle(cx, cy, su(4.8), colors.capInner, 0.98)
    if showSecondHand then
        draw.circle(cx, cy, su(2.1), 0xEF4444, 1.0)
    end
end

return widget.define({
    useCustomStyle = true,
    bg = 0x000000,
    border = 0x000000,
    alpha = 0,
    gradientEndA = 0,
    settings = settings,
    setup = setup,
    render = render,
})
