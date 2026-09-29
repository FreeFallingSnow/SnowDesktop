local cardLayout = {}

cardLayout.sizes = { "1x1", "1x2", "2x1", "2x2" }

function cardLayout.size(value)
    for _, size in ipairs(cardLayout.sizes) do
        if value == size then return size end
    end
    return "1x1"
end

-- Stable first-fit packing fills holes beside tall/wide cards without
-- changing the user's saved size when the containing widget gets narrower.
function cardLayout.pack(cards, columns, sizeFor)
    columns = math.max(1, math.floor(columns or 1))
    local occupied, placements, signature = {}, {}, {}
    local rows = 0
    local function fits(row, column, width, height)
        for y = row, row + height - 1 do
            for x = column, column + width - 1 do
                if occupied[y] and occupied[y][x] then return false end
            end
        end
        return true
    end
    for _, card in ipairs(cards) do
        local size = cardLayout.size(sizeFor(card.id))
        local width = math.min(columns, tonumber(size:sub(1, 1)))
        local height = tonumber(size:sub(3, 3))
        local row, column = 0, 0
        while not fits(row, column, width, height) do
            column = column + 1
            if column + width > columns then row, column = row + 1, 0 end
        end
        for y = row, row + height - 1 do
            occupied[y] = occupied[y] or {}
            for x = column, column + width - 1 do occupied[y][x] = true end
        end
        placements[#placements + 1] = {
            column = column, row = row, columns = width, rows = height,
        }
        rows = math.max(rows, row + height)
        signature[#signature + 1] = card.id .. "=" .. size
    end
    return placements, rows, table.concat(signature, ";")
end

function cardLayout.crossesViewport(placements, visibleRows)
    for _, cell in ipairs(placements) do
        if cell.row < visibleRows and cell.row + cell.rows > visibleRows then
            return true
        end
    end
    return false
end

local function overflowShift(rows, visibleRows, cardHeight, gap, inset,
        viewportHeight)
    if rows <= visibleRows then return 0 end
    local firstOverflowTop = inset + visibleRows * (cardHeight + gap)
    return math.max(0, viewportHeight - firstOverflowTop)
end

function cardLayout.cardHeight(rows, visibleRows, baseHeight,
        viewportHeight, gap, inset)
    rows = math.max(0, math.floor(rows or 0))
    visibleRows = math.max(1, math.floor(visibleRows or 1))
    baseHeight = math.max(1, math.floor(baseHeight or 1))
    if rows == 0 then return baseHeight end
    local fittedRows = math.min(rows, visibleRows)
    local fillHeight = math.floor((viewportHeight - inset * 2 -
        gap * (fittedRows - 1)) / fittedRows)
    if fillHeight <= baseHeight then return baseHeight end
    return math.min(fillHeight,
        baseHeight + math.max(1, math.floor(baseHeight * 0.10)))
end

function cardLayout.rowTop(row, visibleRows, cardHeight, gap, inset,
        viewportHeight, continuous)
    local top = inset + row * (cardHeight + gap)
    if continuous or row < visibleRows then return top end
    return top + math.max(0, viewportHeight -
        (inset + visibleRows * (cardHeight + gap)))
end

function cardLayout.contentHeight(rows, visibleRows, cardHeight, gap,
        inset, viewportHeight, continuous)
    rows = math.max(0, math.floor(rows or 0))
    visibleRows = math.max(1, math.floor(visibleRows or 1))
    viewportHeight = math.max(1, math.ceil(viewportHeight or 1))
    if rows == 0 then return viewportHeight end
    local measured = math.ceil(inset + rows * cardHeight +
        (rows - 1) * gap + inset + (continuous and 0 or
            overflowShift(rows, visibleRows, cardHeight, gap, inset,
                viewportHeight)))
    return math.max(viewportHeight, measured)
end

function cardLayout.maximumOffset(contentHeight, viewportHeight)
    return math.max(0, contentHeight - viewportHeight)
end

return cardLayout
