-- clock.lua - 桌面时钟
name = "时钟"

function render()
    local t = sys.getTime()
    local w = layout.width()
    local h = layout.height()
    local cx = w / 2
    local cy = h / 2
    local r = math.min(w, h) / 2 - 10

    -- 表盘背景
    draw.rect(0, 0, w, h, 0x1a1a2e, 12, 0.8)
    draw.rect(cx - 2, cy - 2, 5, 5, 0x4488ff, 3, 1)

    -- 刻度
    for i = 0, 11 do
        local a = i * math.pi / 6 - math.pi / 2
        local x1 = cx + math.cos(a) * (r - 6)
        local y1 = cy + math.sin(a) * (r - 6)
        local x2 = cx + math.cos(a) * (r - 2)
        local y2 = cy + math.sin(a) * (r - 2)
        if i % 3 == 0 then
            draw.line(x1, y1, x2, y2, 2.5, 0xFFFFFF, 0.9)
        else
            draw.line(x1, y1, x2, y2, 1, 0x666688, 0.6)
        end
    end

    -- 时针
    local ha = ((t.hour % 12) + t.min / 60) * math.pi / 6 - math.pi / 2
    local hx = cx + math.cos(ha) * r * 0.45
    local hy = cy + math.sin(ha) * r * 0.45
    draw.line(cx, cy, hx, hy, 3, 0xFFFFFF, 0.9)

    -- 分针
    local ma = (t.min + t.sec / 60) * math.pi / 30 - math.pi / 2
    local mx = cx + math.cos(ma) * r * 0.65
    local my = cy + math.sin(ma) * r * 0.65
    draw.line(cx, cy, mx, my, 2, 0xCCCCDD, 0.85)

    -- 秒针
    local sa = t.sec * math.pi / 30 - math.pi / 2
    local sx = cx + math.cos(sa) * r * 0.78
    local sy = cy + math.sin(sa) * r * 0.78
    draw.line(cx, cy, sx, sy, 1, 0x4488ff, 1)
end
