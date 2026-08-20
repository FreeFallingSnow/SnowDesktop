local marquee = module.require("modules/marquee.lua")

return {
    ["text scrolls only when it exceeds the viewport"] = function()
        assert(marquee.shouldScroll(121, 120))
        assert(not marquee.shouldScroll(120, 120))
        assert(not marquee.shouldScroll(80, 120))
    end,

    ["travel advances continuously at the configured speed"] = function()
        assert(marquee.advance(10, 250, 24) == 16)
        assert(marquee.advance(16, 0, 24) == 16)
    end,

    ["position wraps after the text and trailing gap"] = function()
        local offset, cycle = marquee.position(135, 100, 20)
        assert(cycle == 120)
        assert(offset == 15)
    end,
}
