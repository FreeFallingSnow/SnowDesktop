local board = {size = 15}
board.count = board.size * board.size
board.empty = string.rep(".", board.count)

local axes = {{1, 0}, {0, 1}, {1, 1}, {1, -1}}

function board.coordinates(index)
    return (index - 1) % board.size + 1, (index - 1) // board.size + 1
end

local function inside(x, y)
    return x >= 1 and x <= board.size and y >= 1 and y <= board.size
end

local function place(cells, index, stone)
    return cells:sub(1, index - 1) .. stone .. cells:sub(index + 1)
end

local function winningLine(cells, index, stone, rule)
    local x, y = board.coordinates(index)
    local restricted = stone == "B" and rule == "standard"
    local overline
    for _, axis in ipairs(axes) do
        local line = {index}
        for sign = -1, 1, 2 do
            local cx, cy = x + sign * axis[1], y + sign * axis[2]
            while inside(cx, cy) do
                local nextIndex = (cy - 1) * board.size + cx
                if cells:sub(nextIndex, nextIndex) ~= stone then break end
                line[#line + 1] = nextIndex
                cx, cy = cx + sign * axis[1], cy + sign * axis[2]
            end
        end
        if #line == 5 or (#line > 5 and not restricted) then return line end
        if #line > 5 then overline = true end
    end
    return nil, overline
end

local function blackThreats(cells, index)
    local x, y = board.coordinates(index)
    local fours, threes = 0, {}
    for _, axis in ipairs(axes) do
        local dx, dy = axis[1], axis[2]
        local sample = {}
        for offset = -5, 5 do
            local cx, cy = x + dx * offset, y + dy * offset
            local cell = index + offset * (dx + dy * board.size)
            sample[offset + 6] = inside(cx, cy) and cells:sub(cell, cell) or "#"
        end
        local line = table.concat(sample)

        -- Count a straight four once even though both ends can complete it.
        local previousFour
        for start = 2, 6 do
            if sample[start - 1] ~= "B" and sample[start + 5] ~= "B" then
                local window = line:sub(start, start + 4)
                local gap = window:match("^B*()%.B*$")
                if gap then
                    local first = gap == 1 and start + 1 or start
                    if first ~= previousFour then
                        fours = fours + 1
                        if fours == 2 then return true end
                        previousFour = first
                    end
                end
            end
        end

        local previousThree, extensions
        for start = 3, 6 do
            if sample[start - 1] == "." and sample[start + 4] == "." and
                sample[start - 2] ~= "B" and sample[start + 5] ~= "B" then
                local window = line:sub(start, start + 3)
                local gap = window:match("^B*()%.B*$")
                if gap then
                    local first = gap == 1 and start + 1 or start
                    if first ~= previousThree then
                        extensions = {}
                        threes[#threes + 1] = extensions
                        previousThree = first
                    end
                    extensions[#extensions + 1] = index + (start + gap - 7) * (dx + dy * board.size)
                end
            end
        end
    end
    return false, threes
end

local function moveResult(cells, index, stone, rule)
    local line, overline = winningLine(cells, index, stone, rule)
    if line or stone ~= "B" or rule ~= "standard" then return line end
    if overline then return nil, "overline" end
    local doubleFour, threes = blackThreats(cells, index)
    if doubleFour then return nil, "double_four" end
    if #threes < 2 then return end

    -- A three only counts if an extension makes a legal straight four without winning.
    -- Each recursive check adds a stone and considers just the threats through that stone.
    local live = 0
    for _, extensions in ipairs(threes) do
        for _, extension in ipairs(extensions) do
            local win, foul = moveResult(place(cells, extension, "B"), extension, "B", rule)
            if not win and not foul then
                live = live + 1
                if live == 2 then return nil, "double_three" end
                break
            end
        end
    end
end

function board.blackForbidden(cells, index)
    local _, foul = moveResult(place(cells, index, "B"), index, "B", "standard")
    return foul
end

local function result(cells, played, turn, rule, winner)
    if winner then return winner end
    if played == board.count then return "draw" end
    if turn == "B" and rule == "standard" then
        for index in cells:gmatch("()%.") do
            if not board.blackForbidden(cells, index) then return end
        end
        return "W"
    end
end

function board.parse(cells, rule, last)
    if type(cells) ~= "string" or #cells ~= board.count or cells:find("[^BW.]") then return end
    rule = rule or "free"
    if rule ~= "free" and rule ~= "standard" then return end
    local _, black = cells:gsub("B", "")
    local _, white = cells:gsub("W", "")
    if black ~= white and black ~= white + 1 then return end
    local played = black + white
    local turn = black == white and "B" or "W"
    local winner, line
    if last then
        local stone = turn == "B" and "W" or "B"
        if last < 1 or last > board.count or cells:sub(last, last) ~= stone then return end
        local found, foul = moveResult(cells, last, stone, rule)
        if foul then return end
        if found then winner, line = stone, found end
    end
    for index = 1, board.count do
        local stone = cells:sub(index, index)
        if stone ~= "." and stone ~= winner then
            local found, overline = winningLine(cells, index, stone, rule)
            if overline then return end
            if found then
                if last or stone == turn then return end
                winner, line = stone, found
            end
        end
    end
    return {cells = cells, played = played, turn = turn, rule = rule, last = last,
        winner = result(cells, played, turn, rule, winner), line = line}
end

function board.play(state, index)
    if not index or index % 1 ~= 0 or index < 1 or index > board.count or state.winner or
        state.cells:sub(index, index) ~= "." then return end
    local stone = state.turn
    local cells = place(state.cells, index, stone)
    local line, foul = moveResult(cells, index, stone, state.rule)
    if foul then return nil, foul end
    local played = state.played + 1
    local turn = stone == "B" and "W" or "B"
    return {cells = cells, played = played, turn = turn, last = index, rule = state.rule,
        winner = result(cells, played, turn, state.rule, line and stone), line = line}
end

return board
