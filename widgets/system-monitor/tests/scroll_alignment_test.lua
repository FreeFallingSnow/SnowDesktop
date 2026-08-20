local alignment = module.require("modules/scroll_alignment.lua")

return {
    ["wheel down advances to the next row boundary"] = function()
        assert(alignment.target(0, 196, 108, -120) == 108)
        assert(alignment.target(108, 196, 108, -120) == 196)
    end,

    ["wheel up returns to the previous row boundary"] = function()
        assert(alignment.target(196, 196, 108, 120) == 108)
        assert(alignment.target(108, 196, 108, 120) == 0)
    end,

    ["short overflow aligns directly to its end"] = function()
        assert(alignment.target(0, 88, 108, -120) == 88)
        assert(alignment.target(88, 88, 108, 120) == 0)
    end,

    ["invalid geometry remains safely clamped"] = function()
        assert(alignment.target(24, 20, 0, -120) == 20)
        assert(alignment.target(24, 20, 8, 0) == 20)
    end,
}
