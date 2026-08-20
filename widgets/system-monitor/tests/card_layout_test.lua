local cardLayout = module.require("modules/card_layout.lua")

local function finalRowBottom(rows, cardHeight, gap, inset, offset)
    return inset + (rows - 1) * (cardHeight + gap) - offset +
        cardHeight
end

return {
    ["short overflow aligns the final row to the bottom inset"] = function()
        local viewport = 240
        local content = cardLayout.contentHeight(3, 104, 4, 4, viewport)
        local offset = cardLayout.maximumOffset(content, viewport)
        assert(finalRowBottom(3, 104, 4, 4, offset) == viewport - 4)
    end,

    ["deep overflow keeps the same bottom inset"] = function()
        local viewport = 240
        local content = cardLayout.contentHeight(5, 104, 4, 4, viewport)
        local offset = cardLayout.maximumOffset(content, viewport)
        assert(finalRowBottom(5, 104, 4, 4, offset) == viewport - 4)
    end,

    ["content that fits does not create a scroll offset"] = function()
        local viewport = 360
        local content = cardLayout.contentHeight(2, 104, 4, 4, viewport)
        assert(content == viewport)
        assert(cardLayout.maximumOffset(content, viewport) == 0)
    end,

    ["an empty grid uses the viewport height"] = function()
        assert(cardLayout.contentHeight(0, 104, 4, 4, 240) == 240)
    end,
}
