local M = {}

local function positive(value)
    value = tonumber(value)
    if not value or value ~= value or value == math.huge or
        value == -math.huge or value <= 0 then
        return 1
    end
    return value
end

function M.plan(width, height)
    width = positive(width)
    height = positive(height)
    local vertical = height > width * 1.05
    local compact
    if vertical then
        compact = height < 300
    else
        compact = width < 340 or height < 220
    end
    return {
        vertical = vertical,
        compact = compact,
    }
end

return M
