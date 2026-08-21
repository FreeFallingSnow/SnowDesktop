local spectrum = module.require("modules/spectrum.lua")

local function approximately(left, right)
    return math.abs(left - right) < 0.000001
end

return {
    ["rising bars attack faster than falling bars release"] = function()
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

    ["display keeps silent bars low and preserves input"] = function()
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

    ["device reset discards the previous endpoint bars"] = function()
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
