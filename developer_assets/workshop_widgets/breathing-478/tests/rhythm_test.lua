-- breathing-478/tests/rhythm_test.lua
local rhythm = module.require("modules/rhythm.lua")

local function state(cycles)
    return rhythm.new(cycles or 4)
end

return {
    ["cycles are normalized into the supported range"] = function()
        assert(rhythm.normalizeCycles(nil) == 4)
        assert(rhythm.normalizeCycles(0) == 1)
        assert(rhythm.normalizeCycles(-3) == 1)
        assert(rhythm.normalizeCycles(2.8) == 2)
        assert(rhythm.normalizeCycles(99) == 12)
        assert(rhythm.normalizeCycles(math.huge) == 4)
        assert(rhythm.normalizeCycles(0 / 0) == 4)
    end,

    ["the pattern text comes from the fixed durations"] = function()
        assert(rhythm.phaseSeconds(1) == 4)
        assert(rhythm.phaseSeconds(2) == 7)
        assert(rhythm.phaseSeconds(3) == 8)
        assert(rhythm.cycleSeconds() == 19)
        assert(rhythm.patternText() == "4 \u{00B7} 7 \u{00B7} 8")
    end,

    ["a new session is idle at the first inhale"] = function()
        local s = state(3)
        assert(rhythm.mode(s) == "idle")
        assert(s.cycle == 1 and s.cycles == 3)
        assert(s.phaseIndex == 1)
        assert(rhythm.remainingSeconds(s, 0) == 4)
        assert(rhythm.progress(s, 0) == 0)
    end,

    ["starting begins inhaling at the supplied time"] = function()
        local s = state(2)
        rhythm.start(s, 1000, 2)
        assert(rhythm.mode(s) == "running")
        assert(rhythm.remainingSeconds(s, 1000) == 4)
        assert(rhythm.remainingSeconds(s, 2000) == 3)
        assert(rhythm.remainingSeconds(s, 3400) == 2)
        assert(rhythm.remainingSeconds(s, 4999) == 1)
    end,

    ["exact phase boundaries advance in order"] = function()
        local s = state(1)
        rhythm.start(s, 0, 1)
        assert(rhythm.advance(s, 3999) == nil)
        assert(s.phaseIndex == 1)
        assert(rhythm.advance(s, 4000) == "phase")
        assert(s.phaseIndex == 2)
        assert(rhythm.remainingSeconds(s, 4000) == 7)
        assert(rhythm.advance(s, 10999) == nil)
        assert(rhythm.advance(s, 11000) == "phase")
        assert(s.phaseIndex == 3)
        assert(rhythm.advance(s, 18999) == nil)
        assert(rhythm.advance(s, 19000) == "complete")
        assert(rhythm.mode(s) == "complete")
        assert(s.cycle == 1)
    end,

    ["exhale wraps into the next round"] = function()
        local s = state(2)
        rhythm.start(s, 0, 2)
        assert(rhythm.advance(s, 19000) == "phase")
        assert(s.phaseIndex == 1)
        assert(s.cycle == 2)
        assert(rhythm.advance(s, 38000) == "complete")
        assert(s.complete)
    end,

    ["a long gap advances several phases without drift"] = function()
        local s = state(2)
        rhythm.start(s, 0, 2)
        assert(rhythm.advance(s, 25000) == "phase")
        assert(s.cycle == 2 and s.phaseIndex == 2)
        -- The phase began at 23 s; at 29.5 s it still has 0.5 s left.
        assert(rhythm.remainingSeconds(s, 29500) == 1)
        assert(rhythm.advance(s, 30000) == "phase")
        assert(s.phaseIndex == 3)
    end,

    ["pause freezes the remaining phase time"] = function()
        local s = state(1)
        rhythm.start(s, 0, 1)
        assert(rhythm.pause(s, 2500))
        assert(rhythm.mode(s) == "paused")
        assert(rhythm.remainingSeconds(s, 2500) == 2)
        assert(rhythm.remainingSeconds(s, 90000) == 2)
        assert(rhythm.advance(s, 90000) == nil)
        assert(rhythm.resume(s, 90000))
        assert(rhythm.mode(s) == "running")
        assert(rhythm.remainingSeconds(s, 90000) == 2)
        assert(rhythm.advance(s, 91500) == "phase")
    end,

    ["skip walks phases and completes the final exhale"] = function()
        local s = state(1)
        rhythm.start(s, 0, 1)
        assert(rhythm.skip(s, 500) == "phase")
        assert(s.phaseIndex == 2)
        assert(rhythm.skip(s, 600) == "phase")
        assert(s.phaseIndex == 3)
        assert(rhythm.skip(s, 700) == "complete")
        assert(rhythm.mode(s) == "complete")
    end,

    ["skipping the last exhale of an earlier round starts the next round"] = function()
        local s = state(2)
        rhythm.start(s, 0, 2)
        rhythm.skip(s, 100)
        rhythm.skip(s, 200)
        assert(rhythm.skip(s, 300) == "phase")
        assert(s.cycle == 2 and s.phaseIndex == 1)
        assert(rhythm.mode(s) == "running")
    end,

    ["stop returns to the first inhale"] = function()
        local s = state(3)
        rhythm.start(s, 0, 3)
        rhythm.advance(s, 12000)
        rhythm.stop(s)
        assert(rhythm.mode(s) == "idle")
        assert(s.cycle == 1 and s.phaseIndex == 1)
        assert(rhythm.remainingSeconds(s, 50000) == 4)
    end,

    ["breath fraction eases up, holds and eases down"] = function()
        assert(rhythm.breathFraction(1, 0) == 0)
        assert(rhythm.breathFraction(1, 1) == 1)
        assert(rhythm.breathFraction(2, 0) == 1)
        assert(rhythm.breathFraction(2, 1) == 1)
        assert(rhythm.breathFraction(3, 0) == 1)
        assert(rhythm.breathFraction(3, 1) == 0)
        local midpoint = rhythm.breathFraction(1, 0.5)
        assert(midpoint == 0.5)
        -- Smoothstep is monotonic and symmetric around the midpoint.
        assert(rhythm.breathFraction(1, 0.25) < midpoint)
        assert(rhythm.breathFraction(1, 0.75) > midpoint)
        assert(rhythm.breathFraction(1, -1) == 0)
        assert(rhythm.breathFraction(1, 2) == 1)
    end,

    ["progress stays within the current phase"] = function()
        local s = state(1)
        rhythm.start(s, 0, 1)
        assert(rhythm.progress(s, 0) == 0)
        assert(rhythm.progress(s, 2000) == 0.5)
        assert(rhythm.progress(s, 4000) == 1)
        rhythm.advance(s, 4000)
        assert(rhythm.progress(s, 4000) == 0)
        assert(rhythm.progress(s, 7500) == 0.5)
    end,
}
