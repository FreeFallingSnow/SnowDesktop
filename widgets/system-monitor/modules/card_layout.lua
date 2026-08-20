local cardLayout = {}

function cardLayout.contentHeight(rows, cardHeight, gap, inset,
        viewportHeight)
    rows = math.max(0, math.floor(rows or 0))
    viewportHeight = math.max(1, math.ceil(viewportHeight or 1))
    if rows == 0 then return viewportHeight end
    local measured = math.ceil(inset + rows * cardHeight +
        (rows - 1) * gap + inset)
    return math.max(viewportHeight, measured)
end

function cardLayout.maximumOffset(contentHeight, viewportHeight)
    return math.max(0, contentHeight - viewportHeight)
end

return cardLayout
