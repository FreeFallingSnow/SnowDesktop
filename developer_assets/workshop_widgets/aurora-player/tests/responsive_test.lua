local responsive = module.require("modules/responsive.lua")

return {
    ["landscape and portrait spans choose different content axes"] =
        function()
            local landscape = responsive.plan(392, 240)
            assert(not landscape.vertical and not landscape.compact)

            local portrait = responsive.plan(192, 480)
            assert(portrait.vertical and not portrait.compact)
        end,

    ["small spans use compact controls without changing their axis"] =
        function()
            local square = responsive.plan(192, 240)
            assert(square.vertical and square.compact)

            local shortLandscape = responsive.plan(290, 240)
            assert(not shortLandscape.vertical and shortLandscape.compact)
        end,

    ["invalid dimensions still produce a stable compact plan"] = function()
        local plan = responsive.plan(0, math.huge)
        assert(not plan.vertical and plan.compact)
    end,
}
