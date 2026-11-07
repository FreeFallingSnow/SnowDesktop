local board = module.require("modules/board.lua")

local ai = {}
local black, white, empty = string.byte("BW.", 1, 3)
local win = 1000000
local axes = {{1, 0}, {0, 1}, {1, 1}, {1, -1}}
local openScores = {1, 12, 110, 1700}
local halfOpenScores = {0, 3, 24, 250}

local function inside(x, y)
    return x >= 1 and y >= 1 and x <= board.size and y <= board.size
end

local function ray(cells, x, y, dx, dy, stone)
    local length = 0
    x, y = x + dx, y + dy
    while inside(x, y) do
        local cell = cells:byte((y - 1) * board.size + x)
        if cell ~= stone then return length, cell == empty end
        length = length + 1
        if length >= 5 then break end
        x, y = x + dx, y + dy
    end
    return length, false
end

local function score(cells, index, stone, exact)
    local x, y = board.coordinates(index)
    local total = 0
    for _, axis in ipairs(axes) do
        local left, leftOpen = ray(cells, x, y, -axis[1], -axis[2], stone)
        local right, rightOpen = ray(cells, x, y, axis[1], axis[2], stone)
        local length = left + right + 1
        if length == 5 or (length > 5 and not exact) then return win end
        if length < 5 and leftOpen and rightOpen then
            total = total + openScores[length]
        elseif length < 5 and (leftOpen or rightOpen) then
            total = total + halfOpenScores[length]
        end
    end
    return total
end

local function nearby(cells, index)
    local x, y = board.coordinates(index)
    for ny = math.max(1, y - 1), math.min(board.size, y + 1) do
        for nx = math.max(1, x - 1), math.min(board.size, x + 1) do
            if cells:byte((ny - 1) * board.size + nx) ~= empty then return true end
        end
    end
    return false
end

local function strongestReply(cells, chosen, ranked, exact)
    local strongest = 0
    for i = 1, math.min(10, #ranked) do
        local index = ranked[i].index
        if index ~= chosen then strongest = math.max(strongest, score(cells, index, black, exact)) end
    end
    local x, y = board.coordinates(chosen)
    for ny = math.max(1, y - 1), math.min(board.size, y + 1) do
        for nx = math.max(1, x - 1), math.min(board.size, x + 1) do
            local index = (ny - 1) * board.size + nx
            if cells:byte(index) == empty then strongest = math.max(strongest, score(cells, index, black, exact)) end
        end
    end
    return strongest
end

function ai.choose(game, level)
    if game.winner or game.turn ~= "W" then return end
    local exact = game.rule == "exact"
    local ranked, block = {}, nil
    for index = 1, board.count do
        if game.cells:byte(index) == empty and nearby(game.cells, index) then
            local attack = score(game.cells, index, white, exact)
            if attack == win then return index end
            local defense = score(game.cells, index, black, exact)
            if defense == win then block = block or index end
            local x, y = board.coordinates(index)
            local center = board.size // 2 + 1
            local value = attack + defense * 1.1 - (math.abs(x - center) + math.abs(y - center)) * 0.15
            ranked[#ranked + 1] = {index = index, value = value}
        end
    end
    if block then return block end
    table.sort(ranked, function(a, b)
        if a.value == b.value then return a.index < b.index end
        return a.value > b.value
    end)
    if not ranked[1] then return game.cells:find(".", 1, true) end
    if level == "casual" then
        local choice = 1 + ((game.last or game.played) + game.played) % math.min(4, #ranked)
        return ranked[choice].index
    end
    if level == "advanced" then
        local best, bestValue = ranked[1].index, -math.huge
        for i = 1, math.min(5, #ranked) do
            local candidate = ranked[i]
            local reply = strongestReply(board.play(game, candidate.index).cells, candidate.index, ranked, exact)
            local value = candidate.value - reply * 0.9
            if value > bestValue then best, bestValue = candidate.index, value end
        end
        return best
    end
    return ranked[1].index
end

return ai
