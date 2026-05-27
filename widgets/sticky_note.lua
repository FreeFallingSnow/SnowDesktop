-- sticky_note.lua - 便签组件
name = "便签"
useCustomStyle = true
showTitle = true

function render()
    local w = layout.width()
    local h = layout.height()

    local content = storage.get("text") or ""

    draw.rect(0, 0, w, h, 0x2d2d1a, 8, 0.9)

    if #content == 0 then
        draw.text(12, 12, "双击编辑便签...", 14, 0x888866)
    else
        local lines = {}
        for line in content:gmatch("[^\n]+") do
            lines[#lines + 1] = line
        end
        for i = 1, math.min(#lines, 12) do
            draw.text(12, 8 + (i-1) * 18, lines[i], 14, 0xDDDDCC)
        end
    end

    local t = sys.getTime()
    draw.text(8, h - 18, string.format("便签 | %02d:%02d", t.hour, t.min), 10, 0x666644)
end

function getEditText()
    return storage.get("text") or ""
end

function onEditCommit(text)
    storage.set("text", text)
end
