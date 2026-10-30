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
-- A custom order starts each search after the previous placement so filling
-- an earlier hole cannot put a later card ahead of the user's chosen order.
function cardLayout.pack(cards, columns, sizeFor, ordered)
    columns = math.max(1, math.floor(columns or 1))
    local occupied, placements, signature = {}, {}, {}
    local rows, cursor = 0, 0
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
        local row = ordered and math.floor(cursor / columns) or 0
        local column = ordered and cursor % columns or 0
        if column + width > columns then row, column = row + 1, 0 end
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
        cursor = row * columns + column + 1
        signature[#signature + 1] = card.id .. "=" .. size
    end
    return placements, rows, table.concat(signature, ";")
end

function cardLayout.crossesViewport(placements, visibleCount, horizontal)
    local origin = horizontal and "column" or "row"
    local span = horizontal and "columns" or "rows"
    for _, cell in ipairs(placements) do
        if cell[origin] < visibleCount and
            cell[origin] + cell[span] > visibleCount then
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
    -- The viewport owns its top/bottom insets and inter-row gaps. Keeping the
    -- full grid-cell height here clips the last card even when every row fits.
    if fillHeight <= baseHeight then return math.max(1, fillHeight) end
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

-- The scroll host rounds the viewport with lround, while a ceiled content
-- extent is one pixel taller whenever the fractional part is below 0.5.
-- That phantom pixel is enough for a wheel step even when every card fits.
function cardLayout.hostExtent(value)
    local number = tonumber(value)
    if type(number) ~= "number" or number ~= number or
        number == math.huge or number == -math.huge then
        return 1
    end
    return math.max(1, math.floor(number + 0.5))
end

function cardLayout.scrollExtent(measured, viewport)
    local host = cardLayout.hostExtent(viewport)
    local content = math.ceil(tonumber(measured) or 0)
    if content ~= content or content == math.huge or content <= host + 1 then
        return host
    end
    return content
end

function cardLayout.contentHeight(rows, visibleRows, cardHeight, gap,
        inset, viewportHeight, continuous)
    local requested = viewportHeight
    rows = math.max(0, math.floor(rows or 0))
    visibleRows = math.max(1, math.floor(visibleRows or 1))
    viewportHeight = math.max(1, math.ceil(tonumber(viewportHeight) or 1))
    if rows == 0 or rows <= visibleRows then
        return cardLayout.hostExtent(requested)
    end
    local measured = math.ceil(inset + rows * cardHeight +
        (rows - 1) * gap + inset + (continuous and 0 or
            overflowShift(rows, visibleRows, cardHeight, gap, inset,
                viewportHeight)))
    return cardLayout.scrollExtent(measured, requested)
end

function cardLayout.gridFrame(cell, cardWidth, cardHeight, gapX, gapY, inset,
        visibleCount, viewportExtent, continuous, horizontal)
    local x, y
    if horizontal then
        x = cardLayout.rowTop(cell.column, visibleCount, cardWidth, gapX,
            inset, viewportExtent, continuous)
        y = inset + cell.row * (cardHeight + gapY)
    else
        x = inset + cell.column * (cardWidth + gapX)
        y = cardLayout.rowTop(cell.row, visibleCount, cardHeight, gapY,
            inset, viewportExtent, continuous)
    end
    local width = cell.columns * cardWidth +
        math.max(0, cell.columns - 1) * gapX
    local height = cell.rows * cardHeight +
        math.max(0, cell.rows - 1) * gapY
    return x, y, width, height
end

-- Packing stays row-major. Horizontal grids swap each card, pack down the
-- fixed row count, then swap the coordinates back.
function cardLayout.swapSize(size)
    size = cardLayout.size(size)
    return size:sub(3, 3) .. "x" .. size:sub(1, 1)
end

function cardLayout.transpose(cells)
    local transposed, columns = {}, 0
    for index, cell in ipairs(cells) do
        transposed[index] = {
            column = cell.row, row = cell.column,
            columns = cell.rows, rows = cell.columns,
        }
        columns = math.max(columns, cell.row + cell.rows)
    end
    return transposed, columns
end

local function sameBand(pointer, origin, size)
    return type(pointer) == "number" and pointer >= origin and
        pointer < origin + math.max(size or 0, 1)
end

-- targetIndex is the 1-based slot after the dragged card is removed.
function cardLayout.dropTarget(frames, dragId, contentX, contentY, horizontal)
    if type(frames) ~= "table" or type(dragId) ~= "string" or
        type(contentX) ~= "number" or type(contentY) ~= "number" then
        return nil
    end
    local source
    for index, frame in ipairs(frames) do
        if frame.id == dragId then source = index break end
    end
    if not source then return nil end
    local function before(frame)
        if horizontal then
            if sameBand(contentX, frame.x, frame.w) then
                return contentY < frame.y + frame.h / 2, "y", frame.y
            end
            return contentX < frame.x + frame.w / 2, "x", frame.x
        end
        if sameBand(contentY, frame.y, frame.h) then
            return contentX < frame.x + frame.w / 2, "x", frame.x
        end
        return contentY < frame.y + frame.h / 2, "y", frame.y
    end
    local seen = 0
    for index, frame in ipairs(frames) do
        if index ~= source then
            local isBefore, axis, at = before(frame)
            if isBefore then return seen + 1, frame, axis, at end
            seen = seen + 1
        end
    end
    local last
    for index = #frames, 1, -1 do
        if index ~= source then last = frames[index] break end
    end
    if not last then return 1, frames[source], "x", frames[source].x end
    if horizontal then
        if sameBand(contentX, last.x, last.w) then
            return seen + 1, last, "y", last.y + last.h
        end
        return seen + 1, last, "x", last.x + last.w
    end
    if sameBand(contentY, last.y, last.h) then
        return seen + 1, last, "x", last.x + last.w
    end
    return seen + 1, last, "y", last.y + last.h
end

function cardLayout.maximumOffset(contentHeight, viewportHeight)
    return math.max(0, contentHeight - viewportHeight)
end

return cardLayout
