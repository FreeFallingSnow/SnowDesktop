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

    ["per-card offset wraps within its stable cycle"] = function()
        assert(marquee.advance(95, 250, 24, 120) == 101)
        assert(marquee.advance(118, 250, 24, 120) == 4)
    end,

    ["scheduled scrolling limits catch-up work"] = function()
        assert(marquee.scheduledDelta(100, 0) == 100)
        assert(marquee.scheduledDelta(100, 1) == 200)
        assert(marquee.scheduledDelta(100, 20) == 200)
    end,

    ["position wraps after the text and trailing gap"] = function()
        local offset, cycle = marquee.position(135, 100, 20)
        assert(cycle == 120)
        assert(offset == 15)
    end,

    ["resizing preserves an offset that remains inside the cycle"] = function()
        local offset = marquee.position(73, 140, 20)
        assert(offset == 73)
        local resized = marquee.position(offset, 145, 20)
        assert(resized == 73)
    end,
}
