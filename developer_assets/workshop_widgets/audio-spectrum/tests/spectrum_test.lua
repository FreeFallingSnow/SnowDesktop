local spectrum = module.require("modules/spectrum.lua")

local function approximately(left, right)
    return math.abs(left - right) < 0.000001
end

return {
    ["rising bands attack faster than falling bands release"] = function()
        local rising = spectrum.smooth({ 0.2 }, { 0.8 }, 1, 16, false)
        local falling = spectrum.smooth({ 0.8 }, { 0.2 }, 1, 16, false)
        assert(rising[1] - 0.2 > 0.8 - falling[1])
        assert(approximately(rising[1], 0.572))
        assert(approximately(falling[1], 0.692))
    end,

    ["sensitivity uses bounded defaults"] = function()
        assert(spectrum.sensitivity(nil) == 1.5)
        assert(spectrum.sensitivity(0.1) == 0.5)
        assert(spectrum.sensitivity(5) == 3)
        assert(spectrum.sensitivity(1.8) == 1.8)
    end,

    ["normalization rejects invalid and out of range samples"] = function()
        local invalid = 0 / 0
        local values = spectrum.normalize(
            { -1, 0.5, math.huge, invalid, 2 }, 1.5, 16)
        assert(#values == 16)
        assert(values[1] == 0)
        assert(values[2] == 0.75)
        assert(values[3] == 0 and values[4] == 0)
        assert(values[5] == 1)
    end,

    ["normalization does not mutate its input"] = function()
        local source = { 0.2, 0.4 }
        local values = spectrum.normalize(source, 2, 16)
        assert(source[1] == 0.2 and source[2] == 0.4)
        assert(values[1] == 0.4 and values[2] == 0.8)
    end,

    ["display keeps silent amplitudes low and preserves input"] = function()
        local source = { 0, 0.5, -1, math.huge }
        local values = spectrum.display(source, 16)
        assert(#values == 16)
        assert(values[1] >= 0.018 and values[1] <= 0.04)
        assert(values[2] == 0.5)
        assert(values[3] >= 0.018 and values[3] <= 0.04)
        assert(values[4] >= 0.018 and values[4] <= 0.04)
        assert(source[1] == 0 and source[2] == 0.5 and
            source[3] == -1 and source[4] == math.huge)
    end,

    ["mirrored curve is symmetric smooth and bounded"] = function()
        local source = { 1, 0.6, 0.2, 0.1 }
        local curve = spectrum.mirroredCurve(source, 16)
        assert(#curve >= 31 and #curve <= 257)
        for index = 1, #curve do
            assert(curve[index] >= 0.018 and curve[index] <= 1)
            assert(approximately(curve[index], curve[#curve - index + 1]))
        end
        local center = math.floor(#curve / 2) + 1
        assert(curve[center] > curve[1])
        assert(source[1] == 1 and source[2] == 0.6 and
            source[3] == 0.2 and source[4] == 0.1)
    end,

    ["idle curve stays close to its center line"] = function()
        local curve = spectrum.mirroredCurve({}, 64)
        for index = 1, #curve do
            assert(curve[index] >= 0.018 and curve[index] <= 0.045)
        end
    end,

    ["inverted curve is bounded and leaves input unchanged"] = function()
        local source = { 0.2, 2, -1, math.huge }
        local inverted = spectrum.invert(source)
        assert(inverted[1] == -0.2 and inverted[2] == -1)
        assert(inverted[3] == 0 and inverted[4] == 0)
        assert(source[1] == 0.2 and source[2] == 2 and
            source[3] == -1 and source[4] == math.huge)
    end,

    ["device reset discards the previous endpoint levels"] = function()
        local reset = spectrum.smooth({ 1 }, { 0.5 }, 1, 16, true)
        local retained = spectrum.smooth({ 1 }, { 0.5 }, 1, 16, false)
        assert(approximately(reset[1], 0.31))
        assert(approximately(retained[1], 0.91))
    end,

    ["bin count is rounded and bounded"] = function()
        assert(spectrum.binCount(nil) == 64)
        assert(spectrum.binCount(17) == 16)
        assert(spectrum.binCount(25) == 32)
        assert(spectrum.binCount(500) == 128)
    end,

    ["snapshot states map to stable component states"] = function()
        assert(spectrum.status({ available = true,
            value = { silent = false } }) == "ready")
        assert(spectrum.status({ available = true,
            value = { silent = true } }) == "silent")
        assert(spectrum.status({ available = true, stale = true,
            value = {} }) == "stale")
        assert(spectrum.status({ warmingUp = true }) == "warming")
        assert(spectrum.status({ error = "permissionDenied" }) ==
            "permission")
        assert(spectrum.status({ error = "notPresent" }) == "notPresent")
        assert(spectrum.status({ error = "providerUnavailable" }) ==
            "unavailable")
    end,
}
