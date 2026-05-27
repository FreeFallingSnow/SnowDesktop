-- clock.lua - 桌面时钟
name = "时钟"

function render()
    local t = sys.getTime()
    local w = layout.width()
    local h = layout.height()
    local cx = w / 2
    local cy = h / 2
    local r = math.min(w, h) / 2 - 8

    -- 表盘背景
    draw.rect(0, 0, w, h, 0x1a1a2e, 12, 0.8)
    draw.rect(cx - 2, cy - 2, 4, 4, 0x4488ff, 2, 1)

    -- 刻度
    for i = 0, 11 do
        local angle = i * math.pi / 6 - math.pi / 2
        local x1 = cx + math.cos(angle) * (r - 4)
        local y1 = cy + math.sin(angle) * (r - 4)
        local x2 = cx + math.cos(angle) * (r - 10)
        local y2 = cy + math.sin(angle) * (r - 10)
        local ix = cx + math.cos(angle) * (r - 2)
        local iy = cy + math.sin(angle) * (r - 2)
        if i % 3 == 0 then
            draw.rect(ix - 2, iy - 2, 5, 5, 0xFFFFFF, 2, 0.8)
        else
            draw.rect(ix - 1, iy - 1, 3, 3, 0x666688, 1, 0.5)
        end
    end

    -- 时针
    local hourAngle = ((t.hour % 12) + t.min / 60) * math.pi / 6 - math.pi / 2
    drawHand(cx, cy, hourAngle, r * 0.45, 4, 0xFFFFFF)

    -- 分针
    local minAngle = (t.min + t.sec / 60) * math.pi / 30 - math.pi / 2
    drawHand(cx, cy, minAngle, r * 0.65, 3, 0xCCCCDD)

    -- 秒针
    local secAngle = t.sec * math.pi / 30 - math.pi / 2
    drawHand(cx, cy, secAngle, r * 0.75, 1, 0x4488ff)
end

function drawHand(x, y, angle, len, thick, color)
    -- 简陋线段模拟：画一系列小矩形
    local steps = 8
    for i = 1, steps do
        local t1 = (i - 1) / steps
        local t2 = i / steps
        local x1 = x + math.cos(angle) * len * t1 - thick / 2
        local y1 = y + math.sin(angle) * len * t1 - thick / 2
        local w = math.abs(math.cos(angle) * len / steps) + thick
        local h = math.abs(math.sin(angle) * len / steps) + thick
        draw.rect(x1, y1, w + 1, h + 1, color, thick / 2, 1)
    end
end
