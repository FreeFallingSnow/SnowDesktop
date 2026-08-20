local M = {}

function M.clamp(value, minimum, maximum)
    local number = tonumber(value) or minimum
    return math.max(minimum, math.min(maximum, number))
end

function M.append(values, value, limit)
    local maximum = math.max(1, math.floor(tonumber(limit) or 1))
    local source = type(values) == "table" and values or {}
    local result = {}
    local first = math.max(1, #source - maximum + 2)
    for index = first, #source do
        result[#result + 1] = source[index]
    end
    result[#result + 1] = value
    return result
end

function M.seed(count, base, amplitude, phase)
    local length = math.max(1, math.floor(tonumber(count) or 1))
    local values = {}
    for index = 1, length do
        local wave = math.sin((index + (phase or 0)) * 0.43)
        local ripple = math.sin((index + (phase or 0)) * 1.17) * 0.24
        values[index] = M.clamp(base + amplitude * (wave + ripple), 0, 100)
    end
    return values
end

function M.latest(values, fallback)
    if type(values) ~= "table" or #values == 0 then return fallback end
    return values[#values]
end

return M
