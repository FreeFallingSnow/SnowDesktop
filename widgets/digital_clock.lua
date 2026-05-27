-- digital_clock.lua - 数字时钟
name = "数字时钟"
useCustomStyle = true

function render()
    local t = sys.getTime()
    local w = layout.width()
    local h = layout.height()
    local timeStr = string.format("%02d:%02d:%02d", t.hour, t.min, t.sec)
    local dateStr = string.format("%d年%02d月%02d日", t.year, t.month, t.day)

    -- 使用传入的边框颜色绘制分隔线
    local sepColor = math.floor(style.borderR * 255) * 0x10000
        + math.floor(style.borderG * 255) * 0x100
        + math.floor(style.borderB * 255)
    draw.line(8, h * 0.52, w - 8, h * 0.52, 1, sepColor, style.alpha * 0.5)

    -- 居中显示
    local tw = #timeStr * 14
    local dw = #dateStr * 7
    draw.text((w - tw) / 2, h * 0.12, timeStr, 28, 0xFFFFFF)
    draw.text((w - dw) / 2, h * 0.58, dateStr, 14, 0x888899)
end
