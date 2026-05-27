-- clock.lua - 桌面时钟小组件
name = "桌面时钟"

function render()
    local t = sys.getTime()
    local timeStr = string.format("%02d:%02d:%02d", t.hour, t.min, t.sec)
    local dateStr = string.format("%d-%02d-%02d", t.year, t.month, t.day)

    -- 阴影
    draw.text(11, 11, timeStr, 32, 0x000000)
    draw.text(11, 45, dateStr, 14, 0x000000)

    -- 前景
    draw.text(10, 10, timeStr, 32, 0xFFFFFF)
    draw.text(10, 44, dateStr, 14, 0xAAAAAA)
end
