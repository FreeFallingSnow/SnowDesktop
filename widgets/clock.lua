-- clock.lua - 桌面时钟
name = "时钟"

function render()
    local t = sys.getTime()
    local timeStr = string.format("%02d:%02d", t.hour, t.min)
    local dateStr = string.format("%d/%02d/%02d", t.year, t.month, t.day)
    local w = layout.width()
    local h = layout.height()

    -- 背景
    draw.rect(0, 0, w, h, 0x1a1a2e, 12, 0.7)

    -- 时间
    local timeW = #timeStr * 18  -- approximate
    draw.text((w - timeW) / 2, h * 0.15, timeStr, 36, 0xFFFFFF)

    -- 日期
    local dateW = #dateStr * 7
    draw.text((w - dateW) / 2, h * 0.55, dateStr, 14, 0x888899)

    -- 秒指示器
    local sec = t.sec
    local dotX = w / 2 + math.cos(sec / 60 * math.pi * 2) * 24
    local dotY = h * 0.35 + math.sin(sec / 60 * math.pi * 2) * 12
    draw.rect(dotX - 2, dotY - 2, 4, 4, 0x4488ff, 2, 1)
end
