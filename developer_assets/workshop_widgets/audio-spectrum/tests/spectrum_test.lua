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

    ["vertical alignment defaults to center"] = function()
        assert(spectrum.alignment(nil) == "center")
        assert(spectrum.alignment("center") == "center")
        assert(spectrum.alignment("bottom") == "bottom")
        assert(spectrum.alignment("top") == "top")
        assert(spectrum.alignment("sideways") == "center")
    end,

    ["alignment plans mirror only the centered bar series"] = function()
        local bottom = spectrum.seriesPlan("bottom")
        assert(bottom.renderer == "spectrum")
        assert(bottom.minimum == 0 and bottom.maximum == 1)
        assert(bottom.negate == false)
        assert(bottom.mirror == false)

        local center = spectrum.seriesPlan("center")
        assert(center.renderer == "barChart")
        assert(center.minimum == -1 and center.maximum == 1)
        assert(center.negate == false)
        assert(center.mirror == true)

        local top = spectrum.seriesPlan("top")
        assert(top.renderer == "barChart")
        assert(top.minimum == -1 and top.maximum == 0)
        assert(top.negate == true)
        assert(top.mirror == false)
    end,

    ["bar colors are finite rounded rgb values"] = function()
        assert(spectrum.color(nil, 0xFFFFFF) == 0xFFFFFF)
        assert(spectrum.color(-1, 0xFFFFFF) == 0)
        assert(spectrum.color(0x123456.4, 0xFFFFFF) == 0x123456)
        assert(spectrum.color(math.huge, 0xABCDEF) == 0xABCDEF)
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

    ["normalization rebins the complete captured spectrum"] = function()
        local source = {}
        for index = 1, 128 do source[index] = 0 end
        source[128] = 1
        local values = spectrum.normalize(source, 1, 16)
        assert(#values == 16)
        assert(values[1] == 0)
        assert(values[16] > 0.65)
        assert(source[128] == 1)
    end,

    ["display keeps silent bars low and preserves input"] = function()
        local source = { 0, 0.5, -1, math.huge }
        local values = spectrum.display(source, 16)
        assert(#values == 16)
        assert(values[1] == 0.018)
        assert(values[2] == 0.5)
        assert(values[3] == 0.018)
        assert(values[4] == 0.018)
        for index = 5, #values do assert(values[index] == values[1]) end
        assert(source[1] == 0 and source[2] == 0.5 and
            source[3] == -1 and source[4] == math.huge)
    end,

    ["preview spectrum is deterministic varied and bounded"] = function()
        local first = spectrum.preview(64, 1.5)
        local second = spectrum.preview(64, 1.5)
        local minimum = 1
        local maximum = 0
        local distinct = {}
        for index = 1, 64 do
            assert(approximately(first[index], second[index]))
            minimum = math.min(minimum, first[index])
            maximum = math.max(maximum, first[index])
            distinct[string.format("%.3f", first[index])] = true
        end
        local distinctCount = 0
        for _ in pairs(distinct) do distinctCount = distinctCount + 1 end
        assert(minimum >= 0.018 and maximum <= 0.88)
        assert(maximum > 0.65 and distinctCount > 24)
        local softer = spectrum.preview(64, 0.5)
        local stronger = spectrum.preview(64, 3.0)
        assert(softer[10] < first[10])
        assert(stronger[10] > first[10])
    end,


    ["negating bars is finite and does not mutate input"] = function()
        local source = { 0.25, 2, -1, math.huge }
        local values = spectrum.negate(source)
        assert(values[1] == -0.25)
        assert(values[2] == -1)
        assert(values[3] == 0 and values[4] == 0)
        assert(source[1] == 0.25 and source[2] == 2 and
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
