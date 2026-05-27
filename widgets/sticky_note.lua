-- sticky_note.lua - 便签组件
name = "便签"
useCustomStyle = true

function render()
    local w = layout.width()
    local h = layout.height()
    local saved = storage.get("text") or ""

    draw.rect(0, 0, w, h, 0x2d2d1a, 8, 0.9)
    local text = widget.input("main", 8, 8, w - 16, h - 24, saved)

    local t = sys.getTime()
    draw.text(8, h - 16, string.format("便签 | %02d:%02d", t.hour, t.min), 10, 0x666644)
end

function onClick(x, y)
    local saved = storage.get("text") or ""
    if x >= 8 and x <= layout.width() - 8 and y >= 8 and y <= layout.height() - 24 then
        widget.focus("main", saved)
    else
        widget.focus("")
    end
end

function onEditCommit(text)
    storage.set("text", text)
    widget.focus("")
end
