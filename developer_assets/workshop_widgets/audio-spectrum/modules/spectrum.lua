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
local IDLE_VARIATION = 0.022
local CURVE_POINT_BUDGET = 257

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

function M.zeroes(count)
    local result = {}
    for index = 1, M.binCount(count) do result[index] = 0 end
    return result
end

function M.normalize(values, sensitivity, count)
    local length = M.binCount(count)
    local gain = M.sensitivity(sensitivity)
    local source = type(values) == "table" and values or {}
    local result = {}
    for index = 1, length do
        local value = finiteNumber(source[index], 0)
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
        local baseline = IDLE_MINIMUM +
            ((index * 7) % 11) / 10 * IDLE_VARIATION
        result[index] = math.max(baseline,
            M.clamp(source[index], 0, 1, 0))
    end
    return result
end

local function catmullRom(left, start, finish, right, amount)
    local amount2 = amount * amount
    local amount3 = amount2 * amount
    return 0.5 * ((2 * start) +
        (-left + finish) * amount +
        (2 * left - 5 * start + 4 * finish - right) * amount2 +
        (-left + 3 * start - 3 * finish + right) * amount3)
end

function M.mirroredCurve(values, count)
    local bands = M.display(values, count)
    local softened = {}
    for index = 1, #bands do
        local left = bands[math.max(1, index - 1)]
        local right = bands[math.min(#bands, index + 1)]
        softened[index] = M.clamp(
            left * 0.25 + bands[index] * 0.5 + right * 0.25,
            IDLE_MINIMUM, 1, IDLE_MINIMUM)
    end

    local mirrored = {}
    for index = #softened, 2, -1 do
        mirrored[#mirrored + 1] = softened[index]
    end
    for index = 1, #softened do
        mirrored[#mirrored + 1] = softened[index]
    end

    local segments = #mirrored - 1
    local subdivisions = math.max(1,
        math.floor((CURVE_POINT_BUDGET - 1) / segments))
    local result = {}
    for index = 1, segments do
        local left = mirrored[math.max(1, index - 1)]
        local start = mirrored[index]
        local finish = mirrored[index + 1]
        local right = mirrored[math.min(#mirrored, index + 2)]
        for step = 0, subdivisions - 1 do
            result[#result + 1] = math.max(IDLE_MINIMUM, M.clamp(
                catmullRom(left, start, finish, right,
                    step / subdivisions), 0, 1, IDLE_MINIMUM))
        end
    end
    result[#result + 1] = mirrored[#mirrored]
    return result
end

function M.invert(values)
    local source = type(values) == "table" and values or {}
    local result = {}
    for index = 1, #source do
        result[index] = -M.clamp(source[index], 0, 1, 0)
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
