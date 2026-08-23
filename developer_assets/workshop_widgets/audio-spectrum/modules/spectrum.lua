local M = {}

local MIN_BINS = 16
local MAX_BINS = 128
local BIN_STEP = 16
local DEFAULT_BINS = 64
local MIN_SENSITIVITY = 0.5
local MAX_SENSITIVITY = 3.0
local DEFAULT_SENSITIVITY = 1.5
local ATTACK = 0.62
local RELEASE = 0.18
local IDLE_MINIMUM = 0.018

local function finiteNumber(value, fallback)
    local number = tonumber(value)
    if not number or number ~= number or number == math.huge or
        number == -math.huge then
        return fallback
    end
    return number
end

function M.clamp(value, minimum, maximum, fallback)
    local number = finiteNumber(value, fallback or minimum)
    return math.max(minimum, math.min(maximum, number))
end

function M.binCount(value)
    local count = M.clamp(value, MIN_BINS, MAX_BINS, DEFAULT_BINS)
    count = math.floor((count + BIN_STEP / 2) / BIN_STEP) * BIN_STEP
    return math.max(MIN_BINS, math.min(MAX_BINS, count))
end

function M.sensitivity(value)
    return M.clamp(value, MIN_SENSITIVITY, MAX_SENSITIVITY,
        DEFAULT_SENSITIVITY)
end

function M.alignment(value)
    if value == "bottom" or value == "top" then return value end
    return "center"
end

function M.seriesPlan(value)
    local alignment = M.alignment(value)
    if alignment == "bottom" then
        return {
            renderer = "spectrum",
            minimum = 0,
            maximum = 1,
            negate = false,
            mirror = false,
        }
    end
    if alignment == "top" then
        return {
            renderer = "barChart",
            minimum = -1,
            maximum = 0,
            negate = true,
            mirror = false,
        }
    end
    return {
        renderer = "barChart",
        minimum = -1,
        maximum = 1,
        negate = false,
        mirror = true,
    }
end

function M.color(value, fallback)
    local default = finiteNumber(fallback, 0xFFFFFF)
    local color = finiteNumber(value, default)
    return math.floor(M.clamp(color, 0, 0xFFFFFF, default) + 0.5)
end

function M.zeroes(count)
    local result = {}
    for index = 1, M.binCount(count) do result[index] = 0 end
    return result
end

function M.normalize(values, sensitivity, count)
    local length = M.binCount(count)
    local gain = M.sensitivity(sensitivity)
    local source = type(values) == "table" and values or {}
    local sourceLength = #source
    local result = {}
    for index = 1, length do
        local value = 0
        if sourceLength > length then
            local first = math.floor((index - 1) * sourceLength / length) + 1
            local last = math.max(first,
                math.floor(index * sourceLength / length))
            local total = 0
            local peak = 0
            for sourceIndex = first, last do
                local sample = M.clamp(source[sourceIndex], 0, 1, 0)
                total = total + sample
                peak = math.max(peak, sample)
            end
            value = peak * 0.65 + total / (last - first + 1) * 0.35
        else
            value = finiteNumber(source[index], 0)
        end
        result[index] = M.clamp(value * gain, 0, 1, 0)
    end
    return result
end

function M.smooth(previous, values, sensitivity, count, reset)
    local target = M.normalize(values, sensitivity, count)
    local source = type(previous) == "table" and previous or {}
    local result = {}
    for index = 1, #target do
        local before = reset and 0 or M.clamp(source[index], 0, 1, 0)
        local coefficient = target[index] >= before and ATTACK or RELEASE
        result[index] = M.clamp(
            before + (target[index] - before) * coefficient, 0, 1, 0)
    end
    return result
end

function M.display(values, count)
    local length = M.binCount(count)
    local source = type(values) == "table" and values or {}
    local result = {}
    for index = 1, length do
        result[index] = math.max(IDLE_MINIMUM,
            M.clamp(source[index], 0, 1, 0))
    end
    return result
end

function M.negate(values)
    local source = type(values) == "table" and values or {}
    local result = {}
    for index = 1, #source do
        result[index] = -M.clamp(source[index], 0, 1, 0)
    end
    return result
end

function M.preview(count, sensitivity)
    local length = M.binCount(count)
    local gain = M.sensitivity(sensitivity) / DEFAULT_SENSITIVITY
    local result = {}
    local function bell(position, center, width)
        local distance = (position - center) / width
        return math.exp(-distance * distance)
    end
    for index = 1, length do
        local position = (index - 1) / math.max(1, length - 1)
        local envelope = 0.035 +
            0.72 * bell(position, 0.12, 0.095) +
            0.50 * bell(position, 0.34, 0.13) +
            0.34 * bell(position, 0.58, 0.12) +
            0.20 * bell(position, 0.79, 0.10)
        local ripple = 0.78 + 0.22 *
            (0.5 + 0.5 * math.sin(index * 2.17 + 0.4))
        result[index] = M.clamp(envelope * ripple * gain,
            IDLE_MINIMUM, 0.88, IDLE_MINIMUM)
    end
    return result
end

function M.status(snapshot)
    if type(snapshot) ~= "table" then return "unavailable" end
    if snapshot.available and type(snapshot.value) == "table" then
        if snapshot.stale then return "stale" end
        if snapshot.value.silent then return "silent" end
        return "ready"
    end
    if snapshot.warmingUp then return "warming" end
    if snapshot.error == "permissionDenied" then return "permission" end
    if snapshot.error == "notPresent" then return "notPresent" end
    return "unavailable"
end

return M
