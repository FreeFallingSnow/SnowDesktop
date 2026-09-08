local responsive = module.require("modules/responsive.lua")

local function close(left, right)
    return math.abs(left - right) < 0.0001
end

local function assertScaled(base, scaled, factor)
    for key, value in pairs(base) do
        if type(value) == "number" then
            assert(close(scaled[key], value * factor), key)
        else
            assert(scaled[key] == value, key)
        end
    end
end

return {
    ["aspect ratio alone selects the content axis"] = function()
        assert(not responsive.plan(392, 240).vertical)
        assert(not responsive.plan(540, 520).vertical)
        assert(responsive.plan(292, 364).vertical)
    end,

    ["same landscape ratio scales every control metric linearly"] =
        function()
            local base = responsive.plan(392, 240)
            local doubled = responsive.plan(784, 480)
            assertScaled(base, doubled, 2)
        end,

    ["same portrait ratio scales every control metric linearly"] =
        function()
            local base = responsive.plan(292, 364)
            local half = responsive.plan(146, 182)
            assertScaled(base, half, 0.5)
        end,

    ["invalid dimensions still produce finite positive metrics"] = function()
        local plan = responsive.plan(0, math.huge)
        assert(not plan.vertical)
        assert(plan.coverSize > 0 and plan.primaryButton > 0)
    end,
}
