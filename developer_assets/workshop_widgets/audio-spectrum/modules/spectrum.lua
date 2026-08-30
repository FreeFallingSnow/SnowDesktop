local M = {}

local MIN_BINS = 16
local MAX_BINS = 128
local BIN_STEP = 16
local DEFAULT_BINS = 48
local MIN_SENSITIVITY = 0.5
local MAX_SENSITIVITY = 3.0
local DEFAULT_SENSITIVITY = 1.5
local ATTACK = 0.62
local RELEASE = 0.18
local IDLE_MINIMUM = 0.018
local RANGE_ENERGY_PERCENTILE = 0.97
local RANGE_MINIMUM_FRACTION = 0.45
local RANGE_PADDING_FRACTION = 0.05
local RANGE_EXPAND = 0.55
local RANGE_CONTRACT = 0.04
local VISUAL_NEIGHBOR_MIX = 0.28
local VISUAL_GAMMA = 0.72

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

function M.adaptiveRange(values, previous)
    local source = type(values) == "table" and values or {}
    local length = #source
    if length == 0 then return 0 end
    local minimum = math.max(1,
        math.floor(length * RANGE_MINIMUM_FRACTION + 0.5))
    local before = math.floor(M.clamp(previous,
        minimum, length, length) + 0.5)
    local energy = 0
    local samples = {}
    for index = 1, length do
        local value = M.clamp(source[index], 0, 1, 0)
        samples[index] = value
        energy = energy + value * value
    end
    if energy <= 0.000001 then return before end

    local threshold = energy * RANGE_ENERGY_PERCENTILE
    local accumulated = 0
    local target = minimum
    for index = 1, length do
        accumulated = accumulated + samples[index] * samples[index]
        if accumulated >= threshold then
            target = index
            break
        end
    end
    target = math.max(minimum, math.min(length,
        target + math.ceil(length * RANGE_PADDING_FRACTION)))
    if math.abs(target - before) <= 1 then return target end
    local coefficient = target > before and
        RANGE_EXPAND or RANGE_CONTRACT
    return math.max(minimum, math.min(length,
        math.floor(before + (target - before) * coefficient + 0.5)))
end

function M.normalize(values, sensitivity, count, sourceLimit)
    local length = M.binCount(count)
    local gain = M.sensitivity(sensitivity)
    local source = type(values) == "table" and values or {}
    local sourceLength = #source
    local activeLength = sourceLength
    if sourceLimit ~= nil and sourceLength > 0 then
        activeLength = math.floor(M.clamp(sourceLimit,
            1, sourceLength, sourceLength) + 0.5)
    end
    local result = {}
    for index = 1, length do
        local value = 0
        if sourceLimit ~= nil and activeLength > 0 and
            activeLength < length then
            local position = length == 1 and 1 or 1 +
                (index - 1) * (activeLength - 1) / (length - 1)
            local left = math.max(1, math.floor(position))
            local right = math.min(activeLength, left + 1)
            local fraction = position - left
            local leftValue = M.clamp(source[left], 0, 1, 0)
            local rightValue = M.clamp(source[right], 0, 1, 0)
            value = leftValue + (rightValue - leftValue) * fraction
        elseif activeLength > length then
            local first = math.floor((index - 1) * activeLength / length) + 1
            local last = math.max(first,
                math.floor(index * activeLength / length))
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

function M.smooth(previous, values, sensitivity, count, reset, sourceLimit)
    local target = M.normalize(values, sensitivity, count, sourceLimit)
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

function M.visual(values, count)
    local source = M.display(values, count)
    local result = {}
    for index = 1, #source do
        local before = source[math.max(1, index - 1)]
        local current = source[index]
        local after = source[math.min(#source, index + 1)]
        local neighborhood = (before + current * 2 + after) / 4
        local blended = current * (1 - VISUAL_NEIGHBOR_MIX) +
            neighborhood * VISUAL_NEIGHBOR_MIX
        result[index] = M.clamp(blended, 0, 1, 0) ^ VISUAL_GAMMA
    end
    return result
end

function M.mixColor(left, right, amount)
    local first = M.color(left, 0)
    local second = M.color(right, 0)
    local fraction = M.clamp(amount, 0, 1, 0)
    local function channel(shift)
        local a = math.floor(first / (2 ^ shift)) % 256
        local b = math.floor(second / (2 ^ shift)) % 256
        return math.floor(a + (b - a) * fraction + 0.5)
    end
    return channel(16) * 0x10000 + channel(8) * 0x100 + channel(0)
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
            0.72 * bell(position, 0.22, 0.14) +
            0.50 * bell(position, 0.48, 0.15) +
            0.34 * bell(position, 0.70, 0.12) +
            0.14 * bell(position, 0.88, 0.08)
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
