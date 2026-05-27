-- sticky_note.lua - 便签组件
name = "便签"
useCustomStyle = true
showTitle = true

function render()
    local w = layout.width()
    local h = layout.height()

    -- 读取保存的内容
    local content = storage.get("text")
    if content == nil then content = "" end

    -- 背景
    draw.rect(0, 0, w, h, 0x2d2d1a, 8, 0.9)

    -- 文字（简单显示，不支持编辑）
    local lines = {}
    for line in content:gmatch("[^\n]+") do
        lines[#lines + 1] = line
    end
    if #lines == 0 then
        draw.text(12, 12, "双击编辑便签...", 14, 0x888866)
    else
        for i = 1, math.min(#lines, 12) do
            draw.text(12, 8 + (i-1) * 18, lines[i], 14, 0xDDDDCC)
        end
    end

    -- 时间戳
    local t = sys.getTime()
    local info = string.format("便签 | %02d:%02d", t.hour, t.min)
    draw.text(8, h - 18, info, 10, 0x666644)
end

-- 保存内容的辅助（需要在组件内编辑，目前不支持）
function setContent(text)
    storage.set("text", text)
end
