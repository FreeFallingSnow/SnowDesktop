local board = {size = 15}
board.count = board.size * board.size
board.empty = string.rep(".", board.count)

local axes = {{1, 0}, {0, 1}, {1, 1}, {1, -1}}

function board.coordinates(index)
    return (index - 1) % board.size + 1, (index - 1) // board.size + 1
end

local function winningLine(cells, index, stone)
    local x, y = board.coordinates(index)
    for _, axis in ipairs(axes) do
        local line = {index}
        for sign = -1, 1, 2 do
            local cx, cy = x + sign * axis[1], y + sign * axis[2]
            while cx >= 1 and cx <= board.size and cy >= 1 and cy <= board.size do
                local nextIndex = (cy - 1) * board.size + cx
                if cells:sub(nextIndex, nextIndex) ~= stone then break end
                line[#line + 1] = nextIndex
                cx, cy = cx + sign * axis[1], cy + sign * axis[2]
            end
        end
        if #line >= 5 then return line end
    end
end

function board.parse(cells)
    if type(cells) ~= "string" or #cells ~= board.count or cells:find("[^BW.]") then return end
    local _, black = cells:gsub("B", "")
    local _, white = cells:gsub("W", "")
    if black ~= white and black ~= white + 1 then return end
    local winner, line
    for index = 1, board.count do
        local stone = cells:sub(index, index)
        if stone ~= "." and stone ~= winner then
            local found = winningLine(cells, index, stone)
            if found then
                if winner or (stone == "B" and black == white) or (stone == "W" and black ~= white) then return end
                winner, line = stone, found
            end
        end
    end
    local played = black + white
    return {cells = cells, played = played, turn = black == white and "B" or "W",
        winner = winner or (played == board.count and "draw" or nil), line = line}
end

function board.play(state, index)
    if not index or index < 1 or index > board.count or state.winner or
        state.cells:sub(index, index) ~= "." then return end
    local stone = state.turn
    local cells = state.cells:sub(1, index - 1) .. stone .. state.cells:sub(index + 1)
    local line = winningLine(cells, index, stone)
    local played = state.played + 1
    return {cells = cells, played = played, turn = stone == "B" and "W" or "B",
        winner = line and stone or (played == board.count and "draw" or nil), line = line}
end

return board
