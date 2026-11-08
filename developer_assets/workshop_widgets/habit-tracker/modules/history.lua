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

return history
