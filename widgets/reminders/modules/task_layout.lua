local taskLayout = {}

function taskLayout.build(tasks, measure, width, fontSize, rowHeight,
    padding, gap, untitled)
    local rows = {}
    local top = 0
    for _, task in ipairs(tasks) do
        local text = task.text ~= "" and task.text or untitled
        -- Keep the final empty line available while editing after Enter.
        local measuredText = text:match("[\r\n]$") and text .. " " or text
        local measured = measure(measuredText, fontSize, width, false)
        local height = math.max(rowHeight, math.ceil(measured.height + padding * 2))
        rows[#rows + 1] = {
            task = task, text = text, measured = measured,
            top = top, height = height,
        }
        top = top + height + gap
    end
    return rows, math.max(0, top - gap)
end

function taskLayout.visibleRange(rows, offset, viewportHeight)
    local first = 1
    while first <= #rows and rows[first].top + rows[first].height <= offset do
        first = first + 1
    end
    local last = first - 1
    while last < #rows and rows[last + 1].top < offset + viewportHeight do
        last = last + 1
    end
    return first, last
end

return taskLayout
