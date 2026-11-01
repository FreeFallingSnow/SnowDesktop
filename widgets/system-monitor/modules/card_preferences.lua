local M = {}

M.defaults = { "cpu", "memory", "gpu", "vram", "network", "battery", "storage", "disk_io", "uptime" }

function M.order(saved)
    local order, known, seen = {}, {}, {}
    for _, id in ipairs(M.defaults) do known[id] = true end
    if type(saved) == "string" then
        for id in saved:gmatch("[^,]+") do
            if known[id] and not seen[id] then
                order[#order + 1], seen[id] = id, true
            end
        end
    end
    for _, id in ipairs(M.defaults) do
        if not seen[id] then order[#order + 1] = id end
    end
    return order
end

function M.customOrder(saved)
    return table.concat(M.order(saved), ",") ~= table.concat(M.defaults, ",")
end

function M.arrange(cards, saved)
    local byId, result = {}, {}
    for _, card in ipairs(cards) do byId[card.id] = card end
    for _, id in ipairs(M.order(saved)) do
        if byId[id] then result[#result + 1] = byId[id] end
    end
    return result
end

-- Move among visible cards while preserving hidden cards and their slots.
-- Return nil at a boundary so a disabled/no-op action never writes storage.
function M.move(saved, id, action, visible)
    local order, shown, index = M.order(saved), {}, nil
    for _, key in ipairs(order) do
        if visible(key) then
            shown[#shown + 1] = key
            if key == id then index = #shown end
        end
    end
    if not index then return nil end
    local target = ({ first = 1, previous = index - 1, next = index + 1, last = #shown })[action]
    if not target or target < 1 or target > #shown or target == index then return nil end
    table.remove(shown, index)
    table.insert(shown, target, id)
    local cursor = 1
    for i, key in ipairs(order) do
        if visible(key) then order[i], cursor = shown[cursor], cursor + 1 end
    end
    return table.concat(order, ",")
end

function M.orientation(value)
    if value == "horizontal" or value == "row" then return "horizontal" end
    return "vertical"
end

-- Live drag preview. The saved card list is left untouched.
function M.preview(cards, id, targetIndex)
    if type(cards) ~= "table" or type(targetIndex) ~= "number" then
        return cards
    end
    local kept, moving, index = {}, nil, nil
    for position, card in ipairs(cards) do
        if card.id == id then moving, index = card, position
        else kept[#kept + 1] = card end
    end
    if not moving then return cards end
    targetIndex = math.floor(targetIndex)
    if targetIndex < 1 or targetIndex > #kept + 1 or targetIndex == index then
        return cards
    end
    table.insert(kept, targetIndex, moving)
    return kept
end

-- targetIndex is the visible slot after the dragged card leaves its old place.
function M.moveTo(saved, id, targetIndex, visible)
    local order, shown, index = M.order(saved), {}, nil
    for _, key in ipairs(order) do
        if visible(key) then
            shown[#shown + 1] = key
            if key == id then index = #shown end
        end
    end
    if not index or type(targetIndex) ~= "number" then return nil end
    targetIndex = math.floor(targetIndex)
    if targetIndex < 1 or targetIndex > #shown or targetIndex == index then
        return nil
    end
    table.remove(shown, index)
    table.insert(shown, targetIndex, id)
    local cursor = 1
    for i, key in ipairs(order) do
        if visible(key) then order[i], cursor = shown[cursor], cursor + 1 end
    end
    return table.concat(order, ",")
end

function M.fontScale(value)
    if type(value) ~= "number" and type(value) ~= "string" then return 1 end
    local percent = tonumber(value)
    if not percent or percent ~= percent or percent == math.huge or percent == -math.huge then return 1 end
    return math.max(70, math.min(150, percent)) / 100
end

return M
