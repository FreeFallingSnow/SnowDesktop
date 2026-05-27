-- analog_clock.lua - 指针时钟
name = "指针时钟"
-- 背景由个性化设置提供

function render()
    local t = sys.getTime()
    local w = layout.width()
    local h = layout.height()
    local cx = w / 2
    local cy = h / 2
    local r = math.min(w, h) / 2 - 10

    -- 中心点
    draw.rect(cx - 2, cy - 2, 5, 5, 0x4488ff, 3, 1)

    -- 刻度
    for i = 0, 11 do
        local a = i * math.pi / 6 - math.pi / 2
        if i % 3 == 0 then
            local x1 = cx + math.cos(a) * (r - 7)
            local y1 = cy + math.sin(a) * (r - 7)
            local x2 = cx + math.cos(a) * r
            local y2 = cy + math.sin(a) * r
            draw.line(x1, y1, x2, y2, 2.5, 0xFFFFFF, 0.9)
        else
            local x1 = cx + math.cos(a) * (r - 4)
            local y1 = cy + math.sin(a) * (r - 4)
            local x2 = cx + math.cos(a) * r
            local y2 = cy + math.sin(a) * r
            draw.line(x1, y1, x2, y2, 1, 0x888899, 0.6)
        end
    end

    -- 时针
    local ha = ((t.hour % 12) + t.min / 60) * math.pi / 6 - math.pi / 2
    draw.line(cx, cy, cx + math.cos(ha) * r * 0.45, cy + math.sin(ha) * r * 0.45, 3, 0xFFFFFF, 0.9)

    -- 分针
    local ma = (t.min + t.sec / 60) * math.pi / 30 - math.pi / 2
    draw.line(cx, cy, cx + math.cos(ma) * r * 0.65, cy + math.sin(ma) * r * 0.65, 2, 0xCCCCDD, 0.85)

    -- 秒针
    local sa = t.sec * math.pi / 30 - math.pi / 2
    draw.line(cx, cy, cx + math.cos(sa) * r * 0.78, cy + math.sin(sa) * r * 0.78, 1, 0x4488ff, 1)
end
