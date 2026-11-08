local history = {}
local emptyByte = string.byte("A")

local function position(date)
    -- Fixed 31-slot months keep the offset stable across leap years.
    local month, day = tonumber(date:sub(6, 7)), tonumber(date:sub(9, 10))
    return (month - 1) * 31 + day
end

local function mask(record, index)
    local value = (record:byte(index) or emptyByte) - emptyByte
    return value >= 0 and value < 32 and value or 0
end

function history.has(record, date, slot)
    return mask(record, position(date)) & (1 << (slot - 1)) ~= 0
end

function history.toggle(record, date, slot)
    local index = position(date)
    local updated = mask(record, index) ~ (1 << (slot - 1))
    local prefix = record:sub(1, index - 1)
    prefix = prefix .. string.rep("A", index - 1 - #prefix)
    local result = prefix .. string.char(emptyByte + updated) .. record:sub(index + 1)
    return (result:gsub("A+$", ""))
end

function history.total(records, throughDate, slot)
    local lastYear = throughDate:sub(1, 4)
    local lastDay = position(throughDate)
    local flag = 1 << (slot - 1)
    local total = 0
    for year, record in pairs(records) do
        if year <= lastYear then
            local length = year == lastYear and math.min(lastDay, #record) or #record
            for index = 1, length do
                if mask(record, index) & flag ~= 0 then total = total + 1 end
            end
        end
    end
    return total
end

function history.shiftMonth(date, delta, today)
    local year, month = tonumber(date:sub(1, 4)), tonumber(date:sub(6, 7))
    local index = year * 12 + month - 1 + delta
    if index < 12 or index >= 120000 then return nil end
    local target = string.format("%04d-%02d-01", index // 12, index % 12 + 1)
    if target:sub(1, 7) > today:sub(1, 7) then return nil end
    return target
end

function history.readOrder(saved, count)
    local order, seen = {}, {}
    if type(saved) == "string" then
        for digit in saved:gmatch("%d") do
            local slot = tonumber(digit)
            if slot >= 1 and slot <= count and not seen[slot] then
                order[#order + 1] = slot
                seen[slot] = true
            end
        end
    end
    for slot = 1, count do
        if not seen[slot] then order[#order + 1] = slot end
    end
    return order
end

function history.swap(order, first, second)
    local result = { table.unpack(order) }
    local left, right
    for index, slot in ipairs(result) do
        if slot == first then left = index end
        if slot == second then right = index end
    end
    if not left or not right then return nil end
    result[left], result[right] = result[right], result[left]
    return result
end

return history
