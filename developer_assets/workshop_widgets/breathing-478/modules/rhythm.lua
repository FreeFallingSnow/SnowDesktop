-- breathing-478/modules/rhythm.lua
-- Pure 4-7-8 breathing timeline math. The desktop surface owns drawing and
-- scheduling; this module only tracks phase progress so the behaviour can be
-- tested without a host.

local M = {}

local PHASE_SECONDS = { 4, 7, 8 }
local PHASE_IDS = { "inhale", "hold", "exhale" }
local PHASE_COUNT = 3
local CYCLE_SECONDS = 19
local MIN_CYCLES = 1
local MAX_CYCLES = 12
local DEFAULT_CYCLES = 4

local function clamp(value, minimum, maximum)
    if value ~= value then return minimum end
    return math.max(minimum, math.min(maximum, value))
end

local function phaseDurationMs(index)
    return PHASE_SECONDS[index] * 1000
end

function M.defaultCycles()
    return DEFAULT_CYCLES
end

function M.normalizeCycles(value)
    local number = tonumber(value)
    if number == nil or number ~= number or
        number == math.huge or number == -math.huge then
        return DEFAULT_CYCLES
    end
    return math.max(MIN_CYCLES, math.min(MAX_CYCLES,
        math.floor(number)))
end

function M.phaseCount()
    return PHASE_COUNT
end

function M.phaseId(index)
    return PHASE_IDS[index]
end

function M.phaseSeconds(index)
    return PHASE_SECONDS[index]
end

function M.cycleSeconds()
    return CYCLE_SECONDS
end

-- The duration sequence is fixed: 4 seconds in, 7 seconds holding, 8 seconds
-- out. "4 · 7 · 8" is derived from the same constants so the idle surface and
-- the runner can never disagree.
function M.patternText()
    local separator = utf8.char(0x00B7)
    return string.format("%d %s %d %s %d",
        PHASE_SECONDS[1], separator,
        PHASE_SECONDS[2], separator,
        PHASE_SECONDS[3])
end

function M.new(cycles)
    return {
        cycle = 1,
        cycles = M.normalizeCycles(cycles),
        phaseIndex = 1,
        phaseStartMs = 0,
        pausedElapsedMs = 0,
        running = false,
        paused = false,
        complete = false,
    }
end

function M.start(state, nowMilliseconds, cycles)
    state.cycles = M.normalizeCycles(cycles)
    state.cycle = 1
    state.phaseIndex = 1
    state.phaseStartMs = nowMilliseconds
    state.pausedElapsedMs = 0
    state.running = true
    state.paused = false
    state.complete = false
end

function M.mode(state)
    if state.complete then return "complete" end
    if state.paused then return "paused" end
    if state.running then return "running" end
    return "idle"
end

function M.phaseDurationMs(state)
    return phaseDurationMs(state.phaseIndex)
end

function M.elapsedMs(state, nowMilliseconds)
    local duration = phaseDurationMs(state.phaseIndex)
    if state.complete then return duration end
    if state.running then
        return clamp(nowMilliseconds - state.phaseStartMs, 0, duration)
    end
    return clamp(state.pausedElapsedMs, 0, duration)
end

function M.progress(state, nowMilliseconds)
    local mode = M.mode(state)
    if mode == "idle" then return 0 end
    if mode == "complete" then return 1 end
    return M.elapsedMs(state, nowMilliseconds) /
        phaseDurationMs(state.phaseIndex)
end

function M.remainingSeconds(state, nowMilliseconds)
    local mode = M.mode(state)
    if mode == "idle" then return PHASE_SECONDS[1] end
    if mode == "complete" then return 0 end
    local remaining = phaseDurationMs(state.phaseIndex) -
        M.elapsedMs(state, nowMilliseconds)
    -- A whole-second countdown never shows "0": the last visible value before
    -- the phase transition is 1.
    return math.max(1, math.ceil(remaining / 1000))
end

-- Advances across every elapsed boundary. Returns "complete", "phase" or nil.
function M.advance(state, nowMilliseconds)
    if not state.running or state.complete then return nil end
    local result = nil
    while state.running do
        local duration = phaseDurationMs(state.phaseIndex)
        if nowMilliseconds - state.phaseStartMs < duration then break end
        if state.phaseIndex < PHASE_COUNT then
            state.phaseIndex = state.phaseIndex + 1
            state.phaseStartMs = state.phaseStartMs + duration
            result = "phase"
        elseif state.cycle >= state.cycles then
            state.complete = true
            state.running = false
            state.paused = false
            result = "complete"
            break
        else
            state.cycle = state.cycle + 1
            state.phaseIndex = 1
            state.phaseStartMs = state.phaseStartMs + duration
            result = "phase"
        end
    end
    return result
end

function M.pause(state, nowMilliseconds)
    if not state.running then return false end
    state.pausedElapsedMs = M.elapsedMs(state, nowMilliseconds)
    state.running = false
    state.paused = true
    return true
end

function M.resume(state, nowMilliseconds)
    if not state.paused then return false end
    state.phaseStartMs = nowMilliseconds - state.pausedElapsedMs
    state.running = true
    state.paused = false
    return true
end

-- Moves to the next phase immediately. Skipping the last exhale of the last
-- round finishes the session.
function M.skip(state, nowMilliseconds)
    local mode = M.mode(state)
    if mode ~= "running" and mode ~= "paused" then return nil end
    local result = "phase"
    if state.phaseIndex < PHASE_COUNT then
        state.phaseIndex = state.phaseIndex + 1
    elseif state.cycle >= state.cycles then
        state.complete = true
        state.running = false
        state.paused = false
        result = "complete"
    else
        state.cycle = state.cycle + 1
        state.phaseIndex = 1
    end
    if state.running then
        state.phaseStartMs = nowMilliseconds
    else
        state.pausedElapsedMs = 0
    end
    return result
end

function M.stop(state)
    state.cycle = 1
    state.phaseIndex = 1
    state.phaseStartMs = 0
    state.pausedElapsedMs = 0
    state.running = false
    state.paused = false
    state.complete = false
    return state
end

-- 0 grows to 1 while inhaling, stays at 1 while holding and falls back to 0
-- while exhaling. Smoothstep keeps both ends of every phase calm instead of
-- starting or stopping abruptly.
function M.breathFraction(phaseIndex, progress)
    local t = clamp(progress, 0, 1)
    local eased = t * t * (3 - 2 * t)
    if phaseIndex == 2 then return 1 end
    if phaseIndex == 3 then return 1 - eased end
    return eased
end

return M
