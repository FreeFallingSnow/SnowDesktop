-- sticky_note.lua - 便签组件
name = "便签"
useCustomStyle = true
showTitle = true

function render()
    local w = layout.width()
    local h = layout.height()

    draw.rect(0, 0, w, h, 0x2d2d1a, 8, 0.9)

    -- 可编辑输入框
    local text = widget.input("main", 8, 8, w - 16, h - 36, storage.get("text") or "")

    -- 时间戳
    local t = sys.getTime()
    draw.text(8, h - 18, string.format("便签 | %02d:%02d", t.hour, t.min), 10, 0x666644)

    return text
end

function onClick(x, y)
    -- 点击输入框区域即激活
    if x >= 8 and x <= layout.width() - 8 and y >= 8 and y <= layout.height() - 36 then
        widget.focus("main")
    else
        widget.focus("")
    end
end

function onEditCommit(text)
    storage.set("text", text)
end
